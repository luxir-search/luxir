#pragma once

#include <cstring>
#include <string_view>
#include <vector>

#include "solux/api/solux_types.hpp"
#include "solux/schema/Schema.h"
#include "solux/util/MemPool.h"

namespace solux {

// Request-scoped context threaded through query parsing and building: the
// pool the query tree is built into, the schema, and the warnings sink
// (declared degradations, surfaced as SearchResponse.warnings).  This is the
// seed of the request-scoped parse/build context from the query-string
// design; the limits-config snapshot and the shared nesting budget join it
// later.
struct ParseContext {
  MemPool& pool;
  Schema& schema;
  // Warning message views must outlive response serialization; warn() copies
  // the message into the pool (whose lifetime spans the request).  May be
  // null when there is nowhere to surface warnings (direct engine tests).
  std::vector<api::Warning>* warnings = nullptr;

  // code must be a string with static storage duration (a literal).
  void warn(std::string_view code, std::string_view message) {
    if (warnings == nullptr) return;
    char* copy = pool.alloc(message.size());
    std::memcpy(copy, message.data(), message.size());
    warnings->push_back({code, std::string_view(copy, message.size())});
  }
};

} // namespace solux
