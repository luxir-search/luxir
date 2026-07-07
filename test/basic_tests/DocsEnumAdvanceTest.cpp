#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/TermQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/search/Similarity.h"

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

  static int32_t impactTfForDoc(int32_t docid) {
    int32_t block = docid / Postings::DOCS_BLOCK_SIZE;
    return 1 + ((block * 7 + (docid % 3)) % 23);
  }

  static int32_t impactTokenCountForDoc(int32_t docid) {
    return impactTfForDoc(docid) + 3 + ((docid * 11) % 67);
  }

  static std::vector<int32_t> expectedBlockMaxTf(int32_t numDocs, bool hasFreqs) {
    int32_t numBlocks = (numDocs + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> expected(numBlocks, hasFreqs ? 0 : 1);
    if (!hasFreqs) {
      return expected;
    }
    for (int32_t doc = 0; doc < numDocs; doc++) {
      int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
      expected[block] = std::max(expected[block], impactTfForDoc(doc));
    }
    return expected;
  }

  static std::vector<int32_t> expectedBlockMinNorm(int32_t numDocs, bool hasNorms) {
    int32_t numBlocks = (numDocs + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    std::vector<int32_t> expected(numBlocks, hasNorms ? 255 : 0);
    if (!hasNorms) {
      return expected;
    }
    for (int32_t doc = 0; doc < numDocs; doc++) {
      int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
      int32_t norm = SmallFloat::intToByte4(impactTokenCountForDoc(doc));
      expected[block] = std::min(expected[block], norm);
    }
    return expected;
  }

  static DocsEnum::ImpactFrontiers expectedBlockFrontiers(int32_t numDocs,
                                                          bool hasFreqs,
                                                          bool hasNorms) {
    int32_t numBlocks = (numDocs + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    DocsEnum::ImpactFrontiers expected;
    expected.offsets.reserve((size_t) numBlocks + 1);
    std::array<int32_t, 256> maxTfPerNorm;
    for (int32_t block = 0; block < numBlocks; block++) {
      expected.offsets.push_back((int32_t) expected.tfs.size());
      if (!(hasFreqs && hasNorms)) {
        continue;
      }
      maxTfPerNorm.fill(0);
      int32_t start = block * Postings::DOCS_BLOCK_SIZE;
      int32_t end = std::min(start + Postings::DOCS_BLOCK_SIZE, numDocs);
      for (int32_t doc = start; doc < end; doc++) {
        int32_t norm = SmallFloat::intToByte4(impactTokenCountForDoc(doc));
        maxTfPerNorm[(size_t) norm] = std::max(maxTfPerNorm[(size_t) norm], impactTfForDoc(doc));
      }
      int32_t runningMaxTf = 0;
      for (int32_t norm = 0; norm < 256; norm++) {
        int32_t tf = maxTfPerNorm[(size_t) norm];
        if (tf > runningMaxTf) {
          expected.norms.push_back(norm);
          expected.tfs.push_back(tf);
          runningMaxTf = tf;
        }
      }
    }
    expected.offsets.push_back((int32_t) expected.tfs.size());
    return expected;
  }

  static std::vector<int32_t> expectedGroupSpanImpacts(const std::vector<int32_t>& blockMaxTf) {
    int32_t numGroups = ((int32_t) blockMaxTf.size() + 31) / 32;
    std::vector<int32_t> expected;
    expected.reserve(numGroups);
    for (int32_t group = 0; group < numGroups; group++) {
      int32_t start = group * 32;
      int32_t end = std::min(start + 32, (int32_t) blockMaxTf.size());
      int32_t spanImpact = 0;
      for (int32_t block = start; block < end; block++) {
        spanImpact = std::max(spanImpact, blockMaxTf[block]);
      }
      expected.push_back(spanImpact);
    }
    return expected;
  }

  static std::vector<int32_t> expectedGroupSpanMinNorms(const std::vector<int32_t>& blockMinNorm) {
    int32_t numGroups = ((int32_t) blockMinNorm.size() + 31) / 32;
    std::vector<int32_t> expected;
    expected.reserve(numGroups);
    for (int32_t group = 0; group < numGroups; group++) {
      int32_t start = group * 32;
      int32_t end = std::min(start + 32, (int32_t) blockMinNorm.size());
      int32_t spanMinNorm = 255;
      for (int32_t block = start; block < end; block++) {
        spanMinNorm = std::min(spanMinNorm, blockMinNorm[block]);
      }
      expected.push_back(blockMinNorm.empty() ? 0 : spanMinNorm);
    }
    return expected;
  }

  void assertImpactHeaders(DocsEnum& denum, const std::vector<int32_t>& expectedBlockMaxTf,
                           const std::vector<int32_t>& expectedBlockMinNorm,
                           std::string_view label,
                           const DocsEnum::ImpactFrontiers* expectedFrontiers = nullptr) {
    std::vector<int32_t> blockMaxTf;
    std::vector<int32_t> groupSpanImpacts;
    std::vector<int32_t> blockMinNorm;
    std::vector<int32_t> groupSpanMinNorms;
    DocsEnum::ImpactFrontiers frontiers;
    denum.readBlockMaxTf(blockMaxTf, &groupSpanImpacts, nullptr, &blockMinNorm, &groupSpanMinNorms,
                         expectedFrontiers == nullptr ? nullptr : &frontiers);
    ASSERT_EQ(blockMaxTf, expectedBlockMaxTf) << label;
    ASSERT_EQ(groupSpanImpacts, expectedGroupSpanImpacts(expectedBlockMaxTf)) << label;
    ASSERT_EQ(blockMinNorm, expectedBlockMinNorm) << label;
    ASSERT_EQ(groupSpanMinNorms, expectedGroupSpanMinNorms(expectedBlockMinNorm)) << label;
    if (expectedFrontiers != nullptr) {
      ASSERT_EQ(frontiers.offsets, expectedFrontiers->offsets) << label;
      ASSERT_EQ(frontiers.tfs, expectedFrontiers->tfs) << label;
      ASSERT_EQ(frontiers.norms, expectedFrontiers->norms) << label;
    }
  }

  void checkRawImpactHeaders(FieldType::flag_type flags, const std::vector<int32_t>& expectedBlockMaxTf,
                             const std::vector<int32_t>& expectedBlockMinNorm, int32_t numDocs,
                             std::string_view label,
                             const DocsEnum::ImpactFrontiers* expectedFrontiers = nullptr) {
    RAMDir dir;
    MemPool pool;
    PostingsWriter postingsWriter(dir, 0, numDocs + Postings::DOCS_BLOCK_SIZE + 100);
    {
      TextWriter writer(postingsWriter);
      auto& finfo = postingsWriter.addField("f");
      finfo.type = FieldType::TEXT;
      finfo.flags = flags;
      writer.startField(&finfo);

      TermRef term(pool, "hot", 3);
      writer.startTerm(term);
      for (int32_t doc = 0; doc < numDocs; doc++) {
        writer.addDoc(doc, impactTfForDoc(doc));
      }
      writer.endTerm(term);

      TermRef nextTerm(pool, "zzz", 3);
      writer.startTerm(nextTerm);
      for (int32_t doc = 0; doc < Postings::DOCS_BLOCK_SIZE + 3; doc++) {
        writer.addDoc(numDocs + 10 + doc, 31);
      }
      writer.endTerm(nextTerm);
      writer.endField();
    }
    postingsWriter.finish();

    PostingsReader reader(dir, 0);
    FieldReader fieldReader(pool, reader);
    ASSERT_TRUE(fieldReader.readNextField()) << label;
    SegFieldInfo fieldInfo;
    fieldReader.readFieldInfo(fieldInfo);
    TermsEnum tenum(pool, reader, fieldInfo);
    ASSERT_TRUE(tenum.seek("hot")) << label;
    DocsEnum denum(pool, reader, tenum);
    assertImpactHeaders(denum, expectedBlockMaxTf, expectedBlockMinNorm, label, expectedFrontiers);
  }

  void addImpactDocs(TestField& f, int32_t firstDoc, int32_t numDocs, int32_t globalBase) {
    std::string text;
    for (int32_t i = 0; i < numDocs; i++) {
      int32_t globalDoc = globalBase + i;
      int32_t tf = impactTfForDoc(globalDoc);
      int32_t tokenCount = impactTokenCountForDoc(globalDoc);
      text.clear();
      for (int32_t j = 0; j < tf; j++) {
        text += "hot ";
      }
      for (int32_t j = tf; j < tokenCount; j++) {
        text += "pad ";
      }
      f.add(firstDoc + i, text);
    }
  }

  void assertImpactHeadersForField(TestField& f, const std::vector<int32_t>& expectedBlockMaxTf,
                                   const std::vector<int32_t>& expectedBlockMinNorm,
                                   std::string_view label,
                                   const DocsEnum::ImpactFrontiers* expectedFrontiers = nullptr) {
    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek("hot")) << label;
    DocsEnum denum(f.testIndex.pool, f.currentSegment()->postingsReader(), tenum);
    assertImpactHeaders(denum, expectedBlockMaxTf, expectedBlockMinNorm, label, expectedFrontiers);
  }

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

TEST_F(DocsEnumAdvanceTest, advanceCrossesL1AndTailOnTrailerFreeSlice) {
  const int32_t N = DocsEnum::L1_DOCS + Postings::DOCS_BLOCK_SIZE + 13;
  RAMDir dir;
  MemPool pool;
  PostingsWriter postingsWriter(dir, 0, N + Postings::DOCS_BLOCK_SIZE + 100);
  {
    TextWriter writer(postingsWriter);
    auto& finfo = postingsWriter.addField("f");
    finfo.type = FieldType::TEXT;
    finfo.flags = FieldType::INDEX_DOCS;
    writer.startField(&finfo);

    TermRef hot(pool, "hot", 3);
    writer.startTerm(hot);
    for (int32_t doc = 0; doc < N; doc++) {
      writer.addDoc(doc, 1);
    }
    writer.endTerm(hot);

    TermRef zzz(pool, "zzz", 3);
    writer.startTerm(zzz);
    for (int32_t doc = 0; doc < Postings::DOCS_BLOCK_SIZE + 7; doc++) {
      writer.addDoc(N + 10 + doc, 1);
    }
    writer.endTerm(zzz);
    writer.endField();
  }
  postingsWriter.finish();

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(pool, reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("hot"));
  DocsEnum denum(pool, reader, tenum);

  for (int32_t target : {0, 1, DocsEnum::L1_DOCS - 1, DocsEnum::L1_DOCS,
                         DocsEnum::L1_DOCS + 1, N - 2, N - 1}) {
    ASSERT_EQ(denum.advance(target), target) << target;
    ASSERT_EQ(denum.termFreq(), 1) << target;
  }
  ASSERT_EQ(denum.advance(N), DocsEnum::END);
}

// The whole-term impact frontier stored in the term dictionary must equal the
// staircase over the term's global (norm -> maxTf) surface - on a fresh
// segment and after a merge (the merger regenerates it by replay).
TEST_F(DocsEnumAdvanceTest, termImpactFrontierRoundTrip) {
  const int32_t N = 72 * Postings::DOCS_BLOCK_SIZE + 17;
  std::array<int32_t, 256> surface{};
  for (int32_t doc = 0; doc < N; doc++) {
    int32_t norm = SmallFloat::intToByte4(impactTokenCountForDoc(doc));
    surface[(size_t) norm] = std::max(surface[(size_t) norm], impactTfForDoc(doc));
  }
  std::vector<int32_t> expNorms;
  std::vector<int32_t> expTfs;
  int32_t running = 0;
  for (int32_t norm = 0; norm < 256; norm++) {
    if (surface[(size_t) norm] > running) {
      expNorms.push_back(norm);
      expTfs.push_back(surface[(size_t) norm]);
      running = surface[(size_t) norm];
    }
  }

  auto check = [&](TestField& f, std::string_view label) {
    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek("hot")) << label;
    std::vector<int32_t> norms;
    std::vector<int32_t> tfs;
    tenum.readTermImpactFrontier(norms, tfs);
    EXPECT_EQ(norms, expNorms) << label;
    EXPECT_EQ(tfs, expTfs) << label;
  };

  {
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, N, 0);
    // a pulsed (single-doc) term shares the block: exact one-point frontier
    f.add(N, "solo x y z");
    testIndex.flush();
    f.startReading();
    check(f, "fresh");

    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek("solo"));
    std::vector<int32_t> norms;
    std::vector<int32_t> tfs;
    ASSERT_EQ(tenum.readTermImpactFrontier(norms, tfs), 1);
    EXPECT_EQ(tfs[0], 1);
    EXPECT_EQ(norms[0], SmallFloat::intToByte4(4));
  }

  {
    const int32_t split = 40 * Postings::DOCS_BLOCK_SIZE + 9;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, split, 0);
    testIndex.flush();
    f.startIndexing();
    addImpactDocs(f, 0, N - split, split);
    testIndex.flush();
    testIndex.iw->mergeSegments();
    f.startReading();
    check(f, "merged");
  }
}

