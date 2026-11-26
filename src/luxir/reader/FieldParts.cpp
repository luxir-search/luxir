// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldRecordCodec.h"

#include "luxir/codec/NumColumnFormat.h"
#include "luxir/codec/OrdColumnFormat.h"
#include "luxir/index/BKDWriter.h"

namespace luxir::field_parts {

std::optional<uint64_t> monotonicBlocksBytes(Slice slice, int64_t numValues) noexcept {
  return NumColumnFormat::monotonicSplit(slice.location.offset(), slice.bytes, numValues);
}

int32_t termBlockCount(const Terms& terms, int64_t numTerms) noexcept {
  const auto& last = terms.runs.back();
  return last.firstBlockOrd + (int32_t)Postings::termBlockCount(numTerms - last.firstTermOrd);
}

uint64_t termBlockOffsetsStart(uint64_t dictionaryEnd) noexcept {
  return alignUp(dictionaryEnd, Terms::OFFSETS_ALIGN);
}

Slice termBlockOffsets(const Terms& terms, int64_t numTerms) {
  auto end = terms.dictionaryBlocks().end();
  auto padding = termBlockOffsetsStart(end.offset()) - end.offset();
  return {end.addOffset(padding), (uint64_t)termBlockCount(terms, numTerms) * sizeof(Terms::BlockOffset)};
}

Slice termTrie(Slice offsets, uint64_t trieBytes) {
  return {offsets.end(), trieBytes};
}

Slice NumberColumn::index() const {
  auto end = location.addOffset(blocksBytes);
  auto start = NumColumnFormat::indexOffset(end.offset());
  return {end.addOffset(start - end.offset()), indexBytes};
}

Slice ordBlockMetadata(const OrdColumn& column, uint64_t count) {
  if (column.mode == OrdMode::DIRECT) return {};
  return {column.location.addOffset(column.valuesBytes), OrdColumnFormat::metadataBytes(count)};
}

int32_t bytesStride(const BytesColumn& column, int64_t numValues) noexcept {
  if (column.endOffsets) return -1;
  assert(numValues > 0);
  auto bytes = column.values.bytes;
  auto stride = bytes / (uint64_t)numValues;
  if (stride > INT32_MAX || bytes % (uint64_t)numValues) return -1;
  return (int32_t)stride;
}

uint64_t pointCount(FieldType::Type type, int64_t numValues) noexcept {
  return type == FieldType::INTERVAL ? (uint64_t)numValues / 2 : (uint64_t)numValues;
}

uint64_t pointLeafCount(const Points& points, FieldType::Type type, int64_t numValues) noexcept {
  return PointsWriter::leafCount(pointCount(type, numValues), points.maxPointsPerLeaf);
}

Slice pointsDirectory(const Points& points, FieldType::Type type, int64_t numValues) {
  auto start = points.leaves().end();
  auto leaves = pointLeafCount(points, type, numValues);
  assert(leaves != 0 && leaves <= UINT32_MAX);
  auto bytes = type == FieldType::GEO_POINT ? BKDWriter::directoryBytes(start.offset(), (uint32_t)leaves)
      : type == FieldType::INTERVAL ? BasicBKDWriter<BKDInt64Traits>::directoryBytes(start.offset(), (uint32_t)leaves)
      : PointsWriter::directoryBytes(start.offset(), (uint32_t)leaves);
  return {start, bytes};
}

} // namespace luxir::field_parts

namespace luxir {

using namespace field_parts;

int32_t DecodedFieldRecord::termBlockCount() const noexcept {
  const auto* dictionary = terms();
  return dictionary ? field_parts::termBlockCount(*dictionary, indexedTotals().numTerms) : 0;
}

Slice DecodedFieldRecord::termBlockOffsets() const {
  const auto* dictionary = terms();
  return dictionary ? field_parts::termBlockOffsets(*dictionary, indexedTotals().numTerms) : Slice();
}

Slice DecodedFieldRecord::termTrie() const {
  const auto* dictionary = terms();
  return dictionary ? field_parts::termTrie(termBlockOffsets(), dictionary->trieBytes) : Slice();
}

Slice DecodedFieldRecord::termNavigation() const {
  auto* dictionary = terms();
  if (!dictionary) return {};
  auto offsets = termBlockOffsets();
  auto trie = field_parts::termTrie(offsets, dictionary->trieBytes);
  return {offsets.location, trie.end().offset() - offsets.location.offset()};
}

uint64_t DecodedFieldRecord::pointCount() const noexcept {
  return points() ? field_parts::pointCount(type, numValues) : 0;
}

uint64_t DecodedFieldRecord::pointLeafCount() const noexcept {
  const auto* index = points();
  return index ? field_parts::pointLeafCount(*index, type, numValues) : 0;
}

Slice DecodedFieldRecord::pointsDirectory() const {
  const auto* index = points();
  return index ? field_parts::pointsDirectory(*index, type, numValues) : Slice();
}

Slice DecodedFieldRecord::normsValues(int32_t maxDoc) const {
  const auto* part = norms();
  return part ? field_parts::normsValues(*part, maxDoc, docsWithField) : Slice();
}

Slice DecodedFieldRecord::ordBlockMetadata(int32_t maxDoc) const {
  const auto* column = ordColumn();
  return column ? field_parts::ordBlockMetadata(*column, OrdColumn::encodedCount(column->indexing, maxDoc, numValues))
                : Slice();
}

int32_t DecodedFieldRecord::bytesStride() const noexcept {
  const auto* column = bytesColumn();
  return column ? field_parts::bytesStride(*column, numValues) : -1;
}

} // namespace luxir
