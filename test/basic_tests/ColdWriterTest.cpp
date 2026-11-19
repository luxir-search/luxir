// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// A writer over a collection whose segments sit on a remote store (cold):
// writes start without fetching old segment data.

#include <gtest/gtest.h>

#include <format>
#include <set>
#include <string>

#include "luxir/index/IdFilter.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/SegmentSummary.h"
#include "luxir/index/SummaryInfo.h"
#include "luxir/store/Directory.h"
#include "luxir/util/thread.h"
#include "test/ColdDirectory.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/StorageTestUtils.h"

using namespace luxir;

namespace {

class ColdWriterTest : public LuxirTest {
public:
  // One flushed, committed segment of these ids; an empty id indexes none.
  // Each batch gets a later update version, so an overwrite outranks the
  // documents it replaces.
  uint64_t nextVersion = 1;

  void addDocs(IndexWriter& iw, const std::vector<std::string>& ids, bool overwrite = true) {
    auto& inv = iw.obtainInverter(nextVersion++);
    inv.overwrite = overwrite;
    for (size_t i = 0; i < ids.size(); i++) {
      inv.startDoc();
      if (!ids[i].empty()) inv.getIndexHandler("id").index(inv, ids[i]);
      inv.getIndexHandler("text_w").index(inv, std::string("some words ") + (i % 2 ? "odd" : "even"));
      inv.finishDoc();
    }
    iw.releaseInverter(inv, true);
    iw.commit();
  }

  void addSegment(IndexWriter& iw, std::string_view idPrefix, int docs, bool overwrite = true) {
    std::vector<std::string> ids;
    for (int i = 0; i < docs; i++) ids.push_back(idPrefix.empty() ? std::string() : std::string(idPrefix) + std::to_string(i));
    addDocs(iw, ids, overwrite);
  }

  // Ids spread over the whole key space, as random ids are.
  static std::string randomId(int seg, int i) { return std::format("{:016x}", IdFilter::hash(std::format("{}/{}", seg, i))); }

  static std::set<std::string> filesOf(const api::SegmentInfo& seg) {
    std::set<std::string> names;
    for (const auto& file : seg.files) names.insert(std::string(file.name));
    for (const auto& overlay : seg.overlays) {
      for (const auto& file : overlay.files) names.insert(std::string(file.name));
    }
    return names;
  }

  // A commit that waits for running merges.
  static void commitAfterMerges(IndexWriter& iw) {
    struct Msg final : UpdateMessage {
      Blocker blocker;
      Msg() {
        commit = COMMIT;
        waitForMerges = true;
      }
      void handle(IndexWriter&) override {}
      void done(IndexWriter&) override { blocker.notify(); }
    } msg;
    ASSERT_TRUE(iw.submitUpdate(&msg));
    msg.blocker.wait();
  }

  // Every file of the manifest's segments (data and overlays).
  static std::set<std::string> segmentFiles(Directory& dir) {
    std::set<std::string> names;
    auto info = test::readDurableIndexInfo(dir);
    for (const auto& seg : info->segments) names.merge(filesOf(seg));
    return names;
  }

  // The collection summary recomputed from every segment's sidecar.
  static CollectionSummary fullSummary(Directory& dir) {
    std::vector<std::shared_ptr<const SegmentSummary>> owned;
    std::vector<const SegmentSummary*> summaries;
    auto info = test::readDurableIndexInfo(dir);
    for (const auto& seg : info->segments) {
      std::shared_ptr<const SegmentSummary> summary;
      for (const auto& overlay : seg.overlays) {
        if (overlay.kind == SegmentSummary::OVERLAY_KIND) summary = SegmentSummary::open(dir, overlay.files[0].name);
      }
      owned.push_back(summary);
      summaries.push_back(summary.get());
    }
    return CollectionSummary::aggregate(summaries);
  }