TEST_F(DocsEnumAdvanceTest, blockImpactHeadersRoundTrip) {
  const int32_t N = 72 * Postings::DOCS_BLOCK_SIZE + 17;
  std::vector<int32_t> expectedFreqImpacts = expectedBlockMaxTf(N, true);
  std::vector<int32_t> expectedDocsOnlyImpacts = expectedBlockMaxTf(N, false);
  std::vector<int32_t> expectedNorms = expectedBlockMinNorm(N, true);
  std::vector<int32_t> expectedNoNorms = expectedBlockMinNorm(N, false);
  DocsEnum::ImpactFrontiers expectedFrontiers = expectedBlockFrontiers(N, true, true);
  DocsEnum::ImpactFrontiers expectedNoFrontiers = expectedBlockFrontiers(N, true, false);

  {
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, N, 0);
    testIndex.flush();
    f.startReading();

    assertImpactHeadersForField(f, expectedFreqImpacts, expectedNorms, "positions", &expectedFrontiers);
  }

  {
    const int32_t split = 40 * Postings::DOCS_BLOCK_SIZE + 9;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addImpactDocs(f, 0, split, 0);
    testIndex.flush();
    f.startIndexing();
    addImpactDocs(f, 0, N - split, split);
    testIndex.flush();

    testIndex.iw->mergeSegments();
    f.startReading();
    ASSERT_EQ(testIndex.reader->segments().size(), 1u);

    assertImpactHeadersForField(f, expectedFreqImpacts, expectedNorms, "merged positions",
                                &expectedFrontiers);
  }

  checkRawImpactHeaders(FieldType::INDEX_DOCS_FREQS, expectedFreqImpacts, expectedNoNorms, N,
                        "docs+freqs", &expectedNoFrontiers);
  checkRawImpactHeaders(FieldType::INDEX_DOCS, expectedDocsOnlyImpacts, expectedNoNorms, N,
                        "docs-only", &expectedNoFrontiers);
}

