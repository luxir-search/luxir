#include "JsonRequest.h"

#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace luxir {

void parseQueryRequest(std::string_view body, luxir::api::SearchRequest& out,
                       std::pmr::memory_resource& arena) {
  namespace api = luxir::api;

  // One strict parse: the SearchRequest dialect reader (json_dialect.h) accepts
  // the full form, the root top_docs shorthand, and request-level keys mixed
  // with shorthand keys at the same root.
  std::string err;
  if (!api::read_json(out, body, arena, &err)) {
    throw std::runtime_error(!err.empty() ? err : "invalid JSON");
  }
}

void overlayQueryRequest(std::string_view overlay, luxir::api::SearchRequest& out,
                         std::pmr::memory_resource& arena) {
  std::string err;
  if (!luxir::api::merge_json(out, overlay, arena, &err)) {
    throw std::runtime_error(!err.empty() ? err : "invalid URL request-field overlay");
  }
}

} // namespace luxir
