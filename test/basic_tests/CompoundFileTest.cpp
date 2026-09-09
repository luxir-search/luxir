// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/codec/StreamVByte.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/NormsReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/reader/PosEnum.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/schema/Schema.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"

using namespace luxir;

namespace {

class TempDirectory {
  std::filesystem::path path_;

public:
  TempDirectory() {
    std::string pattern = "/tmp/luxir-compound-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) throw std::runtime_error("mkdtemp failed");
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }
};

std::shared_ptr<Schema> compoundSchema(bool multiTag = false) {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::STORED,
      "whitespace");
  schema->fieldTypeMap["tag"] = std::make_shared<StrFieldType>(
      "tag", FieldType::COLUMN_STORED | (multiTag ? FieldType::MULTI_VALUED : 0));
  schema->fieldTypeMap["score"] = std::make_shared<IntFieldType>(
      "score", FieldType::COLUMN_STORED | FieldType::INDEX_RANGE);
  return schema;
}

SegFieldInfo fieldInfo(PostingsReader& reader, std::string_view field) {
  FieldReader fields(reader);
  EXPECT_TRUE(fields.seek(field));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
}

std::vector<std::string> baseSegmentFiles(Directory& directory, uint64_t segId) {
  std::vector<Directory::FileInfo> files;
  directory.listFiles(files);
  std::string prefix = Postings::getIndexFileNamePrefix(segId);
  std::vector<std::string> matches;
  for (const auto& file : files) {
    if (file.name.starts_with(prefix + "_")
        && !file.name.starts_with(prefix + "__")) {
      matches.push_back(file.name);
    }
  }
  return matches;
}

std::vector<std::pair<std::string, std::string>> storedValues(
    StoredFieldsReader& reader, int32_t doc) {
  std::vector<std::pair<std::string, std::string>> values;
  reader.readDoc(doc, [&](std::string_view field,
                          std::span<const std::string_view> fieldValues) {
    for (std::string_view value : fieldValues) {
      values.emplace_back(field, value);
    }
  });
  return values;
}

std::string patternedBytes(size_t size, char base = 'a') {
  std::string value(size, '\0');
  for (size_t i = 0; i < size; i++) {
    value[i] = (char) (base + i % 23);
  }
  return value;
}

std::string wideTerm(int32_t ord) {
  std::string term(PackedTerm::MAX_LEN, 'x');
  std::string prefix = std::format("t{:08}_", ord);
  std::copy(prefix.begin(), prefix.end(), term.begin());
  return term;
}

// Large column fixtures use many legal STRING values to retain their original
// byte volume and exercise the same buffering/collapse paths.
void indexTag(Inverter& inverter, std::string_view value) {
  auto& input = inverter.getIndexHandler("tag");
  if (value.size() <= PackedTerm::MAX_LEN) {
    input.index(inverter, value);
    return;
  }
  std::vector<std::string_view> chunks;
  for (size_t offset = 0; offset < value.size(); offset += PackedTerm::MAX_LEN) {
    chunks.push_back(value.substr(offset, PackedTerm::MAX_LEN));
  }
  input.index(inverter, std::span<const std::string_view>(chunks));
}

std::string tagValue(StrColReader& tags, StrColReader::Iterator& iter) {
  if (!tags.isMultiValued()) return std::string(iter.value());
  StrColReader::DocValues values(tags);
  auto [start, end] = iter.valueRange();
  std::string result;
  for (int64_t rank = start; rank < end; rank++) result += values.valueAt(rank);
  return result;
}