// Position reads after far advances must land exactly where sequential decoding
// would, now that skipToBlock repairs the position stream through the L0
// posByteOff anchors.  Every doc holds all three terms, so the per-term
// cumulative tf pins each doc block's anchor alignment deterministically:
//   one (tf=1)    - anchors on exact position-block boundaries, and the final
//                   doc block's anchor at the vint-tail start (N % 128 != 0).
//   cst (tf=128)  - each doc is exactly one position block; anchors aligned.
//   var (tf=1+((d*7+3)%250)) - anchors rotate through in-block ords, docs
//                   straddle position blocks.
// Sized to span multiple L1 groups so far advances take the group step-over
// into a direct seek.  Runs on a single segment and again after a merge (the
// merger regenerates the anchors by replaying positions through TextWriter).
class PositionSeekTest : public DocsEnumAdvanceTest {
protected:
  static constexpr int32_t N = 2 * DocsEnum::L1_DOCS + 300;  // 66 full doc blocks + tail

  static int32_t tfFor(std::string_view term, int32_t globalDoc) {
    if (term == "one") return 1;
    if (term == "cst") return Postings::POSITIONS_BLOCK_SIZE;
    return 1 + ((globalDoc * 7 + 3) % 250);  // var
  }

  static int32_t firstPosFor(std::string_view term, int32_t globalDoc) {
    if (term == "one") return 0;
    if (term == "var") return 1;
    return 1 + tfFor("var", globalDoc);  // cst
  }

