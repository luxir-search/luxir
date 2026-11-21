// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include "IndexReader.h"
#include "Collector.h"
#include "LeafOwnershipMappers.h"

namespace luxir {

// A field in an ancestor view evaluated at descendant rows. The row list
// preserves input slots and segment numbers; repeated owners remain repeated.
class AncestorRows {
  LeafOwnershipMappers mappers;

public:
  IndexReader& reader;

  AncestorRows(IndexReader& source, IndexReader& target) : mappers(source, target.path()), reader(target) {}

  // Indices are one segment's non-decreasing source rows, as in a column
  // reader's bulk input. Output has the same length and order as input.
  void map(std::span<const segdoc> input, std::span<const uint32_t> indices,
           std::span<segdoc> output) const {
    if (indices.empty()) return;
    auto segment = input[indices.front()].segment();
    MemPool pool;
    OwnershipMapper::Cursor cursor(*mappers.get(segment), pool);
    for (auto i : indices) output[i] = segdoc(segment, cursor.owner(input[i].docId()).owner);
  }
};

} // namespace luxir
