#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/PrefixQuery.h"
#include "solux/query/QueryPrep.h"
#include "solux/query/TermQuery.h"
#include "solux/reader/DocsEnum.h"

using namespace solux;
using namespace solux::test;

using ScorerMode = MultiTermQuery::Weight::ScorerMode;

namespace {

class ScorerModeGuard {
  ScorerMode saved;

public:
  explicit ScorerModeGuard(ScorerMode mode)
    : saved(MultiTermQuery::Weight::scorerModeForTests) {
    MultiTermQuery::Weight::scorerModeForTests = mode;
  }

  ~ScorerModeGuard() {
    MultiTermQuery::Weight::scorerModeForTests = saved;
  }
};

class DenseFillGuard {
  bool saved;

public:
  explicit DenseFillGuard(bool disabled)
    : saved(MultiTermQuery::disableDenseFillForTests) {
    MultiTermQuery::disableDenseFillForTests = disabled;
  }

  ~DenseFillGuard() {
    MultiTermQuery::disableDenseFillForTests = saved;
  }
};

class StateBudgetGuard {
  size_t saved;

public:
  explicit StateBudgetGuard(size_t bytes)
    : saved(MultiTermQuery::Weight::maxLazyStateBytes) {
    MultiTermQuery::Weight::maxLazyStateBytes = bytes;
  }

