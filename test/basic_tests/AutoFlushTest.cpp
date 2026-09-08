// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Inverter auto-flush: the size-based flush is checked ONCE per update message, at
// the end of the batch (never mid-request). A whole message stays in one inverter,
// so within-request id overwrites stay correct; a non-stop stream is byte-batched
// into many messages that accumulate in the reused idle inverter, and the
// per-message check flushes it once it grows past a cap. Mid-request flush is
// rejected because it splits a request's shared updateVersion across segments and
// breaks version-gated overwrite deletes. See ProtoUpdateMessage.cpp.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "luxir/index/IndexRamBudget.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/MergeCostModel.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/IndexReader.h"
#include "luxir/store/Directory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace luxir;
using namespace luxir::test;

class AutoFlushTest : public LuxirTest {};

namespace {
// Restores an IndexWriter's auto-flush caps on scope exit. Tests share the "main"
// collection and clear() does not reset the caps, so a tiny test cap must be undone
// or it starves later tests' segments.
struct CapGuard {
  std::shared_ptr<IndexWriter> iw;
  size_t ram;
  size_t docs;
  size_t floor;
  explicit CapGuard(std::shared_ptr<IndexWriter> w)
    : iw(std::move(w)), ram(iw->perInverterRamBytes), docs(iw->perInverterMaxDocs),
      floor(iw->pressureFlushFloorBytes) {}
  ~CapGuard() {
    iw->perInverterRamBytes = ram;
    iw->perInverterMaxDocs = docs;
    iw->pressureFlushFloorBytes = floor;
  }
};

// Restores the node-wide IndexRamBudget cap on scope exit (the node is shared
// across tests).
struct BudgetTotalGuard {
  IndexRamBudget& budget;
  int64_t saved;
  explicit BudgetTotalGuard(IndexRamBudget& b) : budget(b), saved(b.totalBytes()) {}
  ~BudgetTotalGuard() { budget.setTotalBytes(saved); }
};
}  // namespace