void verifyTinySegment(Directory& directory, size_t largeTagBytes = 0) {
  auto schema = compoundSchema(largeTagBytes > PackedTerm::MAX_LEN);
  IndexWriter writer(directory, [schema] { return schema; });
  Inverter& inverter = writer.obtainInverter();
  std::string firstTag = largeTagBytes == 0
      ? std::string("x") : patternedBytes(largeTagBytes);

  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter,
                                           std::string_view("alpha beta"));
  indexTag(inverter, firstTag);
  inverter.getIndexHandler("score").index(inverter, (int64_t) 7);
  inverter.finishDoc();

  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter, std::string_view("beta"));
  inverter.getIndexHandler("score").index(inverter, (int64_t) -2);
  inverter.finishDoc();

  inverter.startDoc();
  inverter.getIndexHandler("tag").index(inverter, std::string_view("y"));
  inverter.finishDoc();

  writer.releaseInverter(inverter, true);
  writer.commit();
  auto reader = writer.getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  Segment& segment = reader->segments()[0];
  auto baseFiles = baseSegmentFiles(directory, segment.segInfo.seg_id);
  ASSERT_EQ(1u, baseFiles.size());

  PostingsReader& postings = segment.postingsReader();
  auto physicalFile = directory.openFile(baseFiles[0], true);
  ASSERT_NE(nullptr, physicalFile);
  EXPECT_EQ((uint64_t) physicalFile->size(), postings.sizeInBytes());
  SegFieldInfo bodyInfo = fieldInfo(postings, "body");
  MemPool pool;
  TermsEnum terms(pool, postings, bodyInfo);
  ASSERT_TRUE(terms.seek("beta"));
  DocsPosEnum docs(terms);
  PosEnum positions(docs);
  ASSERT_EQ(0, docs.nextDoc());
  EXPECT_EQ(1, docs.termFreq());
  positions.startPositions();
  EXPECT_EQ(1, positions.nextPosition());
  ASSERT_EQ(1, docs.nextDoc());
  EXPECT_EQ(1, docs.termFreq());
  positions.startPositions();
  EXPECT_EQ(0, positions.nextPosition());
  EXPECT_EQ(DocsEnumMeta::END, docs.nextDoc());

  NormsReader norms(postings, bodyInfo);
  EXPECT_EQ(2, (int32_t) norms.value(0));
  EXPECT_EQ(1, (int32_t) norms.value(1));

  StrColReader tags(postings, fieldInfo(postings, "tag"));
  StrColReader::Iterator tagIter(tags);
  ASSERT_EQ(0, tagIter.next());
  if (largeTagBytes == 0) {
    EXPECT_EQ("x", tagValue(tags, tagIter));
  } else {
    EXPECT_EQ(largeTagBytes, tagValue(tags, tagIter).size());
    EXPECT_TRUE(tagValue(tags, tagIter) == firstTag);
  }
  ASSERT_EQ(2, tagIter.next());
  EXPECT_EQ("y", tagValue(tags, tagIter));
  EXPECT_EQ(StrColReader::Iterator::ENDDOC, tagIter.next());

  SegFieldInfo scoreInfo = fieldInfo(postings, "score");
  IntColReader scores(postings, scoreInfo);
  IntColReader::Iterator scoreIter(scores);
  ASSERT_EQ(0, scoreIter.next());
  EXPECT_EQ(7, scoreIter.value());
  ASSERT_EQ(1, scoreIter.next());
  EXPECT_EQ(-2, scoreIter.value());
  EXPECT_EQ(IntColReader::ENDDOC, scoreIter.next());
  PointsReader points(postings, scoreInfo);
  std::vector<PointsReader::Point> expectedPoints = {{-2, 1}, {7, 0}};
  EXPECT_EQ(expectedPoints, points.readAll());

  auto stored = StoredFieldsReader::open(postings);
  ASSERT_NE(nullptr, stored);
  EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
                {"body", "alpha beta"}}),
            storedValues(*stored, 0));
  EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
                {"body", "beta"}}),
            storedValues(*stored, 1));
  EXPECT_TRUE(storedValues(*stored, 2).empty());
}

void addMergeSource(IndexWriter& writer, int32_t source,
                    size_t tagBytes = 0) {
  Inverter& inverter = writer.obtainInverter();
  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter,
                                           std::string_view("keep common"));
  inverter.getIndexHandler("score").index(inverter, (int64_t) source);
  std::string tag;
  if (tagBytes != 0) {
    tag = patternedBytes(tagBytes, (char) ('a' + source));
    indexTag(inverter, tag);
  }
  inverter.finishDoc();
  inverter.startDoc();
  inverter.getIndexHandler("body").index(inverter,
                                           std::string_view("gone common"));
  inverter.getIndexHandler("score").index(inverter,
                                            (int64_t) source + 100);
  inverter.finishDoc();
  inverter.deleteDoc(1);
  writer.releaseInverter(inverter, true);
  writer.commit();
}