  ~StateBudgetGuard() {
    MultiTermQuery::Weight::maxLazyStateBytes = saved;
  }
};

class SkipStatsGuard {
  bool saved;

public:
  SkipStatsGuard() : saved(SkipStats::enabled) {
    SkipStats::reset();
    SkipStats::enabled = true;
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
    SkipStats::reset();
  }
};

// Every term starts with "q" so one prefix query unions the whole corpus.
void buildCorpus(TestField& field, const std::map<int32_t, std::string>& docs) {
  field.startIndexing();
  for (auto& [doc, body] : docs) {
    field.add(doc, body);
  }
  field.testIndex.flush();
  field.startReading();
}

void append(std::map<int32_t, std::string>& docs, int32_t doc,
            const std::string& term) {
  auto& body = docs[doc];
  if (!body.empty()) body.push_back(' ');
  body.append(term);
}

// 6000 single-doc (pulsed) terms spread over ~48k docs: sparse-rare unions,
// many empty windows, deferral of terms whose docs sit deep in the segment.
std::map<int32_t, std::string> pulsedSparseDocs() {
  std::map<int32_t, std::string> docs;
  for (int32_t i = 0; i < 6000; i++) {
    append(docs, i * 8 + 3, "q" + std::to_string(i));
  }
  return docs;
}

// Every postings block shape in one segment (3000 docs): contiguous full
// blocks, dense word blocks, packed blocks, StreamVByte tails at the 127/128/
// 129 boundaries, and pulsed terms with early and late first docs.
std::map<int32_t, std::string> mixedShapeDocs() {
  std::map<int32_t, std::string> docs;
  for (int32_t i = 0; i < 3000; i++) {
    append(docs, i, "qall");                             // contiguous
    for (int32_t j = 0; j < 4; j++) {
      if (((i * 31 + j * 17) % 5) < 3) {
        append(docs, i, "qdense" + std::to_string(j));   // ~60% word blocks
      }
    }
    for (int32_t j = 0; j < 20; j++) {
      if (i % 60 == j * 3) {
        append(docs, i, "qmid" + std::to_string(j));     // docFreq 50 tails
      }
    }
  }
  for (int32_t f : {127, 128, 129}) {                    // tail boundaries
    for (int32_t i = 0; i < f; i++) {
      append(docs, i * 20 + 7, "qedge" + std::to_string(f));
    }
  }
  for (int32_t j = 0; j < 50; j++) {
    append(docs, (j * 97) % 3000, "qone" + std::to_string(j));  // pulsed
  }
  append(docs, 2999, "qlast");   // pulsed at the segment's final doc
  return docs;
}

struct DrivePattern {
  int32_t nextSteps;   // next() calls between advances
  int32_t stride;      // 0 = pure next()
};

constexpr DrivePattern kPatterns[] = {
    {0, 0},          // pure next()
    {3, 2500},       // strides within and just past a window
    {1, 9000},       // every step jumps multiple windows
    {5, 40000},      // giant strides toward maxDoc
};

std::vector<int32_t> collectDocs(TestIndex& ti, ScorerMode mode,
                                 DrivePattern pattern) {
  ScorerModeGuard guard(mode);
  auto g = ti.pool.rewindScopeGuard();
  PrefixQuery pq("body_w", "q");
  Query::Context ctx(ti.pool, *ti.reader);
  auto* weight = pq.createWeight(ctx, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
  if (scorer == nullptr) return {};
  std::vector<int32_t> docs;
  int32_t sinceAdvance = 0;
  int32_t d = scorer->next();
  while (d != PostingsReader::END) {
    docs.push_back(d);
    if (pattern.stride > 0 && ++sinceAdvance >= pattern.nextSteps) {
      sinceAdvance = 0;
      d = scorer->advance(d + pattern.stride);
    } else {
      d = scorer->next();
    }
  }
  return docs;
}

void expectModeParity(TestIndex& ti) {
  for (const auto& pattern : kPatterns) {
    auto eager = collectDocs(ti, ScorerMode::FORCE_EAGER, pattern);
    EXPECT_EQ(eager, collectDocs(ti, ScorerMode::FORCE_WINDOWED, pattern))
        << "windowed, stride=" << pattern.stride;
    EXPECT_EQ(eager, collectDocs(ti, ScorerMode::FORCE_HEAP, pattern))
        << "heap, stride=" << pattern.stride;
  }
}

void expectSelectionAndEarlyExit(TestIndex& ti) {
  struct Expect {
    ScorerMode mode;
    bool eager, windowed, heap;
  };
  for (auto [mode, eager, windowed, heap] :
       {Expect{ScorerMode::FORCE_EAGER, true, false, false},
        Expect{ScorerMode::FORCE_WINDOWED, false, true, false},
        Expect{ScorerMode::FORCE_HEAP, false, false, true}}) {
    ScorerModeGuard guard(mode);
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("body_w", "q");
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight =
        pq.createWeight(ctx, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(eager, dynamic_cast<MultiTermQuery::Scorer*>(scorer) != nullptr);
    EXPECT_EQ(windowed,
              dynamic_cast<UnionLazyScorer*>(scorer) != nullptr);
    EXPECT_EQ(heap, dynamic_cast<UnionHeapScorer*>(scorer) != nullptr);
    for (int i = 0; i < 3; i++) {
      ASSERT_NE(PostingsReader::END, scorer->next());
    }
    scorer->setMinCompetitiveScore(
        std::nextafter(1.0f, std::numeric_limits<float>::infinity()));
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }
}

void expectTermShape(const Query::ScorerShape& shape,
                     Query::MatchState matchState) {
  EXPECT_EQ(matchState, shape.matchState);
  EXPECT_EQ(Query::DirectScorerKind::TERM, shape.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::NO, shape.reportedTwoPhase);
  EXPECT_EQ(Query::ClauseShape::DIRECT, shape.windowFillClause);
  EXPECT_EQ(Query::ClauseShape::DIRECT, shape.termDisjunctionClause);
  EXPECT_EQ(Query::IndependentTermAccess::SUPPORTED, shape.independentTerm);
  EXPECT_EQ(Query::DocsOnlyAccess::SUPPORTED, shape.docsOnly);
  EXPECT_EQ(Query::DirectDocSetAccess::UNSUPPORTED, shape.directDocSet);
}

void expectDocSetShape(const Query::ScorerShape& shape,
                       Query::MatchState matchState) {
  EXPECT_EQ(matchState, shape.matchState);
  EXPECT_EQ(Query::DirectScorerKind::DOC_SET, shape.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::NO, shape.reportedTwoPhase);
  EXPECT_EQ(Query::ClauseShape::DIRECT, shape.windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE, shape.termDisjunctionClause);
  EXPECT_EQ(Query::IndependentTermAccess::UNSUPPORTED,
            shape.independentTerm);
  EXPECT_EQ(Query::DocsOnlyAccess::UNSUPPORTED, shape.docsOnly);
  EXPECT_EQ(Query::DirectDocSetAccess::SUPPORTED, shape.directDocSet);
}

void expectMultiTermShape(const Query::ScorerShape& shape,
                          Query::MatchState matchState,
                          Query::ClauseShape windowFillClause) {
  EXPECT_EQ(matchState, shape.matchState);
  EXPECT_EQ(Query::DirectScorerKind::OTHER, shape.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::NO, shape.reportedTwoPhase);
  EXPECT_EQ(windowFillClause, shape.windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE, shape.termDisjunctionClause);
  EXPECT_EQ(Query::IndependentTermAccess::UNSUPPORTED,
            shape.independentTerm);
  EXPECT_EQ(Query::DocsOnlyAccess::UNSUPPORTED, shape.docsOnly);
  EXPECT_EQ(Query::DirectDocSetAccess::UNSUPPORTED, shape.directDocSet);
}

}  // namespace

class MultiTermScorerModesTest : public SoluxTest {};

TEST_F(MultiTermScorerModesTest, pulsedSparseParity) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, pulsedSparseDocs());

