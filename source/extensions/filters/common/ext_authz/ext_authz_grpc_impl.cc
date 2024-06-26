#include "source/extensions/filters/common/ext_authz/ext_authz_grpc_impl.h"

#include "envoy/config/core/v3/base.pb.h"
#include "envoy/service/auth/v3/external_auth.pb.h"

#include "source/common/common/assert.h"
#include "source/common/grpc/async_client_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/network/utility.h"
#include "source/common/protobuf/protobuf.h"

namespace Envoy {
namespace Extensions {
namespace Filters {
namespace Common {
namespace ExtAuthz {

GrpcClientImpl::GrpcClientImpl(const Grpc::RawAsyncClientSharedPtr& async_client,
                               const absl::optional<std::chrono::milliseconds>& timeout)
    : async_client_(async_client), timeout_(timeout),
      service_method_(*Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
          "envoy.service.auth.v3.Authorization.Check")) {}

GrpcClientImpl::~GrpcClientImpl() { ASSERT(!callbacks_); }

void GrpcClientImpl::cancel() {
  ASSERT(callbacks_ != nullptr);
  request_->cancel();
  callbacks_ = nullptr;
}

void GrpcClientImpl::check(RequestCallbacks& callbacks,
                           const envoy::service::auth::v3::CheckRequest& request,
                           Tracing::Span& parent_span, const StreamInfo::StreamInfo& stream_info) {
  ASSERT(callbacks_ == nullptr);
  callbacks_ = &callbacks;
  Http::AsyncClient::RequestOptions options;
  options.setTimeout(timeout_);
  options.setParentContext(Http::AsyncClient::ParentContext{&stream_info});

  ENVOY_LOG(trace, "Sending CheckRequest: {}", request.DebugString());
  request_ = async_client_->send(service_method_, request, *this, parent_span, options);
}

void GrpcClientImpl::onSuccess(std::unique_ptr<envoy::service::auth::v3::CheckResponse>&& response,
                               Tracing::Span& span) {
  ENVOY_LOG(trace, "Received CheckResponse: {}", response->DebugString());
  ResponsePtr authz_response = std::make_unique<Response>(Response{});
  if (response->status().code() == Grpc::Status::WellKnownGrpcStatus::Ok) {
    span.setTag(TracingConstants::get().TraceStatus, TracingConstants::get().TraceOk);
    authz_response->status = CheckStatus::OK;
    if (response->has_ok_response()) {
      toAuthzResponseHeader(authz_response, response->ok_response().headers());
      if (response->ok_response().headers_to_remove_size() > 0) {
        for (const auto& header : response->ok_response().headers_to_remove()) {
          authz_response->headers_to_remove.push_back(Http::LowerCaseString(header));
        }
      }
      if (response->ok_response().query_parameters_to_set_size() > 0) {
        for (const auto& query_parameter : response->ok_response().query_parameters_to_set()) {
          authz_response->query_parameters_to_set.push_back(
              std::pair(query_parameter.key(), query_parameter.value()));
        }
      }
      if (response->ok_response().query_parameters_to_remove_size() > 0) {
        for (const auto& key : response->ok_response().query_parameters_to_remove()) {
          authz_response->query_parameters_to_remove.push_back(key);
        }
      }
      // These two vectors hold header overrides of encoded response headers.
      if (response->ok_response().response_headers_to_add_size() > 0) {
        for (const auto& header : response->ok_response().response_headers_to_add()) {
          if (header.has_append()) {
            if (header.append().value()) {
              authz_response->response_headers_to_add.emplace_back(
                  Http::LowerCaseString(header.header().key()), header.header().value());
            } else {
              authz_response->response_headers_to_set.emplace_back(
                  Http::LowerCaseString(header.header().key()), header.header().value());
            }
          } else {
            switch (header.append_action()) {
              PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
            case Router::HeaderValueOption::APPEND_IF_EXISTS_OR_ADD:
              authz_response->response_headers_to_add.emplace_back(header.header().key(),
                                                                   header.header().value());
              break;
            case Router::HeaderValueOption::ADD_IF_ABSENT:
              authz_response->response_headers_to_add_if_absent.emplace_back(
                  header.header().key(), header.header().value());
              break;
            case Router::HeaderValueOption::OVERWRITE_IF_EXISTS:
              authz_response->response_headers_to_overwrite_if_exists.emplace_back(
                  header.header().key(), header.header().value());
              break;
            case Router::HeaderValueOption::OVERWRITE_IF_EXISTS_OR_ADD:
              authz_response->response_headers_to_set.emplace_back(
                  Http::LowerCaseString(header.header().key()), header.header().value());
              break;
            }
          }
        }
      }
    }
  } else {
    span.setTag(TracingConstants::get().TraceStatus, TracingConstants::get().TraceUnauthz);
    authz_response->status = CheckStatus::Denied;

    // The default HTTP status code for denied response is 403 Forbidden.
    authz_response->status_code = Http::Code::Forbidden;
    if (response->has_denied_response()) {
      toAuthzResponseHeader(authz_response, response->denied_response().headers());

      const uint32_t status_code = response->denied_response().status().code();
      if (status_code > 0) {
        authz_response->status_code = static_cast<Http::Code>(status_code);
      }
      authz_response->body = response->denied_response().body();
    }
  }

  // OkHttpResponse.dynamic_metadata is deprecated. Until OkHttpResponse.dynamic_metadata is
  // removed, it overrides dynamic_metadata field of the outer check response.
  if (response->has_ok_response() && response->ok_response().has_dynamic_metadata()) {
    authz_response->dynamic_metadata = response->ok_response().dynamic_metadata();
  } else {
    authz_response->dynamic_metadata = response->dynamic_metadata();
  }

  callbacks_->onComplete(std::move(authz_response));
  callbacks_ = nullptr;
}

void GrpcClientImpl::onFailure(Grpc::Status::GrpcStatus status, const std::string&,
                               Tracing::Span&) {
  ENVOY_LOG(trace, "CheckRequest call failed with status: {}",
            Grpc::Utility::grpcStatusToString(status));
  ASSERT(status != Grpc::Status::WellKnownGrpcStatus::Ok);
  Response response{};
  response.status = CheckStatus::Error;
  response.status_code = Http::Code::Forbidden;
  callbacks_->onComplete(std::make_unique<Response>(response));
  callbacks_ = nullptr;
}

void GrpcClientImpl::toAuthzResponseHeader(
    ResponsePtr& response,
    const Protobuf::RepeatedPtrField<envoy::config::core::v3::HeaderValueOption>& headers) {
  for (const auto& header : headers) {
    if (header.append().value()) {
      response->headers_to_append.emplace_back(Http::LowerCaseString(header.header().key()),
                                               header.header().value());
    } else {
      response->headers_to_set.emplace_back(Http::LowerCaseString(header.header().key()),
                                            header.header().value());
    }
  }
}

} // namespace ExtAuthz
} // namespace Common
} // namespace Filters
} // namespace Extensions
} // namespace Envoy
