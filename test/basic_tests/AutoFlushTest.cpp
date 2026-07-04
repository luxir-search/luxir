// Inverter auto-flush: the size-based flush is checked ONCE per update message, at
// the end of the batch (never mid-request). A whole message stays in one inverter,
// so within-request id overwrites stay correct; a non-stop stream is byte-batched
// into many messages that accumulate in the reused idle inverter, and the
// per-message check flushes it once it grows past a cap. See ProtoUpdateMessage.cpp
// and solux-private/inverter-autoflush.md for the rationale (mid-request flush was
// rejected because it splits a request's shared updateVersion across segments and
// breaks version-gated overwrite deletes).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

class AutoFlushTest : public SoluxTest {};

namespace {
// Restores an IndexWriter's auto-flush caps on scope exit. Tests share the "main"
// collection and clear() does not reset the caps, so a tiny test cap must be undone
// or it starves later tests' segments.
struct CapGuard {
  std::shared_ptr<IndexWriter> iw;
  size_t ram;
  size_t docs;
  explicit CapGuard(std::shared_ptr<IndexWriter> w)
    : iw(std::move(w)), ram(iw->perInverterRamBytes), docs(iw->perInverterMaxDocs) {}
  ~CapGuard() { iw->perInverterRamBytes = ram; iw->perInverterMaxDocs = docs; }
};
}  // namespace

// A single update message never splits mid-request, even far past the cap: it lands
// in ONE segment, and a duplicate id within the message is overwritten in memory
// (straddling would-be flush boundaries). If the request were split, the two copies
// would share one updateVersion and the earlier one could not be superseded.
TEST_F(AutoFlushTest, singleMessageKeptWholeAndOverwritesResolve) {
  CollectionHelper helper;
  helper.clear();
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
  helper.clear();
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