  EXPECT_EQ(6000u, collectDocs(ti, ScorerMode::FORCE_EAGER, {0, 0}).size());
  expectModeParity(ti);
  expectSelectionAndEarlyExit(ti);
}

TEST_F(MultiTermScorerModesTest, mixedShapeParity) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, mixedShapeDocs());

  EXPECT_EQ(3000u, collectDocs(ti, ScorerMode::FORCE_EAGER, {0, 0}).size());
  expectModeParity(ti);
  expectSelectionAndEarlyExit(ti);
}

// AUTO spills to the eager union when the retained-state budget is exceeded.
TEST_F(MultiTermScorerModesTest, stateBudgetSpillsToEager) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, mixedShapeDocs());

  auto eager = collectDocs(ti, ScorerMode::FORCE_EAGER, {0, 0});
  StateBudgetGuard stateBudget(
      8 * sizeof(TermsEnum::PostingsState));
  {
    ScorerModeGuard guard(ScorerMode::AUTO);
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("body_w", "q");
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = dynamic_cast<MultiTermQuery::Weight*>(
        pq.createWeight(ctx, Query::NEED_SCORES | Query::ALLOW_PRUNING));
    ASSERT_NE(weight, nullptr);
    auto* supplier =
        weight->scorerSupplier(ti.pool, ctx.topReader.segments()[0]);
    Query::ScorerBuildContext buildContext =
        MultiTermQuery::Weight::scorerBuildContext(1);
    expectMultiTermShape(
        supplier->describeScorer(buildContext),
        Query::MatchState::UNKNOWN, Query::ClauseShape::UNKNOWN);
    SkipStatsGuard statsGuard;
    auto* scorer = dynamic_cast<MultiTermQuery::Scorer*>(
        weight->createScorer(ti.pool, ctx.topReader.segments()[0]));
    ASSERT_NE(scorer, nullptr);
    EXPECT_TRUE(weight->expansionMemoUsesBitsetForTests(
        ctx.topReader.segments()[0]));
    EXPECT_EQ(0u, weight->expansionMemoRetainedStatesForTests(
        ctx.topReader.segments()[0]));
    auto* second = dynamic_cast<MultiTermQuery::Scorer*>(
        supplier->get(ti.pool, 1));
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(scorer->bitWordsForTests(), second->bitWordsForTests());
    EXPECT_EQ(1, SkipStats::multitermExpansions);
    expectMultiTermShape(
        supplier->describeScorer(buildContext),
        Query::MatchState::NONEMPTY, Query::ClauseShape::DIRECT);
  }
  EXPECT_EQ(eager, collectDocs(ti, ScorerMode::AUTO, {0, 0}));
}

