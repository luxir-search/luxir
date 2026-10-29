// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ParamReader.h"
#include "luxir/query/Query.h"

namespace luxir {

class SearchOp;
class SearchRequest;
class TopDocsReq;
struct ParseContext;
class LuxirNode;

// What a plugin search op maker receives while the request is parsed. The op
// it returns is a SearchOp subclass allocated with luxir::arenaCreate in
// req.arena; its calculator does the work (see docs/dev/plugins.md).
class PluginOpContext {
public:
  SearchRequest& req;
  std::string_view name;           // the op's key in its ops map
  const api::PluginOp& proto;
  ParamReader params;

  PluginOpContext(SearchRequest& req, std::string_view name, const api::PluginOp& proto, ParamReader params)
    : req(req), name(name), proto(proto), params(std::move(params)) {}
  virtual ~PluginOpContext() = default;

  LuxirNode& node() const;

  // The op's query parse context (pool, schema, reader, coercion, warnings).
  virtual ParseContext& parseContext() = 0;
  // A planning context shared by every query this op parses. Queries from
  // parseQuery are already validated against it.
  virtual Query::PlanningContext& planning() = 0;
  // The execution context for createWeight on those queries.
  virtual Query::Context& weightContext() = 0;

  // Lower a nested api::Query, or a Query-shaped param value (an object or an
  // expression string), and validate it.
  virtual Query* parseQuery(const api::Query& query) = 0;
  virtual Query* parseQuery(const api::Val& value) = 0;

  // A TopDocs-shaped param value as a source the op runs itself, as Fusion
  // runs its sources: the op creates the source's calculator under its own,
  // dispatches it per segment, and receives the merged ranking through the
  // rankingSink it sets. The parser does not wire the source: the op must set
  // source->parent (itself) and source->rankingSink, as FusionOp.h describes.
  virtual TopDocsReq* parseSource(std::string_view sourceName, const api::Val& topDocs) = 0;

  // Parse proto.ops as `op`'s nested ops (op.subOps, parent = op). Nested ops
  // take the domain the op gives them and may not declare their own.
  virtual void addSubs(SearchOp& op) = 0;
};

using SearchOpMaker = std::function<SearchOp*(PluginOpContext& ctx)>;

class SearchOpRegistry {
  std::map<std::string, SearchOpMaker, std::less<>> makers;
  bool isFrozen = false;

public:
  SearchOpRegistry();
  static SearchOpRegistry& global();

  // Throws std::invalid_argument on a bad, built-in or duplicate name or a
  // null maker, std::logic_error after freeze.
  void add(std::string name, SearchOpMaker maker);
  const SearchOpMaker* find(std::string_view name) const;
  // Throws RequestError naming the registered operations.
  const SearchOpMaker& require(std::string_view name) const;
  std::vector<std::string> names() const;
  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

}  // namespace luxir
