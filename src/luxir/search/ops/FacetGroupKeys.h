// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/search/Collapse.h"

namespace luxir {

struct FacetDimension {
  bool dense = false;
  int64_t base = 0;
  uint64_t size = 0;
  uint64_t index(int64_t value) const { return (uint64_t)value - (uint64_t)base; }
  int64_t value(uint64_t index) const { return (int64_t)((uint64_t)base + index); }
};

// Request metadata only. Small tuple ranges have an exact mixed-radix
// identity; wide tuples use accumulator-local exact interning at reduction.
class FacetGroupPlan {
  struct Component { int64_t base; uint64_t radix; };
  std::vector<Component> components;
  static FacetDimension componentDimension(const CollapsePlan& key, const CollapsePlan::Field& field) {
    if (field.ordinal()) return {true, 0,
        (uint64_t)(field.ords ? field.ords->numOrds() : 0)};
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    for (size_t segnum = 0; segnum < key.reader.segments().size(); segnum++) {
      auto* info = field.segments.at(segnum);
      if (!info || info->numValues == 0) continue;
      assert(info->columnBounds);
      lo = std::min(lo, info->columnBounds->min);
      hi = std::max(hi, info->columnBounds->max);
    }
    if (lo > hi) return {true, 0, 0};
    // Unsigned subtraction also covers ranges crossing zero without signed
    // overflow. Share the tuple bound; the reducer's byte cap decides whether
    // a representable dense range is small enough for paged rows.
    uint64_t width = (uint64_t)hi - (uint64_t)lo;
    if (width >= (uint64_t)INT64_MAX) return {};
    return {true, lo, width + 1};
  }
public:
  const CollapsePlan& key;
  FacetDimension dimension;
  bool internTuples = false;

  explicit FacetGroupPlan(const CollapsePlan& key) : key(key) {
    if (key.fields.size() == 1) {
      dimension = componentDimension(key, key.fields[0]);
      return;
    }
    uint64_t product = 1;
    for (const auto& field : key.fields) {
      auto dimension = componentDimension(key, field);
      uint64_t cardinality = dimension.size;
      if (!dimension.dense || cardinality >= (uint64_t)INT64_MAX
          || product > (uint64_t)INT64_MAX / (cardinality + 1)) {
        internTuples = true;
        break;
      }
      components.push_back({dimension.base, cardinality + 1});
      product *= cardinality + 1;
    }
    if (!internTuples) dimension = {true, 0, product};
  }

  int64_t tupleId(const CollapseKey& tuple) const {
    assert(!internTuples);
    uint64_t id = 0;
    for (size_t i = 0; i < components.size(); i++) {
      auto part = tuple.components[i];
      uint64_t digit = part.present ? (uint64_t)part.value - (uint64_t)components[i].base + 1 : 0;
      assert(digit < components[i].radix);
      id = id * components[i].radix + digit;
    }
    return (int64_t)id;
  }
};

} // namespace luxir
