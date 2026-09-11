// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <array>
#include <iostream>
#include <limits>
#include <filesystem>
#include <thread>
#include <random>
#include <atomic>
#include <condition_variable>
#include <map>
#include <set>
#include <mutex>
#include <memory>
#include <latch>

#include <oneapi/tbb/flow_graph.h>
#include <luxir/index/Inverter.h>
#include <luxir/server/ProtoUpdateMessage.h>

#include "luxir/index/IndexWriter.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/IndexReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/Signal.h"
#include <future>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

#define TEST_DEBUG LOG_TRACE
// #define TEST_DEBUG LOG_DEBUG

using namespace std;
using namespace luxir;

class IndexWriterTest : public LuxirTest {
public:
  std::string field = "text_w";

  void addDoc(IndexWriter& iw) {
    auto* inverter = &iw.obtainInverter();
    auto* fieldHandler = &inverter->getIndexHandler(field);
    std::string doc1 = "test";
    inverter->startDoc();
    fieldHandler->index(*inverter, doc1);
    inverter->finishDoc();
    iw.releaseInverter(*inverter);
  }
};

TEST_F(IndexWriterTest, closeIsIdempotentAndRejectsNewEntryPoints) {
  class NoopUpdate final : public UpdateMessage {
  public:
    Blocker blocker;
    bool handled = false;
    void handle(IndexWriter& iw) override { unused(iw); handled = true; }
    void done(IndexWriter& iw) override { unused(iw); blocker.notify(); }
  };

  RAMDir dir;
  IndexWriter writer(dir);
  auto heldReader = writer.getIndexReader();
  writer.close();
  EXPECT_NO_THROW(writer.close());

  // The graph admits the message and rejects it at the entry node, so it completes
  // with an error without ever being handled.
  NoopUpdate update;
  EXPECT_TRUE(writer.submitUpdate(&update));
  update.blocker.wait();
  EXPECT_TRUE(update.result.errored());
  EXPECT_FALSE(update.handled);

  EXPECT_THROW(writer.getIndexReader(), IndexWriterClosedError);
  EXPECT_NE(nullptr, heldReader);
}

namespace {

class TimedCommitMessage final : public UpdateMessage {
  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;

public:
  TimedCommitMessage() {
    commit = COMMIT;
  }

  void handle(IndexWriter& iw) override {
    unused(iw);
  }

  void done(IndexWriter& iw) override {
    unused(iw);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      completed = true;
    }
    condition.notify_one();
  }

  bool waitFor(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, timeout, [this]() { return completed; });
  }
};

bool waitForMergesCommit(IndexWriter& iw) {
  class BlockingWaitForMergesCommit final : public UpdateMessage {
  public:
    Blocker blocker;
    bool success = false;

    BlockingWaitForMergesCommit() {
      commit = COMMIT;
      waitForMerges = true;
    }

    void handle(IndexWriter& iw) override {
      unused(iw);
    }

    void done(IndexWriter& iw) override {
      unused(iw);
      success = !result.errored();
      blocker.notify();
    }
  };

  BlockingWaitForMergesCommit msg;
  bool submitted = iw.submitUpdate(&msg);
  assert(submitted);
  unused(submitted);
  msg.blocker.wait();
  return msg.success;
}

uint32_t randomForceMergeTarget(Rng& r) {
  if (r.rint(100) >= 12) return 0;
  return (uint32_t)r.rint(3) + 1;
}

bool segmentPrefixAbsent(Directory& dir, uint64_t segId) {
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  auto prefix = Postings::getIndexFileNamePrefix(segId);
  for (const auto& file : files) {
    if (file.name.starts_with(prefix)) return false;
  }
  return true;
}

} // namespace


TEST_F(IndexWriterTest, mergeFactorComesFromNodeConfig) {
  EXPECT_EQ(LuxirConfig{}.index.merge_factor,
            IndexWriter::MergePolicy::DEFAULT_MERGE_FACTOR);

  LuxirConfig config;
  CLI::App app;
  config.addOptions(app);
  app.parse("--indexing.merge-factor 1000");
  ASSERT_EQ(config.index.merge_factor, 1000);

  LuxirNode node(config);
  luxir::test::CollectionHelper main(node);
  EXPECT_EQ(main.getIndexWriter()->mergePolicy->mergeFactor, 1000);
  EXPECT_EQ(node.getOrCreateCollection("other")->getShard()->getIndexWriter()->mergePolicy->mergeFactor,
            1000);

  LuxirConfig invalidConfig;
  CLI::App invalidApp;
  invalidConfig.addOptions(invalidApp);
  EXPECT_THROW(invalidApp.parse("--indexing.merge-factor 1"), CLI::ValidationError);
}


// The per-inverter RAM cap is clamped below the MemPool 4 GiB addressability
// ceiling, with headroom for one batch's overshoot past the cap.
TEST_F(IndexWriterTest, inverterRamCapClampedToPoolAddressability) {
  LuxirConfig config;
  CLI::App app;
  config.addOptions(app);
  app.parse("--indexing.max-inverter-ram-mb 8192");
  config.normalize();
  EXPECT_EQ(config.index.max_inverter_ram_mb, IndexConfig::MAX_INVERTER_RAM_CAP_MB);
  // Clamped cap plus a full 256 KiB-block overshoot still fits addressability.
  EXPECT_LE((uint64_t)config.index.max_inverter_ram_mb * 1024 * 1024,
            (uint64_t)MemPool::MAX_BUFFERS * MemPool::BYTE_BLOCK_SIZE - 256 * 1024 * 1024);
}


// The node-wide RAM budget derives from system RAM, and the indexing cap from
// the node budget, unless either is set explicitly.
TEST_F(IndexWriterTest, ramBudgetsDeriveFromNodeBudget) {
  ASSERT_GT(systemRamBytes(), 0);
  LuxirConfig autoConfig;
  autoConfig.normalize();
  EXPECT_EQ(autoConfig.max_ram_mb, systemRamBytes() / (1024 * 1024) / 4);
  EXPECT_EQ(autoConfig.index.max_ram_mb, autoConfig.max_ram_mb / 2);

  auto parsed = [](const std::string& args) {
    LuxirConfig config;
    CLI::App app;
    config.addOptions(app);
    app.parse(args);
    config.normalize();
    return config;
  };

  // The node budget flows to the indexing share; an explicit share overrides it.
  EXPECT_EQ(parsed("--max-ram-mb 16384").index.max_ram_mb, 8192);
  EXPECT_EQ(parsed("--max-ram-mb 16384 --indexing.max-ram-mb 1024").index.max_ram_mb, 1024);
  // 0 is unlimited, and an unlimited node budget leaves indexing unlimited too.
  EXPECT_EQ(parsed("--max-ram-mb 0").index.max_ram_mb, 0);
  // A read-only node never indexes, so it carves out no indexing share.
  EXPECT_EQ(parsed("--read-only --store.backend fs --max-ram-mb 16384").index.max_ram_mb, 0);

  // One inverter may hold the whole indexing budget, but never more than the
  // pool can address - which is all an unlimited budget leaves to bound it.
  EXPECT_EQ(parsed("--max-ram-mb 4096").index.max_inverter_ram_mb, 2048);
  EXPECT_EQ(parsed("--max-ram-mb 0").index.max_inverter_ram_mb,
            IndexConfig::MAX_INVERTER_RAM_CAP_MB);
  EXPECT_EQ(parsed("--max-ram-mb 65536").index.max_inverter_ram_mb,
            IndexConfig::MAX_INVERTER_RAM_CAP_MB);
  EXPECT_EQ(parsed("--max-ram-mb 4096 --indexing.max-inverter-ram-mb 64").index.max_inverter_ram_mb, 64);

  // A node built from a config that never saw normalize() still gets budgets.
  LuxirNode node{LuxirConfig{}};
  EXPECT_EQ(node.getIndexRamBudget().totalBytes(), autoConfig.index.max_ram_mb * 1024 * 1024);
}


TEST_F(IndexWriterTest, firstCommitAfterReloadCompletes) {
  auto dir = std::make_unique<RAMDir>();
  {
    IndexWriter writer(*dir);
    writer.mergePolicy->setMergeFactor(2);

    // Models a later update that auto-flushed while an earlier commit was in
    // flight. Merge it first to ensure the merged segment preserves the source
    // version envelope used to derive the manifest high-water.
    auto flushAtVersion = [&](uint64_t version, std::string_view value) {
      auto& inverter = writer.obtainInverter(version);
      auto& fieldHandler = inverter.getIndexHandler(field);
      inverter.startDoc();
      fieldHandler.index(inverter, value);
      inverter.finishDoc();
      writer.releaseInverter(inverter, true);
      writer.updateGraph.wait_for_all();
    };
    flushAtVersion(9, "first higher version segment");
    flushAtVersion(10, "second higher version segment");
    writer.commit();
    auto info = luxir::test::readDurableIndexInfo(*dir);
    EXPECT_EQ(info->segments.size(), 1u);
    EXPECT_EQ(info->update_version, 10u);
  }

  auto writer = std::make_unique<IndexWriter>(*dir);
  auto msg = std::make_unique<TimedCommitMessage>();
  ASSERT_TRUE(writer->submitUpdate(msg.get()));

  if (!msg->waitFor(std::chrono::seconds(3))) {
    (void)writer.release();
    (void)msg.release();
    (void)dir.release();
    FAIL() << "first commit after reload did not complete";
  }
  writer->updateGraph.wait_for_all();
  EXPECT_FALSE(msg->result.errored());
  EXPECT_EQ(msg->updateVersion, 11u);
  EXPECT_EQ(luxir::test::readDurableIndexInfo(*dir)->update_version, 11u);
}


TEST_F(IndexWriterTest, failedCommitAdmissionDoesNotLeaveSequencerHole) {
  auto dir = std::make_unique<RAMDir>();
  auto writer = std::make_unique<IndexWriter>(*dir);
  auto failed = std::make_unique<TimedCommitMessage>();

  Signal::listen("initiateCommit", [](void*, void*, void*) -> void* {
    throw std::runtime_error("injected commit admission failure");
  });
  bool accepted;
  bool completed;
  size_t suppressed;
  {
    ExpectLog quiet("injected commit admission failure");
    accepted = writer->submitUpdate(failed.get());
    completed = accepted && failed->waitFor(std::chrono::seconds(3));
    suppressed = quiet.suppressed();
  }
  Signal::unlisten("initiateCommit");
  if (!completed) {
    (void)writer.release();
    (void)failed.release();
    (void)dir.release();
    FAIL() << "commit with injected admission failure did not complete";
  }
  ASSERT_TRUE(accepted);
  EXPECT_GT(suppressed, 0u);
  ASSERT_TRUE(failed->result.errored());

  auto subsequent = std::make_unique<TimedCommitMessage>();
  ASSERT_TRUE(writer->submitUpdate(subsequent.get()));
  if (!subsequent->waitFor(std::chrono::seconds(3))) {
    (void)writer.release();
    (void)failed.release();
    (void)subsequent.release();
    (void)dir.release();
    FAIL() << "commit after failed admission did not complete";
  }
  writer->updateGraph.wait_for_all();
  EXPECT_FALSE(subsequent->result.errored());
}


