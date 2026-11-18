// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>

#include "MonoReader.h"
#include "Space.h"
#include "SkipStats.h"
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

  int32_t reverseOwner(int32_t child) const {
    skipCount(SkipStats::ownershipReverseReads);
    int32_t parent = (int32_t)owners->valueAt(child);
    assert(parent >= 0 && parent < nParents);
    return parent;
  }

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
  uint64_t endsBytes() const { return ends.sizeInBytes(); }
  uint64_t ownersBytes() const { return owners ? owners->sizeInBytes() : 0; }
  std::pair<int32_t, int32_t> range(int32_t parent) const {
    assert(parent >= 0 && parent < nParents);
    auto result = ends.valuesAt(parent);
    assert(result.first >= 0 && result.first <= result.second && result.second <= nChildren);
    return {(int32_t)result.first, (int32_t)result.second};
  }
  int32_t owner(int32_t child) const;

  // hugin, 18.2M answer rows, 2026-11-16: a family point read costs
  // about 4 ns; decoding costs 0.7 ns/value plus 1.2 ns per gathered family.
  // Count complete frames, including partial edges, rather than charging
  // only the requested values. Single families always use point access.
  static bool preferPointRanges(int32_t first, int32_t end, size_t count) {
    if (count <= 1) return true;
    int64_t firstValue = first ? first - 1 : 0;
    int64_t lastValue = end - 1;
    int64_t decoded = (lastValue / MonoReader::BULK_SIZE - firstValue / MonoReader::BULK_SIZE + 1)
        * MonoReader::BULK_SIZE;
    constexpr double POINT_FAMILY_NS = 4.0;
    constexpr double DECODE_VALUE_NS = 0.7;
    constexpr double GATHER_FAMILY_NS = 1.2;
    return POINT_FAMILY_NS * count <= DECODE_VALUE_NS * decoded + GATHER_FAMILY_NS * count;
  }

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

  // Retains the parent position for increasing child arrivals. A row just
  // past the current family steps forward through ends; any other lookup
  // reads the persisted reverse map when available, and gallops otherwise.
  class Cursor {
    // hugin, 18.2M answer rows, 2026-11-17: for a join led by one author's
    // 33K scattered rows the point read costs 1.7 ms where the gallop costs
    // 4.0 ms, and no threshold from 1 to 4 rows differs; a dense walk loses
    // 7% if its next-row steps read the map.
    static constexpr int32_t NEAR_ROWS = 2;

    const OwnershipShape& shape;
    Family current{-1, 0, 0};

  public:
    explicit Cursor(const OwnershipShape& shape) : shape(shape) {}

    [[gnu::always_inline]] Family seek(int32_t parent) {
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
      if (shape.owners && (current.owner < 0 || child < current.first || child - current.end >= NEAR_ROWS)) {
        int32_t parent = shape.reverseOwner(child);
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
  assert(child >= 0 && child < nChildren);
  if (owners) return reverseOwner(child);
  // No position to gallop from: bisect for the first end past the child.
  int32_t low = 0, high = nParents - 1;
  while (low < high) {
    int32_t middle = low + (high - low) / 2;
    if (ends.valueAt(middle) > child) high = middle;
    else low = middle + 1;
  }
  return low;
}

} // namespace luxir
