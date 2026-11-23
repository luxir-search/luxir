// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// A writer over a collection whose segments sit on a remote store (cold):
// writes start without fetching old segment data, and overwrites read only the
// id lookup's ranges.

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <vector>

#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/reader/IdFilter.h"
#include "test/ColdDirectory.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/StorageTestUtils.h"

using namespace luxir;

namespace {

class ColdWriterTest : public LuxirTest {
public:
  static constexpr int NO_MERGES = 1000;
  // Each batch gets a later update version, so an overwrite outranks the
  // documents it replaces.
  uint64_t nextVersion = 1;

  // One flushed, committed segment of these ids; an empty id indexes none.
  void addDocs(IndexWriter& iw, const std::vector<std::string>& ids, bool overwrite = true) {
    auto& inv = iw.obtainInverter(nextVersion++);
    inv.overwrite = overwrite;
    for (size_t i = 0; i < ids.size(); i++) {
      inv.startDoc();
      if (!ids[i].empty()) inv.getIndexHandler("id").index(inv, std::string_view(ids[i]));
      inv.getIndexHandler("text_w").index(inv, std::string_view(i % 2 ? "some odd words" : "some even words"));
      inv.finishDoc();
    }
    iw.releaseInverter(inv, true);
    iw.commit();
  }

  // Ids spread over the whole key space, as random ids are.
  static std::string randomId(int seg, int i) { return std::format("{:016x}", IdFilter::hash(std::format("{}/{}", seg, i))); }

  static std::vector<std::string> randomIds(int seg, int count) {
    std::vector<std::string> ids;
    for (int i = 0; i < count; i++) ids.push_back(randomId(seg, i));
    return ids;
  }

  // Segments of these ids written to a local directory, one per batch.
  std::shared_ptr<RAMDir> writeSegments(const std::vector<std::vector<std::string>>& batches, bool overwrite = true) {
    auto ram = std::make_shared<RAMDir>();
    CommitSnapshotRegistry snapshots(test::reserveIncarnation(*ram));
    IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
    for (const auto& ids : batches) addDocs(iw, ids, overwrite);
    return ram;
  }

  struct SegmentReads {
    uint64_t dataBytes = 0, fetched = 0, wholeReads = 0;
  };
  static SegmentReads readsOf(test::ColdDirectory& cold, const api::SegmentInfo& segment) {
    SegmentReads reads;
    for (const auto& file : segment.files) {
      auto stats = cold.counters().fileStats(file.name);
      reads.dataBytes += file.size;
      reads.fetched += stats.fetchedBytes;
      reads.wholeReads += stats.wholeReads;
    }
    return reads;
  }
};

}  // namespace

TEST_F(ColdWriterTest, pureAddsReadNoOldSegment) {
  auto ram = writeSegments({randomIds(0, 50), randomIds(1, 50), randomIds(2, 50)});
  auto old = test::readDurableIndexInfo(*ram);
  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
  addDocs(iw, std::vector<std::string>(20));      // documents without ids
  addDocs(iw, randomIds(9, 20), false);           // ids without overwrite
  for (const auto& segment : old->segments) {
    auto reads = readsOf(cold, segment);
    EXPECT_EQ(0u, reads.fetched) << segment.seg_id;
  }
  EXPECT_EQ(190, (int)snapshots.readers.getReader()->liveDocs());
}

