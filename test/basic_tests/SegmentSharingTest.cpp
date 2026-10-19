// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// A segment's PostingsReader (its mapped files and the views cached on it) is
// shared by every snapshot that holds the segment and by the writer. These
// tests reopen file-backed indexes, where releasing the last holder unmaps
// the files, so a view that outlived its mapping faults.

#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"

namespace luxir::test {

class SegmentSharingTest : public LuxirTest {
protected:
  std::filesystem::path path;
  std::unique_ptr<LuxirNode> node;
  std::unique_ptr<CollectionHelper> helper;
  std::shared_ptr<IndexWriter> writer;
  bool savedOverlays = TestOverlayAuxReader::enabledForTests;
  int32_t nextId = 0;

  void SetUp() override {
    TestOverlayAuxReader::enabledForTests = true;
    path = std::filesystem::temp_directory_path() / ("luxir-sharing-" + newUuid());
    LuxirConfig config;
    config.store.backend = "fs";
    config.store.data_dir = path.string();
    node = std::make_unique<LuxirNode>(config);
    helper = std::make_unique<CollectionHelper>(*node, "share");
    writer = helper->getIndexWriter();
    writer->mergePolicy->setMergeFactor(100);
  }

  void TearDown() override {
    writer.reset();
    helper.reset();
    node.reset();
    std::filesystem::remove_all(path);
    TestOverlayAuxReader::enabledForTests = savedOverlays;
  }

  // Commits one segment of `docs` new docs: id "<n>" with n_i = n.
  void addSegment(int32_t docs) {
    std::vector<Doc> batch;
    for (int32_t i = 0; i < docs; i++, nextId++) {
      batch.push_back(flatdoc("id", std::to_string(nextId), "n_i", (int64_t)nextId));
    }
    ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT).success);
  }

  std::shared_ptr<IndexReader> current() { return writer->snapshots.readers.getReader(); }

  static std::vector<PostingsReader*> postings(IndexReader& reader) {
    std::vector<PostingsReader*> out;
    for (auto& segment : reader.segments()) out.push_back(&segment.postingsReader());
    return out;
  }

  // Reads every live doc of `reader` through each segment's mapped terms,
  // postings and column, and its cached leaf metadata: each id term's live
  // doc has n_i equal to the id, within its leaf's bounds. Returns the live
  // docs seen.
  static int64_t readAll(IndexReader& reader) {
    int64_t live = 0;
    for (auto& segment : reader.segments()) {
      auto& postingsReader = segment.postingsReader();
      MemPool pool;
      const PostingsReader::Field* idFields = nullptr;
      const PostingsReader::Field* valueFields = nullptr;
      SegFieldInfo idInfo;
      SegFieldInfo valueInfo;
      EXPECT_TRUE(((idFields = postingsReader.fieldInfo("id")) != nullptr));
      EXPECT_TRUE(((valueFields = postingsReader.fieldInfo("n_i")) != nullptr));
      idInfo = *idFields;
      valueInfo = *valueFields;
      IntColReader column(postingsReader, valueInfo);
      IntColReader::SparseIterator values(column);
      const auto* leaves = postingsReader.fieldInfo("n_i")->numericLeafMeta();
      EXPECT_NE(nullptr, leaves);
      TermsEnum terms(pool, postingsReader, idInfo);
      while (terms.nextTerm()) {
        auto term = (std::string_view)terms.term();
        int64_t id = -1;
        std::from_chars(term.data(), term.data() + term.size(), id);
        DocsOnlyEnum docs(terms);
        for (int32_t doc = docs.nextDoc(); doc != PostingsReader::END; doc = docs.nextDoc()) {
          if (segment.liveDocs() && !segment.liveDocs()->bitset().get(doc)) continue;
          live++;
          EXPECT_EQ(doc, values.advance(doc));
          EXPECT_EQ(id, values.value()) << term;
          if (leaves == nullptr) continue;
          auto zone = leaves->leafZone(doc / NumColumnFormat::LEAF_ZONE_SIZE);
          EXPECT_LE(zone.min, id);
          EXPECT_GE(zone.max, id);
        }
      }
    }
    EXPECT_EQ(reader.liveDocs(), live);
    return live;
  }

  // The writer's open reader of each segment of the published snapshot, as it
  // handed them over (CommitSnapshot::openReaders), by manifest order.
  std::vector<PostingsReader*> writerSlots() {
    std::vector<PostingsReader*> out;
    for (const auto& open : writer->snapshots.snapshot()->openReaders) out.push_back(open.lock().get());
    return out;
  }
};

