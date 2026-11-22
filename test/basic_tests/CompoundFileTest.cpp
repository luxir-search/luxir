// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include "luxir/index/IndexWriter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/index/SegmentMerger.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/FSDirectory.h"
#include "test/StorageTestUtils.h"
#include "test/TestUtils.h"

using namespace luxir;

namespace {

class CompoundFileTest : public ::testing::Test {
protected:
  std::string path = "/tmp/luxir-compound-XXXXXX";

  void SetUp() override { ASSERT_NE(nullptr, ::mkdtemp(path.data())); }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

} // namespace

TEST_F(CompoundFileTest, SmallAndEmptyCheckoutsUseFileZero) {
  FSDirectory dir(path);
  PostingsWriter writer(dir, 1, 1, 1);
  auto streams = writer.getOutputStreams<3>();
  streams[0]->writeBytes("first");
  streams[1]->writeBytes("second");
  EXPECT_TRUE(std::filesystem::is_empty(path));

  auto first = writer.finishStream(std::move(streams[0]));
  auto empty = writer.finishStream(std::move(streams[2]));
  auto second = writer.finishStream(std::move(streams[1]));
  EXPECT_EQ(5u, first.bytes);
  EXPECT_EQ(0u, empty.bytes);
  EXPECT_EQ(empty, empty.subSlice(0, 0));
  EXPECT_THROW(empty.subSlice(0, 1), std::out_of_range);
  EXPECT_EQ(6u, second.bytes);
  EXPECT_FALSE(empty.location.isNull());
  EXPECT_EQ(empty.location, second.location);
  EXPECT_LT(first.end().offset(), second.location.offset());

  std::vector<std::string> files;
  test::writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish(&files));
  ASSERT_EQ(1u, files.size());
  SegmentCore core(dir, 1, 1);
  for (const auto& slice : {first, empty, second}) {
    auto location = slice.location;
    EXPECT_EQ(0u, location.filenum());
    EXPECT_EQ(0u, location.offset() % MAX_ALIGN);
  }
  EXPECT_EQ("first", std::string_view(core.getInputStreamSeek(first.location).ptr(), 5));
  EXPECT_EQ("second", std::string_view(core.getInputStreamSeek(empty.location).ptr(), 6));
}

TEST_F(CompoundFileTest, ReservationCrossingCapacitySpills) {
  FSDirectory dir(path);
  IndexRamBudget budget;
  auto staged = budget.forceAcquire(0);
  PostingsWriter writer(dir, 1, 1, 1);
  writer.setStagedRamAccounting([&](int64_t delta) { staged.adjustSize(delta); });
  auto out = writer.getOutputStream();
  std::string payload(PostingsWriter::RAM_SPILL_BYTES - 1, 'x');
  out->writeBytes(payload);
  EXPECT_TRUE(std::filesystem::is_empty(path));
  EXPECT_EQ((int64_t)PostingsWriter::RAM_SPILL_BYTES, budget.reservedBytes());

  auto* target = out->getFile();
  EXPECT_GE(out->reserve(2), 2u);
  EXPECT_EQ(target, out->getFile());
  EXPECT_EQ(payload.size(), out.pos());
  EXPECT_EQ(0, budget.reservedBytes());
  auto spillName = Postings::getIndexFileName(Postings::segmentKey(1, 1), 1);
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(path) / (spillName + ".tmp")));
  out->unsafeWrite("yz", 2);
  auto spill = writer.finishStream(std::move(out));

  auto smallOut = writer.getOutputStream();
  smallOut->writeBytes("small");
  auto small = writer.finishStream(std::move(smallOut));
  EXPECT_EQ(payload.size() + 2, spill.bytes);
  EXPECT_EQ(5u, small.bytes);
  EXPECT_EQ(1u, spill.location.filenum());
  EXPECT_EQ(0u, small.location.filenum());

  std::vector<std::string> files;
  test::writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish(&files));
  ASSERT_EQ(2u, files.size());
  SegmentCore core(dir, 1, 1);
  auto input = core.getInputStreamSeek(spill.location);
  EXPECT_EQ(payload, std::string_view(input.ptr(), payload.size()));
  input.skip(payload.size());
  EXPECT_EQ('y', input.readByte());
  EXPECT_EQ('z', input.readByte());
  EXPECT_EQ(SVB_OVERREAD_PAD, input.left());
}

