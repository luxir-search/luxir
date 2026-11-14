// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>

#include "MonoReader.h"
#include "Space.h"
#include "luxir/util/screaming.h"

namespace luxir {
class SegmentCore;

// Immediate ownership in parent-major row order. Empty lists repeat an end;
// field presence and field-local value multiplicity are independent of this.
class OwnershipShape {
  int32_t nParents;
  int32_t nChildren;
  MonoReader ends;
  std::optional<MonoReader> owners;

public:
  OwnershipShape(SegmentCore& core, const OwnershipDescriptor& descriptor,
                 int32_t parentCount, int32_t childCount);

  int32_t parentCount() const noexcept { return nParents; }
  int32_t childCount() const noexcept { return nChildren; }
  bool hasReverseMap() const noexcept { return owners.has_value(); }
  std::pair<int32_t, int32_t> range(int32_t parent) const {
    assert(parent >= 0 && parent < nParents);
    auto result = ends.valuesAt(parent);
    assert(result.first >= 0 && result.first <= result.second && result.second <= nChildren);
    return {(int32_t)result.first, (int32_t)result.second};
  }
  int32_t owner(int32_t child) const;

  // Retains the parent position for increasing child arrivals. Backwards
  // seeks restart; no reverse map is materialized for either direction.
  class Cursor {
    const OwnershipShape& shape;
    int32_t parent = 0;
    int32_t first = 0;
    int32_t end = 0;

  public:
    explicit Cursor(const OwnershipShape& shape) : shape(shape) {}
    int32_t owner(int32_t child) {
      assert(child >= 0 && child < shape.nChildren);
      // Siblings stay in the current range without decoding either column again.
      if (child >= first && child < end) return parent;
      if (shape.owners) {
        int64_t value = shape.owners->valueAt(child);
        assert(value >= 0 && value < shape.nParents);
        parent = (int32_t)value;
      } else {
        if (child < first) parent = 0;
        parent = (int32_t)screaming::gallopLowerBound(
            parent, shape.nParents, (int64_t)child + 1,
            [&](int64_t index) { return shape.ends.valueAt(index); });
        assert(parent < shape.nParents);
      }
      auto range = shape.range(parent);
      first = range.first;
      end = range.second;
      assert(child >= first && child < end);
      return parent;
    }
  };
};

inline int32_t OwnershipShape::owner(int32_t child) const {
  return Cursor(*this).owner(child);
}

} // namespace luxir