TEST_F(SegmentSharingTest, fieldNamesFollowSegmentSet) {
  addSegment(3);
  auto before = current();
  auto field = before->field("n_i");
  auto missing = before->field("missing_i");
  auto* info = field.at(0);
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(IndexReader::FieldSegments{}, missing);
  EXPECT_FALSE(missing.exists());
  EXPECT_EQ(nullptr, missing.at(0));
  EXPECT_EQ(3, field.stats().docsWithField);

  auto same = std::make_shared<IndexReader>(writer->dir, before.get());
  EXPECT_EQ(field, same->field("n_i"));
  EXPECT_EQ(missing, same->field("missing_i"));
  EXPECT_EQ(&field.stats(), &same->field("n_i").stats());

  ASSERT_TRUE(helper->deleteById("0", UpdateMessage::COMMIT).success);
  auto deleted = current();
  auto deletedField = deleted->field("n_i");
  EXPECT_EQ(&field.stats(), &deletedField.stats());
  EXPECT_EQ(3, deletedField.stats().docsWithField);
  EXPECT_EQ(2, deleted->liveDocs());

  // The same segment set can be opened through another publisher's reader.
  auto independent = PostingsReader::create(writer->dir, before->segments()[0].segInfo.seg_id);
  auto snapshot = *writer->snapshots.snapshot();
  snapshot.openReaders[0] = independent;
  auto separate = std::make_shared<IndexReader>(writer->dir, snapshot, before.get());
  auto separateField = separate->field("n_i");
  EXPECT_EQ(independent, separate->segments()[0].postingsReaderShared());
  EXPECT_EQ(&field.stats(), &separateField.stats());
  EXPECT_NE(info, separateField.at(0));
  EXPECT_EQ(independent->fieldInfo("n_i"), separateField.at(0));

  addSegment(2);
  auto after = current();
  ASSERT_EQ(2u, after->segments().size());
  EXPECT_EQ(before->segments()[0].postingsReaderShared(),
            after->segments()[0].postingsReaderShared());
  auto reopenedField = after->field("n_i");
  EXPECT_NE(field, reopenedField);
  EXPECT_EQ(info, reopenedField.at(0));
  EXPECT_EQ(5, reopenedField.stats().docsWithField);
  EXPECT_NE(&field.stats(), &reopenedField.stats());
  EXPECT_FALSE(after->field("missing_i").exists());
}

// A delete-only commit changes one segment's live docs: every PostingsReader
// and every other segment's live docs carry over. The writer applied the
// deletes through the published reader's PostingsReaders.
TEST_F(SegmentSharingTest, deleteOnlyCommitSharesEverySegment) {
  for (int32_t docs : {1100, 700, 600}) addSegment(docs);
  ASSERT_TRUE(helper->deleteByIds({"5", "1500"}, UpdateMessage::COMMIT).success);
  auto before = current();
  ASSERT_EQ(3u, before->segments().size());
  EXPECT_EQ(2398, readAll(*before));
  auto beforeLive = before->segments()[2].liveDocsShared();
  ASSERT_EQ(nullptr, beforeLive);

  ASSERT_TRUE(helper->deleteById("2000", UpdateMessage::COMMIT).success);
  auto after = current();
  EXPECT_EQ(before->coreGen(), after->coreGen());
  EXPECT_EQ(postings(*before), postings(*after));
  EXPECT_EQ(postings(*after), writerSlots());
  EXPECT_EQ(before->segments()[0].liveDocs(), after->segments()[0].liveDocs());
  EXPECT_EQ(before->segments()[1].liveDocs(), after->segments()[1].liveDocs());
  ASSERT_NE(nullptr, after->segments()[2].liveDocs());

  EXPECT_EQ(2398, readAll(*before));  // the old snapshot still serves
  std::weak_ptr<IndexReader> retired = before;
  before.reset();
  EXPECT_TRUE(retired.expired());
  EXPECT_EQ(2397, readAll(*after));
}

