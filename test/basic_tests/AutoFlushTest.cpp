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
