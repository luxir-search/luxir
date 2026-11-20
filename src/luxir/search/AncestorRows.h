// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include "IndexReader.h"
#include "Collector.h"
#include "luxir/reader/OwnershipMapper.h"

namespace luxir {

// A field in an ancestor view evaluated at descendant rows. The row list
// preserves input slots and segment numbers; repeated owners remain repeated.
class AncestorRows {
  std::vector<std::optional<OwnershipMapper>> mappers;

public:
  IndexReader& reader;

  AncestorRows(IndexReader& source, IndexReader& target) : reader(target) {
    mappers.reserve(source.segments().size());
    for (const auto& leaf : source.segments()) {
      if (leaf.hasSpace()) mappers.emplace_back(std::in_place, leaf.segmentCore(), target.path(), source.path());
      else mappers.emplace_back(std::nullopt);
    }
  }

  // Indices are one segment's non-decreasing source rows, as in a column
  // reader's bulk input. Output has the same length and order as input.
  void map(std::span<const segdoc> input, std::span<const uint32_t> indices,
           std::span<segdoc> output) const {
    if (indices.empty()) return;
    auto segment = input[indices.front()].segment();
    MemPool pool;
    OwnershipMapper::Cursor cursor(*mappers[segment], pool);
    for (auto i : indices) output[i] = segdoc(segment, cursor.owner(input[i].docId()).owner);
  }
};

} // namespace luxir
