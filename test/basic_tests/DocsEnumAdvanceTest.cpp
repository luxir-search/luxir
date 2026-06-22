#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/TermQuery.h"
#include "solux/query/BooleanQuery.h"

using namespace solux;
using namespace solux::test;

namespace {

// First element of the sorted set that is >= target, else DocsEnum::END.
int32_t firstGE(const std::vector<int32_t>& sorted, int32_t target) {
  auto it = std::lower_bound(sorted.begin(), sorted.end(), target);
  return it == sorted.end() ? DocsEnum::END : *it;
}

}  // namespace

class DocsEnumAdvanceTest : public SoluxTest {};

// DocsEnum::advance(target) must return the first doc >= target, correctly
// crossing the 128-doc PFor block boundaries (and the StreamVByte tail).  That
// is exactly the path the postings skip-data work will rewrite, and today it is
// only a nextDoc() loop with no test that exceeds a single block (BooleanFuzz
// uses 48 docs).  This pins the advance() contract -- at the DocsEnum level and
// through a conjunction scorer -- so the skip-data version can be checked
// against the same expectations.
TEST_F(DocsEnumAdvanceTest, advanceAcrossBlocks) {
  // One text field, two terms of very different selectivity:
  //   "hot"  -> dense list spanning several full 128-doc blocks plus a tail
  //   "rare" -> sparse list (drives the conjunction leapfrog over "hot")
  // Docs are added at sparse, increasing ids, so a term's postings have gaps
  // and advance targets can land on present docs, in gaps, and on boundaries.
  std::vector<int32_t> hot;
  std::vector<int32_t> rare;  // subset of hot
  int32_t d = 0;
  for (int i = 0; i < 600; i++) {                 // 600 = 4 full blocks + 88 tail
    hot.push_back(d);
    if (rng.rint(0, 8) == 0) rare.push_back(d);   // ~1/8 of hot docs also rare
    d += rng.rint(1, 4);                           // gaps of 1..3
  }
  ASSERT_GT((int)hot.size(), 4 * Postings::DOCS_BLOCK_SIZE) << "want several full blocks";
  ASSERT_GT((int)rare.size(), 0);

  // Add some "rare" docs that are NOT hot, so the conjunction lead (rare) also
  // overshoots non-matching ids -- exercising advance-overshoot/re-advance.
  std::set<int32_t> rareSet(rare.begin(), rare.end());
  for (int i = 0; i < 30; i++) {
    int32_t id = (int32_t)rng.rint(0, hot.back() + 50);
    if (!std::binary_search(hot.begin(), hot.end(), id)) rareSet.insert(id);
  }

  std::set<int32_t> hotSet(hot.begin(), hot.end());
  std::set<int32_t> all(hotSet);
  all.insert(rareSet.begin(), rareSet.end());

  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t id : all) {  // std::set iterates ascending: ids are strictly increasing
    std::string text;
    if (hotSet.count(id)) text += "hot ";
    if (rareSet.count(id)) text += "rare";
    f.add(id, text);
  }
  testIndex.flush();
  f.startReading();

  // ---- Part 1: exhaustive advance() semantics on the dense "hot" list ----
  TermsEnum tenum = f.createTermsEnum();
  ASSERT_TRUE(tenum.seek("hot"));
  ASSERT_EQ(DocsEnum(testIndex.pool, f.currentSegment()->postingsReader(), tenum).numDocs(),
            (int)hot.size());

  // Targets: 0; each present doc and its neighbors (gap landings); every block
  // boundary; and past-the-end (-> END).
  std::vector<int32_t> targets{0, hot.back() + 1, hot.back() + 100000};
  for (int32_t h : hot) {
    targets.push_back(h);
    targets.push_back(h + 1);
    if (h > 0) targets.push_back(h - 1);
  }
  for (int ord = 0; ord < (int)hot.size(); ord += Postings::DOCS_BLOCK_SIZE) {
    targets.push_back(hot[ord]);
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

  // Fresh enum per target (advance is forward-only): advance-from-start landing
  // in any block.
  for (int32_t t : targets) {
    DocsEnum denum(testIndex.pool, f.currentSegment()->postingsReader(), tenum);
    ASSERT_EQ(denum.advance(t), firstGE(hot, t)) << "advance(" << t << ")";
  }

  // Real scorer usage: one enum, a monotonically increasing mix of nextDoc() and
  // advance() to random forward targets, checked against the reference list.
  {
    DocsEnum denum(testIndex.pool, f.currentSegment()->postingsReader(), tenum);
    int32_t cur = denum.nextDoc();
    ASSERT_EQ(cur, hot[0]);
    size_t idx = 0;
    while (cur != DocsEnum::END) {
      if (rng.rbool()) {
        cur = denum.nextDoc();
        idx++;
      } else {
        int32_t t = cur + 1 + (int32_t)rng.rint(0, 200);  // strictly forward
        cur = denum.advance(t);
        idx = (size_t)(std::lower_bound(hot.begin(), hot.end(), t) - hot.begin());
      }
      ASSERT_EQ(cur, idx < hot.size() ? hot[idx] : DocsEnum::END);
    }
  }

  // ---- Part 2: advance() through a conjunction scorer (hot AND rare) ----
  std::vector<int32_t> rareAll(rareSet.begin(), rareSet.end());
  std::vector<int32_t> expected;  // hot INTERSECT rare
  std::set_intersection(hot.begin(), hot.end(), rareAll.begin(), rareAll.end(),
                        std::back_inserter(expected));

  TermQuery hotQ("body_w", "hot");
  TermQuery rareQ("body_w", "rare");
  std::vector<Query*> mand = {&hotQ, &rareQ};
  BooleanQuery q(mand, {}, {}, {});  // pure conjunction

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
  Query::Scorer* scorer = weight->createScorer(testIndex.pool, qContext.topReader.segments()[0]);
  ASSERT_NE(scorer, nullptr);

  std::vector<int32_t> got;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    got.push_back(doc);
  }
  ASSERT_EQ(got, expected);
}
