// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "MonoReader.h"
#include "SegmentCore.h"

namespace luxir {

MonoReader::MonoReader(SegmentCore& core, field_parts::Slice slice, int64_t numValues, PackedTerm field) {
  if (!slice) return;
  auto split = field_parts::monotonicBlocksBytes(slice, numValues);
  if (!split) SegmentReadContext{&core, field, slice.location.filenum()}.invalid("invalid monotonic index layout");
  auto bytes = core.slice(slice);
  values = MonoColumn(bytes.data(), bytes.data() + *split, numValues);
}

MonoReader MonoReader::openEndValueRanks(PostingsReader& reader, const DecodedFieldRecord& fieldInfo) {
  auto slice = fieldInfo.endValueRanks();
  if (!slice) return MonoReader();
  auto context = segmentReadContext(reader.core(), fieldInfo, slice.location.filenum());
  MonoReader ranks(reader.core(), slice, fieldInfo.docsWithField, context.field);
  ranks.requireLast(fieldInfo.numValues, context, "last end-value rank differs from the number of values");
  return ranks;
}

int64_t MonoReader::checkedValueAt(int64_t rank, SegmentReadContext context) const {
  auto block = rank / BLOCK_SIZE;
  const auto& info = values.blockInfo(block);
  auto bits = info.bits();
  if (info.gcd != 1 || (bits > NumBlockInfo::MAX_PACKED_BITS && bits != NumBlockInfo::RAW_BITS)) {
    context.invalid("invalid monotonic endpoint block encoding");
  }
  auto count = values.valuesInBlock(block);
  auto bytes = bits == NumBlockInfo::RAW_BITS ? count * sizeof(int64_t) : LinearPack::packedByteSize(count, bits);
  auto limit = values.payloadBytes();
  if (info.payloadOffset() > limit || bytes > limit - info.payloadOffset()) {
    context.invalid("monotonic endpoint block payload exceeds its index");
  }
  return valueAt(rank);
}

void MonoReader::requireLast(int64_t expected, SegmentReadContext context, const char* reason) const {
  if (checkedValueAt(numValues() - 1, context) != expected) context.invalid(reason);
}

} // namespace luxir
