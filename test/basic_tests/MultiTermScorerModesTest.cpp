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
                          Query::MatchState matchState) {
  EXPECT_EQ(matchState, shape.matchState);
  EXPECT_EQ(Query::DirectScorerKind::OTHER, shape.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::NO, shape.reportedTwoPhase);
  EXPECT_EQ(Query::ClauseShape::NONE, shape.windowFillClause);
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
  size_t saved = MultiTermQuery::Weight::maxLazyStateBytes;
  MultiTermQuery::Weight::maxLazyStateBytes =
      8 * sizeof(TermsEnum::PostingsState);
  {
    ScorerModeGuard guard(ScorerMode::AUTO);
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("body_w", "q");
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight =
        pq.createWeight(ctx, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(dynamic_cast<MultiTermQuery::Scorer*>(scorer), nullptr);
  }
  EXPECT_EQ(eager, collectDocs(ti, ScorerMode::AUTO, {0, 0}));
  MultiTermQuery::Weight::maxLazyStateBytes = saved;
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

  for (ScorerMode mode : {
           ScorerMode::AUTO,
           ScorerMode::FORCE_EAGER,
           ScorerMode::FORCE_WINDOWED,
           ScorerMode::FORCE_HEAP}) {
    ScorerModeGuard modeGuard(mode);
    PrefixQuery prefix("body_w", "q");
    auto* prefixWeight = prefix.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* prefixSupplier = prefixWeight->scorerSupplier(ti.pool, segment);
    ASSERT_NE(nullptr, prefixSupplier);
    expectMultiTermShape(
        prefixSupplier->describeScorer(
            MultiTermQuery::Weight::scorerBuildContext(1)),
        Query::MatchState::UNKNOWN);
  }

  PrefixQuery emptyPrefix("body_w", "z");
  auto* emptyPrefixWeight = emptyPrefix.createWeight(context, 0);
  auto* emptyPrefixSupplier =
      emptyPrefixWeight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(nullptr, emptyPrefixSupplier);
  expectMultiTermShape(emptyPrefixSupplier->describeScorer(buildContext),
                       Query::MatchState::UNKNOWN);

  PrefixQuery absentField("missing_w", "q");
  auto* absentWeight = absentField.createWeight(context, 0);
  auto* absentSupplier = absentWeight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(nullptr, absentSupplier);
  expectMultiTermShape(absentSupplier->describeScorer(buildContext),
                       Query::MatchState::EMPTY);
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
