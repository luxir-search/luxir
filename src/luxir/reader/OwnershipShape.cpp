// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "OwnershipShape.h"
#include "SegmentCore.h"

#include <stdexcept>

namespace luxir {
namespace {

MonoReader readMono(SegmentCore& core, seg_location loc, int64_t metaOff, int32_t count) {
  if (loc.isNull() || metaOff < 0) throw std::runtime_error("invalid ownership column location");
  uint64_t blocks = ((uint64_t)count + MonoReader::BLOCK_SIZE - 1) / MonoReader::BLOCK_SIZE;
  core.checkLocation(loc, (uint64_t)metaOff + blocks * sizeof(NumBlockInfo));
  auto input = core.getInputStreamSeek(loc);
  if (((loc.offset() + (uint64_t)metaOff) & (alignof(NumBlockInfo) - 1)) != 0) {
    throw std::runtime_error("unaligned ownership column metadata");
  }
  // Check the descriptors touched by endpoint validation. Opening does not
  // walk the block directory or decode the ownership column.
  for (uint64_t block : {(uint64_t)0, blocks ? blocks - 1 : 0}) {
    if (blocks == 0) break;
    const auto& info = ((const NumBlockInfo*)input.ptr((int64_t)loc.offset() + metaOff))[block];
    uint64_t values = std::min<uint64_t>(MonoReader::BLOCK_SIZE, (uint64_t)count - block * MonoReader::BLOCK_SIZE);
    uint8_t bits = info.bits();
    if (info.gcd != 1 || (bits > NumBlockInfo::MAX_PACKED_BITS && bits != NumBlockInfo::RAW_BITS)) {
      throw std::runtime_error("invalid ownership column encoding");
    }
    uint64_t bytes = bits == NumBlockInfo::RAW_BITS ? values * sizeof(int64_t)
        : LinearPack::packedByteSize(values, bits);
    if (info.payloadOffset() > (uint64_t)metaOff || bytes > (uint64_t)metaOff - info.payloadOffset()) {
      throw std::runtime_error("ownership column payload exceeds its metadata");
    }
  }
  return MonoReader(input, (int64_t)loc.offset(), metaOff, count);
}

} // namespace

OwnershipShape::OwnershipShape(SegmentCore& core, const OwnershipDescriptor& descriptor,
                               int32_t parentCount, int32_t childCount)
    : nParents(parentCount), nChildren(childCount),
      ends(readMono(core, descriptor.ends, descriptor.endsMetaOff, parentCount)) {
  if ((nParents == 0 && nChildren != 0)
      || (nParents > 0 && (ends.valueAt(0) < 0 || ends.valueAt(0) > nChildren || ends.valueAt(nParents - 1) != nChildren))) {
    throw std::runtime_error("ownership ends do not cover the child space");
  }
  if (!descriptor.owners.isNull()) {
    owners.emplace(readMono(core, descriptor.owners, descriptor.ownersMetaOff, childCount));
    if (nChildren > 0 && (owners->valueAt(0) < 0 || owners->valueAt(0) >= nParents
        || owners->valueAt(nChildren - 1) < 0 || owners->valueAt(nChildren - 1) >= nParents)) {
      throw std::runtime_error("ownership reverse map exceeds the parent space");
    }
  } else if (descriptor.ownersMetaOff != 0) {
    throw std::runtime_error("ownership reverse metadata without a column");
  }
}

} // namespace luxir
