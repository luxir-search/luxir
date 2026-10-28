// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"

namespace luxir {

// Typed reads of component params or a nested object inside them.
// Errors are std::invalid_argument and name the owner and parameter: "lookup: 'max_values' must be an integer".
// Reads record which keys were consumed, so rejectUnknown() can refuse the
// rest with the list of valid names.
class ParamReader {
  mutable std::vector<std::string_view> seen;

public:
  using Entries = api::map_view<std::string_view, ::hpp_proto::indirect_view<api::Val>>;

  std::string owner;  // prefixes every message, e.g. "lookup" or "op 'hood': graph_expand"
  Entries entries;

  ParamReader(std::string owner, Entries entries) : owner(std::move(owner)), entries(entries) {}
  // A nested object param: its own owner path ("graph_expand.hops[0]").
  ParamReader(std::string owner, const api::Map& map) : owner(std::move(owner)), entries(map.fields) {}

  // The raw value, or null when absent (or JSON null).
  const api::Val* get(std::string_view name) const;
  bool has(std::string_view name) const { return get(name) != nullptr; }

  std::string_view getString(std::string_view name, std::string_view fallback = {}) const;
  std::string_view requireString(std::string_view name) const;
  std::optional<int64_t> getInt(std::string_view name) const;
  int64_t getInt(std::string_view name, int64_t fallback) const;
  int64_t requireInt(std::string_view name) const;
  std::optional<double> getDouble(std::string_view name) const;
  double getDouble(std::string_view name, double fallback) const;
  bool getBool(std::string_view name, bool fallback) const;
  // A string or an array of strings; absent is empty.
  std::vector<std::string_view> getStringList(std::string_view name) const;
  // An object param, or null when absent.
  const api::Map* getObject(std::string_view name) const;
  // An array param, or a single value as a one-element list; absent is
  // empty. The values are views into the request, copied out of typed
  // arrays (a string array yields string values).
  std::vector<api::Val> getList(std::string_view name) const;

  // Throws for a key no read asked for, listing the names that were read.
  void rejectUnknown() const;

  [[noreturn]] void fail(std::string_view name, std::string_view what) const;

private:
  void markSeen(std::string_view name) const;
};

}  // namespace luxir