// A single update message never splits mid-request, even far past the cap: it lands
// in ONE segment, and a duplicate id within the message is overwritten in memory
// (straddling would-be flush boundaries). If the request were split, the two copies
// would share one updateVersion and the earlier one could not be superseded.
TEST_F(AutoFlushTest, singleMessageKeptWholeAndOverwritesResolve) {
  CollectionHelper helper;
  CapGuard capGuard(helper.getIndexWriter());
  helper.getIndexWriter()->perInverterMaxDocs = 5;  // tiny cap: a split, if it happened, would be visible

  const int nDocs = 23;
  std::vector<Doc> docs;
  for (int i = 0; i < nDocs; i++) {
    // Two docs (indices 1 and 17, straddling the 5/10/15 would-be flush points) reuse id "dup".
    std::string id = (i == 1 || i == 17) ? "dup" : ("id" + std::to_string(i));
    docs.push_back(flatdoc("id", id, "foo_w", "brown fox jumped"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT, /*overwrite=*/true);  // one message, overwrite mode

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 1u)
      << "a single message must stay in one inverter/segment (no mid-request flush)";

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().withStats().limit(100);
  req->execute();
  ASSERT_OK(req);
  // 23 docs, two share id "dup" -> 22 unique live docs (the earlier "dup" was overwritten).
  EXPECT_EQ(req->getMatchCount("q"), nDocs - 1);
}

// A non-stop stream is many small messages that accumulate in the reused idle
// inverter; the per-message end-of-batch check flushes it once it passes the cap,
// so a run of small messages produces multiple bounded segments.
TEST_F(AutoFlushTest, accumulationAcrossMessagesFlushesAtBoundaries) {
  CollectionHelper helper;
  CapGuard capGuard(helper.getIndexWriter());
  helper.getIndexWriter()->perInverterMaxDocs = 5;

  const int nDocs = 24;
  for (int i = 0; i < nDocs; i++) {
    // One doc per message, no commit: each message reuses+grows the same idle inverter.
    helper.index(flatdoc("id", "id" + std::to_string(i), "foo_w", "brown fox jumped"),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();

  auto reader = helper.getIndexWriter()->getIndexReader();
  EXPECT_GT(reader->segments().size(), 1u)
      << "per-message flush should bound accumulation into multiple segments";

  int64_t totalDocs = 0;
  for (const auto& seg : reader->segments()) totalDocs += seg.postingsReader().maxDoc();
  EXPECT_EQ(totalDocs, nDocs);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().withStats().limit(100);
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(req->getMatchCount("q"), nDocs);
}

// Waits for the budget's reservations to drain back to at most `bound` (the
// last inverter is destroyed shortly AFTER its flush is observable, so a
// post-commit check must poll).
static void awaitDrain(IndexRamBudget& budget, int64_t bound) {
  for (int i = 0; i < 5000 && budget.reservedBytes() > bound; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_LE(budget.reservedBytes(), bound) << "inverter RAM accounting leaked";
}

// Global indexing RAM budget: an over-budget release flushes the largest idle
// inverter even though no per-inverter cap was hit, so accumulation across
// messages is also bounded by the shared (node-wide) budget.
TEST_F(AutoFlushTest, overBudgetReleaseFlushesIdleInverter) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  CapGuard capGuard(iw);
  iw->pressureFlushFloorBytes = 1;  // any idle inverter is a valid victim

  auto& budget = LuxirTest::luxirNode->getIndexRamBudget();
  BudgetTotalGuard budgetGuard(budget);
  const int64_t baselineReserved = budget.reservedBytes();
  budget.setTotalBytes(1);  // any accounted inverter RAM overdraws the cap

  const int nDocs = 8;
  for (int i = 0; i < nDocs; i++) {
    // One doc per message. The first release accounts the inverter's RAM, trips
    // the budget, and pressure-flushes it from the idle pool (no commit involved).
    helper.index(flatdoc("id", "id" + std::to_string(i), "foo_w", "brown fox jumped"),
                 UpdateMessage::NO_COMMIT);
  }
  budget.setTotalBytes(budgetGuard.saved);  // commit-time flushes/merges run unpressured
  helper.commit();

  auto reader = iw->getIndexReader();
  EXPECT_GT(reader->segments().size(), 1u)
      << "an over-budget release should flush an idle inverter into its own segment";

  int64_t totalDocs = 0;
  for (const auto& seg : reader->segments()) totalDocs += seg.postingsReader().maxDoc();
  EXPECT_EQ(totalDocs, nDocs);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().withStats().limit(100);
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(req->getMatchCount("q"), nDocs);

  // The release side: every inverter was destroyed after the commit, so the
  // guards returned all accounted bytes.
  awaitDrain(budget, baselineReserved);
}

// The floor: an over-budget release does not shed inverters smaller than
// pressureFlushFloorBytes - tiny segments give no meaningful RAM relief (e.g.
// when merge reservations hold most of the budget).
TEST_F(AutoFlushTest, pressureFlushRespectsFloor) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  CapGuard capGuard(iw);  // floor left at its default, far above these tiny inverters

  auto& budget = LuxirTest::luxirNode->getIndexRamBudget();
  BudgetTotalGuard budgetGuard(budget);
  budget.setTotalBytes(1);

  const int nDocs = 8;
  for (int i = 0; i < nDocs; i++) {
    helper.index(flatdoc("id", "id" + std::to_string(i), "foo_w", "brown fox jumped"),
                 UpdateMessage::NO_COMMIT);
  }
  budget.setTotalBytes(budgetGuard.saved);
  helper.commit();

  auto reader = iw->getIndexReader();
  EXPECT_EQ(reader->segments().size(), 1u)
      << "below-floor inverters must not be pressure-flushed";
}

// The pressure gate allows concurrent sheds and discounts in-flight flushes:
// while the pool stays over cap net of flushes already draining, every release
// sheds another idle inverter (the old gate allowed only one shed in flight,
// capping the drain rate at one single-threaded flush); once the in-flight
// flushes cover the overage, fresh inverters are NOT shed, so a shed burst
// does not respawn as a stream of floor-sized segments.  Flushes are held in
// flight via the segmentFlushBody test seam.
TEST(PressureShedDirectTest, concurrentShedsWithInFlightDiscount) {
  RAMDir dir;
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");

  IndexRamBudget budget(0);  // per-step caps set below
  std::atomic<int> flushesStarted{0};
  std::atomic<bool> releaseFlushes{false};
  Signal::listen("segmentFlushBody", [&](void*, void*, void*) -> void* {
    flushesStarted.fetch_add(1);
    while (!releaseFlushes.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return nullptr;
  });
  auto cleanup = luxir::scope_guard([]() { Signal::unlisten("segmentFlushBody"); });

  {
    IndexWriter iw(dir, [&]() { return schema; }, &budget);
    iw.pressureFlushFloorBytes = 1;  // every inverter is a valid victim

    // Three concurrently-busy inverters so obtainInverter cannot reuse an
    // idle one: A and B sizable, C small.
    auto addDocs = [](Inverter& inv, int n) {
      for (int i = 0; i < n; i++) {
        inv.startDoc();
        inv.getIndexHandler("body").index(
            inv, std::string_view("some repeated body text for sizing"));
        inv.finishDoc();
      }
    };
    auto& invA = iw.obtainInverter();
    addDocs(invA, 30);
    auto& invB = iw.obtainInverter();
    addDocs(invB, 30);
    auto& invC = iw.obtainInverter();
    addDocs(invC, 1);

    budget.setTotalBytes(1);  // any accounted inverter RAM overdraws the cap
    iw.releaseInverter(invA);  // resyncs A's guard, over cap -> shed A
    int64_t rA = budget.reservedBytes();
    ASSERT_GT(rA, 1);
    iw.releaseInverter(invB);  // over cap even net of A's in-flight flush -> shed B too
    for (int i = 0; i < 5000 && flushesStarted.load() < 2; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(flushesStarted.load(), 2)
        << "second over-budget release must shed with a flush already in flight";

    // Cap between C's size and A+B: the pool is over cap, but the in-flight
    // flushes cover the overage, so releasing C must not shed it.
    int64_t rAB = budget.reservedBytes();
    budget.setTotalBytes(rAB / 2);
    iw.releaseInverter(invC);
    int64_t rC = budget.reservedBytes() - rAB;
    ASSERT_LT(rC, rAB / 2) << "test premise: C must fit under the cap alone";
    ASSERT_GT(budget.reservedBytes(), rAB / 2) << "test premise: pool over cap";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(flushesStarted.load(), 2)
        << "in-flight flushes cover the overage; C must not be shed";

    budget.setTotalBytes(0);  // unlimited: commit-time flushes run unpressured
    releaseFlushes.store(true);
    iw.commit();

    auto reader = iw.getIndexReader();
    EXPECT_EQ(reader->segments().size(), 3u);  // A shed, B shed, C at commit
    int64_t totalDocs = 0;
    for (const auto& seg : reader->segments()) totalDocs += seg.postingsReader().maxDoc();
    EXPECT_EQ(totalDocs, 61);
  }
}

// Demand publication itself must wake an idle writer.  There is deliberately
// no releaseInverter call after demand appears: without the pressure-listener
// registry this inverter would pin the pool forever and the merge driver would
// have no writer-side event on which to trigger shedding.
TEST(PressureShedDirectTest, mergeDemandWakesIdleWriter) {
  RAMDir dir;
  auto schema = std::make_shared<Schema>();
  schema->fieldTypeMap["body"] = std::make_shared<TextFieldType>(
      "body", FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");

  IndexRamBudget budget;
  std::atomic<int> flushesStarted = 0;
  Signal::listen("segmentFlushBody", [&](void*, void*, void*) -> void* {
    flushesStarted.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  });
  auto cleanup = luxir::scope_guard([]() { Signal::unlisten("segmentFlushBody"); });

  {
    IndexWriter iw(dir, [&]() { return schema; }, &budget);
    iw.pressureFlushFloorBytes = 1;
    auto& inverter = iw.obtainInverter();
    for (int i = 0; i < 30; i++) {
      inverter.startDoc();
      inverter.getIndexHandler("body").index(
          inverter, std::string_view("some repeated body text for sizing"));
      inverter.finishDoc();
    }
    iw.releaseInverter(inverter);

    int64_t parked = budget.reservedBytes();
    ASSERT_GT(parked, 0);
    budget.setTotalBytes(parked);
    auto demand = budget.registerMergeDemand([]() {});
    demand.publish(1);

    for (int i = 0; i < 5000 && flushesStarted.load(std::memory_order_relaxed) == 0; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(1, flushesStarted.load(std::memory_order_relaxed));

    demand.publish(0);
    budget.setTotalBytes(0);
    iw.commit();
    EXPECT_EQ(1u, iw.getIndexReader()->segments().size());
  }
}

// Hold the demand-triggered inverter flush.  Once it releases its reservation,
// the budget callback must admit multiple field batches, which are then held at
// segmentMergeBody.  This is the regression shape: without published demand the
// idle inverter parks the pool and merging remains serial until that inverter
// happens to be released by some unrelated event.
TEST(PressureShedDirectTest, mergeDemandRestoresParallelAdmission) {
  RAMDir dir;
  auto schema = std::make_shared<Schema>();
  for (int field = 0; field < 6; field++) {
    std::string name = "text" + std::to_string(field);
    schema->fieldTypeMap[name] = std::make_shared<TextFieldType>(
        name, FieldType::INDEX_DOCS_FREQS_POSITIONS, "whitespace");
  }

  IndexRamBudget budget;
  IndexWriter iw(dir, [&]() { return schema; }, &budget);
  iw.mergePolicy->setMergeFactor(2);
  iw.termPartitionMinBytes = INT64_MAX;
  iw.pressureFlushFloorBytes = 1;

  auto addDoc = [&](Inverter& inverter, int ord) {
    inverter.startDoc();
    for (int field = 0; field < 6; field++) {
      std::string name = "text" + std::to_string(field);
      std::string value = "common term" + std::to_string(ord);
      inverter.getIndexHandler(name).index(inverter, value);
    }
    inverter.finishDoc();
  };

  auto& firstSource = iw.obtainInverter();
  addDoc(firstSource, 0);
  iw.releaseInverter(firstSource, true);
  iw.commit();
  ASSERT_EQ(1u, iw.getIndexReader()->segments().size());

  // Keep the parked inverter busy while obtaining the second source inverter,
  // then hold that source flush.  Releasing the parked inverter while the source
  // is held leaves it idle before the source flush triggers the automatic merge.
  auto& parkedInverter = iw.obtainInverter();
  addDoc(parkedInverter, 10);
  auto& secondSource = iw.obtainInverter();
  addDoc(secondSource, 1);

  std::atomic<int> sourceFlushesStarted = 0;
  std::atomic<int> pressureFlushesStarted = 0;
  std::atomic<int> mergeBodies = 0;
  std::atomic<int64_t> maxDemand = 0;
  std::atomic<int32_t> maxHypotheticalStreams = 0;
  std::atomic<bool> releaseSourceFlush = false;
  std::atomic<bool> releasePressureFlush = false;
  std::atomic<bool> releaseMerges = false;
  auto cleanup = luxir::scope_guard([&]() {
    releaseSourceFlush.store(true, std::memory_order_relaxed);
    releasePressureFlush.store(true, std::memory_order_relaxed);
    releaseMerges.store(true, std::memory_order_relaxed);
    iw.close();
    Signal::unlisten("segmentFlushBody");
    Signal::unlisten("segmentMergeBody");
    Signal::unlisten("mergeAdmissionDemand");
  });

  Signal::listen("segmentFlushBody", [&](void* a, void*, void*) -> void* {
    if (a == &secondSource) {
      sourceFlushesStarted.fetch_add(1, std::memory_order_relaxed);
      while (!releaseSourceFlush.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }
    } else if (a == &parkedInverter) {
      pressureFlushesStarted.fetch_add(1, std::memory_order_relaxed);
      while (!releasePressureFlush.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }
    }
    return nullptr;
  });
  Signal::listen("segmentMergeBody", [&](void*, void*, void*) -> void* {
    mergeBodies.fetch_add(1, std::memory_order_relaxed);
    while (!releaseMerges.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    return nullptr;
  });
  Signal::listen("mergeAdmissionDemand", [&](void* a, void* b, void*) -> void* {
    int64_t demand = *(int64_t*)a;
    int64_t previous = maxDemand.load(std::memory_order_relaxed);
    while (previous < demand
           && !maxDemand.compare_exchange_weak(
               previous, demand, std::memory_order_relaxed)) {
    }
    int32_t streams = *(int32_t*)b;
    int32_t previousStreams = maxHypotheticalStreams.load(std::memory_order_relaxed);
    while (previousStreams < streams
           && !maxHypotheticalStreams.compare_exchange_weak(
               previousStreams, streams, std::memory_order_relaxed)) {
    }
    return nullptr;
  });

  iw.releaseInverter(secondSource, true);
  for (int i = 0; i < 5000
                  && sourceFlushesStarted.load(std::memory_order_relaxed) == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(1, sourceFlushesStarted.load(std::memory_order_relaxed));

  iw.releaseInverter(parkedInverter);
  int64_t cap = 16 * MergeCostModel::LIGHT_BYTES;
  parkedInverter.ramGuard.forceResize(cap);
  budget.setTotalBytes(cap);
  releaseSourceFlush.store(true, std::memory_order_relaxed);

  for (int i = 0; i < 5000
                  && pressureFlushesStarted.load(std::memory_order_relaxed) == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(1, pressureFlushesStarted.load(std::memory_order_relaxed));
  EXPECT_EQ(6 * (MergeCostModel::LIGHT_BYTES + 4),
            maxDemand.load(std::memory_order_relaxed))
      << "all six text batches fit the expanded virtual stream schedule";
  EXPECT_EQ(18, maxHypotheticalStreams.load(std::memory_order_relaxed));

  releasePressureFlush.store(true, std::memory_order_relaxed);
  for (int i = 0; i < 5000 && mergeBodies.load(std::memory_order_relaxed) < 2; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_GE(mergeBodies.load(std::memory_order_relaxed), 2)
      << "flush release must admit another batch while the head remains held";

  releaseMerges.store(true, std::memory_order_relaxed);
  for (int i = 0; i < 5000 && iw.testMergeRunning(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(iw.testMergeRunning());
  EXPECT_EQ(0, budget.pendingMergeDemandBytes());
  budget.setTotalBytes(0);
  iw.commit();
}
