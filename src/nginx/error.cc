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
//
// An Extensible Service Proxy error handling.
//

#include "src/nginx/error.h"
#include "src/api_manager/utils/marshalling.h"
#include "src/nginx/grpc_finish.h"
#include "src/nginx/module.h"
#include "src/nginx/util.h"

using ::google::api_manager::utils::Status;
using ::google::protobuf::util::error::Code;

namespace google {
namespace api_manager {
namespace nginx {
namespace {

ngx_str_t application_json = ngx_string("application/json");
ngx_str_t application_grpc = ngx_string("application/grpc");

ngx_str_t www_authenticate = ngx_string("WWW-Authenticate");
const u_char www_authenticate_lowcase[] = "www-authenticate";
ngx_str_t missing_credential = ngx_string("Bearer");
ngx_str_t invalid_token = ngx_string("Bearer, error=\"invalid_token\"");

ngx_http_output_header_filter_pt ngx_http_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_next_body_filter;

/**
 * Note:
 * We rely on 'err_status' field to detect error responses
 * generated by NGX as opposed to pass-through error responses
 * from upstream.
 *
 * NGX sets this field in ngx_http_send_error and ngx_http_send_refresh
 * before generating HTML response body in ngx_http_special_response_handler.
 *
 * We rely on this exclusive use to change response body with ESP payload.
 *
 * We check that the context belongs to ESP module, not http subrequest
 * (http.cc).
 */
bool ngx_esp_is_error_response(ngx_http_request_t *r,
                               ngx_esp_request_ctx_t *ctx) {
  return r->err_status && r == r->main && ctx &&
         ctx->http_subrequest == nullptr;
}

// Returns WWW-Authenticate header for authentication/authorization
// responses.
// See https://tools.ietf.org/html/rfc6750#section-3.
ngx_int_t ngx_esp_handle_www_authenticate(ngx_http_request_t *r,
                                          ngx_esp_request_ctx_t *ctx) {
  if (r->err_status == NGX_HTTP_UNAUTHORIZED ||
      r->err_status == NGX_HTTP_FORBIDDEN) {
    r->headers_out.www_authenticate = reinterpret_cast<ngx_table_elt_t *>(
        ngx_list_push(&r->headers_out.headers));
    if (r->headers_out.www_authenticate == nullptr) {
      return NGX_ERROR;
    }

    r->headers_out.www_authenticate->key = www_authenticate;
    r->headers_out.www_authenticate->lowcase_key =
        const_cast<u_char *>(www_authenticate_lowcase);
    r->headers_out.www_authenticate->hash =
        ngx_hash_key(const_cast<u_char *>(www_authenticate_lowcase),
                     sizeof(www_authenticate_lowcase) - 1);

    if (ctx->auth_token.len == 0) {
      r->headers_out.www_authenticate->value = missing_credential;
    } else {
      r->headers_out.www_authenticate->value = invalid_token;
    }
  }
  return NGX_OK;
}

ngx_int_t ngx_esp_error_header_filter(ngx_http_request_t *r) {
  ngx_esp_request_ctx_t *ctx = reinterpret_cast<ngx_esp_request_ctx_t *>(
      ngx_http_get_module_ctx(r, ngx_esp_module));

  if (ngx_esp_is_error_response(r, ctx)) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ESP error header code: %d", r->err_status);

    // NGINX generates internal errors in the GRPC processing pipeline
    // (for example, if the original request exceeds the limits)
    if (IsGrpcRequest(r)) {
      // GRPC always uses 200 OK as HTTP status
      r->headers_out.status = NGX_HTTP_OK;
      r->headers_out.content_type = application_grpc;
      r->headers_out.content_type_len = application_grpc.len;
      r->headers_out.content_type_lowcase = nullptr;
    } else {
      // TODO: detect that request was application/json or that the client
      // accepts
      // application/json
      // Update header to JSON content type
      r->headers_out.content_type = application_json;
      r->headers_out.content_type_len = application_json.len;
      r->headers_out.content_type_lowcase = nullptr;

      ngx_int_t ret;
      ret = ngx_esp_handle_www_authenticate(r, ctx);
      if (ret != NGX_OK) return ret;
    }

    // Clear headers (refilled by subsequent NGX header filters)
    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_clear_last_modified(r);
    ngx_http_clear_etag(r);

    return ngx_http_next_header_filter(r);
  }