// An overlay is an aux reader beside the base files, not part of them: an
// overlay-only commit shares every PostingsReader and the live docs.
TEST_F(SegmentSharingTest, overlayOnlyCommitSharesEverySegment) {
  for (int32_t docs : {600, 500}) addSegment(docs);
  ASSERT_TRUE(helper->deleteById("7", UpdateMessage::COMMIT).success);
  auto before = current();
  EXPECT_EQ(1099, readAll(*before));

  helper->commit({std::string(TestOverlayAuxReader::NAME)});
  ASSERT_FALSE(readDurableIndexInfo(writer->dir)->segments[0].overlays.empty());
  auto after = current();
  ASSERT_NE(before, after);
  EXPECT_EQ(postings(*before), postings(*after));
  for (size_t i = 0; i < after->segments().size(); i++) {
    EXPECT_EQ(before->segments()[i].liveDocs(), after->segments()[i].liveDocs());
    EXPECT_TRUE(before->segments()[i].auxReaders().empty());
    EXPECT_FALSE(after->segments()[i].auxReaders().empty());
  }
  before.reset();
  EXPECT_EQ(1099, readAll(*after));
}

// A flush adds a segment no reader has: the old ones carry over, the new one
// is opened. A segment the writer opened before any reader saw it (here for
// a later commit's deletes) is shared with the reader that opens it.
TEST_F(SegmentSharingTest, newSegmentsShareTheWritersReaders) {
  addSegment(800);
  auto before = current();
  EXPECT_EQ(800, readAll(*before));

  addSegment(300);
  auto flushed = current();
  ASSERT_EQ(2u, flushed->segments().size());
  EXPECT_EQ(&before->segments()[0].postingsReader(), &flushed->segments()[0].postingsReader());
  EXPECT_EQ(1100, readAll(*flushed));

  // Two commits without a reopen: the deletes open segment 3 in the writer.
  addSegment(400);
  ASSERT_TRUE(helper->deleteById("1200", UpdateMessage::COMMIT).success);
  auto slots = writerSlots();
  ASSERT_EQ(3u, slots.size());
  ASSERT_NE(nullptr, slots[2]);
  auto after = current();
  ASSERT_EQ(3u, after->segments().size());
  EXPECT_EQ(&flushed->segments()[0].postingsReader(), &after->segments()[0].postingsReader());
  EXPECT_EQ(&flushed->segments()[1].postingsReader(), &after->segments()[1].postingsReader());
  EXPECT_EQ(slots, postings(*after));

  EXPECT_EQ(800, readAll(*before));
  before.reset();
  flushed.reset();
  EXPECT_EQ(1499, readAll(*after));
}

// A merge replaces its sources with a new segment: the reader opens it, and
// the sources' readers are released once neither a snapshot nor the writer
// holds them.
TEST_F(SegmentSharingTest, mergeReleasesItsSources) {
  for (int32_t docs : {500, 600, 700}) addSegment(docs);
  ASSERT_TRUE(helper->deleteById("3", UpdateMessage::COMMIT).success);
  auto before = current();
  EXPECT_EQ(1799, readAll(*before));
  std::vector<std::weak_ptr<PostingsReader>> sources;
  for (auto& segment : before->segments()) sources.push_back(segment.postingsReaderShared());

  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper->submit(merge).success);
  auto after = current();
  ASSERT_EQ(1u, after->segments().size());
  for (auto& source : sources) EXPECT_NE(source.lock().get(), &after->segments()[0].postingsReader());
  EXPECT_EQ(1799, readAll(*after));
  EXPECT_EQ(1799, readAll(*before));

  before.reset();
  // A follow-up commit retires the sources the merge commit still published.
  ASSERT_TRUE(helper->deleteById("4", UpdateMessage::COMMIT).success);
  auto latest = current();
  EXPECT_EQ(&after->segments()[0].postingsReader(), &latest->segments()[0].postingsReader());
  after.reset();
  for (auto& source : sources) EXPECT_TRUE(source.expired());
  EXPECT_EQ(1798, readAll(*latest));
}

