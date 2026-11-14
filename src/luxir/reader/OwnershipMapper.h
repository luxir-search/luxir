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

  class Cursor {
    std::span<OwnershipShape::Cursor> cursors;

  public:
    Cursor(const OwnershipMapper& mapper, MemPool& pool)
        : cursors((OwnershipShape::Cursor*)pool.alloc(mapper.shapes.size() * sizeof(OwnershipShape::Cursor), alignof(OwnershipShape::Cursor)), mapper.shapes.size()) {
      size_t index = 0;
      for (auto it = mapper.shapes.rbegin(); it != mapper.shapes.rend(); ++it) {
        new (&cursors[index++]) OwnershipShape::Cursor(**it);
      }
    }
    int32_t owner(int32_t child) {
      for (auto& cursor : cursors) child = cursor.owner(child);
      return child;
    }
  };
};

} // namespace luxir