TEST_F(ColdWriterTest, upsertReadsOnlyTheLookupsRanges) {
  constexpr int SEGS = 6, DOCS = 5000, HOLDER = 2;
  std::vector<std::vector<std::string>> batches;
  for (int seg = 0; seg < SEGS; seg++) batches.push_back(randomIds(seg, DOCS));
  auto ram = writeSegments(batches);
  auto info = test::readDurableIndexInfo(*ram);
  ASSERT_EQ((size_t)SEGS, info->segments.size());

  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
  addDocs(iw, {randomId(HOLDER, 7)});

  for (int seg = 0; seg < SEGS; seg++) {
    SCOPED_TRACE(seg);
    const auto& segment = info->segments[seg];
    auto reads = readsOf(cold, segment);
    EXPECT_EQ(0u, reads.wholeReads);
    // Every segment's catalog (its exact id bounds) and filter is read; of the
    // dictionaries only the holder's routing and the block the id routes to,
    // never the whole dictionary (about 100 KB here).
    uint64_t filterBytes = IdFilter::blocksFor(DOCS) * IdFilter::BLOCK_BYTES;
    uint64_t pages = (seg == HOLDER ? 6 : 2) * 4096;
    EXPECT_LE(reads.fetched, segment.catalog.length + filterBytes + pages);
  }
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(SEGS * DOCS, (int)reader->liveDocs());
  EXPECT_EQ(SEGS * DOCS + 1, (int)reader->maxDoc());
}

TEST_F(ColdWriterTest, probedDeletesFindEveryHeldId) {
  constexpr int SEGS = 4, DOCS = 300;
  std::vector<std::vector<std::string>> batches;
  for (int seg = 0; seg < SEGS; seg++) batches.push_back(randomIds(seg, DOCS));
  auto ram = writeSegments(batches);
  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
  // Every third id of every segment is overwritten, plus ids no segment holds.
  std::vector<std::string> ids;
  for (int seg = 0; seg < SEGS; seg++) {
    for (int i = 0; i < DOCS; i += 3) ids.push_back(randomId(seg, i));
  }
  for (int i = 0; i < 50; i++) ids.push_back(randomId(99, i));
  addDocs(iw, ids);
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(SEGS * DOCS + 50, (int)reader->liveDocs());
}

// An id indexed several times in one segment has postings outside its block
// (deleted copies included), fetched in the probe's last round.
TEST_F(ColdWriterTest, idsWithSeveralPostingsAreReplaced) {
  std::vector<std::string> ids;
  for (int copy = 0; copy < 3; copy++) {
    for (auto& id : randomIds(0, 200)) ids.push_back(id);
  }
  auto ram = writeSegments({ids}, false);
  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
  auto before = snapshots.readers.getReader();
  ASSERT_EQ(600, (int)before->maxDoc());
  int liveBefore = (int)before->liveDocs();
  addDocs(iw, randomIds(0, 100));
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(700, (int)reader->maxDoc());
  // Every live copy of the 100 overwritten ids is gone.
  EXPECT_EQ(liveBefore - liveBefore / 2 + 100, (int)reader->liveDocs());
}

// Ids longer than a term may be are stored in hashed form; their lookups
// route the same way.
TEST_F(ColdWriterTest, longIdsAreReplaced) {
  std::vector<std::string> ids;
  for (int i = 0; i < 300; i++) ids.push_back(std::string(300, 'a' + (char)(i % 26)) + std::to_string(i));
  auto ram = writeSegments({ids});
  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
  addDocs(iw, {ids[5], ids[150], ids[299], std::string(400, 'z')});
  EXPECT_EQ(300 + 1, (int)snapshots.readers.getReader()->liveDocs());
}

// A filter larger than the probe fetches whole is read at the ids' blocks.
TEST_F(ColdWriterTest, largeFiltersAreProbedByBlock) {
  constexpr int DOCS = 80000;
  auto ram = writeSegments({randomIds(0, DOCS)});
  auto info = test::readDurableIndexInfo(*ram);
  ASSERT_EQ(1u, info->segments.size());
  ASSERT_GT(IdFilter::blocksFor(DOCS) * IdFilter::BLOCK_BYTES, 64u * 1024);
  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots, {}, nullptr, NO_MERGES);
  addDocs(iw, {randomId(0, 11), randomId(0, 70000), randomId(5, 1), randomId(5, 2)});
  auto reads = readsOf(cold, info->segments[0]);
  EXPECT_EQ(0u, reads.wholeReads);
  EXPECT_LT(reads.fetched * 8, reads.dataBytes);
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(DOCS + 2, (int)reader->liveDocs());
}