void verifyCollapsedMerge(Directory& directory, size_t tagBytes = 0) {
  auto schema = compoundSchema(tagBytes > PackedTerm::MAX_LEN);
  IndexWriter writer(directory, [schema] { return schema; });
  writer.mergePolicy->setMergeFactor(1000);
  for (int32_t source = 0; source < 3; source++) {
    addMergeSource(writer, source, tagBytes);
  }

  auto sources = writer.getIndexReader();
  ASSERT_EQ(3u, sources->segments().size());
  for (Segment& segment : sources->segments()) {
    EXPECT_EQ(1u, baseSegmentFiles(directory, segment.segInfo.seg_id).size());
    EXPECT_EQ(1, segment.numDeletes());
  }

  writer.mergeSegments();
  writer.commit();
  auto merged = writer.getIndexReader();
  ASSERT_EQ(1u, merged->segments().size());
  Segment& segment = merged->segments()[0];
  EXPECT_EQ(3, segment.maxDoc());
  EXPECT_EQ(1u, baseSegmentFiles(directory, segment.segInfo.seg_id).size());

  PostingsReader& postings = segment.postingsReader();
  SegFieldInfo bodyInfo = fieldInfo(postings, "body");
  MemPool pool;
  TermsEnum terms(pool, postings, bodyInfo);
  ASSERT_TRUE(terms.seek("keep"));
  DocsPosEnum docs(terms);
  EXPECT_EQ(0, docs.nextDoc());
  EXPECT_EQ(1, docs.nextDoc());
  EXPECT_EQ(2, docs.nextDoc());
  EXPECT_EQ(DocsEnumMeta::END, docs.nextDoc());
  EXPECT_FALSE(terms.seek("gone"));

  IntColReader scores(postings, fieldInfo(postings, "score"));
  IntColReader::Iterator scoreIter(scores);
  for (int32_t doc = 0; doc < 3; doc++) {
    ASSERT_EQ(doc, scoreIter.next());
    EXPECT_EQ(doc, scoreIter.value());
  }

  if (tagBytes != 0) {
    StrColReader tags(postings, fieldInfo(postings, "tag"));
    StrColReader::Iterator tagIter(tags);
    for (int32_t doc = 0; doc < 3; doc++) {
      ASSERT_EQ(doc, tagIter.next());
      std::string value = tagValue(tags, tagIter);
      ASSERT_EQ(tagBytes, value.size());
      EXPECT_EQ((char) ('a' + doc), value.front());
    }
  }

  auto stored = StoredFieldsReader::open(postings);
  ASSERT_NE(nullptr, stored);
  for (int32_t doc = 0; doc < 3; doc++) {
    EXPECT_EQ((std::vector<std::pair<std::string, std::string>>{
                  {"body", "keep common"}}),
              storedValues(*stored, doc));
  }
}

void initializeRawField(PostingsWriter::IndexFieldInfo& info,
                        seg_location location, int64_t bytes) {
  info.type = FieldType::BIN;
  info.flags = FieldType::COLUMN_STORED;
  info.docsWithField = 1;
  info.numValues = 1;
  info.columnLoc = location;
  info.columnMetaOff = bytes;
}

} // namespace

TEST(CompoundFileTest, TinyFlushCollapsesOnRAMDirectory) {
  RAMDir directory;
  verifyTinySegment(directory);
}

TEST(CompoundFileTest, TinyFlushCollapsesOnFSDirectory) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  verifyTinySegment(directory);
}

TEST(CompoundFileTest, MultiMiBFlushCollapsesOnFSDirectory) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  verifyTinySegment(directory, 2ULL << 20);
}

