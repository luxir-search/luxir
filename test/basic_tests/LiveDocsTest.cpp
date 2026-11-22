// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
#include <latch>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "luxir/index/IndexWriter.h"
#include "luxir/index/LiveDocsWriter.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

class LiveDocsTest : public LuxirTest {};

namespace {

using screaming::FixedBitSet;
using screaming::RAMFixedBitSet;

// Indexes ids [from, to) as one committed segment.
void indexRange(CollectionHelper& ch, int from, int to) {
  std::vector<Doc> docs;
  for (int i = from; i < to; i++) docs.push_back(flatdoc("id", std::to_string(i)));
  ASSERT_TRUE(ch.indexAll(std::span<const Doc>(docs), UpdateMessage::COMMIT).success);
}

void deleteIds(CollectionHelper& ch, std::set<int>& live, std::initializer_list<int> ids) {
  std::vector<std::string> names;
  for (int id : ids) {
    names.push_back(std::to_string(id));
    live.erase(id);
  }
  ASSERT_TRUE(ch.deleteByIds(std::span<const std::string>(names), UpdateMessage::COMMIT).success);
}

// The committed live docs file of each segment, in manifest order; 0 for a segment without deletes.
std::vector<uint64_t> liveFileBytes(Directory& dir) {
  auto info = readDurableIndexInfo(dir);
  std::vector<uint64_t> sizes;
  for (const auto& seg : info->segments) {
    uint64_t size = 0;
    if (seg.live_gen > 0) {
      auto name = Postings::getLiveDocsFileName(Postings::segmentKey(seg.seg_id, seg.creator_epoch), seg.live_gen, seg.live_epoch);
      for (const auto& file : seg.files) if (file.name == name) size = file.size;
      EXPECT_EQ(size, dir.openFile(name)->size());
    }
    sizes.push_back(size);
  }
  return sizes;
}

// Every id in [0, maxId) matches exactly when it is in live.
void expectLive(CollectionHelper& ch, std::string_view name, const std::set<int>& live, int maxId) {
  auto reader = ch.getIndexWriter()->snapshots.readers.getReader();
  EXPECT_EQ((int64_t)live.size(), reader->liveDocs());
  for (int id = 0; id < maxId; id++) {
    auto req = localReq(ch.getSearchEngine());
    req->collection(name).topDocs().exprQuery("id:" + std::to_string(id)).countLimit().limit(0);
    req->execute(1);
    ASSERT_OK(req);
    EXPECT_EQ(live.contains(id) ? 1 : 0, req->getMatchCount()) << "id " << id;
  }
}

std::set<int> range(int from, int to) {
  std::set<int> ids;
  for (int i = from; i < to; i++) ids.insert(i);
  return ids;
}

void writeRaw(Directory& dir, uint64_t kind, int32_t maxDoc, int32_t numLive, std::span<const uint32_t> ids) {
  auto file = dir.createFile(Postings::getLiveDocsFileName(Postings::segmentKey(1, 1), 1, 1));
  OutputStream out(file.get());
  out.writeBytes(Postings::LUXIR_HEADER);
  out.writeLong((int64_t)kind);
  out.writeInt(maxDoc);
  out.writeInt(numLive);
  out.write(ids.data(), ids.size_bytes());
  out.close();
  dir.finishFile(*file);
}

} // namespace

TEST_F(LiveDocsTest, writerPicksSmallerKindAndReaderRestoresBits) {
  for (int32_t deletes : {0, 1, 3, 4, 10}) {
    RAMDir dir;
    RAMFixedBitSet bits(100, true);
    for (int32_t i = 0; i < deletes; i++) bits.clear(i * 9 + 1);
    // 100 docs take two bitmap words: SPARSE below four deletes.
    uint64_t expected = deletes < 4 ? 24 + 4 * (uint64_t)deletes : 24 + 16;
    EXPECT_EQ(expected, LiveDocsWriter::fileBytes(100, 100 - deletes));
    EXPECT_EQ(expected, LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 100, 100 - deletes));
    auto name = Postings::getLiveDocsFileName(Postings::segmentKey(1, 1), 1, 1);
    EXPECT_EQ(expected, dir.openFile(name)->size());

    auto before = fieldMetadataMemory.liveViewBytes.load();
    auto live = LiveDocs::create(dir, 1, 1, 1, 1, 100, false);
    ASSERT_NE(nullptr, live);
    // A sparse file is expanded into a heap bitmap, reported as a live view.
    uint64_t heap = deletes < 4 ? sizeof(LiveDocs) + 16 : 0;
    EXPECT_EQ(before + heap, fieldMetadataMemory.liveViewBytes.load());
    EXPECT_EQ(deletes, live->numDeletes());
    for (int32_t doc = 0; doc < 100; doc++) EXPECT_EQ(bits.get(doc), live->bitset().get(doc)) << doc;
    live.reset();
    EXPECT_EQ(before, fieldMetadataMemory.liveViewBytes.load());
  }
}

TEST_F(LiveDocsTest, writerRejectsCountThatDisagreesWithBits) {
  RAMDir dir;
  RAMFixedBitSet bits(100, true);
  bits.clear(5);
  EXPECT_THROW(LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 100, 98), std::logic_error);
}

