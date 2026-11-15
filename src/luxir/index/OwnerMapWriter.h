// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include "IntColWriter.h"
#include "PostingsWriter.h"

namespace luxir {

// Writes the owner map of a nested row space into its scope segment: one
// cumulative element end per owner row (the endValueRank shape), and one owner
// row per element row (the valDoc shape, for candidates in random order).
// `ends` must be non-decreasing and end at the scope segment's maxDoc.
inline void writeOwnerMap(PostingsWriter& writer, MemPool& pool, std::span<const int64_t> ends) {
  assert(ends.empty() || ends.back() == writer.getMaxDoc());
  writer.owner.ownerMaxDoc = (int32_t)ends.size();
  {
    auto out = writer.getOutputStream();
    MonoWriter endsWriter(pool, *out);
    for (int64_t end : ends) endsWriter.addInt64(end);
    endsWriter.finish();
    writer.owner.endsLoc = endsWriter.blockLoc;
    writer.owner.endsMetaOff = endsWriter.metaOff;
  }
  {
    auto out = writer.getOutputStream();
    MonoWriter ownersWriter(pool, *out);
    int64_t start = 0;
    for (size_t owner = 0; owner < ends.size(); owner++) {
      for (int64_t row = start; row < ends[owner]; row++) ownersWriter.addInt64((int64_t)owner);
      start = ends[owner];
    }
    ownersWriter.finish();
    writer.owner.ownersLoc = ownersWriter.blockLoc;
    writer.owner.ownersMetaOff = ownersWriter.metaOff;
  }
}

} // namespace luxir
