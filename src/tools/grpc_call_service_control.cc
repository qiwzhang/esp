// Copyright (C) Extensible Service Proxy Authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//
////////////////////////////////////////////////////////////////////////////////
//
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "google/api/servicecontrol/v1/service_controller.grpc.pb.h"


const char* const SERVICE_NAME =
    "esp-bookstore-n2golbgymq-uc.a.run.app";

using ::grpc::ClientContext;
using ::grpc::Status;

using ::google::api::servicecontrol::v1::ServiceController;
using ::google::api::servicecontrol::v1::ReportRequest;
using ::google::api::servicecontrol::v1::ReportResponse;

int main(int argc, char** argv) {
  auto stub = ServiceController::NewStub(::grpc::CreateChannel("servicecontrol.googleapis.com",
							     ::grpc::GoogleDefaultCredentials()));
  
  ReportRequest report_req;
  report_req.set_service_name(SERVICE_NAME);
  std::cout << "Sending Report requst:" << report_req.DebugString() << std::endl;

  // The actual RPC.
  ReportResponse report_resp;
  ClientContext context;
  Status status = stub->Report(&context, report_req, &report_resp);

  // Act upon its status.
  if (status.ok()) {
    std::cout << "Successfully received Report response:" <<  report_resp.DebugString() << std::endl;
  } else {
    std::cout << status.error_code() << ": " << status.error_message() << std::endl;
  }
  return 0;
}
