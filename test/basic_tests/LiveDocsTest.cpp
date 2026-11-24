// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <future>
#include <latch>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "luxir/index/IndexWriter.h"
#include "luxir/index/LiveDocsWriter.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/LiveDocsMemory.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/Signal.h"
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

// The "main" collection matches exactly the ids in live.
void expectLive(CollectionHelper& ch, const std::set<int>& live) {
  EXPECT_EQ((int64_t)live.size(), ch.getIndexWriter()->snapshots.readers.getReader()->liveDocs());
  std::vector<std::string> ids;
  for (int id : live) ids.push_back(std::to_string(id));
  std::ranges::sort(ids);
  EXPECT_EQ(ids, allIds(ch));
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

std::string rawBytes(Directory& dir) {
  auto file = dir.openFile(Postings::getLiveDocsFileName(Postings::segmentKey(1, 1), 1, 1));
  auto hold = file->acquire({0, file->size()});
  return std::string(hold.getData(), hold.getLength());
}

} // namespace

TEST_F(LiveDocsTest, writerPicksSmallerKindAndReaderRestoresBits) {
  for (int32_t deletes : {0, 1, 4, 10}) {
    RAMDir dir;
    RAMFixedBitSet bits(100, true);
    for (int32_t i = 0; i < deletes; i++) bits.clear(i * 9 + 1);
    // 100 docs take two bitmap words (16 bytes): SPARSE for at most one delete.
    bool sparse = deletes <= 1;
    uint64_t expected = sparse ? 24 + 4 * (uint64_t)deletes : 24 + 16;
    EXPECT_EQ(expected, LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 100, 100 - deletes));
    auto name = Postings::getLiveDocsFileName(Postings::segmentKey(1, 1), 1, 1);
    EXPECT_EQ(expected, dir.openFile(name)->size());

    auto before = liveDocsMemory.sparseRoots.bytes.load();
    auto views = liveDocsMemory.views.bytes.load();
    auto live = LiveDocs::create(dir, 1, 1, 1, 1, 100, -1, 0, false);
    ASSERT_NE(nullptr, live);
    // A sparse file is expanded into a heap bitmap with its own counter; child
    // live views keep theirs.
    uint64_t heap = sparse ? sizeof(LiveDocs) + 16 : 0;
    EXPECT_EQ(heap, live->heapBytes());
    EXPECT_EQ(before + heap, liveDocsMemory.sparseRoots.bytes.load());
    EXPECT_EQ(views, liveDocsMemory.views.bytes.load());
    EXPECT_EQ(deletes, live->numDeletes());
    for (int32_t doc = 0; doc < 100; doc++) EXPECT_EQ(bits.get(doc), live->bitset().get(doc)) << doc;
    live.reset();
    EXPECT_EQ(before, liveDocsMemory.sparseRoots.bytes.load());
  }
}

TEST_F(LiveDocsTest, sparseRoundTripWhenMaxDocFillsWholeWords) {
  RAMDir dir;
  RAMFixedBitSet bits(256, true);
  bits.clear(0);
  bits.clear(255);
  EXPECT_EQ(24u + 8, LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 256, 254));
  // Pins the SPARSE bytes, which the format reference index (one small segment
  // delete, written DENSE) does not cover.
  RAMDir expected;
  writeRaw(expected, LiveDocsWriter::SPARSE, 256, 254, std::vector<uint32_t>{0, 255});
  EXPECT_EQ(rawBytes(expected), rawBytes(dir));
  auto live = LiveDocs::create(dir, 1, 1, 1, 1, 256, -1, 0, false);
  EXPECT_EQ(254, live->numLive());
  for (int32_t doc = 0; doc < 256; doc++) EXPECT_EQ(doc != 0 && doc != 255, live->bitset().get(doc)) << doc;
}

TEST_F(LiveDocsTest, writerRejectsBitsTheHeaderWouldMisstate) {
  RAMDir dir;
  RAMFixedBitSet bits(100, true);
  bits.clear(5);
  // A count that disagrees with the bits, for a SPARSE and a DENSE count.
  EXPECT_THROW(LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 100, 100), std::logic_error);
  EXPECT_THROW(LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 100, 90), std::logic_error);
  // A live bit past maxDoc, which a dense reader would map.
  bits.words[1] |= 1ULL << 40;
  EXPECT_THROW(LiveDocsWriter::writeLiveDocs(dir, 1, 1, 1, 1, bits, 100, 99), std::logic_error);
}