// Nothing carries over between incarnations, even byte-identical files: the
// same root re-published under a new incarnation (as a recovered older root
// is) opens every segment afresh, and so does re-creating the index, where
// segment ids restart and rewrite the same file names.
TEST_F(SegmentSharingTest, newIncarnationSharesNothing) {
  addSegment(900);
  auto before = current();
  EXPECT_EQ(900, readAll(*before));

  auto root = readDurableIndexInfo(writer->dir);
  root.info.incarnation = api::build::arenaStr(*root.arena, CommitId::newIncarnation());
  root.info.index_gen++;
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(root.info, bytes));
  Manifest::write(writer->dir, root.info.index_gen, bytes);
  auto rerooted = std::make_shared<IndexReader>(writer->dir, before.get());
  ASSERT_EQ(root.info.incarnation, rerooted->incarnation());
  EXPECT_NE(&before->segments()[0].postingsReader(), &rerooted->segments()[0].postingsReader());
  EXPECT_EQ(900, readAll(*rerooted));
  rerooted.reset();
  writer->dir.deleteFile(Manifest::name(root.info.index_gen));

  helper->clear();
  nextId = 0;
  addSegment(900);
  auto snapshot = writer->snapshots.snapshot();
  ASSERT_NE(before->incarnation(), snapshot->id.incarnation);
  auto recreated = std::make_shared<IndexReader>(writer->dir, *snapshot, before.get());
  ASSERT_EQ(1u, recreated->segments().size());
  EXPECT_EQ(before->segments()[0].segInfo.seg_id, recreated->segments()[0].segInfo.seg_id);
  EXPECT_NE(&before->segments()[0].postingsReader(), &recreated->segments()[0].postingsReader());
  EXPECT_EQ(900, readAll(*before));
  before.reset();
  EXPECT_EQ(900, readAll(*recreated));
}

// Searches keep their snapshots while commits, merges and independent reopens
// share and release segments underneath them.
TEST_F(SegmentSharingTest, concurrentReopensAndQueries) {
  for (int32_t docs : {700, 600}) addSegment(docs);
  std::atomic<bool> stop = false;
  std::atomic<int64_t> reads = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; t++) {
    threads.emplace_back([&] {
      while (!stop.load()) {
        auto reader = current();
        readAll(*reader);
        reads++;
      }
    });
  }
  for (int t = 0; t < 2; t++) {
    // A reopen chain of its own, sharing from the reader it last opened.
    threads.emplace_back([&] {
      std::shared_ptr<IndexReader> previous;
      while (!stop.load()) {
        auto snapshot = writer->snapshots.snapshot();
        std::shared_ptr<IndexReader> next;
        try {
          next = std::make_shared<IndexReader>(writer->dir, *snapshot, previous.get());
        } catch (const std::exception& e) {
          // A newer commit retired a file of this unpinned snapshot first.
          EXPECT_NE(snapshot, writer->snapshots.snapshot()) << e.what();
          continue;
        }
        readAll(*next);
        previous = std::move(next);
        reads++;
      }
    });
  }
  threads.emplace_back([&] {
    while (!stop.load()) {
      auto req = localReq(node->getSearchEngine());
      req->collection("share");
      auto& cur = req->topDocs("q").limit(3);
      cur.allQuery();
      qb::sort(cur, "n_i", qb::DESC);
      req->execute(false);
      EXPECT_TRUE(req->ok()) << req->errorMsg();
      reads++;
    }
  });
  for (int round = 0; round < 12; round++) {
    addSegment(300 + 50 * round);
    ASSERT_TRUE(helper->deleteById(std::to_string(round * 97), UpdateMessage::COMMIT).success);
    if (round % 4 == 3) {
      CollectionHelper::UpdateBuilder merge;
      merge.commit(true, 2);
      ASSERT_TRUE(helper->submit(merge).success);
    }
  }
  while (reads.load() < 50) std::this_thread::yield();
  stop = true;
  for (auto& thread : threads) thread.join();
  readAll(*current());
}

}  // namespace luxir::test