TEST(CompoundFileTest, RelocationPreservesSparseFilenumAndTailSlack) {
  constexpr uint32_t VALUE_COUNT = 33;
  RAMDir directory;
  PostingsWriter writer(directory, 17, 1);
  auto streams = writer.getOutputStreams<3>();
  ASSERT_EQ(0, streams[0]->streamNumber);
  ASSERT_EQ(1, streams[1]->streamNumber);
  ASSERT_EQ(2, streams[2]->streamNumber);

  std::array<uint32_t, VALUE_COUNT> values;
  for (uint32_t i = 0; i < VALUE_COUNT; i++) values[i] = i * i + 3;
  uint32_t keyBytes = svbKeyBytes(VALUE_COUNT);
  std::vector<uint8_t> encoded(keyBytes + VALUE_COUNT * sizeof(uint32_t));
  uint8_t* encodedEnd = svb_encode_scalar_d1_init(
      values.data(), encoded.data(), encoded.data() + keyBytes, VALUE_COUNT, 0);
  encoded.resize((size_t) (encodedEnd - encoded.data()));

  auto& moved = writer.addField("moved");
  moved.type = FieldType::BIN;
  moved.flags = FieldType::COLUMN_STORED;
  moved.docsWithField = 1;
  moved.numValues = 1;
  moved.columnLoc = streams[1]->slocation();
  moved.columnMetaOff = (int64_t) encoded.size();
  streams[1]->write(encoded.data(), encoded.size());

  auto& survivor = writer.addField("survivor");
  survivor.type = FieldType::BIN;
  survivor.flags = FieldType::COLUMN_STORED;
  survivor.docsWithField = 1;
  survivor.numValues = 1;
  survivor.columnLoc = streams[2]->slocation();
  survivor.columnMetaOff = 1;
  streams[2]->write((char) 0x5a);
  streams[2]->flush();

  for (auto& stream : streams) stream.reset();
  writer.finish();

  std::string seg = Postings::getSortableString(17);
  std::vector<Directory::FileInfo> files;
  directory.listFiles(files);
  ASSERT_EQ(2u, files.size());
  EXPECT_EQ(nullptr, directory.openFile(Postings::getIndexFileName(seg, 1)).get());
  EXPECT_NE(nullptr, directory.openFile(Postings::getIndexFileName(seg, 2)).get());

  PostingsReader reader(directory, 17);
  SegFieldInfo movedInfo = fieldInfo(reader, "moved");
  SegFieldInfo survivorInfo = fieldInfo(reader, "survivor");
  EXPECT_EQ(0u, movedInfo.columnLoc.filenum());
  EXPECT_EQ(2u, survivorInfo.columnLoc.filenum());
  EXPECT_EQ((uint64_t) 0, movedInfo.columnLoc.offset() % 8);

  InputStream movedInput = reader.getInputStreamSeek(movedInfo.columnLoc);
  ASSERT_GE(movedInput.left(),
            (int64_t) encoded.size() + (int64_t) SVB_OVERREAD_PAD);
  std::array<uint32_t, VALUE_COUNT> decoded;
  uint8_t* keys = (uint8_t*) movedInput.ptr();
  svb_decode_avx_d1_init(decoded.data(), keys, keys + keyBytes,
                         VALUE_COUNT, 0);
  EXPECT_EQ(values, decoded);

  InputStream survivorInput = reader.getInputStreamSeek(survivorInfo.columnLoc);
  EXPECT_EQ((char) 0x5a, survivorInput.readByte());
  EXPECT_EQ((uint64_t) files[0].size + files[1].size, reader.sizeInBytes());
}

TEST(CompoundFileTest, HeaderOnlyUnreferencedStreamIsDropped) {
  // Two writers producing the same one-field segment, except the second also
  // reserves streams it never writes past their headers.  Dropped header-only
  // files must leave file 0 byte-identical, not appended as 8-byte stubs.
  auto build = [](Directory& directory, uint64_t segId, int32_t extraStreams) {
    PostingsWriter writer(directory, segId, 1);
    auto used = writer.getOutputStream();
    if (extraStreams != 0) {
      auto unused = writer.getOutputStreams<2>();
      for (auto& stream : unused) stream.reset();
    }
    auto& field = writer.addField("f");
    field.type = FieldType::BIN;
    field.flags = FieldType::COLUMN_STORED;
    field.docsWithField = 1;
    field.numValues = 1;
    field.columnLoc = used->slocation();
    field.columnMetaOff = 1;
    used->write((char) 0x7f);
    used.reset();
    writer.finish();
  };

  RAMDir plain;
  RAMDir withExtras;
  build(plain, 21, 0);
  build(withExtras, 22, 2);

  std::vector<Directory::FileInfo> plainFiles;
  std::vector<Directory::FileInfo> extraFiles;
  plain.listFiles(plainFiles);
  withExtras.listFiles(extraFiles);
  ASSERT_EQ(1u, plainFiles.size());
  ASSERT_EQ(1u, extraFiles.size());
  EXPECT_EQ(plainFiles[0].size, extraFiles[0].size)
      << "dropped header-only streams must not grow file 0";

  PostingsReader reader(withExtras, 22);
  SegFieldInfo info = fieldInfo(reader, "f");
  InputStream in = reader.getInputStreamSeek(info.columnLoc);
  EXPECT_EQ((char) 0x7f, in.readByte());
}

