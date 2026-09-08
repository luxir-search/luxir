// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Error classification.  Every failure that can reach a client resolves to an
// ErrorInfo {kind, code, message}: kind is the coarse class that picks the
// transport status, code is the stable machine key, message is human detail.
// The value is owning and transport-independent, so it can cross threads (the
// update graph, streaming accumulators) and be rendered by HTTP, gRPC, and the
// in-band response messages alike.  Exceptions are one way to produce it: an
// ApiError knows its classification at the throw site; a bare std::exception
// is classified by the catching phase's fallback kind.

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace luxir {

// Mirrors luxir.Error.Kind value for value (api/build.h asserts the
// pairing).  Default transport mapping, refined per code by the transports:
//   INVALID_REQUEST      HTTP 400  gRPC INVALID_ARGUMENT
//   NOT_FOUND            HTTP 404  gRPC NOT_FOUND
//   ALREADY_EXISTS       HTTP 409  gRPC ALREADY_EXISTS
//   FAILED_PRECONDITION  HTTP 403  gRPC FAILED_PRECONDITION
//   RESOURCE_EXHAUSTED   HTTP 429  gRPC RESOURCE_EXHAUSTED
//   UNAVAILABLE          HTTP 503  gRPC UNAVAILABLE
//   INTERNAL             HTTP 500  gRPC INTERNAL
enum class ErrorKind : uint8_t {
  UNKNOWN = 0,
  INVALID_REQUEST = 1,
  NOT_FOUND = 2,
  ALREADY_EXISTS = 3,
  FAILED_PRECONDITION = 4,
  RESOURCE_EXHAUSTED = 5,
  UNAVAILABLE = 6,
  INTERNAL = 7,
};

// The kind's wire spelling; doubles as the code of an unclassified failure.
constexpr std::string_view errorKindName(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::INVALID_REQUEST: return "invalid_request";
    case ErrorKind::NOT_FOUND: return "not_found";
    case ErrorKind::ALREADY_EXISTS: return "already_exists";
    case ErrorKind::FAILED_PRECONDITION: return "failed_precondition";
    case ErrorKind::RESOURCE_EXHAUSTED: return "resource_exhausted";
    case ErrorKind::UNAVAILABLE: return "unavailable";
    case ErrorKind::INTERNAL: return "internal";
    case ErrorKind::UNKNOWN: break;
  }
  return "unknown";
}

struct ErrorInfo {
  ErrorKind kind = ErrorKind::INTERNAL;
  std::string code;     // stable machine key, snake_case ("collection_not_found")
  std::string message;  // human detail; wording is not part of the contract

  // An error of `kind` with no more specific code than the kind itself.
  static ErrorInfo of(ErrorKind kind, std::string message) {
    return {kind, std::string(errorKindName(kind)), std::move(message)};
  }
};

// An exception that carries its classification.  Throw it (or a subclass)
// where the failure's class is known; the transports render it as-is.
class ApiError : public std::runtime_error {
public:
  ErrorKind kind;
  std::string code;

  ApiError(ErrorKind kind, std::string code, const std::string& message)
    : std::runtime_error(message), kind(kind), code(std::move(code)) {}

  ErrorInfo info() const { return {kind, code, what()}; }
};

// The request as written cannot be served.
class RequestError : public ApiError {
public:
  explicit RequestError(const std::string& message, std::string code = "invalid_request")
    : ApiError(ErrorKind::INVALID_REQUEST, std::move(code), message) {}
};

// A document that cannot be indexed as given: a value its field rejects by
// type, shape, or cardinality.  invalid_value unless the site says more.
class DocumentError : public ApiError {
public:
  explicit DocumentError(const std::string& message, std::string code = "invalid_value")
    : ApiError(ErrorKind::INVALID_REQUEST, std::move(code), message) {}
};

// A request outgrew a size or memory ceiling.
class ResourceExhaustedError : public ApiError {
public:
  explicit ResourceExhaustedError(const std::string& message,
                                  std::string code = "resource_exhausted")
    : ApiError(ErrorKind::RESOURCE_EXHAUSTED, std::move(code), message) {}
};

// An ApiError yields its own classification; anything else takes the caller's
// phase fallback (request parsing: INVALID_REQUEST; execution: INTERNAL) and
// `code` - by default the kind's name, the honest answer for an unclassified
// failure.
inline ErrorInfo classifyException(const std::exception& e, ErrorKind fallback,
                                   std::string_view code = {}) {
  if (const auto* api = dynamic_cast<const ApiError*>(&e)) return api->info();
  if (code.empty()) code = errorKindName(fallback);
  return {fallback, std::string(code), e.what()};
}

// classifyException for the in-flight exception inside a catch(...) block.
inline ErrorInfo currentExceptionInfo(ErrorKind fallback, std::string_view code = {}) {
  try {
    throw;
  } catch (const std::exception& e) {
    return classifyException(e, fallback, code);
  } catch (...) {
    if (code.empty()) code = errorKindName(fallback);
    return {fallback, std::string(code), "unknown non-standard exception"};
  }
}

}  // namespace luxir
