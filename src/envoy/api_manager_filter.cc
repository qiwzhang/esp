#include "precompiled/precompiled.h"

#include "api_manager_env.h"
#include "common/buffer/buffer_impl.h"
#include "common/common/logger.h"
#include "common/http/filter/ratelimit.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "envoy/server/instance.h"
#include "include/api_manager/api_manager.h"
#include "server/config/network/http_connection_manager.h"

namespace Http {
namespace ApiManager {

std::string ReadFile(const std::string& file_name) {
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

    std::unique_ptr<google::api_manager::ApiManagerEnvInterface> env(
        new Env(cm));

    api_manager_ = api_manager_factory_.GetOrCreateApiManager(
        std::move(env), service_config_content, "");

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
  Request(HeaderMap& header_map) : header_map_(header_map) {}
  virtual std::string GetRequestHTTPMethod() override {
    return header_map_.get(Headers::get().Method);
  }
  virtual std::string GetRequestPath() override {
    return header_map_.get(Headers::get().Path);
  }
  virtual std::string GetUnparsedRequestPath() override {
    return header_map_.get(Headers::get().Path);
  }
  virtual std::string GetClientIP() override { return ""; }
  virtual bool FindQuery(const std::string& name, std::string* query) override {
    return false;
  }
  virtual bool FindHeader(const std::string& name,
                          std::string* header) override {
    LowerCaseString lower_key(name);
    if (header_map_.has(lower_key)) {
      *header = header_map_.get(lower_key);
      return true;
    }
    return false;
  }
  virtual google::api_manager::protocol::Protocol GetRequestProtocol()
      override {
    return google::api_manager::protocol::Protocol::HTTP;
  }
  virtual google::api_manager::utils::Status AddHeaderToBackend(
      const std::string& key, const std::string& value) override {
    header_map_.addViaCopy(LowerCaseString(key), value);
    return google::api_manager::utils::Status::OK;
  }
  virtual void SetAuthToken(const std::string& auth_token) override {}
};

class EnvoyZeroCopyInputStream
    : public google::protobuf::io::ZeroCopyInputStream {
 private:
  std::deque<Buffer::RawSlice> data_;
  std::vector<Buffer::OwnedImpl> owned_;

 public:
  void Add(Buffer::Instance& instance) {
    owned_.emplace_back(instance);

    Buffer::OwnedImpl& data = owned_.back();
    uint64_t num = data.getRawSlices(nullptr, 0);
    std::vector<Buffer::RawSlice> slices(num);
    data.getRawSlices(slices.data(), num);

    for (const auto& slice : slices) {
      data_.emplace_back(slice);
    }
  }
  virtual bool Next(const void** data, int* size) override {
    if (!data_.empty()) {
      Buffer::RawSlice slice = data_.front();
      data_.pop_front();
      *data = slice.mem_;
      *size = slice.len_;
      return true;
    }
    return false;
  }
  virtual void BackUp(int count) override {}
  virtual bool Skip(int count) override { return false; }
  virtual google::protobuf::int64 ByteCount() const override {
    google::protobuf::int64 count = 0;
    for (const auto& slice : data_) {
      count += slice.len_;
    }
    return count;
  }
};

class Instance : public Http::StreamFilter,
                 public Logger::Loggable<Logger::Id::http> {
 private:
  std::shared_ptr<google::api_manager::ApiManager> api_manager_;
  std::unique_ptr<google::api_manager::RequestHandlerInterface>
      request_handler_;

  enum State { NotStarted, Calling, Complete, Responded };
  State state_;

  StreamDecoderFilterCallbacks* decoder_callbacks_;
  StreamEncoderFilterCallbacks* encoder_callbacks_;

  bool initiating_call_;

  std::unique_ptr<google::api_manager::transcoding::Transcoder> transcoder_;
  EnvoyZeroCopyInputStream request_in_, response_in_;

 public:
  Instance(ConfigPtr config)
      : api_manager_(config->api_manager()),
        state_(NotStarted),
        initiating_call_(false) {
    log().notice("Called ApiManager::Instance : {}", __func__);
  }

  FilterHeadersStatus decodeHeaders(HeaderMap& headers,
                                    bool end_stream) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    std::unique_ptr<google::api_manager::Request> request(new Request(headers));
    request_handler_ = api_manager_->CreateRequestHandler(std::move(request));
    state_ = Calling;
    initiating_call_ = true;
    request_handler_->Check([this](google::api_manager::utils::Status status) {
      completeCheck(status);
    });
    initiating_call_ = false;

    if (request_handler_->CanBeTranscoded()) {
      std::string method_name = request_handler_->GetRpcMethodFullName();
      log().notice("Called ApiManager::Instance : creatingTranscoder {}",
                   method_name);

      headers.replaceViaMoveValue(Headers::get().Method, "POST");
      headers.replaceViaMoveValue(Headers::get().Path, std::move(method_name));
      headers.replaceViaMoveValue(Headers::get().ContentType,
                                  "application/grpc");
      headers.replaceViaMove(LowerCaseString("te"), "trailers");

      request_handler_->CreateTranscoder(&request_in_, &response_in_,
                                         &transcoder_);
    }
    if (state_ == Complete) {
      return FilterHeadersStatus::Continue;
    }
    log().notice("Called ApiManager::Instance : {} Stop", __func__);
    return FilterHeadersStatus::StopIteration;
  }