TEST(CompoundFileTest, MergeReadsCollapsedSourcesWithDeletes) {
  RAMDir directory;
  verifyCollapsedMerge(directory);
}

TEST(CompoundFileTest, MergeReadsMultiBufferCollapsedSourcesOnFSDirectory) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  verifyCollapsedMerge(directory, 64ULL << 10);
}

TEST(CompoundFileTest, SpillDisqualifiesRelocationAndKeepsStableName) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  IndexRamBudget budget;
  PostingsWriter writer(directory, 18, 1, &budget, true);
  auto streams = writer.getOutputStreams<2>();
  std::string payload = patternedBytes(PostingsWriter::RAM_SPILL_BYTES + 4096);
  auto& field = writer.addField("spill");
  initializeRawField(field, streams[1]->slocation(), (int64_t) payload.size());
  streams[1]->write(payload.data(), payload.size());
  EXPECT_FALSE(streams[1]->isRelocatable());
  EXPECT_LT(budget.reservedBytes(), (int64_t) PostingsWriter::RAM_SPILL_BYTES);

  for (auto& stream : streams) stream.reset();
  std::vector<std::string> filenames;
  writer.finish(&filenames);
  directory.sync(filenames);
  EXPECT_EQ(0, budget.reservedBytes());

  ASSERT_EQ(2u, baseSegmentFiles(directory, 18).size());
  std::string seg = Postings::getSortableString(18);
  EXPECT_NE(nullptr, directory.openFile(
                         Postings::getIndexFileName(seg, 1), true).get());
  PostingsReader reader(directory, 18);
  SegFieldInfo info = fieldInfo(reader, "spill");
  EXPECT_EQ(1u, info.columnLoc.filenum());
  InputStream input = reader.getInputStreamSeek(info.columnLoc);
  ASSERT_GE(input.left(), (int64_t) payload.size());
  EXPECT_EQ(0, std::memcmp(input.ptr(), payload.data(), payload.size()));
}

TEST(CompoundFileTest, ProtectedRamFileMaterializesAsSurvivor) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  IndexRamBudget budget;
  PostingsWriter writer(directory, 19, 1, &budget, true);
  auto streams = writer.getOutputStreams<2>();
  std::string payload = patternedBytes(64ULL << 10);
  auto& field = writer.addField("protected");
  initializeRawField(field, streams[1]->slocation(), (int64_t) payload.size());
  streams[1]->write(payload.data(), payload.size());
  ASSERT_TRUE(streams[1]->isRelocatable());
  std::array<uint32_t, 1> protectedFiles = {1};
  writer.protectOutputFiles(protectedFiles);

  for (auto& stream : streams) stream.reset();
  std::vector<std::string> filenames;
  writer.finish(&filenames);
  directory.sync(filenames);
  EXPECT_EQ(0, budget.reservedBytes());
  ASSERT_EQ(2u, baseSegmentFiles(directory, 19).size());

  PostingsReader reader(directory, 19);
  SegFieldInfo info = fieldInfo(reader, "protected");
  EXPECT_EQ(1u, info.columnLoc.filenum());
  InputStream input = reader.getInputStreamSeek(info.columnLoc);
  ASSERT_GE(input.left(), (int64_t) payload.size());
  EXPECT_EQ(0, std::memcmp(input.ptr(), payload.data(), payload.size()));
}

