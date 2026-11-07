// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"

namespace luxir::source {

// Narrows the source kept for a document to some of its paths: only those
// matching `includes`, or all but those matching `excludes` (never both). The
// document is indexed in full; only what the source store keeps changes.
//
// A path joins object keys with '.' ("user.name"); an array adds no step, so a
// pattern reaches the objects inside an array at the array's own path. A
// pattern matches the whole path, and '*' matches any run of characters, dots
// included. A matching path keeps (includes) or drops (excludes) its whole
// value. With includes, an object or array left with nothing is dropped
// together with its key; with excludes it stays, emptied.
class SourceFilter {
public:
  std::vector<std::string> includes;
  std::vector<std::string> excludes;

  SourceFilter() = default;
  // Throws std::invalid_argument if both lists are set or a pattern is empty.
  SourceFilter(std::span<const std::string_view> includes, std::span<const std::string_view> excludes);

  bool empty() const { return includes.empty() && excludes.empty(); }

  // Appends the filtered form of `json`, one JSON object, to `out`. False if
  // `json` is not shaped as an object (the caller keeps the document another
  // way); `out` then holds a partial result.
  bool filterJson(std::string_view json, std::string& out) const;

  // The filtered form of `doc`. New nodes are allocated in `mr`; kept values
  // are shared with `doc`, so the result lives no longer than either.
  api::Map filterMap(const api::Map& doc, std::pmr::memory_resource& mr) const;

private:
  enum class Decision { KEEP, DROP, DESCEND };
  Decision decide(std::string_view path) const;

  bool filterObject(std::string_view s, size_t& i, std::string& path, std::string& out, size_t& kept) const;
  bool filterArray(std::string_view s, size_t& i, std::string& path, std::string& out, size_t& kept) const;
  api::Map filterMapAt(const api::Map& map, std::string& path, std::pmr::memory_resource& mr) const;
  const api::Val* filterVal(const api::Val& val, std::string& path, std::pmr::memory_resource& mr) const;
};

}  // namespace luxir::source
