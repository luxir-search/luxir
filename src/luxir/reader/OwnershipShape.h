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
  struct Family {
    int32_t owner;
    int32_t first;
    int32_t end;
  };

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

  // Boundary positions first..end enclose owners [first, end). Position 0
  // precedes the first family and has the synthetic child offset zero.
  void readBoundaries(int32_t first, int32_t end, std::span<int64_t> out) const {
    assert(first >= 0 && first <= end && end <= nParents);
    assert(out.size() == (size_t)(end - first) + 1);
    if (first == 0) {
      out[0] = 0;
      ends.readValues(0, out.subspan(1));
    } else {
      ends.readValues(first - 1, out);
    }
  }

  // Translate increasing parent boundary positions to child positions in
  // place. Repeated positions share a value; each visited frame decodes once.
  void projectBoundaries(std::span<int64_t> boundaries) const {
    MonoReader::BulkValues decoded(ends);
    for (auto& position : boundaries) {
      assert(position >= 0 && position <= nParents);
      if (position == 0) continue;
      int64_t index = position - 1;
      if (index != decoded.index()) decoded.advance(index);
      position = decoded.value();
    }
  }

  // Retains the parent position for increasing child arrivals. Backwards
  // seeks restart; no reverse map is materialized for either direction.
  class Cursor {
    const OwnershipShape& shape;
    Family current{-1, 0, 0};

  public:
    explicit Cursor(const OwnershipShape& shape) : shape(shape) {}

    Family seek(int32_t parent) {
      assert(parent >= 0 && parent < shape.nParents);
      if (parent == current.owner) return current;
      if (parent == current.owner + 1) {
        current = {parent, current.end, (int32_t)shape.ends.valueAt(parent)};
      } else {
        auto [first, end] = shape.range(parent);
        current = {parent, first, end};
      }
      return current;
    }

    // Parent positions may include the boundary just past the last parent.
    int32_t firstChild(int32_t parent) const {
      assert(parent >= 0 && parent <= shape.nParents);
      if (parent == current.owner + 1) return current.end;
      if (parent == current.owner) return current.first;
      if (parent == 0) return 0;
      if (parent == shape.nParents) return shape.nChildren;
      return (int32_t)shape.ends.valueAt(parent - 1);
    }

    Family owner(int32_t child) {
      assert(child >= 0 && child < shape.nChildren);
      // Siblings stay in the current range without decoding either column again.
      if (child >= current.first && child < current.end) return current;
      if (shape.owners) {
        int32_t parent = (int32_t)shape.owners->valueAt(child);
        assert(parent >= 0 && parent < shape.nParents);
        auto [first, end] = shape.range(parent);
        current = {parent, first, end};
      } else {
        int32_t parent = child < current.first ? 0 : current.owner + 1;
        int32_t first = parent ? current.end : 0;
        int32_t end = 0;
        // Keep the gallop's bracket: its last lower and upper values are the
        // adjacent ends of the resulting family, so neither needs rereading.
        parent = (int32_t)screaming::gallopLowerBound(parent, shape.nParents, (int64_t)child + 1,
            [&](int64_t index) {
              int64_t value = shape.ends.valueAt(index);
              if (value <= child) first = (int32_t)value;
              else end = (int32_t)value;
              return value;
            });
        assert(parent < shape.nParents);
        current = {parent, first, end};
      }
      assert(child >= current.first && child < current.end);
      return current;
    }
  };
};

inline int32_t OwnershipShape::owner(int32_t child) const {
  return Cursor(*this).owner(child).owner;
}

} // namespace luxir