TEST_F(IndexWriterTest, simple) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto* inverter = &iw.obtainInverter();
  auto* fieldHandler = &inverter->getIndexHandler(field);

  std::string doc1 = "now is the time for all good men";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1);
  inverter->finishDoc();

  iw.releaseInverter(*inverter);
  iw.commit();

  IndexReader r1(dir);
  ASSERT_EQ(1, r1.segments().size());
  ASSERT_EQ(1, r1.maxDoc());

  inverter = &iw.obtainInverter();
  fieldHandler = &inverter->getIndexHandler(field);

  doc1 = "to come to the aid";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1);
  inverter->finishDoc();
  doc1 = "of their country";
  inverter->startDoc();
  fieldHandler->index(*inverter, doc1);
  inverter->finishDoc();

  iw.releaseInverter(*inverter);
  iw.commit();

  IndexReader r2(dir);
  ASSERT_EQ(2, r2.segments().size());
  ASSERT_EQ(3, r2.maxDoc());
  ASSERT_GT(r2.commitTime(), r1.commitTime());
}

// stats() is the one reader that runs concurrently with the commit/merge
// threads, so hammer it while both are active.  It must always observe a
// coherent snapshot: committed segments are a subset of writer-visible ones,
// and live docs never exceed max docs.
TEST_F(IndexWriterTest, statsConcurrentWithCommitsAndMerges) {
  RAMDir dir;
  IndexWriter iw(dir);
  std::atomic_bool stop = false;
  std::atomic_uint64_t samples = 0;

  std::thread reader([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto s = iw.stats(true);
      ASSERT_LE(s.committedSegments, s.segments);
      ASSERT_EQ(s.segments, s.segmentStats.size());
      ASSERT_LE(s.liveDocs, s.maxDocs);
      uint64_t committed = 0, maxDocs = 0, liveDocs = 0, segBytes = 0;
      for (const auto& seg : s.segmentStats) {
        if (seg.committed) committed++;
        ASSERT_LE(seg.liveDocs, seg.maxDoc);
        maxDocs += (uint64_t)seg.maxDoc;
        liveDocs += (uint64_t)seg.liveDocs;
        segBytes += seg.bytes;
      }
      // Per-segment records must add up to the rolled-up totals.
      ASSERT_EQ(committed, s.committedSegments);
      ASSERT_EQ(maxDocs, s.maxDocs);
      ASSERT_EQ(liveDocs, s.liveDocs);
      // Segment bytes and totalBytes come from the same listing; a segment
      // that disappeared between the segment snapshot and the listing
      // reports 0, so <= holds even mid-merge.
      ASSERT_LE(segBytes, s.totalBytes);
      samples++;
    }
  });

  for (int round = 0; round < 40; round++) {
    for (int i = 0; i < 5; i++) addDoc(iw);
    iw.commit();
    if (round % 8 == 7) iw.mergeSegments();
  }
  stop.store(true, std::memory_order_relaxed);
  reader.join();

  EXPECT_GT(samples.load(), 0u);
  auto final = iw.stats(true);
  EXPECT_EQ(200u, final.maxDocs);
  EXPECT_EQ(final.segments, final.committedSegments);
}

TEST_F(IndexWriterTest, statsBytes) {
  RAMDir dir;
  IndexWriter iw(dir);
  for (int i = 0; i < 3; i++) addDoc(iw);
  iw.commit();
  for (int i = 0; i < 2; i++) addDoc(iw);
  iw.commit();

  auto s = iw.stats(true);
  EXPECT_EQ(s.totalBytes, dir.totalBytes());
  uint64_t segBytes = 0;
  for (const auto& seg : s.segmentStats) {
    EXPECT_GT(seg.bytes, 0u);
    segBytes += seg.bytes;
  }
  // The manifest (and any other non-segment files) account for the gap.
  EXPECT_LT(segBytes, s.totalBytes);
}

// Test retrieving IndexReader from the IndexWriter
TEST_F(IndexWriterTest, getReader) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto reader = iw.getIndexReader();
  ASSERT_EQ(0, reader->maxDoc());
  ASSERT_EQ(0, reader->segments().size());  // could change depending on impl

  addDoc(iw);

  // no commit, so getIndexReader() should return the same reader
  auto reader2 = iw.getIndexReader();
  ASSERT_EQ(reader, reader2);

  // now make it visible.
  iw.commit();
  reader = iw.getIndexReader();
  ASSERT_EQ(1, reader->maxDoc());
  ASSERT_EQ(1, reader->segments().size());

  // now add another doc, commit, but get a reader with permissive freshness
  addDoc(iw);
  iw.commit();
  reader2 = iw.getIndexReader(10000000);  // can be up to 10 seconds old
  ASSERT_EQ(reader, reader2);  // not guaranteed, but should be the case

  // sleep current thread for a microsecond
  std::this_thread::sleep_for(std::chrono::microseconds(1));

  // now test a reader that is not fresh enough
  reader2 = iw.getIndexReader(1);  // can be up to 1 microseconds old! (0 is a special case, so we just chose smallest value we can)
  ASSERT_NE(reader, reader2);  // It's possible this could spuriously fail if the sleep wasn't long enough or the system clock is changed.
  ASSERT_EQ(2, reader2->maxDoc());
  ASSERT_EQ(2, reader2->segments().size());
}

// A request the current reader satisfies must not wait for a reopen in
// progress: from inside the reopen (lock held), a permissive-freshness
// acquisition on another thread returns the previous reader at once.
TEST_F(IndexWriterTest, acquisitionSatisfiedByCurrentReaderDoesNotWaitForReopen) {
  RAMDir dir;
  IndexWriter iw(dir);
  addDoc(iw);
  iw.commit();
  auto reader = iw.getIndexReader();
  addDoc(iw);
  iw.commit();

  std::future<std::shared_ptr<IndexReader>> concurrent;
  bool returnedDuringReopen = false;
  Signal::listen("indexReaderOpened", [&](void* source, void*, void*) -> void* {
    if (source != &iw) return nullptr;
    concurrent = std::async(std::launch::async, [&] { return iw.getIndexReader(10'000'000); });
    returnedDuringReopen = concurrent.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("indexReaderOpened"); });
  auto fresh = iw.getIndexReader();
  ASSERT_TRUE(concurrent.valid());
  EXPECT_TRUE(returnedDuringReopen);
  EXPECT_EQ(reader, concurrent.get());
  EXPECT_NE(reader, fresh);
  EXPECT_EQ(2, fresh->maxDoc());
  EXPECT_EQ(fresh, iw.getIndexReader());
}

TEST_F(IndexWriterTest, filterCachePublishesOnlyInstalledReaders) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto cache = iw.getFilterCache();

  addDoc(iw);
  iw.commit();
  auto reader = iw.getIndexReader();
  EXPECT_EQ(cache.get(), reader->filterCache());
  uint64_t publications = cache->readerPublicationsForTest();
  EXPECT_EQ(reader, iw.getIndexReader());
  EXPECT_EQ(publications, cache->readerPublicationsForTest());

  addDoc(iw);
  iw.commit();
  EXPECT_EQ(reader, iw.getIndexReader(10000000));
  EXPECT_EQ(publications, cache->readerPublicationsForTest());

  auto newer = iw.getIndexReader();
  EXPECT_NE(reader, newer);
  EXPECT_EQ(cache.get(), newer->filterCache());
  EXPECT_EQ(publications + 1, cache->readerPublicationsForTest());
  EXPECT_EQ(newer, iw.getIndexReader());
  EXPECT_EQ(publications + 1, cache->readerPublicationsForTest());

  IndexReader standalone(dir);
  EXPECT_EQ(nullptr, standalone.filterCache());
}

TEST_F(IndexWriterTest, queryContextOwnsAndDeduplicatesFilterUses) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto reader = iw.getIndexReader();
  MemPool pool;
  Query::Context context(pool, *reader, {}, nullptr,
                         {.schemaGen = 9, .coreGen = 0,
                          .fuzzyMaxExpansions = 10000, .timeZone = {}});
  TermQuery query("text_w", "test");

  auto* first = context.getFilterUse(query);
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(first, context.getFilterUse(query));
  EXPECT_EQ(1u, context.filterUses->size());

  MemPool otherPool;
  Query::Context other(otherPool, *reader, {}, nullptr,
                       context.filterKeyContext,
                       context.filterUses);
  EXPECT_EQ(first, other.getFilterUse(query));
  EXPECT_EQ(1u, other.filterUses->size());
}

TEST_F(IndexWriterTest, booleanAcquiresUsesAfterFilterNormalization) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto reader = iw.getIndexReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery first("text_w", "test");
  TermQuery duplicate("text_w", "test");
  std::array<Query*, 2> filters{&first, &duplicate};
  BooleanQuery query({}, {}, {}, filters);

  ASSERT_NE(nullptr, query.createWeight(context, 0));
  EXPECT_EQ(1u, context.filterUses->size());
}


// Test automatic merging kick-off
TEST_F(IndexWriterTest, autoMerge) {
  // This scope guard no longer needed since LuxirTest clears all listeners.
  // auto cleaner = luxir::scope_guard([](){ luxir::Signal::unlisten("mergeStart");});

  auto iterations = 1;  // increase for more thorough testing
  for (auto iter=0; iter<iterations; iter++) {
    RAMDir dir;
    IndexWriter iw(dir);
    int MERGE_FACTOR = 3;
    iw.mergePolicy->setMergeFactor(MERGE_FACTOR);
    iw.mergePolicy->refresh();  // should be a no-op at this point since no existing segs.

    // add  segments
    for (int i = 0; i < MERGE_FACTOR - 1; i++) {
      addDoc(iw);
      iw.commit();
    }

    // make sure we're making the segments we think we are:
    auto reader = iw.getIndexReader();
    ASSERT_EQ(MERGE_FACTOR - 1, reader->segments().size());
    ASSERT_EQ(MERGE_FACTOR - 1, reader->maxDoc());

    addDoc(iw);

    // MERGE_FACTOR segments of the same size will set off an auto-merge.
    // For this reason, we need a callback to try grabbing the new IndexReader before the merge completes.
    // We need to block the merge operation until we've grabbed a new reader.
    std::latch mergeStart(1);
    auto callback = [&mergeStart](void* a, void* b, void* c) -> void* {
      unused(a,b,c);
      mergeStart.wait();
      return nullptr;
    };

    luxir::Signal::listen("mergeStart", std::move(callback));

    // this should cause a segment flush and a merge to kick off, but we've blocked the merge from completing until
    // later.

    // NOTE: code below deadlocked since the wait() in the commit call work-steals the mergeSegments task (which will
    // only be un-blocked after the commit call completes).
    // Seems like we can't have both the commit work and the merge work be on the same thread.

    /* deadlock version
    iw.commit();
    // pre-merge view
    reader = iw.getIndexReader();
    ASSERT_EQ(reader->maxDoc(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), MERGE_FACTOR);
    mergeStart.count_down(); // let the merge continue
    */

    // What about using an async callback?
    // What if I force it to be called from a separate thread?

    // finish commit callback
    auto finishCommit = [&]() {
      auto reader = iw.getIndexReader();  // refresh the reader to see the new doc, but before the merge completes.
      mergeStart.count_down();  // let merge continue
      EXPECT_EQ(reader->maxDoc(), MERGE_FACTOR);
      EXPECT_EQ(reader->segments().size(), MERGE_FACTOR);
    };

    // this commit will cause the segment to flush and the merge to kick off
    iw.commit(std::move(finishCommit), UpdateMessage::CommitType::COMMIT);
    /* not needed to avoid deadlock... using a callback and not a blocker that could work-steal was enough.
    arena.execute([&](){
      iw.commit(UpdateMessage::CommitType::COMMIT, false, std::move(finishCommit));
    });
    */

    // Now wait until the merge completes.
    // If the commit is run in the same thread, this call can work-steal the mergeSegments task and deadlock.
    iw.updateGraph.wait_for_all();

    // depending on if the merger does a commit on its own, we may not see the merged segment
    // yet.  Currently, if the merger detects no indexing activity, it will request a new commit.

    reader = iw.getIndexReader();
    ASSERT_EQ(reader->maxDoc(), MERGE_FACTOR);
    ASSERT_EQ(reader->segments().size(), 1);
  }
}

