// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdio>
#include "ApiError.h"
#include "log.h"

namespace luxir {

// Borrowed context, used only during the call. Diagnostics must never prevent
// the caller from completing a failed request or releasing its resources.
struct ErrorContext {
  std::string_view operation;
  std::string_view tenant;
  std::string_view collection;
  std::string_view requestId;
};

inline void logError(const ErrorInfo& error, ErrorContext context,
                     std::string_view description = "Request failed") noexcept {
  try {
    auto level = error.kind == ErrorKind::INTERNAL || error.kind == ErrorKind::UNKNOWN
        ? spdlog::level::err : spdlog::level::warn;
    spdlog::log(spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}, level,
        "{} operation={} tenant={} collection={} request_id={} code={} message={}",
        description, context.operation, context.tenant, context.collection, context.requestId,
        error.code, error.message);
  } catch (...) {
    std::fputs("Luxir: failed to log an error\n", stderr);
  }
}

// Use only inside a catch block. Even classification can allocate; the fixed
// fallback still records a failure when memory is exhausted.
inline void logCurrentException(ErrorContext context) noexcept {
  try { logError(currentExceptionInfo(ErrorKind::INTERNAL), context); }
  catch (...) { std::fputs("Luxir: exception reporting failed\n", stderr); }
}

} // namespace luxir
