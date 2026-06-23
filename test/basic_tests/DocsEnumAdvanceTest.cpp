#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/TermQuery.h"
#include "solux/query/BooleanQuery.h"

using namespace solux;
using namespace solux::test;

// Fuzz for DocsEnum advance() and friends.
// Builds randomized indexes and replays random nextDoc()/advance()/position-read sequences against an
// independent model, checking doc id, term freq, and positions every step.
class DocsEnumAdvanceTest : public SoluxTest {
protected:
  struct Posting { int32_t docid; int32_t firstPos; int32_t tf; };

  static constexpr int VOCAB_SIZE = 40;       // t%9 sets density 1, 1/2, ... 1/256
  static constexpr int INDEX_ITERATIONS = 5;  // increase with WALKS_PER_TERM when changing DocsEnum
  static constexpr int WALKS_PER_TERM = 6;

  void checkAdvanceWalk(TestIndex& testIndex, PostingsReader& reader, TermsEnum& tenum,
                        const std::vector<Posting>& model, const std::string& term,
                        int maxDoc, int indexIter, int walk) {
    SCOPED_TRACE(::testing::Message() << "indexIter=" << indexIter << " term=" << term << " walk=" << walk);

    DocsEnum denum(testIndex.pool, reader, tenum);
    ASSERT_EQ(denum.numDocs(), (int) model.size()) << term;

    auto byDoc = [](const Posting& p, int32_t v) { return p.docid < v; };
    size_t i = 0;       // model index of the next doc the enum will return
    int32_t cur = -1;
    for (;;) {
      size_t j;         // model index this op must land on
      if (rng.rbool()) {
        cur = denum.nextDoc();
        j = i;
      } else {
        int32_t span = rng.rbool() ? 200 : maxDoc;  // small step or far jump
        int32_t target = cur + 1 + (int32_t) rng.rint(0, span + 1);
        cur = denum.advance(target);
        j = (size_t) (std::lower_bound(model.begin(), model.end(), target, byDoc) - model.begin());
      }
      int32_t expected = j < model.size() ? model[j].docid : DocsEnum::END;
      ASSERT_EQ(cur, expected) << term;
      if (cur == DocsEnum::END) break;
      ASSERT_EQ(denum.termFreq(), model[j].tf) << term << " tf at doc " << cur;

      if (rng.rbool()) {
        denum.startPositions();
        int toRead = (int) rng.rint(0, model[j].tf + 1);
        for (int k = 0; k < toRead; k++) {
          ASSERT_EQ(denum.nextPosition(), model[j].firstPos + k) << term << " pos " << k << " doc " << cur;
        }
        if (toRead == model[j].tf) {
          ASSERT_EQ(denum.nextPosition(), DocsEnum::END) << term << " pos end doc " << cur;
        }
      }
      i = j + 1;
    }
  }

  void runAdvanceFuzzIndex(int indexIter, int walksPerTerm) {
    // indexIter 0 ends exactly on a block boundary and is sized to span multiple L1
    // skip groups (L1 period is 32 blocks): 72 blocks => t0 (every doc) covers 2 full
    // groups + a partial third, so far advances exercise the L1 group step-over and
    // mid-density terms land their tails in later groups.  Later indexes use arbitrary sizes.
    const int N = indexIter == 0 ? 72 * Postings::DOCS_BLOCK_SIZE : 500 + (int) rng.rint(0, 2000);

    std::vector<std::vector<Posting>> model(VOCAB_SIZE);
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    std::string text;
    for (int d = 0; d < N; d++) {
      text.clear();
      int32_t posn = 0;
      for (int t = 0; t < VOCAB_SIZE; t++) {
        if (rng.rint(0, 1 << (t % 9)) != 0) continue;
        int tf = 1 + (int) rng.rint(0, 3);
        model[t].push_back({d, posn, tf});
        for (int k = 0; k < tf; k++) { text += 't'; text += std::to_string(t); text += ' '; }
        posn += tf;
      }
      f.add(d, text);  // t0 is present in every doc, so maxDoc == N
    }
    testIndex.flush();
    f.startReading();

    auto& reader = f.currentSegment()->postingsReader();
    for (int t = 0; t < VOCAB_SIZE; t++) {
      const auto& m = model[t];
      if (m.empty()) continue;
      std::string term = "t" + std::to_string(t);
      TermsEnum tenum = f.createTermsEnum();
      ASSERT_TRUE(tenum.seek(term)) << term;

      for (int walk = 0; walk < walksPerTerm; walk++) {
        checkAdvanceWalk(testIndex, reader, tenum, m, term, N, indexIter, walk);
      }
    }
  }
};


TEST_F(DocsEnumAdvanceTest, advanceFuzz) {
  for (int iter = 0; iter < INDEX_ITERATIONS; iter++) {
    runAdvanceFuzzIndex(iter, WALKS_PER_TERM);
  }
}

// DOCS-only fields use the shorter skip payload (no cumTf) and an implicit term
// frequency of 1 -- a layout the positions-field fuzz above never exercises.
TEST_F(DocsEnumAdvanceTest, advanceDocsOnly) {
  const int N = 400;  // dense ids 0..399 => 3 full blocks + tail
  TestIndex testIndex;
  TestField f(testIndex, "tag_s");
  f.startIndexing();
  for (int d = 0; d < N; d++) f.add(d, std::string_view("x"));  // term "x" in every doc
  testIndex.flush();
  f.startReading();

  TermsEnum tenum = f.createTermsEnum();
  ASSERT_TRUE(tenum.seek("x"));
  ASSERT_EQ(DocsEnum(testIndex.pool, f.currentSegment()->postingsReader(), tenum).numDocs(), N);

  // Random forward walk on one enum (resumes the skip cursor, never restarts at 0).
  DocsEnum denum(testIndex.pool, f.currentSegment()->postingsReader(), tenum);
  int32_t cur = -1;
  while (cur != DocsEnum::END) {
    int32_t target = cur + 1 + (int32_t) rng.rint(0, rng.rbool() ? 5 : N);
    cur = denum.advance(target);
    ASSERT_EQ(cur, target < N ? target : DocsEnum::END) << "advance(" << target << ")";  // dense -> exact
    if (cur != DocsEnum::END) { ASSERT_EQ(denum.termFreq(), 1); }
  }
}

// A conjunction's advance() must route through the skip list (TermQuery::Scorer
// forwards advance() to DocsEnum::advance()).  The sparse term leads and advance()s
// the dense one across many blocks; some sparse docs are not dense, exercising the
// advance-overshoot/re-advance path.  The result must equal the true intersection.
TEST_F(DocsEnumAdvanceTest, conjunctionLeapfrog) {
  const int N = 1500;
  std::vector<int32_t> dense, sparse;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int d = 0; d < N; d++) {
    std::string text;
    if (rng.rint(0, 3) != 0) { text += "dense "; dense.push_back(d); }    // ~2/3 of docs, spans many blocks
    if (rng.rint(0, 20) == 0) { text += "sparse"; sparse.push_back(d); }  // ~1/20, some not in dense
    if (!text.empty()) f.add(d, text);                                    // skip docs with neither term
  }
  testIndex.flush();
  f.startReading();

  std::vector<int32_t> expected;  // dense INTERSECT sparse
  std::set_intersection(dense.begin(), dense.end(), sparse.begin(), sparse.end(),
                        std::back_inserter(expected));

  TermQuery denseQ("body_w", "dense");
  TermQuery sparseQ("body_w", "sparse");
  std::vector<Query*> mand = {&denseQ, &sparseQ};
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
