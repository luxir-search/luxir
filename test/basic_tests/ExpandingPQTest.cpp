#include <algorithm>
#include <limits>
#include <vector>

#include "solux/search/Collector.h"
#include "solux/util/heap.h"
#include "test/SoluxTest.h"

namespace solux::test {

namespace {
struct IntLess {
  bool operator()(int a, int b) const { return a < b; }
};
}

// Max-heap of bounded size keeps the largest values; storage grows on demand
// instead of allocating maxSize up front.
TEST(ExpandingPQTest, GrowsOnDemandAndKeepsTopK) {
  ExpandingPQ<int, IntLess> pq(5);
  ASSERT_EQ(pq.size(), 0u);
  ASSERT_EQ(pq.capacity(), 5u);

  // Under capacity: every insert is kept, size tracks inserts exactly.
  for (int v : {7, 3, 9}) {
    ASSERT_FALSE(pq.insertWithOverflow(v));
  }
  ASSERT_EQ(pq.size(), 3u);
  ASSERT_EQ(pq.top(), 9);

  for (int v : {1, 5}) {
    pq.insertWithOverflow(v);
  }
  ASSERT_EQ(pq.size(), 5u);

  // Full: a competitive value evicts the max, a losing value is a no-op.
  ASSERT_TRUE(pq.insertWithOverflow(2));   // ejects 9
  ASSERT_FALSE(pq.insertWithOverflow(8));  // 8 > current max 7, rejected
  ASSERT_EQ(pq.size(), 5u);

  std::vector<int> kept(pq.span().begin(), pq.span().end());
  std::sort(kept.begin(), kept.end());
  ASSERT_EQ(kept, (std::vector<int>{1, 2, 3, 5, 7}));
}

// A bound past the constructor's initial reservation forces vector reallocation while
// the heap is live; the invariant must survive every growth step.
TEST(ExpandingPQTest, GrowthReallocationKeepsHeapAndTopK) {
  constexpr size_t cap = 130;
  ExpandingPQ<int, IntLess> pq(cap);
  std::vector<int> vals;
  for (int i = 0; i < 400; i++) {
    vals.push_back((i * 919) % 1000);
  }
  for (int v : vals) {
    pq.insertWithOverflow(v);
    ASSERT_TRUE(std::is_heap(pq.span().begin(), pq.span().end(), IntLess{}));
    // Growth doubles then clamps: the backing allocation never runs past the
    // bound the way push_back's own doubling would (64 -> 128 -> 130 here).
    ASSERT_LE(pq.storageCapacity(), cap);
  }
  ASSERT_EQ(pq.size(), cap);
  ASSERT_EQ(pq.storageCapacity(), cap);

  std::vector<int> kept(pq.span().begin(), pq.span().end());
  std::sort(kept.begin(), kept.end());
  std::sort(vals.begin(), vals.end());
  vals.resize(cap);
  ASSERT_EQ(kept, vals);
}

// release() hands back exactly the live heap contents so a caller can sort or
// keep them without copying out of the queue.
TEST(ExpandingPQTest, ReleaseMovesOutLiveContents) {
  ExpandingPQ<int, IntLess> pq(3);
  for (int v : {4, 1, 3, 2}) {
    pq.insertWithOverflow(v);
  }
  std::vector<int> vals = pq.release();
  ASSERT_EQ(pq.size(), 0u);
  std::sort(vals.begin(), vals.end());
  ASSERT_EQ(vals, (std::vector<int>{1, 2, 3}));
}

// End-to-end across the growth boundary: sliced collection + merge must equal unsliced
// collection exactly (deterministic (score, seg, doc) total order at heavy score ties),
// and the competitive threshold goes live at exactly topCount collected docs.
TEST(ExpandingPQTest, TopDocsCollectorSliceMergeEquivalence) {
  constexpr int64_t k = 100;
  constexpr int32_t nDocs = 500;
  auto scoreOf = [](int32_t doc) { return float(doc % 7); };

  TopDocsCollector whole(k);
  TopDocsCollector sliceA(k);
  TopDocsCollector sliceB(k);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    if (doc == (int32_t)k - 1) {
      ASSERT_EQ(whole.minCompetitiveVal, std::numeric_limits<float>::lowest());
    }
    whole.collect(0, doc, scoreOf(doc));
    if (doc == (int32_t)k - 1) {
      ASSERT_GT(whole.minCompetitiveVal, std::numeric_limits<float>::lowest());
    }
    (doc % 2 == 0 ? sliceA : sliceB).collect(0, doc, scoreOf(doc));
  }

  sliceA.merge(sliceB);
  ASSERT_EQ(sliceA.totalHits(), whole.totalHits());
  ASSERT_EQ(sliceA.minCompetitiveVal, whole.minCompetitiveVal);

  auto merged = sliceA.sort();
  auto expected = whole.sort();
  ASSERT_EQ(merged.size(), expected.size());
  ASSERT_EQ(merged.size(), (size_t)k);
  for (size_t i = 0; i < merged.size(); i++) {
    ASSERT_EQ(merged[i].score, expected[i].score);
    ASSERT_EQ(merged[i].doc, expected[i].doc);
  }
  // Best doc overall: highest score, smallest (seg, doc) among its ties.
  ASSERT_EQ(merged[0].score, 6.0f);
  ASSERT_EQ(merged[0].doc, segdoc(0, 6));
}

}  // namespace solux::test