TEST_F(IndexWriterTest, mergeFailureContainmentRestoresSourcesAndGate) {
  using namespace luxir::test;

  CollectionHelper helper("main");
  auto iw = helper.getIndexWriter();

  std::vector<std::string> expectedIds;
  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 4; i++) {
      std::string id = "s" + std::to_string(seg) + "_" + std::to_string(i);
      expectedIds.push_back(id);
      helper.index(flatdoc("id", id, "text_w", "merge containment"));
    }
    helper.commit();
  }
  std::sort(expectedIds.begin(), expectedIds.end());

  auto beforeReader = iw->getIndexReader();
  ASSERT_EQ(beforeReader->segments().size(), 2u);
  ASSERT_EQ(allIds(helper), expectedIds);

  std::latch mergeStarted(1);
  std::latch releaseMerge(1);
  luxir::Signal::listen("mergeStart", [&](void* a, void* b, void* c) -> void* {
    unused(a, b, c);
    mergeStarted.count_down();
    releaseMerge.wait();
    return nullptr;
  });

  luxir::Signal::listen("segmentMergeBody", [](void*, void*, void*) -> void* {
    throw std::runtime_error("injected segment merge failure");
  });
  std::atomic_bool waitCommitDone = false;
  std::atomic_bool waitCommitSuccess = false;
  {
    // The injected merge failure is contained and logged at error level; drop
    // that one expected line so real failures stand out.  Installed before the
    // threads start and torn down after they join, so the sink swap cannot race
    // a concurrent log.
    ExpectLog quiet("injected segment merge failure");
    std::thread mergeThread([&]() {
      iw->mergeSegments();
    });
    mergeStarted.wait();

    std::thread waitCommitThread([&]() {
      waitCommitSuccess.store(waitForMergesCommit(*iw), std::memory_order_relaxed);
      waitCommitDone.store(true, std::memory_order_relaxed);
    });

    releaseMerge.count_down();
    mergeThread.join();
    waitCommitThread.join();
  }
  luxir::Signal::unlisten("mergeStart");
  luxir::Signal::unlisten("segmentMergeBody");

  EXPECT_FALSE(iw->testMergeRunning());
  EXPECT_EQ(0, LuxirTest::luxirNode->getIndexRamBudget().pendingMergeDemandBytes())
      << "cancelled merge driver leaked published demand";
  EXPECT_TRUE(waitCommitDone.load(std::memory_order_relaxed));
  // Benign ordering: the waitForMerges commit may register before or after the
  // failure tail's decrement walk.  Both orderings succeed - a late joiner sees
  // no outstanding merges and does not wait - so this is not a flaky race, the
  // success holds either way.  Do not "fix" it with added synchronization.
  EXPECT_TRUE(waitCommitSuccess.load(std::memory_order_relaxed));
  auto failure = iw->testLastMergeFailure();
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->phase, "segment_merge");
  EXPECT_FALSE(failure->outputPublished);
  EXPECT_EQ(failure->sourceSegIds.size(), 2u);
  EXPECT_TRUE(segmentPrefixAbsent(iw->dir, failure->outputSegId));

  EXPECT_TRUE(waitForMergesCommit(*iw));
  auto afterFailureReader = iw->getIndexReader();
  EXPECT_EQ(afterFailureReader->segments().size(), 2u);
  EXPECT_EQ(allIds(helper), expectedIds);

  iw->mergeSegments();
  auto afterSuccessReader = iw->getIndexReader();
  EXPECT_EQ(afterSuccessReader->segments().size(), 1u);
  EXPECT_EQ(allIds(helper), expectedIds);
}


// Multithreaded test of IndexWriter updates, commits, merges, and IndexReader reopen during those.
TEST_F(IndexWriterTest, multiThreaded) {
  using namespace luxir::test;
  int requestThreads = 4;
  int docsToAdd = 100;
  int percentReads = 20;
  int percentCommits = 50;  // really stress segment flushing / merging
  int percentWaitForMerges = 30;  // of commits, fraction that wait for in-flight merges before publishing

  RAMDir dir;
  IndexWriter iw(dir);
  int MERGE_FACTOR = 3;
  iw.mergePolicy->setMergeFactor(MERGE_FACTOR);
  iw.mergePolicy->refresh();  // should be a no-op at this point since no existing segs.

  std::atomic_long docsRequested(0);
  std::atomic_long docsAdded(0);
  std::atomic_long docsVisible(0);
  std::atomic_long commitsRequested(0);
  std::atomic_long commits(0);
  std::atomic_long forceMergesRequested(0);
  std::atomic_long forceMergesCompleted(0);
  std::atomic_long forceMergeErrors(0);
  uint64_t requestSeed = rng();

  class UpdateInfo {
  public:
    int32_t seqNum;
    short numAdds;
    byte commitType;
  };
  std::vector<UpdateInfo> updates;
  bool recordUpdates = false;
  if (recordUpdates) {
    updates.reserve(docsToAdd * 2);
  }
  std::mutex testMutex;


  // Holds the build arena + concrete (non-owning) request the test submits.  As a
  // base it is constructed before the ProtoUpdateMessage base (which reads
  // request.commit), so the request is fully built in time (base-from-member idiom).
  struct TestReq {
    std::pmr::monotonic_buffer_resource mr;
    luxir::api::UpdateRequest request;
    TestReq(std::string_view fieldName, int numAdds, bool doCommit, bool waitForMerges,
            uint32_t maxSegments) {
      if (numAdds > 0) {
        luxir::api::Map* docs = luxir::api::build::allocArray(request.docs, numAdds, mr);
        for (int i = 0; i < numAdds; i++) {
          CollectionHelper::convertDocToProto(
              flatdoc(std::string(fieldName), std::string("now is the time for all")), docs[i], mr);
        }
      }
      if (doCommit) {
        auto& params = request.commit.emplace();
        params.wait_for_merges = waitForMerges;
        params.max_segments = maxSegments;
      }
    }
  };

  class TestProtoUpdateMessage : private TestReq, public ProtoUpdateMessage {
  public:
    luxir::api::UpdateRequest& updateRequest;
    std::function<void(TestProtoUpdateMessage&)> callback = nullptr;

    TestProtoUpdateMessage(std::string_view fieldName, int numAdds, bool doCommit,
                           bool waitForMerges, uint32_t maxSegments,
                           std::function<void(TestProtoUpdateMessage&)> callback)
      : TestReq(fieldName, numAdds, doCommit, waitForMerges, maxSegments),
        ProtoUpdateMessage(&this->request),
        updateRequest(this->request),
        callback(std::move(callback)) {}

    void done(IndexWriter& iw) override {
      unused(iw);
      if (callback) {
        callback(*this);
      }
      delete this;
    }
  };

  auto cb = [&](TestProtoUpdateMessage& msg) {
            TEST_DEBUG("done called! adds in this request={}", msg.updateRequest.docs.size());
    if (msg.updateRequest.commit.has_value()) {
      commits++;
      if (msg.updateRequest.commit->max_segments > 0) {
        forceMergesCompleted++;
        if (msg.result.errored()) forceMergeErrors++;
      }
    }
    docsAdded += msg.updateRequest.docs.size();
    if (recordUpdates) {
      std::lock_guard<std::mutex> lock(testMutex);
      for (int i = 0; i < (int)msg.updateRequest.docs.size(); i++) {
        UpdateInfo ui;
        ui.seqNum = msg.updateVersion;
        ui.numAdds = msg.updateRequest.docs.size();
        ui.commitType = (byte)msg.commit;
        updates.push_back(ui);
      }
    }

#ifdef REMOVED
    // this isn't valid test code since done() messages *can* be called out of order.  Updates with a commit
    // have to go through further through the pipeline and can thus have their done() method called later.
    auto localLastUpdateSeen = lastUpdateSeen.load();
    if ((int64_t)msg.seqNum < localLastUpdateSeen) {
      LOG_ERROR("Out of order update done()! {} vs {}", msg.seqNum, localLastUpdateSeen);
      FAIL();
    }
    if ((int64_t)msg.seqNum != localLastUpdateSeen + 1) {
      LOG_ERROR("Update done() skipped a sequence number! this={} last={}", msg.seqNum, localLastUpdateSeen);
      FAIL();
    }
    while ((int64_t)msg.seqNum > localLastUpdateSeen) {
      lastUpdateSeen.compare_exchange_weak(localLastUpdateSeen, (int64_t)msg.seqNum);
      localLastUpdateSeen = lastUpdateSeen.load();
    }
#endif

  };



  try {
    // change to using standard threads
    std::vector<std::thread> threads;

    for (int iter = 0; iter < requestThreads; iter++) {
      // create a thread
      threads.emplace_back([&, iter]() {
                  try {
                    Rng r(requestSeed + (uint64_t)iter);
                    bool writesDone = false;
                    for (;;) {
                      if (writesDone || r.rint(100) < percentReads) {
                        // do this *before* opening the reader, so we can ensure that the reader should see at least
                        // that many updates.
                        auto globalDocsVisible = docsVisible.load();

                        auto reader = iw.getIndexReader();
                        auto localDocsVisible = reader->maxDoc();
                        EXPECT_GE(localDocsVisible, globalDocsVisible);
                        while (localDocsVisible > globalDocsVisible) {
                          if (!docsVisible.compare_exchange_weak(globalDocsVisible, localDocsVisible)) {
                            globalDocsVisible = docsVisible.load();
                          }
                        }

                        // LOG_DEBUG("\tvisible docs: {} segs: {}", localDocsVisible, reader->segments().size());

                        /*
                        if (globalDocsVisible >= docsToAdd && globalDocsVisible >= docsAdded) {
                          // we've seen all the docs
                          break;
                        }
                        */
                        if (localDocsVisible >= docsToAdd) {
                          // we've seen all the docs
                          TEST_DEBUG("Reader has all docs visible: localDocsVisible={} docsVisible={}", localDocsVisible, docsVisible.load());
                          break;
                        }

                      }

                      // NOTE: since write submission is async and done in a loop, this can pile up a lot of writes in the queue
                      // really fast!  Perhaps we should yield when docsRequested - docsAdded is too large?
                      if (!writesDone) {
                        // Decide the request shape here; the message builds the concrete
                        // (arena-backed) request from these params and the base ctor reads
                        // commit/wait_for_merges off it.
                        bool doCommit = r.rint(100) < percentCommits;
                        int64_t numAdds = r.rint(doCommit ? 0 : 1,
                                                 3);  // lower bound on number of adds is 0 if we're going to commit

                        // make sure we don't go over the number of docs we want to add
                        // this makes it harder to figure out when we should do final commits.
                        if (numAdds > 0) {
                          for (;;) {
                            auto localDocsRequested = docsRequested.load();
                            numAdds = std::min(numAdds, docsToAdd - localDocsRequested);
                            if (numAdds == 0) {
                              doCommit = true;  // turn into a commit if it wasn't already.
                            }
                            assert(localDocsRequested + numAdds <= docsToAdd);  // sanity check
                            if (docsRequested.compare_exchange_weak(localDocsRequested, localDocsRequested + numAdds)) {
                              break;
                            }
                          }
                        }

                        bool waitForMerges = false;
                        if (doCommit && r.rint(100) < percentWaitForMerges) {
                          waitForMerges = true;
                        }
                        uint32_t maxSegments = doCommit ? randomForceMergeTarget(r) : 0;

                        if (doCommit) {
                          commitsRequested++;
                          if (maxSegments > 0) forceMergesRequested++;
                        }

                        if (doCommit && docsRequested.load() >= docsToAdd) {
                          // we're done with writes as long as we ended with a commit.
                          writesDone = true;
                        }

                        TEST_DEBUG("\tsubmitting update with {} adds, commit={}", numAdds, doCommit);
                        auto* msg = new TestProtoUpdateMessage(
                            field, (int)numAdds, doCommit, waitForMerges, maxSegments, cb);
                        auto success = iw.submitUpdate(msg);
                        if (!success) {
                          FAIL();
                        }
                      }

                      TEST_DEBUG("\t\tdocsRequested: {}, docsAdded: {}, docsVisible: {}, commitsRequested: {}, commits: {}",
                              docsRequested.load(), docsAdded.load(), docsVisible.load(), commitsRequested.load(),
                              commits.load());
                    }
                    TEST_DEBUG("Done with request thread");

                  } catch (std::exception& e) {
                    LOG_ERROR("################# Exception in request thread: {}", e.what());
                    FAIL();
                  }
                }

      );
    }

    // whait for all threads to finish
    for (auto& t : threads) {
      t.join();
    }

    // wait for all IW activity to finish.
    iw.updateGraph.wait_for_all();

    // If request threads are failing to stop, but this block of code above tasks.wait() and uncomment the sleep
    // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    while (docsRequested.load() < docsToAdd || docsVisible.load() < docsToAdd) {
      auto reader = iw.getIndexReader();
      LOG_INFO("### Main Thread docsRequested: {}, docsAdded: {}, docsVisible: {}, commitsRequested: {}, commits: {}",
              docsRequested.load(), docsAdded.load(), reader->maxDoc(), commitsRequested.load(), commits.load());


      if (docsRequested.load() >= docsToAdd && reader->maxDoc() < docsToAdd) {
        if (iw.lastAdvertisedCommitTime != reader->commitTime()) {
          LOG_ERROR("Reader not seeing last advertised commit time! {} vs {}", iw.lastAdvertisedCommitTime.load(), reader->commitTime());
        }

        auto stats = iw.stats(true);
        LOG_INFO("IndexWriter: segments={} committed={} updates={} commitTime={} activeMerges={}",
                 stats.segments, stats.committedSegments, stats.updateVersion,
                 stats.commitTime, stats.activeMerges);
        for (const auto& seg : stats.segmentStats) {
          LOG_INFO("\t\tsegId={} nDocs={} mergeLevel={} commitTime={}", seg.segId,
                   seg.maxDoc, seg.mergeLevel, seg.firstCommitTime);
        }

        // let's look at the last number of updates:
        if (!recordUpdates) {
          LOG_INFO("Test: consider re-running with recordUpdates=true in this test to see the last updates");
        }
        int start = std::max(0, (int)updates.size() - 100);
        for (int i = start; i < (int)updates.size(); i++) {
          auto& ui = updates[i];
          LOG_INFO("{}: seqNum={} numAdds={} commitType={}", i, ui.seqNum, ui.numAdds, (int)ui.commitType);
        }

        iw.commit();
        reader = iw.getIndexReader();
        if (reader->maxDoc() == docsToAdd) {
          LOG_ERROR("FINAL COMMIT MADE DOCS VISIBLE! Test Bug or IW bug?");
          FAIL();
        } else {
          break;
        }
      }

      break; // don't loop anymore - nothing should be
    }


  } catch (std::exception& e) {
    LOG_ERROR("################# Exception in main thread: {}", e.what());
    FAIL();
  }

  EXPECT_EQ(docsRequested.load(), docsToAdd);
  EXPECT_EQ(docsRequested.load(), docsAdded.load());
  EXPECT_EQ(docsRequested.load(), docsVisible.load());
  EXPECT_EQ(commitsRequested.load(), commits.load());
  EXPECT_EQ(forceMergesRequested.load(), forceMergesCompleted.load());
  EXPECT_EQ(0, forceMergeErrors.load());
  EXPECT_FALSE(iw.testMergeRunning());
}


