// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "luxir/query/BKDQueryWeight.h"
#include "luxir/query/RangePointSource.h"
#include "luxir/query/Query.h"

namespace luxir {

// Inclusive encoded box over (min, max). Every range-field relation is one
// such box (see RangeFieldQuery); an unbounded side is the int64 extreme.
struct RangeBoxRelation {
  int64_t lo[2];
  int64_t hi[2];

  BKDRelation compare(int64_t min0, int64_t max0, int64_t min1, int64_t max1) const {
    int64_t cellLo[2] = {min0, min1};
    int64_t cellHi[2] = {max0, max1};
    bool inside = true;
    for (int d = 0; d < 2; d++) {
      if (cellHi[d] < lo[d] || hi[d] < cellLo[d]) return BKDRelation::OUTSIDE;
      inside = inside && lo[d] <= cellLo[d] && cellHi[d] <= hi[d];
    }
    return inside ? BKDRelation::INSIDE : BKDRelation::CROSSES;
  }

  BKDPointCost pointCost() const { return {0.0, 0.0, false}; }

  bool matches(int64_t min, int64_t max) const {
    return lo[0] <= min && min <= hi[0] && lo[1] <= max && max <= hi[1];
  }
};

// Relates the [min, max] intervals of a RANGE field to an inclusive encoded
// query interval [lo, hi]. Every relation is one box over (min, max):
//   intersects: min <= hi AND max >= lo
//   contains:   min <= lo AND max >= hi
//   within:     min >= lo AND max <= hi
// Shared BKD execution chooses materialization, sparse column verification
// or a column scan using the range point source's prices.
class RangeFieldQuery final : public Query {
public:
  enum class Relation : uint8_t { INTERSECTS, CONTAINS, WITHIN };

private:
  std::string_view field;
  Relation relation;
  RangeBoxRelation box;
  // REQUEST_STABLE when a DATE bound came from unrounded NOW math, as for
  // NumericPredicateQuery. equals()/hash() ignore it.
  FilterKeyScope filterKeyScope = FilterKeyScope::SEGMENT_STABLE;

  static RangeBoxRelation makeBox(Relation relation, int64_t lo, int64_t hi) {
    constexpr int64_t MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t MAX = std::numeric_limits<int64_t>::max();
    switch (relation) {
      case Relation::INTERSECTS: return {{MIN, lo}, {hi, MAX}};
      case Relation::CONTAINS: return {{MIN, hi}, {lo, MAX}};
      case Relation::WITHIN: return {{lo, MIN}, {MAX, hi}};
    }
    std::unreachable();
  }

public:
  static constexpr std::string_view QUERY_NAME = "RangeFieldQuery";
  using PointSource = RangePointSource;
  using Weight = BKDQueryWeight<RangeFieldQuery, RangeBoxRelation>;

  bool recoverableAsPredicate() const noexcept override { return true; }

  // lo <= hi: an empty query interval is the builder's match-nothing.
  RangeFieldQuery(std::string_view field, Relation relation, int64_t lo, int64_t hi,
                  FilterKeyScope filterKeyScope = FilterKeyScope::SEGMENT_STABLE)
    : Query(QueryKind::RANGE_FIELD), field(field), relation(relation),
      box(makeBox(relation, lo, hi)), filterKeyScope(filterKeyScope) {
    assert(lo <= hi);
  }

  std::string_view getField() const { return field; }
  Relation getRelation() const { return relation; }
  const RangeBoxRelation& getBox() const { return box; }
  const RangeBoxRelation& makeRelation() const { return box; }
  bool isEmpty() const { return false; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const RangeFieldQuery&>(other);
    return field == rhs.field && relation == rhs.relation
        && std::ranges::equal(box.lo, rhs.box.lo) && std::ranges::equal(box.hi, rhs.box.hi);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, (uint8_t)relation);
    for (int d = 0; d < 2; d++) {
      value = mixHash(value, box.lo[d]);
      value = mixHash(value, box.hi[d]);
    }
    return value;
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendKind(kind);
    out.appendString(field);
    out.appendByte((uint8_t)relation);
    for (int d = 0; d < 2; d++) {
      out.appendInt64(box.lo[d]);
      out.appendInt64(box.hi[d]);
    }
    return filterKeyScope;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
