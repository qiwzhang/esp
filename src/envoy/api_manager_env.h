#pragma once

#include "precompiled/precompiled.h"

#include "common/common/logger.h"
#include "envoy/upstream/cluster_manager.h"
#include "include/api_manager/env_interface.h"

namespace Http {
namespace ApiManager {

class Env : public google::api_manager::ApiManagerEnvInterface,
            public Logger::Loggable<Logger::Id::http> {
 private:
  Upstream::ClusterManager& cm_;

 public:
  Env(Upstream::ClusterManager& cm) : cm_(cm){};

  virtual void Log(LogLevel level, const char* message) override;
  virtual google::api_manager::AsyncGrpcQueue* GetAsyncQueue() override;
  virtual std::unique_ptr<google::api_manager::PeriodicTimer>
  StartPeriodicTimer(std::chrono::milliseconds interval,
                     std::function<void()> continuation) override;
  virtual void RunHTTPRequest(
      std::unique_ptr<google::api_manager::HTTPRequest> request) override;
};
}
}