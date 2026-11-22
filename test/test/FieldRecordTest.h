// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "gtest/gtest.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/FieldRecordCodec.h"

namespace luxir::test {

inline std::vector<uint64_t> segmentFileSizes(Directory& dir, uint64_t id, uint64_t epoch) {
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  auto prefix = Postings::getIndexFileNamePrefix(id, epoch);
  std::vector<uint64_t> sizes;
  for (const auto& file : files) {
    if (!file.name.starts_with(prefix)) continue;
    auto suffix = std::string_view(file.name).substr(prefix.size());
    auto number = Postings::parseSortableString(suffix);
    if (number > seg_location::maxFilenum() || Postings::getSortableString(number) != suffix) continue;
    if (sizes.size() <= number) sizes.resize(number + 1);
    sizes[number] = file.size;
  }
  return sizes;
}

inline field_parts::Bounds recordBounds(const DecodedFieldRecord& record) {
  if (const auto* scalar = record.scalarBounds()) return *scalar;
  if (const auto* dictionary = record.dictionaryBounds()) return *dictionary;
  if (const auto* geo = record.geoBounds()) return *geo;
  if (const auto* interval = record.intervalBounds()) return *interval;
  return {};
}

inline FieldRecordHeader recordHeader(const DecodedFieldRecord& record) {
  return {record.name, record.type, record.flags, record.typeParameters, record.segFlags,
          record.partSet(), record.docsWithField, record.numValues, record.indexedTotals(), recordBounds(record)};
}

// Encodes a writer field or a copy of a decoded field for round-trip and corruption tests.
struct EncodedFieldRecord {
  IndexFieldInfo field;
  std::vector<field_parts::TermRun> runs;  // Mutable copy of the dictionary's run descriptors.
  std::vector<char> bytes;
  size_t headerBytes = 0;

  EncodedFieldRecord(const IndexFieldInfo& source, int32_t maxDoc) : field(source) {
    copyRuns();
    encode(maxDoc);
  }

  EncodedFieldRecord(const DecodedFieldRecord& source, int32_t maxDoc) {
    field.header = recordHeader(source);
    if (const auto* terms = source.terms()) field.terms = *terms;
    if (const auto* number = source.numberColumn()) std::construct_at(&field.number, *number);
    if (const auto* ord = source.ordColumn()) std::construct_at(&field.ord, *ord);
    if (const auto* bytes = source.bytesColumn()) std::construct_at(&field.bytes, *bytes);
    if (const auto* points = source.points()) field.points = *points;
    if (const auto* norms = source.norms()) field.norms = *norms;
    if (const auto* stored = source.stored()) field.stored = *stored;
    field.docs = source.docsBitmap();
    field.endValueRanks = source.endValueRanks();
    field.owners = source.owners();
    copyRuns();
    encode(maxDoc);
  }

private:
  void copyRuns() {
    if (!field.header.parts.has(FieldPart::TERMS)) return;
    runs.assign(field.terms.runs.begin(), field.terms.runs.end());
    field.terms.runs = runs;
  }

  void encode(int32_t maxDoc) {
    RAMFile encoded("");
    OutputStream output(&encoded);
    FieldRecordCodec::encode(output, field, maxDoc);
    output.close();
    bytes.resize(encoded.size());
    encoded.copyTo(bytes.data());
    FieldRecordCodec::fieldHeader(bytes, maxDoc, &headerBytes);
  }

public:
  void check(int32_t maxDoc, field_parts::FileSizes files, bool reportSize = false) const {
    const auto& header = field.header;
    SCOPED_TRACE(std::string_view(header.name));
    FieldRecordPtr decoded;
    ASSERT_NO_THROW(decoded = FieldRecordCodec::decode(bytes, maxDoc, files, 0));
    ASSERT_NE(nullptr, decoded);
    FieldRecordHeader readHeader;
    ASSERT_NO_THROW(readHeader = FieldRecordCodec::fieldHeader(std::span(bytes).first(headerBytes), maxDoc));
    EXPECT_EQ(readHeader, recordHeader(*decoded));
    EXPECT_EQ(header, readHeader);
    EXPECT_EQ(bytes.data() + 1, readHeader.name.data());
    InputStream wire(bytes.data(), bytes.data() + headerBytes);
    wire.checkedPackedTerm();
    wire.checkedVint();  // Type.
    auto flagsStart = wire.offset();
    EXPECT_EQ((uint32_t)header.flags, wire.checkedVint());
    EXPECT_EQ(header.flags >= 128 ? 2 : 1, wire.offset() - flagsStart);
    if (const auto* dictionary = std::get_if<field_parts::DictionaryBounds>(&readHeader.bounds)) {
      auto borrowed = [&](PackedTerm endpoint) {
        auto start = (uintptr_t)bytes.data(), at = (uintptr_t)(endpoint.data() - 1);
        EXPECT_GE(at, start);
        EXPECT_LE(at - start + endpoint.memorySize(), headerBytes);
      };
      borrowed(dictionary->lower);
      if (dictionary->upperMode != field_parts::DictionaryBounds::Upper::UNBOUNDED) borrowed(dictionary->upper);
    }
    auto inBlock = [&](const void* ptr, size_t size) {
      auto start = (uintptr_t)decoded.get(), at = (uintptr_t)ptr;
      EXPECT_GE(at, start);
      EXPECT_LE(at - start + size, decoded->allocatedBytes());
    };
    const auto& totals = decoded->indexedTotals();
    if (decoded->flags & FieldType::INDEX_DOCS) {
      inBlock(&totals, sizeof(totals));
    } else {
      EXPECT_EQ(field_parts::IndexedTotals{}, totals);
      EXPECT_FALSE(decoded->hasDisjointTermPostings());
    }
    auto part = [&](bool present, const auto& expected, const auto* actual) {
      ASSERT_EQ(present, actual != nullptr);
      if (actual) {
        EXPECT_EQ(expected, *actual);
        inBlock(actual, sizeof(*actual));
        EXPECT_EQ(0u, (uintptr_t)actual % alignof(decltype(expected)));
      }
    };
    part(header.parts.has(FieldPart::TERMS), field.terms, decoded->terms());
    if (const auto* actual = decoded->terms()) inBlock(actual->runs.data(), actual->runs.size_bytes());
    if (header.parts.has(FieldPart::COLUMN)) {
      switch (field_parts::columnKind(header.type, header.flags)) {
        case field_parts::ColumnKind::NUMBER: part(true, field.number, decoded->numberColumn()); break;
        case field_parts::ColumnKind::ORD: part(true, field.ord, decoded->ordColumn()); break;
        case field_parts::ColumnKind::BYTES: part(true, field.bytes, decoded->bytesColumn()); break;
        case field_parts::ColumnKind::NONE: FAIL() << "no column kind"; break;
      }
    }
    part(header.parts.has(FieldPart::POINTS), field.points, decoded->points());
    part(header.parts.has(FieldPart::NORMS), field.norms, decoded->norms());
    part(header.parts.has(FieldPart::STORED), field.stored, decoded->stored());
    EXPECT_EQ(field.docs, decoded->docsBitmap());
    EXPECT_EQ(field.endValueRanks, decoded->endValueRanks());
    EXPECT_EQ(field.owners, decoded->owners());
    if (reportSize) {
      ::testing::Test::RecordProperty("record_" + std::string(std::string_view(header.name)), std::format("{}/{}", bytes.size(), headerBytes));
    }
  }
};

} // namespace luxir::test
