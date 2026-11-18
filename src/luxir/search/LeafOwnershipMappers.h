// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>
#include "IndexReader.h"
#include "luxir/reader/OwnershipMapper.h"
#include "luxir/util/SharedLazyCell.h"

namespace luxir {

// One OwnershipMapper per leaf of a descendant view, bound on first use so
// building a join opens only the segments a query actually reaches.
class LeafOwnershipMappers {
  IndexReader& descendant;
  std::string ancestor;
  std::unique_ptr<SharedLazyCell<OwnershipMapper>[]> cells;

public:
  LeafOwnershipMappers(IndexReader& descendant, std::string_view ancestor)
      : descendant(descendant), ancestor(ancestor),
        cells(std::make_unique<SharedLazyCell<OwnershipMapper>[]>(descendant.segments().size())) {}

  // Null when the leaf has no rows in the descendant space.
  const OwnershipMapper* get(size_t leaf) const {
    auto& segment = descendant.segments()[leaf];
    if (!segment.hasSpace()) return nullptr;
    return cells[leaf].getOrCreate([&] {
      return std::make_shared<OwnershipMapper>(segment.segmentCore(), ancestor, descendant.path());
    }).get();
  }
};

} // namespace luxir