// Test that _version_ field is present when overwrite=true and absent otherwise
TEST_F(IndexWriterTest, versionFieldOverwrite) {
  using namespace luxir::test;
  
  CollectionHelper helper("main");

  Doc doc1 = flatdoc("id", "doc1", "text_w", "hello world");
  helper.index(doc1, UpdateMessage::COMMIT, false);

  Doc doc2 = flatdoc("id", "doc2", "text_w", "hello version world");
  auto result2 = helper.index(doc2, UpdateMessage::COMMIT, true);

  Doc doc3 = flatdoc("id", "doc3", "text_w", "hello third world");
  auto result3 = helper.index(doc3, UpdateMessage::COMMIT, true);
  
  EXPECT_GT(result3.updateVersion, result2.updateVersion);

  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "text_w", "_version_"}).limit(-1);
  req->execute();
  auto docs = req->getDocs();

  req->done();
  ASSERT_EQ(3, docs.size());

  // text_w is STORED by default, so the raw value comes back with the doc.
  // doc1 was indexed without overwrite, so it has no _version_ value and the
  // field is absent from the returned doc.
  Doc expectedDoc1 = flatdoc("id", "doc1", "text_w", std::string("hello world"));
  Doc expectedDoc2 = flatdoc("id", "doc2", "text_w", std::string("hello version world"),
                             "_version_", (int64_t)(result2.updateVersion));
  Doc expectedDoc3 = flatdoc("id", "doc3", "text_w", std::string("hello third world"),
                             "_version_", (int64_t)(result3.updateVersion));
  
  bool foundDoc1 = containsDoc(docs, expectedDoc1);
  EXPECT_TRUE(foundDoc1);

  bool foundDoc2 = containsDoc(docs, expectedDoc2);
  EXPECT_TRUE(foundDoc2);

  bool foundDoc3 = containsDoc(docs, expectedDoc3);
  EXPECT_TRUE(foundDoc3);

  // Now index doc2 again with overwrite
  Doc doc2Overwrite = flatdoc("id", "doc2", "text_w", "hello version world again");
  helper.index(doc2Overwrite, UpdateMessage::COMMIT, true);
  req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "text_w", "_version_"}).limit(-1);
  req->execute();
  docs = req->getDocs();

  req->done();
  ASSERT_EQ(3, docs.size());

  /* TODO: not ready yet
  expectedDoc2 = flatdoc("id", "doc2", "_version_", (int64_t)(result4.updateVersion));
  foundDoc2 = containsDoc(docs, expectedDoc2);
  EXPECT_TRUE(foundDoc2);
  */
}

// _version_ columns can be sparse within a segment: docs indexed without an id
// field get no version value.  applyDeletes must read versions by rank, not
// docid - it used to index the dense decoder by docid, reading the wrong doc's
// version past a gap (and asserting past the end of the column).
TEST_F(IndexWriterTest, sparseVersionColumnDeletes) {
  using namespace luxir::test;

  CollectionHelper helper("main");

  // One batch -> one segment: doc 0 has no id (no _version_ value), doc 1 has
  // an id and an overwrite version.  The column holds 1 value but maxDoc is 2,
  // so reading doc 1's version by docid lands past the end.
  std::vector<Doc> docs = {
    flatdoc("text_w", "anonymous filler"),
    flatdoc("id", "sv1", "text_w", "versioned target"),
  };
  helper.indexAll(docs, UpdateMessage::COMMIT, true);

  // Deleting the versioned doc reads its version during applyDeletes.
  helper.deleteById("sv1", UpdateMessage::COMMIT);

  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("text_w", "versioned").fields({"text_w"}).limit(-1);
  req->execute();
  auto docs1 = req->getDocs();
  req->done();
  EXPECT_EQ(0u, docs1.size());

  req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("text_w", "anonymous").fields({"text_w"}).limit(-1);
  req->execute();
  auto docs2 = req->getDocs();
  req->done();
  EXPECT_EQ(1u, docs2.size());
}