TEST(CompoundFileTest, DelegatingFlushAndMergeChargeBudget) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  IndexRamBudget budget;
  auto schema = compoundSchema(true);
  std::atomic<int32_t> phase = 0;
  std::atomic<int64_t> flushReserved = 0;
  std::atomic<int64_t> mergeReserved = 0;
  Signal::listen("postingsBeforeCollapse", [&](void*, void*, void*) -> void* {
    int64_t reserved = budget.reservedBytes();
    if (phase.load(std::memory_order_relaxed) == 0) {
      flushReserved.store(reserved, std::memory_order_relaxed);
    } else if (phase.load(std::memory_order_relaxed) == 1) {
      mergeReserved.store(reserved, std::memory_order_relaxed);
    }
    return nullptr;
  });
  auto signalCleanup = luxir::scope_guard([]() {
    Signal::unlisten("postingsBeforeCollapse");
  });

  IndexWriter writer(directory, [schema] { return schema; }, &budget);
  writer.mergePolicy->setMergeFactor(1000);
  Inverter& first = writer.obtainInverter();
  first.startDoc();
  first.getIndexHandler("body").index(first, std::string_view("first common"));
  std::string firstTag = patternedBytes(256ULL << 10);
  indexTag(first, firstTag);
  first.finishDoc();
  int64_t inverterBytes = (int64_t) first.memSize();
  writer.releaseInverter(first, true);
  writer.commit();
  EXPECT_GT(flushReserved.load(std::memory_order_relaxed), inverterBytes);
  EXPECT_EQ(0, budget.reservedBytes());

  phase.store(-1, std::memory_order_relaxed);
  Inverter& second = writer.obtainInverter();
  second.startDoc();
  second.getIndexHandler("body").index(second, std::string_view("second common"));
  std::string secondTag = patternedBytes(256ULL << 10, 'b');
  indexTag(second, secondTag);
  second.finishDoc();
  writer.releaseInverter(second, true);
  writer.commit();
  EXPECT_EQ(0, budget.reservedBytes());

  phase.store(1, std::memory_order_relaxed);
  writer.mergeSegments();
  writer.commit();
  EXPECT_GT(mergeReserved.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(0, budget.reservedBytes());
}

TEST(CompoundFileTest, PartitionRowsMaterializeDelegatingFiles) {
  TempDirectory temp;
  FSDirectory directory(temp.path());
  auto schema = compoundSchema();
  IndexWriter writer(directory, [schema] { return schema; });
  writer.mergePolicy->setMergeFactor(1000);
  writer.termPartitionMinBytes = 1;
  writer.termPartitionMinRangeBytes = 1;
  writer.termPartitionMaxRanges = 4;

  std::string body;
  for (int32_t term = 0; term < 1600; term++) {
    body += wideTerm(term);
    body.push_back(' ');
  }
  for (int32_t source = 0; source < 4; source++) {
    Inverter& inverter = writer.obtainInverter();
    inverter.startDoc();
    inverter.getIndexHandler("body").index(inverter, std::string_view(body));
    inverter.finishDoc();
    inverter.startDoc();
    inverter.getIndexHandler("body").index(inverter, std::string_view(body));
    inverter.finishDoc();
    writer.releaseInverter(inverter, true);
    writer.commit();
  }

  writer.mergeSegments();
  writer.commit();
  auto reader = writer.getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  PostingsReader& postings = reader->segments()[0].postingsReader();
  SegFieldInfo info = fieldInfo(postings, "body");
  ASSERT_FALSE(info.rangeTableLoc.isNull());
  InputStream table = postings.getInputStreamSeek(info.rangeTableLoc);
  const auto* header = reinterpret_cast<const TermRangeTableHeader*>(table.ptr());
  const auto* rows = reinterpret_cast<const TermRangeRow*>(header + 1);
  ASSERT_GT(header->nRanges, 1u);
  for (uint32_t i = 0; i < header->nRanges; i++) {
    EXPECT_NE(nullptr, postings.getFile(rows[i].termsBase.filenum()));
    EXPECT_NE(nullptr, postings.getFile(rows[i].docsBase.filenum()));
    EXPECT_NE(nullptr, postings.getFile(rows[i].posBase.filenum()));
  }

  MemPool pool;
  TermsEnum terms(pool, postings, info);
  ASSERT_TRUE(terms.seek(wideTerm(800)));
  DocsPosEnum docs(terms);
  for (int32_t doc = 0; doc < 8; doc++) EXPECT_EQ(doc, docs.nextDoc());
  EXPECT_EQ(DocsEnumMeta::END, docs.nextDoc());
}