// A conjunction-driven supplier (finite leadCost) keeps the lazy union;
// unscored and non-pruning contexts stay eager.
TEST_F(MultiTermScorerModesTest, drivenSupplierSelection) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, mixedShapeDocs());

  struct Expect {
    int32_t flags;
    bool lazy;
  };
  for (auto [flags, lazy] :
       {Expect{Query::NEED_SCORES | Query::ALLOW_PRUNING, true},
        Expect{Query::NEED_SCORES, false},
        Expect{0, false}}) {
    ScorerModeGuard guard(ScorerMode::AUTO);
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("body_w", "q");
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, flags);
    auto* supplier =
        weight->scorerSupplier(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(supplier, nullptr);
    auto* scorer = supplier->get(ti.pool, 100);  // driven: finite leadCost
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(lazy, dynamic_cast<UnionLazyScorer*>(scorer) != nullptr)
        << "flags=" << flags;
    EXPECT_EQ(!lazy, dynamic_cast<MultiTermQuery::Scorer*>(scorer) != nullptr)
        << "flags=" << flags;
  }
}

TEST_F(MultiTermScorerModesTest, supplierScorerShapes) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, {{0, "qalpha"}, {1, "qbeta"}, {2, "other"}});

  auto guard = ti.pool.rewindScopeGuard();
  auto& segment = ti.reader->segments()[0];
  Query::Context context(ti.pool, *ti.reader);
  Query::ScorerBuildContext buildContext{
      .leadCost = 1,
  };

  TermQuery presentTerm("body_w", "qalpha");
  auto* presentWeight = presentTerm.createWeight(context, 0);
  auto* presentSupplier = presentWeight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(nullptr, presentSupplier);
  expectTermShape(presentSupplier->describeScorer(buildContext),
                  Query::MatchState::NONEMPTY);

  TermQuery missingTerm("body_w", "missing");
  auto* missingWeight = missingTerm.createWeight(context, 0);
  auto* missingSupplier = missingWeight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(nullptr, missingSupplier);
  expectTermShape(missingSupplier->describeScorer(buildContext),
                  Query::MatchState::EMPTY);

  DocSetBuilder docsBuilder(segment.maxDoc());
  docsBuilder.add(1);
  auto docs = docsBuilder.build();
  QueryPrep::DocSetSupplier docsSupplier(docs.get(), segment);
  expectDocSetShape(docsSupplier.describeScorer(buildContext),
                    Query::MatchState::NONEMPTY);
  QueryPrep::DocSetSupplier nullDocsSupplier(nullptr, segment);
  expectDocSetShape(nullDocsSupplier.describeScorer(buildContext),
                    Query::MatchState::EMPTY);
  DocSetBuilder emptyBuilder(segment.maxDoc());
  auto emptyDocs = emptyBuilder.build();
  QueryPrep::DocSetSupplier emptyDocsSupplier(emptyDocs.get(), segment);
  expectDocSetShape(emptyDocsSupplier.describeScorer(buildContext),
                    Query::MatchState::EMPTY);

  struct ModeShape {
    ScorerMode mode;
    Query::ClauseShape windowFillClause;
  };
  for (auto [mode, windowFillClause] : {
           // AUTO may overflow retained state into the eager fill-capable
           // scorer, so its fill answer is open rather than a definite NONE.
           ModeShape{ScorerMode::AUTO, Query::ClauseShape::UNKNOWN},
           ModeShape{ScorerMode::FORCE_EAGER, Query::ClauseShape::DIRECT},
           ModeShape{ScorerMode::FORCE_WINDOWED, Query::ClauseShape::NONE},
           ModeShape{ScorerMode::FORCE_HEAP, Query::ClauseShape::NONE}}) {
    ScorerModeGuard modeGuard(mode);
    PrefixQuery prefix("body_w", "q");
    auto* prefixWeight = prefix.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* prefixSupplier = prefixWeight->scorerSupplier(ti.pool, segment);
    ASSERT_NE(nullptr, prefixSupplier);
    expectMultiTermShape(
        prefixSupplier->describeScorer(
            MultiTermQuery::Weight::scorerBuildContext(1)),
        Query::MatchState::UNKNOWN, windowFillClause);

    Query::ScorerBuildContext modeContext =
        MultiTermQuery::Weight::scorerBuildContext(1);
    EXPECT_TRUE(prefixSupplier->fillExpansionMemo(modeContext));
    Query::ClauseShape resolvedWindowFill =
        mode == ScorerMode::FORCE_EAGER
        ? Query::ClauseShape::DIRECT
        : Query::ClauseShape::NONE;
    expectMultiTermShape(
        prefixSupplier->describeScorer(modeContext),
        Query::MatchState::NONEMPTY, resolvedWindowFill);
    EXPECT_FALSE(prefixSupplier->fillExpansionMemo(modeContext));
  }

  {
    DenseFillGuard denseFillGuard(true);
    ScorerModeGuard modeGuard(ScorerMode::FORCE_EAGER);
    PrefixQuery prefix("body_w", "q");
    auto* prefixWeight = prefix.createWeight(context, 0);
    auto* prefixSupplier = prefixWeight->scorerSupplier(ti.pool, segment);
    ASSERT_NE(nullptr, prefixSupplier);
    expectMultiTermShape(
        prefixSupplier->describeScorer(
            MultiTermQuery::Weight::scorerBuildContext(1)),
        Query::MatchState::UNKNOWN, Query::ClauseShape::NONE);
  }

  PrefixQuery emptyPrefix("body_w", "z");
  auto* emptyPrefixWeight = emptyPrefix.createWeight(context, 0);
  auto* emptyPrefixSupplier =
      emptyPrefixWeight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(nullptr, emptyPrefixSupplier);
  expectMultiTermShape(emptyPrefixSupplier->describeScorer(buildContext),
                       Query::MatchState::UNKNOWN,
                       Query::ClauseShape::DIRECT);
  EXPECT_TRUE(emptyPrefixSupplier->fillExpansionMemo(buildContext));
  expectMultiTermShape(emptyPrefixSupplier->describeScorer(buildContext),
                       Query::MatchState::EMPTY,
                       Query::ClauseShape::DIRECT);

  PrefixQuery absentField("missing_w", "q");
  auto* absentWeight = absentField.createWeight(context, 0);
  auto* absentSupplier = absentWeight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(nullptr, absentSupplier);
  expectMultiTermShape(absentSupplier->describeScorer(buildContext),
                       Query::MatchState::EMPTY,
                       Query::ClauseShape::DIRECT);
}