// Test deletion functionality - verify delete infrastructure works
TEST_F(IndexWriterTest, deletionInfrastructure) {
  using namespace luxir::test;
  
  CollectionHelper helper("main");

  // Delete-by-id on a fresh/empty index - exercises the delete-only inverter path
  // where no documents are indexed but deletes still need to flow through IdHandler.
  auto earlyDelete = helper.deleteById("nonexistent", UpdateMessage::COMMIT);
  EXPECT_TRUE(earlyDelete.success);

  // Add 3 documents with versions (overwrite=true adds _version_ field)
  Doc doc1 = flatdoc("id", "doc1", "text_w", "hello world");
  helper.index(doc1, UpdateMessage::COMMIT, true);

  // for now, put the doc to be deleted in a seg with another doc since we
  // haven't implemented the logic to drop an entire segment!
  Doc doc2 = flatdoc("id", "doc2", "text_w", "goodbye world");
  helper.index(doc2, UpdateMessage::NO_COMMIT, true);

  Doc doc3 = flatdoc("id", "doc3", "text_w", "test document");
  auto result3 = helper.index(doc3, UpdateMessage::COMMIT, true);

  
  // Delete doc2
  auto deleteResult = helper.deleteById("doc2", UpdateMessage::COMMIT);
  EXPECT_TRUE(deleteResult.success);
  EXPECT_GT(deleteResult.updateVersion, result3.updateVersion);
  
  // Verify that the delete was applied at the index level
  auto indexWriter = helper.getIndexWriter();
  // Get a fresh IndexReader after the delete commit
  auto indexReader = indexWriter->getIndexReader(0);  // Force fresh reader

  // Check that at least one segment has deletes applied
  bool foundDeletes = false;
  int32_t totalDeletesFound = 0;
  int32_t totalLiveDocs = 0;
  
  for (const auto& segment : indexReader->segments()) {
    const auto& segInfo = segment.segInfo;
    totalDeletesFound += segment.numDeletes();
    totalLiveDocs += segment.numLive();

    if (segInfo.live_gen > 0) {
      foundDeletes = true;

      // Verify delete metadata is consistent
      EXPECT_GT(segInfo.live_gen, 0);
      EXPECT_NE(segment.liveDocs(), nullptr);
      EXPECT_EQ(segment.numLive(), segInfo.live_docs);
      
      // Verify delete bitmap functionality
      int32_t numDocs = segment.postingsReader().maxDoc();
      int32_t deletedCount = 0;
      
      auto* liveDocs = segment.liveDocs();
      ASSERT_NE(liveDocs, nullptr);
      const auto& bitset = liveDocs->bitset();
      
      for (int32_t docId = 0; docId < numDocs; docId++) {
        if (!bitset.get(docId)) {  // if not live, then deleted
          deletedCount++;
        }
      }
      
      EXPECT_EQ(deletedCount, segInfo.max_doc - segInfo.live_docs);
      
      LOG_TRACE("Segment {} has {} live docs (generation {}), verified {} deleted docs of {} total", 
                segInfo.seg_id, segInfo.live_docs, segInfo.live_gen, 
                deletedCount, numDocs);
    } else {
      // Segments without deletes should have consistent state
      EXPECT_EQ(segInfo.live_gen, 0);
      EXPECT_EQ(segment.liveDocs(), nullptr);
      EXPECT_EQ(segment.numDeletes(), 0);
    }
  }
  
  // Verify we found the expected delete
  EXPECT_TRUE(foundDeletes);
  EXPECT_EQ(totalDeletesFound, 1);
  EXPECT_EQ(totalLiveDocs, 2);  // Should have 2 live docs (doc1 and doc3)
  
  // Delete infrastructure verification completed
  
  LOG_TRACE("Delete infrastructure verification completed: {} segments checked, {} total deletes found", 
            indexReader->segments().size(), totalDeletesFound);

  // Now test deleting the same document again - should not create a new live_gen
  uint64_t originalLiveGen = 0;
  for (const auto& segment : indexReader->segments()) {
    const auto& segInfo = segment.segInfo;
    if (segment.numDeletes() > 0) {
      originalLiveGen = segInfo.live_gen;
      break;
    }
  }
  
  EXPECT_GT(originalLiveGen, 0);  // Should have found a segment with deletes
  
  // Delete the same document again
  auto deleteResult2 = helper.deleteById("doc2", UpdateMessage::COMMIT);
  EXPECT_TRUE(deleteResult2.success);
  
  // Get a fresh IndexReader after the second delete
  auto indexReader2 = indexWriter->getIndexReader();
  
  // Verify that live_gen did not increment (no new deletes should be applied)
  bool foundDeletedSegment = false;
  for (const auto& segment : indexReader2->segments()) {
    const auto& segInfo = segment.segInfo;
    if (segment.numDeletes() > 0) {
      foundDeletedSegment = true;
      EXPECT_EQ(segInfo.live_gen, originalLiveGen);
      EXPECT_EQ(segInfo.live_docs, 1);
    }
  }
  EXPECT_TRUE(foundDeletedSegment);

  // Test deleting doc3 to verify entire segment deletion
  // First, let's record the current number of segments
  auto initialSegmentCount = indexReader2->segments().size();
  
  // Delete doc3 (which should be in its own segment)
  auto deleteResult3 = helper.deleteById("doc3", UpdateMessage::COMMIT);
  EXPECT_TRUE(deleteResult3.success);
  EXPECT_GT(deleteResult3.updateVersion, deleteResult2.updateVersion);
  
  // Get a fresh IndexReader after deleting doc3
  auto indexReader3 = indexWriter->getIndexReader(0);  // Force fresh reader
  
  // Verify that the segment containing doc3 has been completely removed
  auto finalSegmentCount = indexReader3->segments().size();
  EXPECT_LT(finalSegmentCount, initialSegmentCount);
  
  // Verify we now have only 1 live document (doc1)
  EXPECT_EQ(indexReader3->maxDoc(), 1);
  
  // Record the segment ID that should have been deleted (doc3's segment)
  // We need to capture this before adding another document that helps triggers the deletion
  // The deletion happens asynchronously.
  uint64_t deletedSegmentId = 0;
  for (const auto& segment : indexReader2->segments()) {
    bool foundInFinalReader = false;
    for (const auto& finalSegment : indexReader3->segments()) {
      if (finalSegment.segInfo.seg_id == segment.segInfo.seg_id) {
        foundInFinalReader = true;
        break;
      }
    }
    if (!foundInFinalReader) {
      deletedSegmentId = segment.segInfo.seg_id;
      break;
    }
  }
  EXPECT_GT(deletedSegmentId, 0); // Should have found a deleted segment
  
  // Add another document to force another commit, which should wait long enough for the segment to be deleted.
  Doc doc4 = flatdoc("id", "doc4", "text_w", "final document");
  helper.index(doc4, UpdateMessage::COMMIT, true);
  
  // Verify that the index files for the deleted segment have actually been removed
  // The deletePrefix method should have removed all files starting with the segment prefix
  std::string deletedPrefix = Postings::getIndexFileNamePrefix(deletedSegmentId);

  // Check that no files exist with the deleted segment prefix
  // Use the Directory's listFiles method to get all files
  auto& dir = helper.getIndexWriter()->dir;
  std::vector<Directory::FileInfo> allFiles;
  dir.listFiles(allFiles);

  // Verify that no files exist with the deleted segment's prefix
  for (const auto& file : allFiles) {
    EXPECT_FALSE(file.name.starts_with(deletedPrefix))
      << "File " << file.name << " should have been deleted by deletePrefix() but still exists";
  }
  
  // Verify the new document is searchable and we now have 2 documents
  auto* req2 = LocalReq::create(helper.getSearchEngine());
  req2->collection("main").topDocs("q").allQuery().fields({"id"}).limit(-1);
  req2->execute();
  auto finalDocs2 = req2->getDocs();
  req2->done();
  
  EXPECT_EQ(2, finalDocs2.size()); // doc1 and doc4
}

// test that components in the IndexReader factory methods handle missing files correctly
TEST_F(IndexWriterTest, testMissingFiles) {
  // Create a separate RAMDir for testing the factory methods
  RAMDir testDir;

  // Test missingFileOK = false (should throw on missing files)
  EXPECT_THROW({
      auto reader1 = PostingsReader::create(testDir, 999, false);  // non-existent segment
  }, std::filesystem::filesystem_error);

  // Test missingFileOK = true (should return nullptr on missing files)
  auto reader2 = PostingsReader::create(testDir, 999, true);
  EXPECT_EQ(reader2, nullptr);

  // Test missingFileOK = false (should throw on missing files)
  EXPECT_THROW({
      auto liveDocs1 = LiveDocs::create(testDir, 999, 1, 10, false);  // non-existent segment
  }, std::filesystem::filesystem_error);

  // Test missingFileOK = true (should return nullptr on missing files)
  auto liveDocs2 = LiveDocs::create(testDir, 999, 1, 10, true);
  EXPECT_EQ(liveDocs2, nullptr);
}