  // The stored summary counts what a full recompute counts, and its bounds
  // contain the recomputed ones.
  static void expectSummaryCovers(Directory& dir) {
    auto info = test::readDurableIndexInfo(dir);
    auto stored = CollectionSummary::fromManifest(info->field_summaries, info->summary_complete);
    auto full = fullSummary(dir);
    ASSERT_TRUE(full.complete);
    ASSERT_TRUE(stored.complete);
    ASSERT_EQ(full.fields.size(), stored.fields.size());
    for (size_t i = 0; i < full.fields.size(); i++) {
      const auto& f = full.fields[i];
      const auto& s = stored.fields[i];
      SCOPED_TRACE(f.fieldname);
      EXPECT_EQ(f.fieldname, s.fieldname);
      EXPECT_EQ(f.segmentsWithField, s.segmentsWithField);
      EXPECT_EQ(f.docsWithField, s.docsWithField);
      EXPECT_EQ(f.nTerms, s.nTerms);
      EXPECT_EQ(f.sumDocFreq, s.sumDocFreq);
      if (s.summaryFlags & SegmentSummary::HAS_TERM_BOUNDS) {
        ASSERT_TRUE(f.summaryFlags & SegmentSummary::HAS_TERM_BOUNDS);
        EXPECT_LE(s.termMin, f.termMin);
        if (!(s.summaryFlags & SegmentSummary::TERM_MAX_UNBOUNDED)) {
          EXPECT_GE(s.termMax, f.termMax);
        }
      }
    }
  }
};

}  // namespace

TEST_F(ColdWriterTest, pureAddsReadNoOldSegment) {
  auto ram = std::make_shared<RAMDir>();
  {
    CommitSnapshotRegistry snapshots(test::reserveIncarnation(*ram));
    IndexWriter iw(snapshots);
    iw.setMergeFactor(1000);
    for (int seg = 0; seg < 4; seg++) addSegment(iw, "old" + std::to_string(seg) + "-", 50);
  }
  auto old = segmentFiles(*ram);
  ASSERT_FALSE(old.empty());

  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots);
  iw.setMergeFactor(1000);
  addSegment(iw, "", 20);                       // documents without ids
  addSegment(iw, "new-", 20, false);            // allow_dups: ids, no overwrite
  EXPECT_EQ(2u, iw.collectionSummaryUpdates.load());

  for (const auto& name : old) {
    SCOPED_TRACE(name);
    EXPECT_EQ(0u, cold.statsOf(name).fetchedBytes);
  }
  expectSummaryCovers(*ram);
}

TEST_F(ColdWriterTest, incrementalSummaryFollowsMergesAndDeletes) {
  auto ram = std::make_shared<RAMDir>();
  for (int round = 0; round < 2; round++) {
    // The second round starts from the summary stored in the manifest.
    CommitSnapshotRegistry snapshots(round == 0 ? test::reserveIncarnation(*ram) : *ram);
    IndexWriter iw(snapshots);
    iw.setMergeFactor(2);
    for (int seg = 0; seg < 5; seg++) {
      addSegment(iw, "r" + std::to_string(round) + "s" + std::to_string(seg) + "-", 10 + seg);
      commitAfterMerges(iw);
      expectSummaryCovers(*ram);
    }
    // Overwrite every document of one earlier segment: it empties and retires.
    addSegment(iw, "r" + std::to_string(round) + "s0-", 10);
    commitAfterMerges(iw);
    expectSummaryCovers(*ram);
    EXPECT_GT(iw.collectionSummaryUpdates.load(), 0u);
  }
}