TEST_F(CompoundFileTest, FinishedSpillsAreReused) {
  FSDirectory dir(path);
  PostingsWriter writer(dir, 1, 1, 1);
  std::string payload(PostingsWriter::RAM_SPILL_BYTES + 1, 'x');
  auto firstOut = writer.getOutputStream();
  firstOut->writeBytes(payload);
  auto first = writer.finishStream(std::move(firstOut));

  auto secondOut = writer.getOutputStream();
  payload.front() = 'y';
  secondOut->writeBytes(payload);
  auto second = writer.finishStream(std::move(secondOut));
  EXPECT_EQ(payload.size(), first.bytes);
  EXPECT_EQ(payload.size(), second.bytes);
  EXPECT_EQ(1u, first.location.filenum());
  EXPECT_EQ(first.location.filenum(), second.location.filenum());
  EXPECT_GE(second.location.offset(), first.end().offset());
  EXPECT_EQ(0u, second.location.offset() % MAX_ALIGN);

  std::vector<std::string> files;
  test::writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish(&files));
  EXPECT_EQ(2u, files.size());
  SegmentCore core(dir, 1, 1);
  EXPECT_EQ('x', core.getInputStreamSeek(first.location).readByte());
  EXPECT_EQ(payload, std::string_view(core.getInputStreamSeek(second.location).ptr(), payload.size()));
}

TEST_F(CompoundFileTest, BusySpillsGetNewFilesAndShortestFinishedFileIsReused) {
  FSDirectory dir(path);
  PostingsWriter writer(dir, 1, 1, 1);
  auto streams = writer.getOutputStreams<2>();
  std::string payload(PostingsWriter::RAM_SPILL_BYTES + 1, 'x');
  streams[0]->writeBytes(payload);
  streams[0]->writeBytes(payload);
  streams[1]->writeBytes(payload);

  EXPECT_EQ(1u, writer.finishStream(std::move(streams[0])).location.filenum());
  EXPECT_EQ(2u, writer.finishStream(std::move(streams[1])).location.filenum());
  auto third = writer.getOutputStream();
  third->writeBytes(payload);
  EXPECT_EQ(2u, writer.finishStream(std::move(third)).location.filenum());
  std::vector<std::string> files;
  test::writeUnversionedRuns(writer);
  ASSERT_TRUE(writer.finish(&files));
  EXPECT_EQ(3u, files.size());
}

TEST_F(CompoundFileTest, StagedBudgetReleasedOnFinishAndAbandon) {
  FSDirectory dir(path);
  IndexRamBudget budget;
  auto staged = budget.forceAcquire(0);
  PostingsWriter writer(dir, 1, 1, 1);
  writer.setStagedRamAccounting([&](int64_t delta) { staged.adjustSize(delta); });
  {
    auto abandoned = writer.getOutputStream();
    abandoned->writeBytes("discard");
    EXPECT_GT(budget.reservedBytes(), 0);
  }
  EXPECT_EQ(0, budget.reservedBytes());
  EXPECT_TRUE(std::filesystem::is_empty(path));

  auto kept = writer.getOutputStream();
  kept->writeBytes("keep");
  EXPECT_GT(budget.reservedBytes(), 0);
  writer.finishStream(std::move(kept));
  EXPECT_EQ(0, budget.reservedBytes());
  test::writeUnversionedRuns(writer);
  EXPECT_TRUE(writer.finish());
}

TEST_F(CompoundFileTest, AbandoningSpillLeavesSegmentUnfinishable) {
  FSDirectory dir(path);
  PostingsWriter writer(dir, 1, 1, 1);
  auto spillName = Postings::getIndexFileName(Postings::segmentKey(1, 1), 1);
  auto tmp = std::filesystem::path(path) / (spillName + ".tmp");
  uint64_t flushedSize;
  {
    auto out = writer.getOutputStream();
    out->writeBytes(std::string(PostingsWriter::RAM_SPILL_BYTES + 1, 'x'));
    ASSERT_GT(out->buffered(), 0u);
    flushedSize = std::filesystem::file_size(tmp);
  }
  EXPECT_EQ(flushedSize, std::filesystem::file_size(tmp));
  test::writeUnversionedRuns(writer);
  EXPECT_THROW(writer.finish(), std::logic_error);
}