//
// This is one of the main tests for testing multithreaded updates with deletes and segment
// merges.  If changes are made to the IndexWriter, pump up opsPerThread to really stress
// test the system.
//
// Shared body for the multithreaded update/delete/read stress test.  When
// mergeFailPercent > 0, a fraction of merges throw mid-flight to exercise the
// merge-failure containment teardown under concurrency.  When
// updateFailPercent > 0, a fraction of updates contain a doc that fails to
// index (unknown field), either as a single-doc update or an all_or_none
// batch.  All injected failures are invisible to queries, so the version
// oracle below must hold exactly as if they were never submitted.
static void runMultithreadedUpdates(uint64_t seed, int mergeFailPercent, int updateFailPercent = 0) {
  using namespace luxir::test;

  CollectionHelper helper("main");

  auto indexWriter = helper.getIndexWriter();
  indexWriter->mergePolicy->setMergeFactor(3);  // low merge factor to stress merge concurrency with other operations

  std::atomic<int> injectedDocFailures{0};
  std::atomic<int> forceMergesRequested{0};
  std::atomic<int> forceMergesCompleted{0};
  std::atomic<int> forceMergeErrorResponses{0};

  auto nextCommitMaxSegments = [&](Rng& r) {
    uint32_t maxSegments = randomForceMergeTarget(r);
    if (maxSegments > 0) forceMergesRequested++;
    return maxSegments;
  };

  auto recordForceMergeResponse = [&](uint32_t maxSegments, const IndexResult& result) {
    if (maxSegments == 0) return;
    forceMergesCompleted++;
    if (!result.success) forceMergeErrorResponses++;
  };

  auto checkCommittedResponse = [&](uint32_t maxSegments, const IndexResult& result) {
    recordForceMergeResponse(maxSegments, result);
    if (result.success) return;
    EXPECT_GT(maxSegments, 0u);
    EXPECT_GT(mergeFailPercent, 0);
    EXPECT_EQ(luxir::api::UpdateResponse_::Status::ERROR, result.status);
  };

  auto numThreads = 16;
  auto opsPerThread = 50;  // total operations per thread
  auto docsPerThread = 4;  // number of unique documents per thread.. keep this low to generate high contention.


  // Merge-failure injection.  Driven by an Rng (not a fixed stride) so failure
  // runs - back-to-back, bursts, gaps - occur naturally; future IW failure
  // handling (backoff/quarantine) will branch on consecutiveness.  Field merge
  // tasks emit this signal concurrently, so the listener guards the shared Rng.
  Rng mergeRng(seed ^ 0x9e3779b97f4a7c15ULL);
  std::mutex mergeRngMutex;
  if (mergeFailPercent > 0) {
    luxir::Signal::listen("segmentMergeBody",
        [&mergeRng, &mergeRngMutex, mergeFailPercent](void*, void*, void*) -> void* {
          bool shouldFail = false;
          {
            const std::lock_guard<std::mutex> lock(mergeRngMutex);
            shouldFail = mergeRng.rint(100) < mergeFailPercent;
          }
          if (shouldFail) {
            throw std::runtime_error("injected merge failure (hammer)");
          }
          return nullptr;
        });
  }

  // Injected merge failures are contained and logged at error level.  Suppress
  // exactly those expected lines (matched by message) for the whole run so they
  // do not bury real problems; a genuine unexpected error still surfaces.  When
  // no failures are injected nothing matches, so this is a harmless pass-through.
  ExpectLog quietFailures("injected merge failure (hammer)");

  // True while an enabled injection kind has not fired yet.  Threads run their
  // planned ops and then keep going (bounded) until every enabled kind has fired
  // at least once, so the EXPECT_GT guards at the end can't trip on an unlucky
  // run while every injection stays probabilistic (a deterministic forced
  // failure could mask a bug that killed all the random ones).  The cap turns
  // structurally broken injection into a guard failure instead of an endless
  // loop.
  auto injectionsPending = [&]() {
    return (updateFailPercent > 0 && injectedDocFailures.load() == 0)
        || (mergeFailPercent > 0 && quietFailures.suppressed() == 0);
  };

  std::vector<std::thread> threads;

#ifdef TBB_TEST_VERSION
  std::atomic<bool> done(false);

  std::thread deadlockDetector([&]() {
    while (!done.load()) {
      uint64_t lastUpdateNumber = indexWriter->updateNumber;
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (indexWriter->updateNumber == lastUpdateNumber) {
        LOG_ERROR("Deadlock detector: No Indexing activity for 1 second!");
        try {
          // If an uncaught exception occured in the updateGraph, waiting for all will throw it.
          indexWriter->updateGraph.wait_for_all();
        } catch (const std::exception& e) {
          LOG_ERROR("Deadlock detector caught exception: {}", e.what());
        }
      }
    }
  });

  oneapi::tbb::task_arena arena(numThreads);

  arena.execute([&]() {
    oneapi::tbb::task_group tg;

    for (int tid = 0; tid < numThreads; tid++) {
        tg.run([&, tid]() {
        Rng r(seed + tid);
        std::vector<int64_t> docVersions(docsPerThread, 0);

        for (int op = 0; op < opsPerThread; op++) {
          // Use numeric IDs: thread 0 uses 1000-1007, thread 1 uses 2000-2007, etc
          // One thread never modifies another threads documents.
          int localDoc = r.rint(docsPerThread);
          std::string docId = std::to_string(localDoc + tid * 1000);
          int64_t lastUpdateVersion = 0;

          int operation = r.rint(3);  // 0 == update, 1 == delete, 2 = read

          if (operation == 0) { // Index
            uint32_t maxSegments = nextCommitMaxSegments(r);
            Doc doc = flatdoc("id", docId);
            auto result = helper.index(doc, UpdateMessage::COMMIT, true, maxSegments);
            checkCommittedResponse(maxSegments, result);
            // TODO: expose and get LuxirError for actual error message / stack trace.
            docVersions[localDoc] = result.updateVersion;

          } else if (operation == 1) { // Delete
            uint32_t maxSegments = nextCommitMaxSegments(r);
            auto result = helper.deleteById(docId, UpdateMessage::COMMIT, maxSegments);
            checkCommittedResponse(maxSegments, result);
            docVersions[localDoc] = -1; // Mark as deleted
          } else { // Read
            indexWriter->getIndexReader();
            // TODO: store, expose, and test the update verision in the IndexReader

            auto* req = LocalReq::create(helper.getSearchEngine());
            req->collection("main").topDocs("q").matchQuery("id", docId).fields({"id", "_version_"});
            req->execute();
            auto docs = req->getDocs();
            req->done();

            if (docVersions[localDoc] <= 0) {
              EXPECT_TRUE(docs.empty());
            } else if (docVersions[localDoc] > 0) {
              EXPECT_EQ(docs.size(), 1);
              if (!docs.empty()) {
                int64_t foundVersion = -1;
                if (auto* val = find(docs[0], "_version_")) {
                  foundVersion = std::get<int64_t>(*val);
                }
                EXPECT_EQ(foundVersion, docVersions[localDoc])
                  << "Thread " << tid << " doc " << docId << " version mismatch"
                  << " expected: " << docVersions[localDoc] << " found: " << foundVersion;
              }
            }

          } // end Read
        } // end for opsPerThread
      });
    }

    tg.wait(); // wait for all threads to finish.
  }); // arena.execute

  done.store(true); // signal the deadlock detector to stop
  deadlockDetector.join();
#endif

   for (int tid = 0; tid < numThreads; tid++) {
      threads.emplace_back([&, tid]() {
      Rng r(seed + tid);
      std::vector<int64_t> docVersions(docsPerThread, 0);

      for (int op = 0; op < opsPerThread || (op < opsPerThread * 10 && injectionsPending()); op++) {
        // Use numeric IDs: thread 0 uses 1000-1007, thread 1 uses 2000-2007, etc
        // One thread never modifies another threads documents.
        int localDoc = r.rint(docsPerThread);
        std::string docId = std::to_string(localDoc + tid * 1000);

        int operation = r.rint(3);  // 0 == update, 1 == delete, 2 = read

        if (operation == 0) { // Index
          uint32_t maxSegments = nextCommitMaxSegments(r);
          // Inject a failing update: the doc must keep its previous state, so
          // docVersions is deliberately NOT updated and the version oracle in
          // the read path verifies the rollback was invisible.
          if (updateFailPercent > 0 && r.rint(100) < updateFailPercent) {
            injectedDocFailures++;
            if (r.rint(2) == 0) {
              // Single doc that fails on an unknown field.
              Doc doc = flatdoc("id", docId, "no_such_field", "boom");
              auto result = helper.index(doc, UpdateMessage::COMMIT, true, maxSegments);
              recordForceMergeResponse(maxSegments, result);
              ASSERT_FALSE(result.success);
              ASSERT_EQ(luxir::api::UpdateResponse_::Status::ERROR, result.status);
              ASSERT_EQ(1, (int)result.errors.size());
              EXPECT_EQ(docId, result.errors[0].id);
            } else {
              // all_or_none batch: a good update of another owned doc gets
              // indexed, then the bad doc voids the batch; both docs must be
              // left exactly as they were.
              std::string otherId = std::to_string(r.rint(docsPerThread) + tid * 1000);
              CollectionHelper::UpdateBuilder b;
              b.add(flatdoc("id", otherId));
              b.add(flatdoc("id", docId, "no_such_field", "boom"));
              b.overwrite(true);  // allow_dups = false
              b.allOrNone(true);
              b.commit(false, maxSegments);
              auto result = helper.submit(b);
              recordForceMergeResponse(maxSegments, result);
              ASSERT_FALSE(result.success);
              ASSERT_EQ(luxir::api::UpdateResponse_::Status::ERROR, result.status);
              ASSERT_EQ(1, (int)result.errors.size());
              EXPECT_EQ(docId, result.errors[0].id);
              EXPECT_EQ(1, result.errors[0].index);
            }
            continue;
          }

          Doc doc = flatdoc("id", docId);
          auto result = helper.index(doc, UpdateMessage::COMMIT, true, maxSegments);
          checkCommittedResponse(maxSegments, result);
          // TODO: expose and get LuxirError for actual error message / stack trace.
          docVersions[localDoc] = result.updateVersion;

        } else if (operation == 1) { // Delete
          uint32_t maxSegments = nextCommitMaxSegments(r);
          auto result = helper.deleteById(docId, UpdateMessage::COMMIT, maxSegments);
          checkCommittedResponse(maxSegments, result);
          docVersions[localDoc] = -1; // Mark as deleted
        } else { // Read
          indexWriter->getIndexReader();
          // TODO: store, expose, and test the update verision in the IndexReader

          auto* req = LocalReq::create(helper.getSearchEngine());
          req->collection("main").topDocs("q").matchQuery("id", docId).fields({"id", "_version_"});
          req->execute();
          auto docs = req->getDocs();
          req->done();

          if (docVersions[localDoc] <= 0) {
            EXPECT_TRUE(docs.empty()) << "Thread " << tid << " found deleted doc " << docId;
          } else if (docVersions[localDoc] > 0) {
            EXPECT_EQ(docs.size(), 1) << "Thread " << tid << " doc " << docId << " not found";
            if (!docs.empty()) {
              int64_t foundVersion = -1;
              if (auto* val = find(docs[0], "_version_")) {
                foundVersion = std::get<int64_t>(*val);
              }
              EXPECT_EQ(foundVersion, docVersions[localDoc])
                << "Thread " << tid << " doc " << docId << " version mismatch"
                << " expected: " << docVersions[localDoc] << " found: " << foundVersion;
            }
          }

        } // end Read
      } // end for opsPerThread
    });
  }

  // Wait for all threads
  for (auto& thread : threads) {
    thread.join();
  }

  // make sure we don't leave any tasks in the updateGraph (drains in-flight
  // merges, including injected-failure ones, before we drop the listener).
  indexWriter->updateGraph.wait_for_all();
  EXPECT_GT(forceMergesRequested.load(), 0);
  EXPECT_EQ(forceMergesRequested.load(), forceMergesCompleted.load());
  EXPECT_LE(forceMergeErrorResponses.load(), forceMergesCompleted.load());
  if (mergeFailPercent == 0 && updateFailPercent == 0) {
    EXPECT_EQ(0, forceMergeErrorResponses.load());
  }
  EXPECT_FALSE(indexWriter->testMergeRunning());

  if (mergeFailPercent > 0) {
    // The run is meaningless if no merge actually failed; the version oracle
    // above would still pass.  Assert at least one injected failure landed.
    EXPECT_GT(quietFailures.suppressed(), 0u);
    luxir::Signal::unlisten("segmentMergeBody");
  }

  if (updateFailPercent > 0) {
    EXPECT_GT(injectedDocFailures.load(), 0);
  }
}

TEST_F(IndexWriterTest, testMultithreadedUpdates) {
  runMultithreadedUpdates(rng(), /*mergeFailPercent=*/0);
}

// Same hammer, but a fraction of merges fail mid-flight.  Verifies merge
// failure containment under real contention: contained failures must not
// corrupt visible state (the version oracle holds), wedge the writer
// (wait_for_merges commits still complete - no hang), or crash the
// concurrent source-restoration path.
TEST_F(IndexWriterTest, testMultithreadedUpdatesWithMergeFailures) {
  runMultithreadedUpdates(rng(), /*mergeFailPercent=*/25);
}

// Same hammer, but a fraction of updates contain a failing doc (single-doc and
// all_or_none batch variants).  Verifies per-doc failure recovery under real
// contention: id-map rollback, tombstoning, and inverter reuse racing with
// deletes, commits, and merges.  The version oracle must hold exactly as if
// the failed updates were never submitted.
TEST_F(IndexWriterTest, testMultithreadedUpdatesWithDocFailures) {
  runMultithreadedUpdates(rng(), /*mergeFailPercent=*/0, /*updateFailPercent=*/25);
}

// Doc failures and merge failures together.
TEST_F(IndexWriterTest, testMultithreadedUpdatesWithDocAndMergeFailures) {
  runMultithreadedUpdates(rng(), /*mergeFailPercent=*/25, /*updateFailPercent=*/25);
}