  FilterDataStatus decodeData(Buffer::Instance& data,
                              bool end_stream) override {
    log().notice("Called ApiManager::Instance : {} ({}, {})", __func__,
                 data.length(), end_stream);
    if (transcoder_) {
      request_in_.Add(data);
      data.drain(data.length());

      auto output = transcoder_->RequestOutput();

      const void* out;
      int size;
      while (output->Next(&out, &size)) {
        data.add(out, size);
      }
    }
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
  void setDecoderFilterCallbacks(
      StreamDecoderFilterCallbacks& callbacks) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    decoder_callbacks_ = &callbacks;
    decoder_callbacks_->addResetStreamCallback(
        [this]() { state_ = Responded; });
  }
  void completeCheck(const google::api_manager::utils::Status& status) {
    log().notice("Called ApiManager::Instance : check complete {}",
                 status.ToJson());
    if (!status.ok() && state_ != Responded) {
      state_ = Responded;
      Utility::sendLocalReply(*decoder_callbacks_, Code(status.HttpCode()),
                              status.ToJson());
      return;
    }
    state_ = Complete;
    if (!initiating_call_) {
      decoder_callbacks_->continueDecoding();
    }
  }

  virtual FilterHeadersStatus encodeHeaders(HeaderMap& headers,
                                            bool end_stream) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    return FilterHeadersStatus::Continue;
  }
  virtual FilterDataStatus encodeData(Buffer::Instance& data,
                                      bool end_stream) override {
    log().notice("Called ApiManager::Instance : {} ({}, {})", __func__,
                 data.length(), end_stream);
    if (transcoder_) {
      response_in_.Add(data);

      data.drain(data.length());

      auto output = transcoder_->ResponseOutput();

      const void* out;
      int size;
      while (output->Next(&out, &size)) {
        log().notice(
            "Called ApiManager::Instance : response out {} bytes, status: {}",
            size, transcoder_->ResponseStatus().error_code());
        if (size == 0) break;
        data.add(out, size);
      }
    }
    return FilterDataStatus::Continue;
  }
  virtual FilterTrailersStatus encodeTrailers(HeaderMap& trailers) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    return FilterTrailersStatus::Continue;
  }
  virtual void setEncoderFilterCallbacks(
      StreamEncoderFilterCallbacks& callbacks) override {
    log().notice("Called ApiManager::Instance : {}", __func__);
    encoder_callbacks_ = &callbacks;
  }
};
}
}

namespace Server {
namespace Configuration {

class ApiManagerConfig : public HttpFilterConfigFactory {
 public:
  HttpFilterFactoryCb tryCreateFilterFactory(
      HttpFilterType type, const std::string& name, const Json::Object& config,
      const std::string&, Server::Instance& server) override {
    if (type != HttpFilterType::Both || name != "esp") {
      return nullptr;
    }

    Http::ApiManager::ConfigPtr api_manager_config(
        new Http::ApiManager::Config(config, server.clusterManager()));
    return [api_manager_config](
               Http::FilterChainFactoryCallbacks& callbacks) -> void {
      callbacks.addStreamFilter(Http::StreamFilterPtr{
          new Http::ApiManager::Instance(api_manager_config)});
    };
  }
};

static RegisterHttpFilterConfigFactory<ApiManagerConfig> register_;
}
}