TEST_F(CompoundFileTest, InverterStagingCountsTowardFlushAndDraining) {
  FSDirectory dir(path);
  IndexRamBudget budget(PostingsWriter::RAM_SPILL_BYTES - 1);
  Inverter inverter(dir, 1, 1, std::make_shared<Schema>(), &budget);
  inverter.ramGuard.forceResize((int64_t)inverter.memSize());
  auto& writer = inverter.getPostingsWriter();
  size_t cap = inverter.memSize() + PostingsWriter::RAM_SPILL_BYTES - 1;
  EXPECT_FALSE(inverter.shouldFlush(cap, SIZE_MAX));
  auto out = writer.getOutputStream();
  out->writeBytes(std::string(PostingsWriter::RAM_SPILL_BYTES - 1, 'x'));
  EXPECT_TRUE(inverter.shouldFlush(cap, SIZE_MAX));
  ASSERT_TRUE(budget.pressureNeeded());
  EXPECT_EQ(inverter.ramGuard.size(), budget.reservedBytes());

  ASSERT_TRUE(inverter.ramGuard.tryMarkDrainingForPressure());
  EXPECT_FALSE(budget.pressureNeeded());
  EXPECT_EQ(budget.reservedBytes(), budget.drainingBytesCount());
  int64_t reserved = budget.reservedBytes();
  writer.finishStream(std::move(out));
  EXPECT_EQ(reserved - (int64_t)PostingsWriter::RAM_SPILL_BYTES, budget.reservedBytes());
  EXPECT_EQ(budget.reservedBytes(), budget.drainingBytesCount());
}

TEST_F(CompoundFileTest, FailedSegmentFileCreationAbortsAndCleansUp) {
  auto dir = std::make_shared<FSDirectory>(path);
  CheckedDirectory checked(dir, CheckedDirMode::THROW);
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["tag"] = std::make_shared<StrFieldType>("tag", FieldType::COLUMN_STORED);
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(checked));
  IndexRamBudget budget;
  IndexWriter writer(snapshots, schema, &budget);
  for (uint32_t failedFile : {0u, 1u}) {
    auto& inverter = writer.obtainInverter();
    auto& postings = inverter.getPostingsWriter();
    auto postingsSegId = postings.segId;
    auto epoch = postings.creatorEpoch;
    auto key = Postings::segmentKey(postingsSegId, epoch);
    auto spillName = Postings::getIndexFileName(key, 1);
    auto failedName = Postings::getIndexFileName(key, failedFile);
    std::string payload(PostingsWriter::RAM_SPILL_BYTES + 1, 'x');
    if (failedFile == 0) {
      // A spill file already exists when file 0 fails; otherwise the flush's own spill fails.
      auto first = postings.getOutputStream();
      first->writeBytes(payload);
      ASSERT_EQ(1u, postings.finishStream(std::move(first)).location.filenum());
      ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(path) / (spillName + ".tmp")));
    }
    inverter.startDoc();
    inverter.getIndexHandler("tag").index(inverter, std::string_view(payload));
    inverter.finishDoc();
    writer.releaseInverter(inverter);

    Signal::listen("checkedCreateFile", [&](void* source, void* name, void*) -> void* {
      return source == &checked && *(std::string_view*)name == failedName ? source : nullptr;
    });
    auto fault = scope_guard([] { Signal::unlisten("checkedCreateFile"); });
    ExpectLog quiet("Failed to create segment file");
    try {
      writer.commit();
      ADD_FAILURE() << "Expected FileIOException for " << failedName;
    } catch (const FileIOException& error) {
      EXPECT_NE(std::string_view::npos, std::string_view(error.what()).find(failedName));
    }
    writer.updateGraph->wait_for_all();
    EXPECT_EQ(0, budget.reservedBytes());
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      EXPECT_FALSE(entry.path().filename().string().starts_with(Postings::getIndexFileNamePrefix(postingsSegId, epoch)));
    }
  }
}