TEST_F(LiveDocsTest, corruptSparseFilesThrow) {
  auto load = [](uint64_t kind, int32_t maxDoc, int32_t numLive, std::vector<uint32_t> ids) {
    RAMDir dir;
    writeRaw(dir, kind, maxDoc, numLive, ids);
    return LiveDocs::create(dir, 1, 1, 1, 1, 100, -1, 0, false);
  };
  EXPECT_EQ(97, load(LiveDocsWriter::SPARSE, 100, 97, {3, 50, 99})->numLive());
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 98, {50, 3}), std::runtime_error);   // unsorted
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 98, {3, 3}), std::runtime_error);    // duplicate
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 99, {100}), std::runtime_error);     // id >= maxDoc
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 98, {3}), std::runtime_error);       // too short
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 100, 99, {3, 4}), std::runtime_error);    // too long
  EXPECT_THROW(load(LiveDocsWriter::SPARSE, 99, 98, {3}), std::runtime_error);        // maxDoc mismatch
  EXPECT_THROW(load(3, 100, 99, {3}), std::runtime_error);                            // unknown kind
  // A DENSE body is exactly two words for 100 docs.
  EXPECT_EQ(100, load(LiveDocsWriter::DENSE, 100, 100, {~0u, ~0u, ~0u, 0xFu})->numLive());
  EXPECT_THROW(load(LiveDocsWriter::DENSE, 100, 100, {~0u, ~0u, ~0u}), std::runtime_error);
  EXPECT_THROW(load(LiveDocsWriter::DENSE, 100, 100, {~0u, ~0u, ~0u, 0xFu, 0}), std::runtime_error);
}

TEST_F(LiveDocsTest, kindFollowsDeleteCountAcrossGenerationsAndMerges) {
  LuxirNode node;
  CollectionHelper ch(node, "main");
  auto writer = ch.getIndexWriter();
  writer->mergePolicy->setMergeFactor(1000);
  // 640 docs take ten bitmap words (80 bytes): SPARSE for at most five deletes.
  indexRange(ch, 0, 640);
  auto live = range(0, 640);
  auto written = writer->stats(false).liveDocsBytesWritten;

  deleteIds(ch, live, {7});
  EXPECT_EQ(std::vector<uint64_t>{28}, liveFileBytes(writer->dir));
  EXPECT_EQ(written + 28, writer->stats(false).liveDocsBytesWritten);
  EXPECT_EQ(1, writer->snapshots.readers.getReader()->segments()[0].numDeletes());
  expectLive(ch, live);

  deleteIds(ch, live, {0, 63, 64, 639});
  EXPECT_EQ(std::vector<uint64_t>{24 + 4 * 5}, liveFileBytes(writer->dir));
  expectLive(ch, live);
  deleteIds(ch, live, {100});
  EXPECT_EQ(std::vector<uint64_t>{24 + 80}, liveFileBytes(writer->dir));
  expectLive(ch, live);

  // The merged segment has no deletes; its first delete is sparse again.
  indexRange(ch, 640, 650);
  for (int id = 640; id < 650; id++) live.insert(id);
  writer->mergeSegments();
  writer->commit();
  EXPECT_EQ(std::vector<uint64_t>{0}, liveFileBytes(writer->dir));
  deleteIds(ch, live, {645});
  EXPECT_EQ(std::vector<uint64_t>{28}, liveFileBytes(writer->dir));
  expectLive(ch, live);
}

TEST_F(LiveDocsTest, mergeDiffWritesEitherKind) {
  for (bool dense : {false, true}) {
    LuxirNode node;
    CollectionHelper ch(node, "main");
    auto writer = ch.getIndexWriter();
    writer->mergePolicy->setMergeFactor(1000);
    // The merged 640 docs take 80 bytes: SPARSE for at most five deletes.
    indexRange(ch, 0, 320);
    indexRange(ch, 320, 640);
    auto live = range(0, 640);
    std::latch started(1), release(1);
    Signal::listen("mergeStart", [&](void*, void*, void*) -> void* {
      started.count_down();
      release.wait();
      return nullptr;
    });
    auto unlisten = scope_guard([] { Signal::unlisten("mergeStart"); });
    auto merge = std::async(std::launch::async, [&] { writer->mergeSegments(); });
    {
      // Released on any exit, so the future's destructor never waits on a blocked merge.
      auto releaseMerge = scope_guard([&] { release.count_down(); });
      started.wait();
      // Deletes landing during the merge reach the output through its live docs diff.
      deleteIds(ch, live, {1, 319, 400});
      if (dense) deleteIds(ch, live, {2, 3, 320, 321});
    }
    merge.get();
    writer->updateGraph->wait_for_all();
    writer->commit();
    EXPECT_EQ(std::vector<uint64_t>{dense ? 24u + 80 : 24u + 4 * 3}, liveFileBytes(writer->dir));
    expectLive(ch, live);
  }
}

TEST_F(LiveDocsTest, indexingErrorsWriteSparseFile) {
  LuxirNode node;
  CollectionHelper ch(node, "main");
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
  expectLive(ch, live);
}

} // namespace luxir::test
