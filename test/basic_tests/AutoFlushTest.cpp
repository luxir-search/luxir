// Inverter auto-flush: a single indexing request that grows past IndexWriter's
// per-inverter caps flushes the current inverter to a segment mid-request and
// continues into a fresh one, bounding the RAM one (e.g. non-stop-stream) request
// holds.  Only non-atomic requests may flush mid-request; an all_or_none request
// keeps every doc in one inverter so a later failure can roll the whole request back.

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
// Restores an IndexWriter's auto-flush caps on scope exit.  Tests share one "main"
// collection (getCollection ignores the name for now) and clear() does not reset the
// caps, so a tiny test cap must be undone or it starves later tests' segments.
struct CapGuard {
  std::shared_ptr<IndexWriter> iw;
  size_t ram;
  size_t docs;
  explicit CapGuard(std::shared_ptr<IndexWriter> w)
    : iw(std::move(w)), ram(iw->perInverterRamBytes), docs(iw->perInverterMaxDocs) {}
  ~CapGuard() { iw->perInverterRamBytes = ram; iw->perInverterMaxDocs = docs; }
};
}  // namespace

// A non-atomic request of many docs, with a small doc cap, must flush to more than
// one segment, and every doc must be durable + searchable after commit.
TEST_F(AutoFlushTest, nonAtomicAutoFlushesToMultipleSegments) {
  CollectionHelper helper;
  helper.clear();
  CapGuard capGuard(helper.getIndexWriter());
  helper.getIndexWriter()->perInverterMaxDocs = 5;  // force a flush every few docs

  const int nDocs = 23;
  std::vector<Doc> docs;
  for (int i = 0; i < nDocs; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "foo_w", "brown fox jumped"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);  // one non-atomic message, then commit

  auto reader = helper.getIndexWriter()->getIndexReader();
  EXPECT_GT(reader->segments().size(), 1u)
      << "expected mid-request auto-flush to produce multiple segments";

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

// An all_or_none request must NOT auto-flush mid-request even past the cap: all docs
// stay in one inverter (one segment) so the whole request could still be rolled back.
TEST_F(AutoFlushTest, allOrNoneDoesNotAutoFlushMidRequest) {
  CollectionHelper helper;
  helper.clear();
  CapGuard capGuard(helper.getIndexWriter());
  helper.getIndexWriter()->perInverterMaxDocs = 5;

  const int nDocs = 23;
  CollectionHelper::UpdateBuilder b;
  for (int i = 0; i < nDocs; i++) {
    b.add(flatdoc("id", std::to_string(i), "foo_w", "brown fox jumped"));
  }
  b.allOrNone(true);
  b.commit();
  auto result = helper.submit(b);
  EXPECT_TRUE(result.success);

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 1u)
      << "all_or_none must keep every doc in one inverter (no mid-request flush)";
  EXPECT_EQ(reader->segments()[0].postingsReader().maxDoc(), nDocs);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().withStats().limit(100);
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(req->getMatchCount("q"), nDocs);
}