TEST_F(CompoundFileTest, MergeWithDeletesOmitsDensePresenceBitmap) {
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["score"] = std::make_shared<IntFieldType>("score", FieldType::COLUMN_STORED);
  RAMDir sources, output;
  IndexRamBudget budget;
  StoredDictionaryRegistry dictionaries;
  // An eager bitmap writer would emit a complete bucket before discovering all rows are present.
  constexpr int32_t numDocs = screaming::BitSet::BUCKET_SIZE + 1;
  constexpr int32_t missing = numDocs / 2;
  auto words = screaming::FixedBitSet::allocate(numDocs + 1, true);
  screaming::FixedBitSet(words.get(), numDocs + 1).clear(missing);
  LiveDocs liveDocs(std::move(words), numDocs + 1, numDocs);
  std::array<size_t, 2> sizes;

  for (bool sparse : {false, true}) {
    uint64_t segment = sparse ? 2 : 1;
    Inverter inverter(sources, segment, 1, schema);
    auto& score = inverter.getIndexHandler("score");
    for (int32_t doc = 0; doc <= numDocs; doc++) {
      if (doc == missing && !sparse) continue;
      inverter.startDoc();
      if (doc != missing) score.index(inverter, (int64_t)doc);
      inverter.finishDoc();
    }
    ASSERT_TRUE(inverter.flush());
    std::array cores{std::make_shared<SegmentCore>(sources, segment, 1)};
    LiveDocs* live[] = {sparse ? &liveDocs : nullptr};
    const auto* source = cores[0]->rootReader().fieldInfo("score");
    ASSERT_NE(nullptr, source);
    EXPECT_EQ(sparse, source->docsBitmap().has_value());

    PostingsWriter writer(output, segment, 1);
    SegmentMerger merger(cores, live, writer, budget, INT64_MAX, 1, 4, *schema, dictionaries);
    merger.merge();
    std::vector<FileDescriptor> files;
    ASSERT_TRUE(writer.finish(nullptr, &files));
    ASSERT_EQ(1u, files.size());
    sizes[sparse] = files.front().size;
    SegmentCore merged(output, segment, 1);
    const auto* field = merged.rootReader().fieldInfo("score");
    ASSERT_NE(nullptr, field);
    EXPECT_EQ(numDocs, field->docsWithField);
    EXPECT_FALSE(field->docsBitmap());
  }
  EXPECT_EQ(sizes[0], sizes[1]);
}

TEST_F(CompoundFileTest, ReopensWithSpilledColumnAndSmallFields) {
  FSDirectory dir(path);
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["tag"] = std::make_shared<StrFieldType>("tag", FieldType::COLUMN_STORED);
  schema->fieldTypeMap["score"] = std::make_shared<IntFieldType>("score", FieldType::COLUMN_STORED);
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");
  CommitSnapshotRegistry snapshots(test::reserveIncarnation(dir));
  IndexWriter writer(snapshots, schema);

  auto& inverter = writer.obtainInverter();
  auto segId = inverter.getSegId();
  auto epoch = inverter.getPostingsWriter().creatorEpoch;
  std::string payload(PostingsWriter::RAM_SPILL_BYTES + 1, 'x');
  inverter.startDoc();
  inverter.getIndexHandler("tag").index(inverter, std::string_view(payload));
  inverter.getIndexHandler("score").index(inverter, (int64_t)17);
  inverter.getIndexHandler("body").index(inverter, std::string_view("alpha beta"));
  inverter.finishDoc();
  writer.releaseInverter(inverter, true);
  writer.commit();

  SegmentCore core(dir, segId, epoch);
  auto& postings = core.rootReader();
  const auto* tag = postings.fieldInfo("tag");
  const auto* score = postings.fieldInfo("score");
  const auto* body = postings.fieldInfo("body");
  ASSERT_TRUE(tag && score && body);
  EXPECT_EQ(1u, tag->bytesColumn()->values.location.filenum());
  EXPECT_EQ(0u, score->numberColumn()->location.filenum());
  EXPECT_EQ(0u, body->terms()->location.filenum());

  StrColReader tags(postings, *tag);
  StrColReader::Iterator tagIter(tags);
  ASSERT_EQ(0, tagIter.next());
  EXPECT_TRUE(tagIter.value() == payload);

  IntColReader scores(postings, *score);
  IntColReader::Iterator scoreIter(scores);
  ASSERT_EQ(0, scoreIter.next());
  EXPECT_EQ(17, scoreIter.value());

  MemPool pool;
  TermsEnum terms(pool, postings, *body);
  ASSERT_TRUE(terms.seek("beta"));
  DocsPosEnum docs(terms);
  ASSERT_EQ(0, docs.nextDoc());
  EXPECT_EQ(1, docs.termFreq());
}