TEST_F(ColdWriterTest, upsertReadsOnlyTheSegmentHoldingTheId) {
  auto ram = std::make_shared<RAMDir>();
  constexpr int SEGS = 6, DOCS = 5000;
  {
    CommitSnapshotRegistry snapshots(test::reserveIncarnation(*ram));
    IndexWriter iw(snapshots);
    iw.setMergeFactor(1000);
    for (int seg = 0; seg < SEGS; seg++) {
      std::vector<std::string> ids;
      for (int i = 0; i < DOCS; i++) ids.push_back(randomId(seg, i));
      addDocs(iw, ids);
    }
  }
  auto info = test::readDurableIndexInfo(*ram);
  ASSERT_EQ((size_t)SEGS, info->segments.size());

  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots);
  iw.setMergeFactor(1000);
  constexpr int HOLDER = 2;
  addDocs(iw, {randomId(HOLDER, 7)});

  for (int seg = 0; seg < SEGS; seg++) {
    const auto& segment = info->segments[seg];
    uint64_t dataBytes = 0, fetched = 0;
    for (const auto& file : segment.files) {
      std::string name(file.name);
      SCOPED_TRACE(name);
      auto stats = cold.statsOf(name);
      // Segments that do not hold the id are ruled out by their filter; the
      // one that does is probed by range.
      if (seg != HOLDER) {
        EXPECT_EQ(0u, stats.fetchedBytes);
      }
      EXPECT_EQ(0u, stats.wholeReads);
      dataBytes += file.size;
      fetched += stats.fetchedBytes;
    }
    if (seg == HOLDER) {
      EXPECT_LT(fetched * 4, dataBytes);
    }
  }
  auto reader = snapshots.readers.getReader();
  EXPECT_EQ(SEGS * DOCS, (int)reader->liveDocs());
  EXPECT_EQ(SEGS * DOCS + 1, (int)reader->maxDoc());
}

TEST_F(ColdWriterTest, filteredDeletesFindEveryHeldId) {
  auto ram = std::make_shared<RAMDir>();
  constexpr int SEGS = 4, DOCS = 300;
  {
    CommitSnapshotRegistry snapshots(test::reserveIncarnation(*ram));
    IndexWriter iw(snapshots);
    iw.setMergeFactor(1000);
    for (int seg = 0; seg < SEGS; seg++) {
      std::vector<std::string> ids;
      for (int i = 0; i < DOCS; i++) ids.push_back(randomId(seg, i));
      addDocs(iw, ids);
    }
  }
  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots);
  iw.setMergeFactor(1000);
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

TEST_F(ColdWriterTest, segmentsWithoutFiltersAreProbedByRange) {
  auto ram = std::make_shared<RAMDir>();
  constexpr int SEGS = 3, DOCS = 5000;
  {
    // Segments written before id filters existed.
    struct NoFilters {
      NoFilters() { IndexWriter::writeIdFiltersForTests = false; }
      ~NoFilters() { IndexWriter::writeIdFiltersForTests = true; }
    } noFilters;
    CommitSnapshotRegistry snapshots(test::reserveIncarnation(*ram));
    IndexWriter iw(snapshots);
    iw.setMergeFactor(1000);
    for (int seg = 0; seg < SEGS; seg++) {
      std::vector<std::string> ids;
      for (int i = 0; i < DOCS; i++) ids.push_back(randomId(seg, i));
      addDocs(iw, ids);
    }
  }
  auto info = test::readDurableIndexInfo(*ram);

  test::ColdDirectory cold(ram);
  CommitSnapshotRegistry snapshots(cold);
  IndexWriter iw(snapshots);
  iw.setMergeFactor(1000);
  addDocs(iw, {randomId(0, 1), randomId(2, 2), randomId(7, 7)});

  for (const auto& segment : info->segments) {
    uint64_t dataBytes = 0, fetched = 0;
    for (const auto& file : segment.files) {
      auto stats = cold.statsOf(file.name);
      EXPECT_EQ(0u, stats.wholeReads) << file.name;
      dataBytes += file.size;
      fetched += stats.fetchedBytes;
    }
    EXPECT_LT(fetched * 4, dataBytes);
  }
  EXPECT_EQ(SEGS * DOCS + 1, (int)snapshots.readers.getReader()->liveDocs());
}
