// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/SearchOpRegistry.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "luxir/util/ApiError.h"

namespace luxir {

namespace {

// SearchOp arm keys and the other keys a SearchOp object reads.
constexpr std::array<std::string_view, 10> RESERVED = {
    "top_docs", "fusion", "field_facet", "range_facet", "query_facet", "expr_op", "plugin",
    "domain", "flags", "flags_set"};

}  // namespace

void SearchOpRegistry::add(std::string name, SearchOpMaker maker) {
  if (isFrozen) throw std::logic_error("search op registry is frozen; operations must be added during startup");
  if (name.empty()) throw std::invalid_argument("search op name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
      throw std::invalid_argument("invalid search op name '" + name + "'; use [a-z0-9_]+");
  }
  if (std::ranges::find(RESERVED, std::string_view(name)) != RESERVED.end())
    throw std::invalid_argument("search op '" + name + "' is a built-in SearchOp key");
  if (!maker) throw std::invalid_argument("search op '" + name + "' has no maker");
  if (makers.contains(name)) throw std::invalid_argument("duplicate search op '" + name + "'");
  makers.emplace(std::move(name), std::move(maker));
}

const SearchOpMaker* SearchOpRegistry::find(std::string_view name) const {
  auto it = makers.find(name);
  return it == makers.end() ? nullptr : &it->second;
}

const SearchOpMaker& SearchOpRegistry::require(std::string_view name) const {
  if (const auto* maker = find(name)) return *maker;
  std::string valid;
  for (const auto& [key, maker] : makers) {
    if (!valid.empty()) valid += ", ";
    valid += key;
  }
  throw RequestError("unknown search op '" + std::string(name) + "'; registered plugin operations: "
                     + (valid.empty() ? "none" : valid));
}

std::vector<std::string> SearchOpRegistry::names() const {
  std::vector<std::string> out;
  for (const auto& [key, maker] : makers) out.push_back(key);
  return out;
}

}  // namespace luxir
