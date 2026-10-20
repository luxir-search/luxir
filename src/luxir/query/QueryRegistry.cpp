// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/QueryRegistry.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "luxir/query/ExprFunctions.h"
#include "luxir/query/ParseContext.h"
#include "luxir/util/ApiError.h"

namespace luxir {

Query* PluginQueryContext::parseQuery(const api::Val& value) {
  // The parser reads api::Query views; a param holds the same JSON as a Val,
  // so round-trip it. The text and the views it yields live in the pool.
  std::string json;
  if (!api::write_json(value, json)) throw RequestError(std::string(name) + ": cannot serialize a query param");
  char* stored = (char*)parse.pool.allocate(json.size(), 1);
  std::memcpy(stored, json.data(), json.size());
  auto* query = new (parse.pool.allocate(sizeof(api::Query), alignof(api::Query))) api::Query();
  std::string error;
  if (!api::read_json(*query, std::string_view(stored, json.size()), parse.pool, &error)) {
    throw RequestError(std::string(name) + ": invalid query: " + error);
  }
  return parseApi(*query);
}

Query* PluginQueryContext::requireQuery(std::string_view param) {
  const api::Val* value = params.get(param);
  if (value == nullptr) params.fail(param, "is required");
  return parseQuery(*value);
}

void QueryRegistry::add(std::string name, PluginQueryDef def) {
  if (isFrozen) throw std::logic_error("query registry is frozen; query types must be added during startup");
  if (name.empty()) throw std::invalid_argument("query type name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      throw std::invalid_argument("invalid query type name '" + name + "'; use [a-z0-9_]+");
  }
  if (std::ranges::find(expr::ARM_NAMES, std::string_view(name)) != expr::ARM_NAMES.end())
    throw std::invalid_argument("query type '" + name + "' is a built-in query arm");
  if (!def.maker) throw std::invalid_argument("query type '" + name + "' has no maker");
  if (!def.positionalArg.empty()
      && std::ranges::none_of(def.exprArgs, [&](const auto& arg) { return arg.name == def.positionalArg; }))
    throw std::invalid_argument("query type '" + name + "': positional argument '" + def.positionalArg
                                + "' is not one of its expression arguments");
  if (defs.contains(name)) throw std::invalid_argument("duplicate query type '" + name + "'");
  defs.emplace(std::move(name), std::move(def));
}

const PluginQueryDef* QueryRegistry::find(std::string_view name) const {
  auto it = defs.find(name);
  return it == defs.end() ? nullptr : &it->second;
}

const PluginQueryDef& QueryRegistry::require(std::string_view name) const {
  if (const auto* def = find(name)) return *def;
  std::string valid;
  for (const auto& [key, def] : defs) {
    if (!valid.empty()) valid += ", ";
    valid += key;
  }
  throw RequestError("unknown query type '" + std::string(name) + "'; registered plugin query types: "
                     + (valid.empty() ? "none" : valid));
}

std::vector<std::string> QueryRegistry::names() const {
  std::vector<std::string> out;
  for (const auto& [key, def] : defs) out.push_back(key);
  return out;
}

}  // namespace luxir