TEST_F(LiveDocsTest, corruptSparseFilesThrow) {
  auto load = [](uint64_t kind, int32_t maxDoc, int32_t numLive, std::vector<uint32_t> ids) {
    RAMDir dir;
    writeRaw(dir, kind, maxDoc, numLive, ids);
    return LiveDocs::create(dir, 1, 1, 1, 1, 100, false);
  };
  EXPECT_EQ(97, load(LiveDocsWriter::SPARSE, 100, 97, {3, 50, 99})->numLive());
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 98, {50, 3}), std::runtime_error);   // unsorted
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 98, {3, 3}), std::runtime_error);    // duplicate
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 99, {100}), std::runtime_error);     // id >= maxDoc
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 98, {3}), std::runtime_error);       // too short
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 99, {3, 4}), std::runtime_error);    // too long
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 99, 98, {3}), std::runtime_error);        // maxDoc mismatch
  EXPECT_THROW(load(3, 100, 99, {3}), std::runtime_error);                            // unknown kind
}

TEST_F(LiveDocsTest, kindFollowsDeleteCountAcrossGenerationsAndMerges) {
  LuxirNode node;
  CollectionHelper ch(node, "live_kinds");
  auto writer = ch.getIndexWriter();
  writer->mergePolicy->setMergeFactor(1000);
  // 300 docs take five bitmap words (40 bytes): SPARSE below ten deletes.
  indexRange(ch, 0, 300);
  auto live = range(0, 300);
  auto written = writer->stats(false).liveDocsBytesWritten;

  deleteIds(ch, live, {7});
  EXPECT_EQ(std::vector<uint64_t>{28}, liveFileBytes(writer->dir));
  EXPECT_EQ(written + 28, writer->stats(false).liveDocsBytesWritten);
  EXPECT_EQ(1, writer->snapshots.readers.getReader()->segments()[0].numDeletes());
  expectLive(ch, "live_kinds", live, 300);

  deleteIds(ch, live, {0, 63, 64, 299});
  EXPECT_EQ(std::vector<uint64_t>{24 + 4 * 5}, liveFileBytes(writer->dir));
  deleteIds(ch, live, {100, 101, 102, 103, 104});
  EXPECT_EQ(std::vector<uint64_t>{24 + 40}, liveFileBytes(writer->dir));
  expectLive(ch, "live_kinds", live, 300);

  // The merged segment has no deletes; its first delete is sparse again.
  indexRange(ch, 300, 310);
  for (int id = 300; id < 310; id++) live.insert(id);
  writer->mergeSegments();
  writer->commit();
  EXPECT_EQ(std::vector<uint64_t>{0}, liveFileBytes(writer->dir));
  deleteIds(ch, live, {305});
  EXPECT_EQ(std::vector<uint64_t>{28}, liveFileBytes(writer->dir));
  expectLive(ch, "live_kinds", live, 310);
}

TEST_F(LiveDocsTest, mergeDiffWritesEitherKind) {
  for (bool dense : {false, true}) {
    LuxirNode node;
    CollectionHelper ch(node, "live_merge");
    auto writer = ch.getIndexWriter();
    writer->mergePolicy->setMergeFactor(1000);
    indexRange(ch, 0, 150);
    indexRange(ch, 150, 300);
    auto live = range(0, 300);
    std::latch started(1), release(1);
    Signal::listen("mergeStart", [&](void*, void*, void*) -> void* {
      started.count_down();
      release.wait();
      return nullptr;
    });
    auto unlisten = scope_guard([] { Signal::unlisten("mergeStart"); });
    auto merge = std::async(std::launch::async, [&] { writer->mergeSegments(); });
    started.wait();
    // Deletes landing during the merge reach the output through its live docs diff.
    deleteIds(ch, live, {1, 149, 200});
    if (dense) deleteIds(ch, live, {2, 3, 4, 5, 6, 7, 8, 150, 151, 152});
    release.count_down();
    merge.get();
    writer->updateGraph->wait_for_all();
    writer->commit();
    EXPECT_EQ(std::vector<uint64_t>{dense ? 24u + 40 : 24u + 4 * 3}, liveFileBytes(writer->dir));
    expectLive(ch, "live_merge", live, 300);
  }
}

TEST_F(LiveDocsTest, indexingErrorsWriteSparseFile) {
  LuxirNode node;
  CollectionHelper ch(node, "live_errors");
  auto writer = ch.getIndexWriter();
  std::vector<Doc> docs;
  for (int i = 0; i < 300; i++) docs.push_back(flatdoc("id", std::to_string(i)));
  docs[42] = flatdoc("id", "42", "no_such_field", "boom");
  auto result = ch.indexAll(std::span<const Doc>(docs), UpdateMessage::COMMIT);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ(std::vector<uint64_t>{28}, liveFileBytes(writer->dir));
  EXPECT_EQ(28u, writer->stats(false).liveDocsBytesWritten);
  auto live = range(0, 300);
  live.erase(42);
  expectLive(ch, "live_errors", live, 300);
}

TEST_F(LiveDocsTest, sparseFileReloadsAfterRestart) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-livedocs-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  auto live = range(0, 300);
  {
    LuxirNode node(config);
    CollectionHelper ch(node, "main");
    indexRange(ch, 0, 300);
    deleteIds(ch, live, {9, 290});
    EXPECT_EQ(std::vector<uint64_t>{32}, liveFileBytes(ch.getIndexWriter()->dir));
  }
  LuxirNode node(config);
  CollectionHelper ch(node, "main");
  auto reader = ch.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(2, reader->segments()[0].numDeletes());
  expectLive(ch, "main", live, 300);
  // A standalone reader over the same files sees the same deletes.
  IndexReader standalone(ch.getIndexWriter()->dir);
  EXPECT_EQ(298, standalone.liveDocs());
}

} // namespace luxir::test