  static void addSeekDocs(TestField& f, int32_t firstDoc, int32_t numDocs, int32_t globalBase) {
    std::string text;
    for (int32_t i = 0; i < numDocs; i++) {
      int32_t globalDoc = globalBase + i;
      text = "one ";
      for (int32_t k = 0; k < tfFor("var", globalDoc); k++) text += "var ";
      for (int32_t k = 0; k < tfFor("cst", globalDoc); k++) text += "cst ";
      f.add(firstDoc + i, text);
    }
  }

  // readMode: 0 = all positions + END, 1 = half, 2 = none except one position
  // every 4th landing (repeated seeks over stale buffer states).
  void checkSeekWalk(TestField& f, std::string_view term,
                     const std::vector<int32_t>& targets, int readMode) {
    SCOPED_TRACE(::testing::Message() << "term=" << term << " readMode=" << readMode);
    TermsEnum tenum = f.createTermsEnum();
    ASSERT_TRUE(tenum.seek(term));
    DocsEnum denum(f.testIndex.pool, f.currentSegment()->postingsReader(), tenum);

    int32_t landings = 0;
    for (int32_t target : targets) {
      if (target <= denum.docId()) continue;
      int32_t doc = denum.advance(target);
      ASSERT_EQ(doc, target < N ? target : DocsEnum::END) << "advance(" << target << ")";
      if (doc == DocsEnum::END) break;
      int32_t tf = tfFor(term, doc);
      ASSERT_EQ(denum.termFreq(), tf) << "doc " << doc;
      int32_t toRead = readMode == 0 ? tf
                     : readMode == 1 ? tf / 2
                     : (landings % 4 == 0 ? 1 : 0);
      landings++;
      if (readMode == 2 && toRead == 0) continue;
      denum.startPositions();
      int32_t firstPos = firstPosFor(term, doc);
      for (int32_t k = 0; k < toRead; k++) {
        ASSERT_EQ(denum.nextPosition(), firstPos + k) << "doc " << doc << " pos " << k;
      }
      if (toRead == tf) {
        ASSERT_EQ(denum.nextPosition(), DocsEnum::END) << "doc " << doc << " pos end";
      }
    }
  }

