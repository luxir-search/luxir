// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include "IntColReader.h"
#include "PostingsReader.h"

namespace luxir {

// The persisted owner map of one segment's nested row space: owner row ->
// [begin, end) of its element rows, and element row -> owner row. Element rows
// are stored in owner order, then array order, so both maps are monotone.
// A segment that does not carry the path has no element rows at all.
class OwnerMap {
  std::optional<MonoReader> ends;
  std::optional<MonoReader> owners;
  int32_t ownerMaxDoc_ = 0;
  int32_t rows_ = 0;

public:
  OwnerMap() = default;
  OwnerMap(PostingsReader& scope, int32_t ownerMaxDoc) : ownerMaxDoc_(ownerMaxDoc), rows_(scope.maxDoc()) {
    const auto& loc = scope.ownerLocations();
    if (loc.ownerMaxDoc < 0) return;  // empty scope: no rows
    if (loc.ownerMaxDoc != ownerMaxDoc) {
      throw std::runtime_error("nested row space '" + scope.scope() + "' was written for " +
                               std::to_string(loc.ownerMaxDoc) + " owner rows, not " +
                               std::to_string(ownerMaxDoc));
    }
    ends.emplace(scope, loc.endsLoc, loc.endsMetaOff, ownerMaxDoc);
    owners.emplace(scope, loc.ownersLoc, loc.ownersMetaOff, rows_);
  }

  int32_t ownerMaxDoc() const noexcept { return ownerMaxDoc_; }
  int32_t rows() const noexcept { return rows_; }
  bool empty() const noexcept { return rows_ == 0; }

  // Element rows of owner row `owner`: [begin, end).
  std::pair<int32_t, int32_t> range(int32_t owner) const {
    if (!ends) return {0, 0};
    auto [begin, end] = ends->valuesAt(owner);
    return {(int32_t)begin, (int32_t)end};
  }
  // First element row of owner `owner` (== rows() past the last owner).
  int32_t begin(int32_t owner) const {
    if (!ends || owner <= 0) return 0;
    if (owner >= ownerMaxDoc_) return rows_;
    return (int32_t)ends->valueAt(owner - 1);
  }
  int32_t owner(int32_t row) const { return (int32_t)owners->valueAt(row); }

  const MonoReader* endsReader() const noexcept { return ends ? &*ends : nullptr; }
  const MonoReader* ownersReader() const noexcept { return owners ? &*owners : nullptr; }

  // Forward cursor over element row -> owner row, for streaming joins.
  class OwnerCursor {
    std::optional<MonoReader::BulkValues> values;

  public:
    explicit OwnerCursor(const OwnerMap& map) {
      if (map.owners) values.emplace(*map.owners);
    }
    int32_t ownerOf(int32_t row) { return (int32_t)values->valueAt(row); }
  };

  // Forward cursor over owner row -> element range.
  class RangeCursor {
    std::optional<MonoReader::BulkValues> values;

  public:
    explicit RangeCursor(const OwnerMap& map) {
      if (map.ends) values.emplace(*map.ends);
    }
    std::pair<int32_t, int32_t> range(int32_t owner) {
      if (!values) return {0, 0};
      int64_t begin = owner == 0 ? 0 : values->valueAt(owner - 1);
      return {(int32_t)begin, (int32_t)values->valueAt(owner)};
    }
  };
};

} // namespace luxir
