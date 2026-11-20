// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ParamReader.h"

namespace luxir {

class Query;
struct ParseContext;
class LuxirNode;

// What a plugin query maker receives: the request's parse context (pool,
// schema, reader, node, coercion context, warnings, field resolver), the arm's
// params, and a way back into the query parser for nested queries.
class PluginQueryContext {
  std::function<Query*(const api::Query&)> parseApi;

public:
  ParseContext& parse;
  const std::string_view name;  // view of the registry's stable key
  ParamReader params;

  PluginQueryContext(ParseContext& parse, std::string_view name, ParamReader params,
                     std::function<Query*(const api::Query&)> parseApi)
    : parseApi(std::move(parseApi)), parse(parse), name(name), params(std::move(params)) {}

  // Null for offline parsing without a node.
  LuxirNode* node() const;

  // Lower a nested api::Query through the ordinary parser (same context and
  // nesting budget).
  Query* parseQuery(const api::Query& query) { return parseApi(query); }
  // Lower a Query-shaped param value: an object ({"match": ...}) or a string
  // (the expression language), the same JSON a Query field accepts.
  Query* parseQuery(const api::Val& value);
  // The param `name` as a nested query; throws when it is absent.
  Query* requireQuery(std::string_view name);
};

// Lowers one plugin query arm to an engine Query (pool-allocated from
// ctx.parse.pool, trivially destructible like every Query). Throw
// RequestError for invalid params.
using QueryMaker = std::function<Query*(PluginQueryContext& ctx)>;

// How the expression language calls a plugin query: name(arg, key=value).
// Each argument's kind decides its grammar, as member types do for the
// built-in functions. A QUERY argument is a sub-expression and reaches the
// maker as Query JSON (parse it with PluginQueryContext::parseQuery).
struct PluginQueryArg {
  enum class Kind { STRING, NUMBER, BOOL, STRING_LIST, QUERY };
  std::string name;
  Kind kind = Kind::STRING;
};

struct PluginQueryDef {
  QueryMaker maker;
  // Empty: structured JSON only (the expression language rejects the name).
  std::vector<PluginQueryArg> exprArgs;
  // The argument a positional value fills, or empty for named-only.
  std::string positionalArg;
};

class QueryRegistry {
  std::map<std::string, PluginQueryDef, std::less<>> defs;
  bool isFrozen = false;

public:
  using Entry = std::pair<const std::string, PluginQueryDef>;

  QueryRegistry();
  static QueryRegistry& global();

  // Throws std::invalid_argument on a bad, built-in or duplicate name or a
  // null maker, std::logic_error after freeze.
  void add(std::string name, PluginQueryDef def);
  void add(std::string name, QueryMaker maker) { add(std::move(name), PluginQueryDef{std::move(maker), {}, {}}); }
  const PluginQueryDef* find(std::string_view name) const;
  // Returns the stable name and definition. Throws RequestError naming the
  // registered query types when absent.
  const Entry& require(std::string_view name) const;
  std::vector<std::string> names() const;
  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

}  // namespace luxir