  void checkAllWalks(TestField& f) {
    // Block/group boundary singles (L1_DOCS = 4096; tail starts at 8448 for one/cst).
    std::vector<int32_t> boundaries = {1, 2, 127, 128, 130, 258, 4095, 4096, 4097,
                                       5000, 8191, 8192, 8300, 8447, 8448, 8449,
                                       8460, N - 1, N};
    for (std::string_view term : {"one", "cst", "var"}) {
      checkSeekWalk(f, term, boundaries, 0);
      int readMode = 0;
      for (int32_t stride : {997, 313, 129}) {
        std::vector<int32_t> targets;
        for (int32_t t = stride / 2; t <= N; t += stride) targets.push_back(t);
        checkSeekWalk(f, term, targets, readMode++);
      }
      // Sequential reads resuming after a far seek, then another far seek.
      std::vector<int32_t> mixed;
      mixed.push_back(3000);
      for (int32_t d = 3001; d < 3200; d++) mixed.push_back(d);
      mixed.push_back(8400);
      checkSeekWalk(f, term, mixed, 0);
    }
  }
};

TEST_F(PositionSeekTest, positionSeekAlignments) {
  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  {
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addSeekDocs(f, 0, N, 0);
    testIndex.flush();
    f.startReading();
    checkAllWalks(f);
    ASSERT_GT(SkipStats::posSeeks, 0);  // the new anchor-seek path engaged
  }

  {
    const int32_t split = 40 * Postings::DOCS_BLOCK_SIZE + 9;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    addSeekDocs(f, 0, split, 0);
    testIndex.flush();
    f.startIndexing();
    addSeekDocs(f, 0, N - split, split);
    testIndex.flush();

    testIndex.iw->mergeSegments();
    f.startReading();
    ASSERT_EQ(testIndex.reader->segments().size(), 1u);
    checkAllWalks(f);
  }

  SkipStats::enabled = savedStats;
  SkipStats::reset();
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