TEST_F(MultiTermScorerModesTest,
       expansionMemoReusedAcrossSuppliersAndScorerBuilds) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, {{0, "qalpha"}, {1, "qbeta"}, {2, "other"}});

  auto run = [&](size_t maxStates, bool expectBitset) {
    StateBudgetGuard stateBudget(
        maxStates * sizeof(TermsEnum::PostingsState));
    auto guard = ti.pool.rewindScopeGuard();
    auto& segment = ti.reader->segments()[0];
    Query::Context context(ti.pool, *ti.reader);
    PrefixQuery prefix("body_w", "q");
    auto* weight = dynamic_cast<MultiTermQuery::Weight*>(
        prefix.createWeight(context, 0));
    ASSERT_NE(weight, nullptr);
    Query::ScorerBuildContext buildContext =
        MultiTermQuery::Weight::scorerBuildContext(1);

    SkipStatsGuard statsGuard;
    auto* firstSupplier = weight->scorerSupplier(ti.pool, segment);
    ASSERT_NE(firstSupplier, nullptr);
    firstSupplier->describeScorer(buildContext);
    firstSupplier->describeScorer(buildContext);
    EXPECT_EQ(0, SkipStats::multitermExpansions);
    EXPECT_TRUE(firstSupplier->fillExpansionMemo(buildContext));
    EXPECT_EQ(expectBitset,
              weight->expansionMemoUsesBitsetForTests(segment));
    EXPECT_EQ(expectBitset ? 0u : 2u,
              weight->expansionMemoRetainedStatesForTests(segment));
    auto* first = dynamic_cast<MultiTermQuery::Scorer*>(
        firstSupplier->get(ti.pool, 1));
    ASSERT_NE(first, nullptr);

    auto* secondSupplier = weight->scorerSupplier(ti.pool, segment);
    ASSERT_NE(secondSupplier, nullptr);
    EXPECT_FALSE(secondSupplier->fillExpansionMemo(buildContext));
    auto* second = dynamic_cast<MultiTermQuery::Scorer*>(
        secondSupplier->get(ti.pool, 1));
    ASSERT_NE(second, nullptr);
    auto* third = dynamic_cast<MultiTermQuery::Scorer*>(
        weight->createScorer(ti.pool, segment));
    ASSERT_NE(third, nullptr);

    if (expectBitset) {
      EXPECT_EQ(first->bitWordsForTests(), second->bitWordsForTests());
      EXPECT_EQ(first->bitWordsForTests(), third->bitWordsForTests());
    }
    for (auto* scorer : {first, second, third}) {
      EXPECT_EQ(0, scorer->next());
      EXPECT_EQ(1, scorer->next());
      EXPECT_EQ(PostingsReader::END, scorer->next());
    }
    EXPECT_EQ(1, SkipStats::multitermExpansions);
  };

  run(2, false);  // exactly at the cap retains both states
  run(1, true);   // the next state crosses the cap into the bitset form
}

