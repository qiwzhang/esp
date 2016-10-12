#include "precompiled/precompiled.h"

#include "common/common/logger.h"
#include "envoy/server/instance.h"
#include "common/http/filter/ratelimit.h"
#include "server/config/network/http_connection_manager.h"
#include "include/api_manager/api_manager.h"
#include "common/http/headers.h"
#include "api_manager_env.h"

namespace Http {
namespace ApiManager {

std::string ReadFile(const std::string &file_name) {
  std::ifstream t(file_name);
  std::string content((std::istreambuf_iterator<char>(t)),
                      std::istreambuf_iterator<char>());
  return content;
}

class Config : public Logger::Loggable<Logger::Id::http> {
 private:
  google::api_manager::ApiManagerFactory api_manager_factory_;
  std::shared_ptr<google::api_manager::ApiManager> api_manager_;
  Upstream::ClusterManager& cm_;
 public:
  Config(const Json::Object& config, Upstream::ClusterManager& cm) : cm_(cm) {
    const std::string service_config = config.getString("service_config");

    std::string service_config_content = ReadFile(service_config);

    std::unique_ptr<google::api_manager::ApiManagerEnvInterface> env(new Env(cm));

    api_manager_ =
        api_manager_factory_.GetOrCreateApiManager(std::move(env), service_config_content, "");

    api_manager_->Init();
    log().notice("Called ApiManager::Config constructor: {}", __func__);
  }

  std::shared_ptr<google::api_manager::ApiManager>& api_manager() {
    return api_manager_;
  }
};

typedef std::shared_ptr<Config> ConfigPtr;

class Request : public google::api_manager::Request {
 private:
  HeaderMap& header_map_;
 public:
  Request(HeaderMap& header_map) : header_map_(header_map) {

  }
  virtual std::string GetRequestHTTPMethod() override {
    return header_map_.get(Headers::get().Method);
  }
  virtual std::string GetRequestPath() override {
    return header_map_.get(Headers::get().Path);
  }
  virtual std::string GetUnparsedRequestPath() override {
    return header_map_.get(Headers::get().Path);
  }
  virtual std::string GetClientIP() override {
    return "";
  }
  virtual bool FindQuery(const std::string &name, std::string *query) override {
    return false;
  }
  virtual bool
  FindHeader(const std::string &name, std::string *header) override {
    LowerCaseString lower_key(name);
    if (header_map_.has(lower_key)) {
      *header = header_map_.get(lower_key);
      return true;
    }
    return false;
  }
  virtual google::api_manager::protocol::Protocol
  GetRequestProtocol() override {
    return google::api_manager::protocol::Protocol::HTTP;
  }
  virtual google::api_manager::utils::Status
  AddHeaderToBackend(const std::string &key,
                     const std::string &value) override {
    header_map_.addViaCopy(LowerCaseString(key), value);
    return google::api_manager::utils::Status::OK;
  }
  virtual void SetAuthToken(const std::string &auth_token) override {
  }
};

const Http::HeaderMapImpl BadRequest{
    {Http::Headers::get().Status, "400"}};

class Instance : public Http::StreamDecoderFilter, public Logger::Loggable<Logger::Id::http> {
 private:
  std::shared_ptr<google::api_manager::ApiManager> api_manager_;
  std::unique_ptr<google::api_manager::RequestHandlerInterface> request_handler_;

  enum State { NotStarted, Calling, Complete, Responded };
  State state_;

  StreamDecoderFilterCallbacks* callbacks_;

  bool initiating_call_;
 public:
  Instance(ConfigPtr config) : api_manager_(config->api_manager()), state_(NotStarted), initiating_call_(false) {
    log().notice("Called ApiManager::Instance : {}", __func__);
  }

  FilterHeadersStatus decodeHeaders(HeaderMap& headers, bool end_stream) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    std::unique_ptr<google::api_manager::Request> request(
        new Request(headers));
    request_handler_ = api_manager_->CreateRequestHandler(std::move(request));
    state_ = Calling;
    initiating_call_ = true;
    request_handler_->Check([this](google::api_manager::utils::Status status){
      complete(status);
    });
    initiating_call_ = false;
    if (state_ == Complete) {
      return FilterHeadersStatus::Continue;
    }
    log().notice("Called ApiManager::Instance : {} Stop", __func__);
    return FilterHeadersStatus::StopIteration;
  }

  FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    if (state_ == Calling) {
      return FilterDataStatus::StopIterationAndBuffer;
    }
    return FilterDataStatus::Continue;
  }
  FilterTrailersStatus decodeTrailers(HeaderMap& trailers) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    if (state_ == Calling) {
      return FilterTrailersStatus::StopIteration;
    }
    return FilterTrailersStatus::Continue;
  }
  void setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    callbacks_ = &callbacks;
    callbacks_->addResetStreamCallback([](){});
  }
  void complete(const google::api_manager::utils::Status &status) {
    log().notice("Called ApiManager::Instance : check complete {}", status.ToJson());
    if (!status.ok()) {
      state_ = Responded;
      HeaderMapPtr response { new HeaderMapImpl(BadRequest) };
      callbacks_->encodeHeaders(std::move(response), true);
      return;
    }
    state_ = Complete;
    if (!initiating_call_) {
      callbacks_->continueDecoding();
    }
  }
};

}
}

namespace Server {
namespace Configuration {


class ApiManagerConfig : public HttpFilterConfigFactory {
public:
  HttpFilterFactoryCb tryCreateFilterFactory(HttpFilterType type, const std::string& name,
                                             const Json::Object& config, const std::string&,
                                             Server::Instance& server) override {
    if (type != HttpFilterType::Decoder || name != "esp") {
      return nullptr;
    }

    Http::ApiManager::ConfigPtr api_manager_config(new Http::ApiManager::Config(config, server.clusterManager()));
    return [api_manager_config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
      callbacks.addStreamDecoderFilter(Http::StreamDecoderFilterPtr{new Http::ApiManager::Instance(
          api_manager_config)});
    };
  }
};

static RegisterHttpFilterConfigFactory<ApiManagerConfig> register_;
}
}
