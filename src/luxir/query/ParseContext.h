#pragma once

#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <google/protobuf/arena.h>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "luxir/util/Clock.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Request-scoped context threaded through query parsing and building: the
// pool the query tree is built into, the schema, the warnings sink (declared
// degradations, surfaced as SearchResponse.warnings), and the shared nesting
// budget.  This is the request-scoped parse/build context from the
// query-string design; the limits-config snapshot joins it later.
struct ParseContext {
  MemPool& pool;
  Schema& schema;
  google::protobuf::Arena& arena;
  // Warning message views must outlive response serialization; warn() copies
  // the message into the pool (whose lifetime spans the request).  May be
  // null when there is nowhere to surface warnings (direct engine tests).
  std::vector<api::Warning>* warnings = nullptr;
  // Top-level operation owning this parse. Warning text uses it to identify
  // the affected request path.
  std::string_view opName;

  // ONE nesting budget for everything recursive a request can stack: the
  // structured-query walk and any string parse it triggers (expr, including
  // re-entrant parses mid-walk) debit the same counter, so nesting one
  // parser inside another cannot reset the available depth.  Request-shape
  // limit: deterministic for a given request, so exceeding it errors freely.
  // The value is a generous default until the limits-config snapshot exists.
  int nestingBudget = 128;

  // Stable across every short-lived QueryBuilder participating in this parse.
  // Request-facing constructors require this explicitly so adding a parser
  // call site cannot silently fall back to UTC.
  CoerceContext coerceContext;

  ParseContext(MemPool& pool, Schema& schema, google::protobuf::Arena& arena,
               const CoerceContext& coerceContext,
               std::string_view opName,
               std::vector<api::Warning>* warnings = nullptr)
    : pool(pool), schema(schema), arena(arena), warnings(warnings), opName(opName),
      coerceContext(coerceContext) {}

  // code must be a string with static storage duration (a literal).
  void warn(std::string_view code, std::string_view message) {
    if (warnings == nullptr) return;
    char* copy = pool.alloc(message.size());
    std::memcpy(copy, message.data(), message.size());
    warnings->push_back({code, std::string_view(copy, message.size())});
  }
};

// RAII debit of the shared nesting budget for one recursion level.  The check
// precedes the debit: a throwing ctor runs no dtor, and the budget must
// balance on unwind (it is shared across the request).
struct NestingScope {
  ParseContext& context;
  explicit NestingScope(ParseContext& context) : context(context) {
    if (context.nestingBudget <= 0) {
      throw std::runtime_error("request nesting exceeds the supported depth");
    }
    --context.nestingBudget;
  }
  ~NestingScope() { ++context.nestingBudget; }
};

} // namespace luxir
