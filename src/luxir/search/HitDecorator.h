// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>
#include "luxir/query/Query.h"
#include "luxir/util/ParamReader.h"

namespace luxir {
class SearchRequest;
class segdoc;
struct ParseContext;

// A decorator adds a column to emitted rows without changing ranking or membership.
// A call owns one segment's sorted rows within a batch; other calls may run concurrently.
// Mutable readers/iterators must be call-local: the next batch can have lower doc IDs.
class HitDecorator {
public:
  virtual ~HitDecorator() = default;
  virtual std::string_view columnName() const = 0;
  virtual void decorate(std::span<const segdoc> docs, std::span<const uint32_t> rows,
                        std::span<api::Val> out, std::pmr::memory_resource& mr) const = 0;
};

struct DecoratorContext {
  SearchRequest& req;
  ParseContext& parse;
  Query::PlanningContext& planning;
  // Original ranking queries, before separately supplied filters are folded in.
  struct QuerySource { Query* query; Query::PlanningContext& planning; };
  std::span<const QuerySource> queries;
};

class HitDecoratorRegistry {
public:
  using Maker = std::function<std::shared_ptr<HitDecorator>(DecoratorContext&, const ParamReader&)>;
private:
  std::map<std::string, Maker, std::less<>> makers;
  bool frozen = false;
public:
  static HitDecoratorRegistry& global() { static HitDecoratorRegistry registry; return registry; }
  bool contains(std::string_view name) const { return makers.contains(name); }
  void freeze() { frozen = true; }
  void add(std::string name, Maker maker) {
    if (frozen) throw std::logic_error("hit decorator registry is frozen");
    if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
      throw std::invalid_argument("invalid hit decorator name: " + name);
    if (!maker || !makers.emplace(name, std::move(maker)).second)
      throw std::invalid_argument("invalid or duplicate hit decorator: " + name);
  }
  std::vector<std::shared_ptr<HitDecorator>> make(ParamReader::Entries specs, DecoratorContext& ctx) const {
    std::vector<std::shared_ptr<HitDecorator>> result;
    std::set<std::string_view> names, columns;
    for (const auto& [name, value] : specs) {
      if (!names.insert(name).second) throw RequestError("duplicate decorator '" + std::string(name) + "'");
      auto found = makers.find(name);
      if (found == makers.end()) throw RequestError("unknown hit decorator '" + std::string(name) + "'");
      const auto* params = std::get_if<api::Map>(&value->kind);
      if (!params) throw RequestError("decorator '" + std::string(name) + "' requires an object");
      ParamReader reader("decorate." + std::string(name), *params);
      auto decorator = found->second(ctx, reader);
      reader.rejectUnknown();
      if (!decorator || decorator->columnName().empty() || !columns.insert(decorator->columnName()).second)
        throw std::logic_error("decorators must produce unique nonempty columns");
      result.push_back(std::move(decorator));
    }
    return result;
  }
};
} // namespace luxir