// Test segment merging with deleted documents
TEST_F(IndexWriterTest, segmentMergerWithDeletes) {
  using namespace luxir::test;
  
  CollectionHelper helper("main");
  
  auto indexWriter = helper.getIndexWriter();
  indexWriter->mergePolicy->setMergeFactor(3);
  
  // Create first segment with doc1
  Doc doc1 = flatdoc("id", "doc1", "text_w", "hello world");
  helper.index(doc1, UpdateMessage::COMMIT, true);
  
  // Create second segment with doc2 and doc3 together (tests reordering when doc2 is deleted)
  Doc doc2 = flatdoc("id", "doc2", "text_w", "goodbye world");
  Doc doc3 = flatdoc("id", "doc3", "text_w", "test document");
  helper.index(doc2, UpdateMessage::NO_COMMIT, true);
  helper.index(doc3, UpdateMessage::COMMIT, true);
  
  // Verify we have 2 segments
  auto reader1 = indexWriter->getIndexReader();
  EXPECT_EQ(2, reader1->segments().size());
  EXPECT_EQ(3, reader1->maxDoc());
  
  // Delete doc2 (should be in the second segment with doc3)
  helper.deleteById("doc2", UpdateMessage::COMMIT);
  
  // Verify the delete was applied
  auto reader2 = indexWriter->getIndexReader();
  bool foundDeletes = false;
  for (const auto& segment : reader2->segments()) {
    LOG_TRACE("Segment {}: maxDoc={}, liveDocs={}, numDeletes={}, numLive={}",
             segment.segInfo.seg_id, segment.segInfo.max_doc, 
             segment.segInfo.live_docs, segment.numDeletes(), segment.numLive());
    if (segment.numDeletes() > 0) {
      foundDeletes = true;
      EXPECT_EQ(1, segment.numDeletes());
      EXPECT_EQ(1, segment.numLive());
    }
  }
  EXPECT_TRUE(foundDeletes);
  
  // Add third segment to trigger merge (3 segments total, mergeFactor=3)
  Doc doc4 = flatdoc("id", "doc4", "text_w", "trigger merge");
  helper.index(doc4, UpdateMessage::COMMIT, true);
  
  // Wait for merge to complete
  indexWriter->updateGraph.wait_for_all();
  
  // Get fresh reader
  auto reader3 = indexWriter->getIndexReader();
  
  LOG_TRACE("After merge: {} segments, {} total docs", reader3->segments().size(), reader3->maxDoc());
  for ([[maybe_unused]] const auto& segment : reader3->segments()) {
    LOG_TRACE("Merged segment {}: maxDoc={}, liveDocs={}, numDeletes={}, numLive={}",
             segment.segInfo.seg_id, segment.segInfo.max_doc,
             segment.segInfo.live_docs, segment.numDeletes(), segment.numLive());
  }
  
  // Should have fewer segments now due to merge (likely 1 merged segment)
  EXPECT_LE(reader3->segments().size(), 2);
  
  // Should have 3 live documents (doc1, doc3, doc4) - doc2 was deleted and should be excluded from merge
  EXPECT_EQ(3, reader3->maxDoc());
  
  // Verify we can still search and find the expected documents
  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id"}).limit(-1);
  req->execute();
  auto docs = req->getDocs();
  req->done();

  EXPECT_EQ(3, docs.size());

  // Verify doc2 is not in results
  bool foundDoc1 = false, foundDoc3 = false, foundDoc4 = false, foundDoc2 = false;
  for (const auto& doc : docs) {
    if (auto* val = find(doc, "id")) {
      const auto& id = std::get<std::string>(*val);
      if (id == "doc1") foundDoc1 = true;
      if (id == "doc2") foundDoc2 = true;
      if (id == "doc3") foundDoc3 = true;
      if (id == "doc4") foundDoc4 = true;
    }
  }
  
  EXPECT_TRUE(foundDoc1);
  EXPECT_FALSE(foundDoc2); // This should be deleted
  EXPECT_TRUE(foundDoc3);
  EXPECT_TRUE(foundDoc4);
}


//
// Test that tests remapping of docs and positions after segment merging and deletes.
// This test also  inadvertently tests the case where a merge and commit happen concurrently,
// and the merge merges a segment before deletes have been applied and "personalDeletes" come into play.
//
TEST_F(IndexWriterTest, segmentMergerPositions) {
  using namespace luxir::test;
  int docsPerSeg = 10;
  int numSegs = 10;

  CollectionHelper helper("main");

  auto indexWriter = helper.getIndexWriter();
  indexWriter->mergePolicy->setMergeFactor(numSegs);

  // Track which documents we're deleting for verification later
  std::set<std::string> deletedIds;

  // create segments and delete random documents before committing.
  for (int seg = 0; seg < numSegs; seg++) {  // FIXME - no merge
    for (int doc = 0; doc < docsPerSeg; doc++) {
      int id = seg * 100 + doc; // Unique ID for each document
      std::string idStr = "doc" + std::to_string(id);

      // Add a random number of terms to the text field to make positions of following terms different.
      int nTerms = rng.rint(1, 10);
      std::string textField = "";
      for (int i=0; i<nTerms; i++) {
        textField += " term" + std::to_string(i);
      }
      textField += " hello world ";
      textField += idStr;
      Doc d = flatdoc("id", idStr, "text_w", textField);
      // LOG_DEBUG("Indexing doc:{} text_w:{}", idStr, textField);
      helper.index(d, UpdateMessage::NO_COMMIT, true);
    }
    // now delete some random document in this segment
    // don't delete all of them since we want enough segments to merge together.
    auto nDeletes = rng.rint(1,docsPerSeg-1);
    for (int del = 0; del < nDeletes; del++) {
      int deleteDocId = seg * 100 + (rng.rint(docsPerSeg)); // Delete random doc in each segment
      std::string deleteIdStr = "doc" + std::to_string(deleteDocId);
      deletedIds.insert(deleteIdStr);
      helper.deleteById(deleteIdStr, UpdateMessage::NO_COMMIT);
    }
    // now finally commit (this tends to kick off the merge at the same time as deletes are applied!)
    helper.commit();
  }

  // wait for previous merge to finish.
  indexWriter->updateGraph.wait_for_all();

  // force another merge to squeeze out deletes
  indexWriter->mergeSegments();

  // verify we have a single segment
  auto reader = indexWriter->getIndexReader();
  EXPECT_EQ(1, reader->segments().size());

  auto numDocs = numSegs * docsPerSeg - deletedIds.size();

  EXPECT_EQ(numDocs, reader->maxDoc());

  // Test 2: Verify term/match queries on the "text_w" field for "world" retrieve documents with valid IDs
  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("text_w", "world").limit(-1).fields({"id", });
  req->execute();
  auto docs = req->getDocs();
  req->done();

  // should be all docs
  ASSERT_EQ(numDocs, docs.size());
  // verify document remapping
  for (int seg = 0; seg < numSegs; seg++) {
    for (int doc = 0; doc < docsPerSeg; doc++) {
      int id = seg * 100 + doc;
      std::string idStr = "doc" + std::to_string(id);

      // Test 1: Verify term/match queries on the "id" field retrieve the correct "id"
      req = LocalReq::create(helper.getSearchEngine());
      req->collection("main").topDocs("q").matchQuery("id", idStr).fields({"id"});
      req->execute();
      docs = req->getDocs();
      req->done();

      bool wasDeleted = deletedIds.find(idStr) != deletedIds.end();
      if (wasDeleted) {
        ASSERT_TRUE(docs.empty());
      } else {
        ASSERT_EQ(docs.size(), 1);
        ASSERT_EQ(idStr, std::get<std::string>(docs[0][0].val));
      }

      // Verify term/match queries on the "text_w" field for the id term is on the right doc.
      req = LocalReq::create(helper.getSearchEngine());
      req->collection("main").topDocs("q").matchQuery("text_w", idStr).fields({"id", });
      req->execute();
      docs = req->getDocs();
      req->done();

      // should be a single result if not deleted.
      if (wasDeleted) {
        ASSERT_EQ(0, docs.size());
      } else {
        ASSERT_EQ(1, docs.size());
        ASSERT_EQ(idStr, std::get<std::string>(docs[0][0].val));
      }

      // Verify that the positions lookups are correct.
      req = LocalReq::create(helper.getSearchEngine());
      req->collection("main").topDocs("q").phraseQuery("text_w", {"world", idStr}).fields({"id", });
      req->execute();
      docs = req->getDocs();
      req->done();

      // should be a single result if not deleted.
      if (wasDeleted) {
        ASSERT_EQ(0, docs.size());
      } else {
        ASSERT_EQ(1, docs.size());
        ASSERT_EQ(idStr, std::get<std::string>(docs[0][0].val));
      }

    }
  }
}


// Test deletes by docid during indexing (when something goes wrong)
TEST_F(IndexWriterTest, inverterDeletes) {
  RAMDir dir;
  IndexWriter iw(dir);
  
  // Get an inverter and index some documents, marking some as deleted
  auto& inverter = iw.obtainInverter(1);
  auto& fieldHandler = inverter.getIndexHandler(field);
  
  // Add some normal documents
  inverter.startDoc(); // doc 0
  fieldHandler.index(inverter, "hello world");
  inverter.finishDoc();
  
  inverter.startDoc(); // doc 1
  fieldHandler.index(inverter, "test document");
  inverter.finishDoc();
  
  // Simulate an error on doc 2 - mark it as deleted
  inverter.startDoc(); // doc 2
  // Simulate partial indexing...
  inverter.deleteDoc(2);
  inverter.finishDoc();
  
  inverter.startDoc(); // doc 3
  fieldHandler.index(inverter, "another test");
  inverter.finishDoc();
  
  // Mark doc 1 as deleted too
  inverter.deleteDoc(1);
  
  // Release the inverter to trigger flush
  iw.releaseInverter(inverter);
  iw.commit();
  
  // Verify the segment was created with the correct live docs
  auto reader = iw.getIndexReader();
  ASSERT_EQ(1, reader->segments().size());
  
  const auto& segment = reader->segments()[0];
  ASSERT_EQ(4, segment.maxDoc()); // 4 documents total
  ASSERT_EQ(2, segment.numDeletes()); // 2 deleted
  ASSERT_EQ(2, segment.segInfo.live_docs); // 2 live
  
  // Check that liveDocs were written correctly
  auto* liveDocs = segment.liveDocs();
  ASSERT_NE(nullptr, liveDocs);
  
  // Verify which documents are live
  const auto& bitset = liveDocs->bitset();
  EXPECT_TRUE(bitset.get(0));  // doc 0 is live
  EXPECT_FALSE(bitset.get(1)); // doc 1 is deleted
  EXPECT_FALSE(bitset.get(2)); // doc 2 is deleted
  EXPECT_TRUE(bitset.get(3));  // doc 3 is live
}


// A delete at version V can only affect a segment whose minVersion < V; a segment
// whose minVersion is >= V holds only docs newer than the delete, so it must be
// skipped. Under concurrent load this is routine: update/delete messages are
// versioned in order at intake but execute in parallel into different inverters,
// so a delete can be applied in the same commit that flushes a higher-versioned
// segment. We reproduce that partition deterministically by seating inverters at
// chosen versions via obtainInverter and flushing them in one commit whose delete
// set sits between their minVersions. A Signal hook in applyDeletes reports each
// segment the delete is actually applied to. The commit applies its delete to
// three kinds of segment:
//   - OLD:    committed in a prior commit (minVersion 1 < 2)              -> applied
//   - INFLIGHT: flushed in this same commit (minVersion 1 < 2)           -> applied
//   - NEW:    flushed in this same commit but newer (minVersion 3 >= 2)  -> skipped
//
// Before the minVersion fix this case could not be exercised: minVersion was pinned
// to 0, so every segment passed 0 < V and was always a delete candidate.
TEST_F(IndexWriterTest, deleteCandidacyGatedByMinVersion) {
  RAMDir dir;
  IndexWriter iw(dir);

  std::set<int64_t> applied;  // segIds the delete was applied to this commit
  std::mutex appliedMu;       // applyDeletes fires the hook from parallel per-segment tasks
  luxir::Signal::listen("deleteAppliedToSegment", [&](void* a, void*, void*) -> void* {
    std::lock_guard<std::mutex> lk(appliedMu);
    applied.insert((int64_t)a);
    return nullptr;
  });

  // OLD segment at minVersion 1, holding id "dup", committed on its own first.
  // (commit -> updateVersion 1, which flushes the minVersion-1 inverter.)
  auto& oldInv = iw.obtainInverter(1);
  int64_t oldSeg = oldInv.getSegId();
  oldInv.startDoc();
  oldInv.getIndexHandler("id").index(oldInv, "dup");
  oldInv.finishDoc();
  iw.releaseInverter(oldInv);
  iw.commit();  // updateVersion 1

  // Bump the update counter so the next commit's version clears the membership
  // gate for the minVersion-3 inverter below (commit version must be >= 3).
  iw.commit();  // updateVersion 2, nothing to flush

  // Now flush, in a single commit, three inverters: a delete-bearing inverter at
  // version 2, an in-flight low-version doc inverter at version 1, and a newer
  // doc inverter at version 3. Obtain all before releasing any so the idle pool
  // can't hand back the same object. The commit's maxDeleteVersion is 2 (from the
  // delete), which sits between the low (1) and high (3) inverter minVersions.
  auto& delInv = iw.obtainInverter(2);
  delInv.deleteId("dup", 2);

  auto& inflightInv = iw.obtainInverter(1);
  int64_t inflightSeg = inflightInv.getSegId();
  inflightInv.startDoc();
  inflightInv.getIndexHandler("id").index(inflightInv, "live");
  inflightInv.finishDoc();

  auto& newInv = iw.obtainInverter(3);
  int64_t newSeg = newInv.getSegId();
  newInv.startDoc();
  newInv.getIndexHandler("id").index(newInv, "newdoc");
  newInv.finishDoc();

  iw.releaseInverter(delInv);
  iw.releaseInverter(inflightInv);
  iw.releaseInverter(newInv);
  iw.commit();  // updateVersion 3; flushes all three, maxDeleteVersion == 2

  // The delete (version 2) is applied to both the pre-existing OLD segment and
  // the just-flushed INFLIGHT segment (both minVersion 1 < 2), and skipped for
  // NEW (minVersion 3 >= 2), whose docs are all newer than the delete.
  EXPECT_TRUE(applied.contains(oldSeg));
  EXPECT_TRUE(applied.contains(inflightSeg));
  EXPECT_FALSE(applied.contains(newSeg));
}


