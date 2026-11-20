// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <vector>

#include "SegmentCore.h"
#include "OwnershipShape.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Binds an ancestor and descendant in one immutable bundle. The encoded
// shapes stay in the core; the binding stores only one pointer per level.
class OwnershipMapper {
  std::vector<const OwnershipShape*> shapes;

public:
  OwnershipMapper(SegmentCore& core, std::string_view ancestor,
                  std::string_view descendant) {
    while (descendant != ancestor) {
      if (descendant.empty()) throw std::invalid_argument("ownership paths must be ancestor and descendant");
      const auto* shape = core.ownership(descendant);
      if (!shape) throw std::invalid_argument("ownership path absent in bundle");
      shapes.push_back(shape);
      descendant = spaceParentPath(descendant);
    }
    std::reverse(shapes.begin(), shapes.end());
  }

  std::pair<int32_t, int32_t> range(int32_t first, int32_t end) const {
    for (const auto* shape : shapes) {
      first = first == 0 ? 0 : shape->range(first - 1).second;
      end = end == 0 ? 0 : shape->range(end - 1).second;
    }
    return {first, end};
  }

  std::pair<int32_t, int32_t> range(int32_t owner) const {
    return range(owner, owner + 1);
  }

  // Random lookups use each level's reverse map when present. Forward walks
  // use Cursor to retain family boundaries instead.
  int32_t owner(int32_t child) const {
    for (auto it = shapes.rbegin(); it != shapes.rend(); ++it) child = (*it)->owner(child);
    return child;
  }

  // Compose one boundary run into the descendant space without retaining
  // intermediate rows or decoding the gaps between sampled frames.
  void readBoundaries(int32_t first, int32_t end, std::span<int64_t> out) const {
    assert(first >= 0 && first <= end);
    assert(out.size() == (size_t)(end - first) + 1);
    if (shapes.empty()) {
      for (auto& boundary : out) boundary = first++;
      return;
    }
    shapes[0]->readBoundaries(first, end, out);
    for (size_t level = 1; level < shapes.size(); level++) shapes[level]->projectBoundaries(out);
  }

  class Cursor {
    std::span<OwnershipShape::Cursor> cursors;
    OwnershipShape::Family current{-1, 0, 0};

    [[gnu::always_inline]] OwnershipShape::Family descend(OwnershipShape::Family family) {
      current = family;
      // The top family's range is in its immediate child space. Descend its
      // boundaries through the same cursors to get the full descendant range.
      for (size_t level = cursors.size(); level > 1;) {
        auto& cursor = cursors[--level - 1];
        current.first = cursor.firstChild(current.first);
        current.end = cursor.firstChild(current.end);
      }
      return current;
    }

  public:
    Cursor(const OwnershipMapper& mapper, MemPool& pool)
        : cursors((OwnershipShape::Cursor*)pool.alloc(mapper.shapes.size() * sizeof(OwnershipShape::Cursor), alignof(OwnershipShape::Cursor)), mapper.shapes.size()) {
      size_t index = 0;
      for (auto it = mapper.shapes.rbegin(); it != mapper.shapes.rend(); ++it) {
        new (&cursors[index++]) OwnershipShape::Cursor(**it);
      }
    }
    [[gnu::always_inline]] OwnershipShape::Family seek(int32_t owner) {
      if (owner == current.owner) return current;
      return descend(cursors.empty() ? OwnershipShape::Family{owner, owner, owner + 1}
          : cursors.back().seek(owner));
    }

    int32_t firstChild(int32_t owner) {
      if (owner == current.owner + 1) return current.end;
      if (owner == current.owner) return current.first;
      for (auto it = cursors.rbegin(); it != cursors.rend(); ++it) owner = it->firstChild(owner);
      return owner;
    }

    OwnershipShape::Family owner(int32_t child) {
      if (child >= current.first && child < current.end) return current;
      OwnershipShape::Family family{child, child, child + 1};
      for (auto& cursor : cursors) family = cursor.owner(family.owner);
      return descend(family);
    }
  };
};

} // namespace luxir
