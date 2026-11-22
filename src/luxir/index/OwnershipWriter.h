// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "IntColWriter.h"
#include "luxir/reader/Space.h"

namespace luxir {

// Writes both directions from the same parent-major family stream.
class OwnershipWriter {
  PostingsWriter& writer;
  PostingsWriter::OutputStreamPtr endsOutput;
  PostingsWriter::OutputStreamPtr ownersOutput;
  MonoWriter ends;
  std::optional<MonoWriter> owners;
  int32_t parentCount = 0;
  int32_t childCount = 0;

public:
  OwnershipWriter(PostingsWriter& writer, bool reverseMap)
      : writer(writer), endsOutput(writer.getOutputStream()),
        ownersOutput(reverseMap ? writer.getOutputStream() : PostingsWriter::OutputStreamPtr{}),
        ends(writer.pool, *endsOutput) {
    if (ownersOutput) owners.emplace(writer.pool, *ownersOutput);
  }

  static size_t workspaceBytes(int32_t parents, int32_t children, bool reverseMap) {
    auto column = [](int32_t count) {
      size_t blocks = ((size_t)count + NumColumnFormat::BLOCK_SIZE - 1) / NumColumnFormat::BLOCK_SIZE;
      return 2 * NumColumnFormat::BLOCK_SIZE * sizeof(int64_t)
          + 2 * blocks * (sizeof(NumBlockInfo) + sizeof(NumBlockZone));
    };
    return sizeof(OwnershipWriter) + column(parents) + (reverseMap ? column(children) : 0);
  }

  void addEnd(int32_t end) {
    assert(end >= childCount);
    ends.addInt64(end);
    if (owners) {
      for (int32_t row = childCount; row < end; row++) owners->addInt64(parentCount);
    }
    childCount = end;
    parentCount++;
  }

  OwnershipDescriptor finish() {
    OwnershipDescriptor result{};
    auto endsData = ends.finish();
    assert(endsData.numValues == parentCount);
    if (owners) {
      auto ownersData = owners->finish();
      assert(ownersData.numValues == childCount);
      result.owners = MonoWriter::placeIndex(writer, std::move(ownersOutput), ownersData);
    }
    result.ends = MonoWriter::placeIndex(writer, std::move(endsOutput), endsData);
    return result;
  }
};

} // namespace luxir