// Test that fields are removed from the index after document deletion and merging
TEST_F(IndexWriterTest, removeFields) {
  using namespace luxir::test;
  
  CollectionHelper helper("main");
  
  auto indexWriter = helper.getIndexWriter();

  // Add document with all field types
  Doc bigDoc = flatdoc(
    "id", "doc1",
    "text_w", "hello world fulltext search",
    "string_s", "single_value",
    "string_ss", std::vector<std::string>{"multi1", "multi2", "multi3"},
    "int_i", 42,
    "int_is", std::vector<int64_t>{100, 200, 300}
  );
  helper.index(bigDoc, UpdateMessage::COMMIT, true);
  
  // Add dummy document to prevent empty segment
  Doc dummyDoc = flatdoc("id", "doc2");
  helper.index(dummyDoc, UpdateMessage::NO_COMMIT, true);
  
  // Delete the big document
  helper.deleteById("doc1", UpdateMessage::NO_COMMIT);
  helper.commit();

  indexWriter->mergeSegments();  // synchronous merge

  // Get fresh reader after merge
  auto reader = indexWriter->getIndexReader();
  ASSERT_EQ(1, reader->segments().size());
  ASSERT_EQ(reader->liveDocs(), reader->maxDoc());

  auto guard = MemPool::threadLocalPoolGuard();
  // Verify fields from deleted document are gone after merge
  for (const auto& segment : reader->segments()) {
    FieldReader fieldsReader(segment.postingsReader());
    EXPECT_TRUE(fieldsReader.seek("id")); // id field should remain
    EXPECT_FALSE(fieldsReader.seek("text_w"));
    EXPECT_FALSE(fieldsReader.seek("string_s"));
    EXPECT_FALSE(fieldsReader.seek("string_ss"));
    EXPECT_FALSE(fieldsReader.seek("int_i"));
    EXPECT_FALSE(fieldsReader.seek("int_is"));
  }
}


// Test concurrent flush and commit - reproduces issue where an already
// flushing segment may not be included in commit
TEST_F(IndexWriterTest, concurrentFlushAndCommit) {
  RAMDir dir;
  IndexWriter iw(dir);
  int nDocs = 100;  // 100 docs was enough to reliably reproduce the issue with debug/asan at least
  
  // Create a large segment that takes time to flush
  auto& largeInverter = iw.obtainInverter();
  auto& largeFieldHandler = largeInverter.getIndexHandler(field);
  
  // Add many documents to make flush expensive
  for (int i = 0; i < nDocs; i++) {
    largeInverter.startDoc();
    std::string text = "document " + std::to_string(i) + " with some text to make it bigger";
    for (int j = 0; j < 10; j++) {
      text += " extra content " + std::to_string(j);
    }
    largeFieldHandler.index(largeInverter, text);
    largeInverter.finishDoc();
  }
  
  // Create a small segment that's cheap to flush
  auto& smallInverter = iw.obtainInverter();
  auto& smallFieldHandler = smallInverter.getIndexHandler(field);
  
  smallInverter.startDoc();
  smallFieldHandler.index(smallInverter, "tiny doc");
  smallInverter.finishDoc();
  
  // Release large inverter with immediate flush request
  // This starts an async flush of the large segment
  iw.releaseInverter(largeInverter, true);
  
  // Immediately release small inverter without flush request
  // This should be quick
  iw.releaseInverter(smallInverter, false);
  
  // Now commit - this should wait for the large segment flush to complete
  // If it doesn't, we'll be missing the large segment
  iw.commit();
  
  // Verify both segments are present
  auto reader = iw.getIndexReader();
  ASSERT_EQ(2, reader->segments().size());
  
  // Verify we have the correct number of documents
  int totalDocs = 0;
  bool foundLargeSegment = false;
  bool foundSmallSegment = false;
  
  for (const auto& segment : reader->segments()) {
    totalDocs += segment.maxDoc();
    if (segment.maxDoc() == nDocs) {
      foundLargeSegment = true;
    } else if (segment.maxDoc() == 1) {
      foundSmallSegment = true;
    }
  }
  
  EXPECT_TRUE(foundLargeSegment);
  EXPECT_TRUE(foundSmallSegment);
  EXPECT_EQ(nDocs + 1, totalDocs);
}

TEST_F(IndexWriterTest, dynamicFieldNameRules) {
  using namespace luxir::test;
  // Doc-supplied names hit the id-like check on first use; a template suffix
  // match must not admit an invalid name.
  CollectionHelper helper("dyn_field_names");
  auto result = helper.indexAll({
    flatdoc("id", "g1", "camelCase_s", "ok"),
    flatdoc("id", "b1", "bad-name_s", "rejected"),
  }, UpdateMessage::COMMIT);
  ASSERT_EQ(luxir::api::UpdateResponse_::Status::PARTIAL, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ("b1", result.errors[0].id);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("bad-name_s"));
}

// Test coreGen tracking and segment commit_time
TEST_F(IndexWriterTest, testCoreGen) {
  using namespace luxir::test;

  CollectionHelper helper("core_gen_test");
  helper.clear();

  auto iw = helper.getIndexWriter();

  // Get initial coreGen (may not be 0 due to persistent IndexWriter)
  auto reader1 = iw->getIndexReader();
  uint64_t initialCoreGen = reader1->coreGen();
  size_t initialSegments = reader1->segments().size();

  // Add multiple documents in single segment and commit
  std::vector<Doc> firstBatch;
  firstBatch.push_back(flatdoc("id", "doc1"));
  firstBatch.push_back(flatdoc("id", "doc2"));
  firstBatch.push_back(flatdoc("id", "doc3"));
  helper.indexAll(firstBatch, UpdateMessage::COMMIT, true);

  auto reader2 = iw->getIndexReader();
  EXPECT_EQ(initialCoreGen + 1, reader2->coreGen()); // First segment added
  EXPECT_EQ(initialSegments + 1, reader2->segments().size());
  
  // Check that the new segment has a commit_time set
  uint64_t firstSegmentCommitTime = 0;
  uint64_t firstSegmentId = 0;
  for (const auto& segment : reader2->segments()) {
    if (segment.segInfo.max_doc == 3) { // Find our newly added segment
      firstSegmentCommitTime = segment.segInfo.commit_time;
      firstSegmentId = segment.segInfo.seg_id;
      EXPECT_GT(firstSegmentCommitTime, 0) << "First segment should have commit_time set";
      break;
    }
  }

  // Add another batch in a new segment
  std::vector<Doc> secondBatch;
  secondBatch.push_back(flatdoc("id", "doc4"));
  secondBatch.push_back(flatdoc("id", "doc5"));
  helper.indexAll(secondBatch, UpdateMessage::COMMIT, true);

  auto reader3 = iw->getIndexReader();
  EXPECT_EQ(initialCoreGen + 2, reader3->coreGen()); // Second segment added
  EXPECT_EQ(initialSegments + 2, reader3->segments().size());
  
  // Check that the second segment has a different commit_time
  uint64_t secondSegmentCommitTime = 0;
  for (const auto& segment : reader3->segments()) {
    if (segment.segInfo.max_doc == 2) { // Find the second segment
      secondSegmentCommitTime = segment.segInfo.commit_time;
      EXPECT_GT(secondSegmentCommitTime, 0) << "Second segment should have commit_time set";
      EXPECT_GT(secondSegmentCommitTime, firstSegmentCommitTime);
    } else if (segment.segInfo.seg_id == firstSegmentId) {
      // Verify first segment's commit_time hasn't changed
      EXPECT_EQ(segment.segInfo.commit_time, firstSegmentCommitTime);
    }
  }

  // Delete a document - this should NOT increment coreGen if segment still has live docs
  uint64_t coreGenBeforeDelete = reader3->coreGen();
  helper.deleteById("doc1", UpdateMessage::COMMIT);

  auto reader4 = iw->getIndexReader();
  EXPECT_EQ(coreGenBeforeDelete, reader4->coreGen()); // No segment composition change
  EXPECT_EQ(initialSegments + 2, reader4->segments().size()); // Still have same number of segments
  
  // Verify that segment commit_times haven't changed after delete
  for (const auto& segment : reader4->segments()) {
    if (segment.segInfo.seg_id == firstSegmentId) {
      EXPECT_EQ(segment.segInfo.commit_time, firstSegmentCommitTime);
    } else if (segment.segInfo.max_doc == 2) {
      EXPECT_EQ(segment.segInfo.commit_time, secondSegmentCommitTime);
    }
  }

  // Merge segments - this should increment coreGen
  iw->mergePolicy->mergeFactor = 2; // Force merge
  iw->mergeSegments();

  // Add a doc to trigger commit after merge
  Doc doc6 = flatdoc("id", "doc6", field, "test document 6");
  helper.index(doc6, UpdateMessage::COMMIT, true);

  auto reader5 = iw->getIndexReader();
  EXPECT_GT(reader5->coreGen(), coreGenBeforeDelete); // Merge changed segments
  
  // Check that merged segment has a new commit_time
  uint64_t mergedSegmentCommitTime = 0;
  int mergedSegmentCount = 0;
  for (const auto& segment : reader5->segments()) {
    // The merged segment should have more docs than any individual segment before
    if (segment.segInfo.max_doc > 3) {
      mergedSegmentCommitTime = segment.segInfo.commit_time;
      mergedSegmentCount++;
      EXPECT_GT(mergedSegmentCommitTime, 0);
      EXPECT_GT(mergedSegmentCommitTime, secondSegmentCommitTime);
    }
  }
  EXPECT_EQ(mergedSegmentCount, 1);
}