TEST_F(MultiTermScorerModesTest, eagerWindowFillCopiesUnalignedMaskedRange) {
  constexpr int32_t maxDoc = 192;
  uint64_t words[FixedBitSet::sizeInWords(maxDoc)] = {};
  FixedBitSet bits(words, maxDoc);
  for (int32_t doc : {0, 60, 61, 63, 64, 65, 92, 124, 125, 126,
                      127, 128, 137, 138, 139, 190}) {
    bits.set(doc);
  }

  DenseFillGuard denseFillGuard(false);
  MultiTermQuery::Scorer scorer(bits, maxDoc, 1.0f);
  ASSERT_TRUE(scorer.supportsWindowFilter());
  EXPECT_EQ(nullptr, scorer.windowFilterProbeDocsEnum());

  constexpr int32_t windowStart = 61;
  constexpr int32_t windowEnd = 139;
  uint64_t windowWords[2] = {1ULL << 5, 0};
  scorer.fillWindowBits(windowWords, windowStart, windowEnd);

  for (int32_t relative = 0; relative < 128; relative++) {
    bool expected = relative == 5
        || (relative < windowEnd - windowStart
            && bits.get(windowStart + relative));
    EXPECT_EQ(expected,
              (windowWords[relative >> 6] & (1ULL << (relative & 63))) != 0)
        << "relative=" << relative;
  }

  DenseFillGuard disabledGuard(true);
  EXPECT_FALSE(scorer.supportsWindowFilter());
}

// firstDocLowerBound is a lower bound for every term and exact below the
// full-block threshold, across pulsed, tail, and packed postings.
TEST_F(MultiTermScorerModesTest, firstDocLowerBound) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, mixedShapeDocs());

  auto guard = ti.pool.rewindScopeGuard();
  auto* seg = field.currentSegment();
  TermsEnum te(guard.pool(), seg->postingsReader(), field.fieldInfo);
  int32_t pulsed = 0, tails = 0, packed = 0;
  while (te.nextTerm()) {
    auto state = te.postingsState();
    int32_t lowerBound = DocsOnlyEnum::firstDocLowerBound(state);
    DocsOnlyEnum docsEnum(state);
    int32_t first = docsEnum.nextDoc();
    ASSERT_NE(PostingsReader::END, first);
    ASSERT_LE(lowerBound, first) << (std::string_view) te.term();
    if (state.docsEnd == state.docsStart) {
      ASSERT_EQ(lowerBound, first) << (std::string_view) te.term();
      pulsed++;
    } else if (state.docFreq < Postings::DOCS_BLOCK_SIZE) {
      ASSERT_EQ(lowerBound, first) << (std::string_view) te.term();
      tails++;
    } else {
      ASSERT_EQ(0, lowerBound) << (std::string_view) te.term();
      packed++;
    }
  }
  // The corpus must actually cover all three classes, including the 127/128
  // docFreq boundary on either side of the tail rule.
  EXPECT_GT(pulsed, 0);
  EXPECT_GT(tails, 20);
  EXPECT_GT(packed, 5);
}