  return ngx_http_next_header_filter(r);
}

ngx_int_t ngx_esp_error_body_filter(ngx_http_request_t *r, ngx_chain_t *in) {
  ngx_esp_request_ctx_t *ctx = reinterpret_cast<ngx_esp_request_ctx_t *>(
      ngx_http_get_module_ctx(r, ngx_esp_module));

  if (ngx_esp_is_error_response(r, ctx)) {
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ESP error message: %s", ctx->status.message().c_str());

    // Update error code from upstream if error originates from the backend
    if (ctx->status.error_cause() == Status::APPLICATION) {
      ctx->status = Status(r->err_status, Status::CodeToString(r->err_status),
                           Status::APPLICATION);
    }

    if (!r->header_only) {
      if (!IsGrpcRequest(r)) {
        // Serialize error as JSON
        std::string status_in_json;
        if (ctx->transcoder_factory && !ctx->grpc_status_details.empty()) {
          status_in_json = utils::BinStatusToJson(
              ctx->transcoder_factory->GetStatusResolver(),
              ctx->grpc_status_details);
        } else {
          status_in_json = ctx->status.ToJson();
        }

        ngx_buf_t *body = nullptr;
        ngx_str_t json_error;
        if (ngx_str_copy_from_std(r->pool, status_in_json, &json_error) !=
            NGX_OK) {
          return NGX_ERROR;
        }
        // Create temporary buffer to hold data, discard "in"
        body = reinterpret_cast<ngx_buf_t *>(ngx_calloc_buf(r->pool));
        if (body == nullptr) {
          return NGX_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "send error response: %v", &json_error);
        body->temporary = 1;
        body->pos = json_error.data;
        body->last = json_error.data + json_error.len;
        body->last_in_chain = 1;
        body->last_buf = 1;
        ngx_chain_t out = {body, nullptr};
        return ngx_http_next_body_filter(r, &out);
      }
    }
  }

  return ngx_http_next_body_filter(r, in);
}

ngx_int_t ngx_esp_error_postconfiguration(ngx_conf_t *cf) {
  ngx_http_next_header_filter = ngx_http_top_header_filter;
  ngx_http_top_header_filter = ngx_esp_error_header_filter;

  ngx_http_next_body_filter = ngx_http_top_body_filter;
  ngx_http_top_body_filter = ngx_esp_error_body_filter;

  return NGX_OK;
}

ngx_http_module_t ngx_esp_error_module_ctx = {
    nullptr, ngx_esp_error_postconfiguration,
    nullptr, nullptr,
    nullptr, nullptr,
    nullptr, nullptr};
}  // namespace

ngx_int_t ngx_esp_return_error(ngx_http_request_t *r) {
  ngx_esp_request_ctx_t *ctx = reinterpret_cast<ngx_esp_request_ctx_t *>(
      ngx_http_get_module_ctx(r, ngx_esp_module));
  if (ctx == nullptr) {
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "ESP context is NULL, cannot return the error.");
    return NGX_DONE;
  }

  if (ctx->status.code() == NGX_HTTP_CLOSE) {
    return ctx->status.code();
  }

  // Error status marks errors generated by ESP
  // This field update activates the error filter
  r->err_status = ctx->status.HttpCode();

  if (ngx_http_discard_request_body(r) != NGX_OK) {
    r->keepalive = 0;
  }

  // Send error headers if the headers haven't been sent for this request yet.
  // TODO: Make sure that we are sending a valid HTTP response and that the
  //       connection is closed after we send the error.
  if (!r->header_sent) {
    ngx_int_t rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR) {
      return NGX_DONE;
    }
  }

  if (r->header_only) {
    return NGX_DONE;
  }

  // kick in the filter chain that includes the error filter
  ngx_int_t rc = ngx_http_output_filter(r, nullptr);

  if (IsGrpcRequest(r)) {
    return GrpcFinish(r, ctx->status, {});
  } else if (rc == NGX_ERROR) {
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "Failed to write the error output.");
    return NGX_DONE;
  } else {
    return rc;
  }
}
}  // namespace nginx
}  // namespace api_manager
}  // namespace google

//
// Globally scoped module definition
//
ngx_module_t ngx_esp_error_module = {
    NGX_MODULE_V1,                                            // v1 module type
    &::google::api_manager::nginx::ngx_esp_error_module_ctx,  // ctx
    nullptr,                                                  // commands
    NGX_HTTP_MODULE,                                          // type

    // ngx_int_t (*init_master)(ngx_log_t *log)
    nullptr,
    // ngx_int_t (*init_module)(ngx_cycle_t *cycle);
    nullptr,
    // ngx_int_t (*init_process)(ngx_cycle_t *cycle);
    nullptr,
    // ngx_int_t (*init_thread)(ngx_cycle_t *cycle);
    nullptr,
    // void (*exit_thread)(ngx_cycle_t *cycle);
    nullptr,
    // void (*exit_process)(ngx_cycle_t *cycle);
    nullptr,
    // void (*exit_master)(ngx_cycle_t *cycle);
    nullptr,

    NGX_MODULE_V1_PADDING  // padding the rest of the ngx_module_t structure
};
