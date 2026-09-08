// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "JsonRequest.h"

#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "luxir/util/ApiError.h"

namespace luxir {

void parseQueryRequest(std::string_view body, luxir::api::SearchRequest& out,
                       std::pmr::memory_resource& arena) {
  namespace api = luxir::api;

  // One strict parse: the SearchRequest dialect reader (json_dialect.h) accepts
  // the full form, the root top_docs shorthand, and request-level keys mixed
  // with shorthand keys at the same root.
  std::string err;
  if (!api::read_json(out, body, arena, &err)) {
    throw RequestError(!err.empty() ? err : "invalid JSON", "invalid_json");
  }
}

void overlayQueryRequest(std::string_view overlay, luxir::api::SearchRequest& out,
                         std::pmr::memory_resource& arena) {
  std::string err;
  if (!luxir::api::merge_json(out, overlay, arena, &err)) {
    throw RequestError(!err.empty() ? err : "invalid URL request-field overlay");
  }
}

} // namespace luxir
