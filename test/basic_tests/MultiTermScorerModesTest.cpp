// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/search/Collector.h"

using namespace luxir;
using namespace luxir::test;

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

class TruthfulCostGuard {
  bool saved;

public:
  explicit TruthfulCostGuard(bool disabled)
    : saved(MultiTermQuery::disableTruthfulCostForTests) {
    MultiTermQuery::disableTruthfulCostForTests = disabled;
  }

  ~TruthfulCostGuard() {
    MultiTermQuery::disableTruthfulCostForTests = saved;
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

struct ConstantTopKRun {
  std::vector<TopDocsCollector::ScoreDoc> docs;
  int64_t exactPartitions = 0;
  int64_t deadOuterJumps = 0;
  int64_t requiredIntersectionWindows = 0;
  int64_t requiredProbeCandidates = 0;
};

ConstantTopKRun runConstantTopK(
    const std::vector<std::vector<int32_t>>& clauseDocs,
    std::span<const float> contributions, int32_t maxDoc, int32_t topK,
    bool allowPruning, bool reverseClauses = false) {
  assert(clauseDocs.size() == contributions.size());
  MemPool pool;
  auto scorers = pool.make_span<Query::Scorer*>(clauseDocs.size());
  auto costs = pool.make_span<int64_t>(clauseDocs.size());
  int64_t aggregateCost = 0;
  for (size_t i = 0; i < clauseDocs.size(); i++) {
    size_t source = reverseClauses ? clauseDocs.size() - 1 - i : i;
    auto words = pool.make_span<uint64_t>(FixedBitSet::sizeInWords(maxDoc));
    std::fill(words.begin(), words.end(), 0);
    FixedBitSet bits(words.data(), maxDoc);
    for (int32_t doc : clauseDocs[source]) {
      bits.set(doc);
    }
    scorers[i] = pool.make<MultiTermQuery::Scorer>(
        bits, maxDoc, contributions[source]);
    costs[i] = (int64_t) clauseDocs[source].size();
    aggregateCost += costs[i];
  }

  auto* bulk = pool.make<BooleanQuery::MaxScoreBulkScorer>(
      pool, scorers, costs, std::span<Query::Scorer*>{}, maxDoc,
      aggregateCost, false, false, 0, true);
  TopDocsCollector collector(topK);
  ConstantTopKRun run;
  {
    SkipStatsGuard stats;
    collectTopKWindowed(0, bulk, nullptr, collector, nullptr, maxDoc,
                        allowPruning);
    run.exactPartitions = SkipStats::maxScoreExactConstantPartitions;
    run.deadOuterJumps = SkipStats::maxScoreDeadOuterJumps;
    run.requiredIntersectionWindows =
        SkipStats::maxScoreRequiredIntersectionWindows;
    run.requiredProbeCandidates = SkipStats::maxScoreRequiredProbeCandidates;
  }
  auto sorted = collector.sort();
  run.docs.assign(sorted.begin(), sorted.end());
  return run;
}

void expectSameScoreDocs(const ConstantTopKRun& expected,
                         const ConstantTopKRun& actual) {
  ASSERT_EQ(expected.docs.size(), actual.docs.size());
  for (size_t i = 0; i < expected.docs.size(); i++) {
    EXPECT_EQ(expected.docs[i].doc, actual.docs[i].doc) << "i=" << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(expected.docs[i].score),
              std::bit_cast<uint32_t>(actual.docs[i].score)) << "i=" << i;
  }
}

ConstantTopKRun runPrefixDisjunctionTopK(
    TestIndex& ti, ScorerMode mode, bool allowPruning,
    bool reverseClauses = false) {
  ScorerModeGuard modeGuard(mode);
  auto poolGuard = ti.pool.rewindScopeGuard();
  Query::Context context(ti.pool, *ti.reader);
  PrefixQuery a("body_w", "a");
  PrefixQuery b("body_w", "b");
  std::array<Query*, 2> optional = reverseClauses
      ? std::array<Query*, 2>{&b, &a}
      : std::array<Query*, 2>{&a, &b};
  BooleanQuery query({}, optional, {}, {});
  auto* weight = query.createWeight(
      context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  auto* supplier = weight->scorerSupplier(
      ti.pool, context.topReader.segments()[0]);
  EXPECT_NE(nullptr, supplier);
  auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(ti.pool);
  EXPECT_NE(nullptr, dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(bulk));

  TopDocsCollector collector(10);
  ConstantTopKRun run;
  if (bulk != nullptr) {
    SkipStatsGuard stats;
    collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                        context.topReader.segments()[0].maxDoc(), allowPruning);
    run.exactPartitions = SkipStats::maxScoreExactConstantPartitions;
    run.deadOuterJumps = SkipStats::maxScoreDeadOuterJumps;
    run.requiredIntersectionWindows =
        SkipStats::maxScoreRequiredIntersectionWindows;
    run.requiredProbeCandidates = SkipStats::maxScoreRequiredProbeCandidates;
  }
  auto sorted = collector.sort();
  run.docs.assign(sorted.begin(), sorted.end());
  return run;
}

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

class MultiTermScorerModesTest : public LuxirTest {};

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

TEST_F(MultiTermScorerModesTest,
       uniformConstantLatticeTerminatesAfterTopKScoreSaturates) {
  constexpr int32_t maxDoc = 9000;
  std::vector<std::vector<int32_t>> clauses(2);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    if (doc % 2 == 0) clauses[0].push_back(doc);
    if (doc % 3 == 0) clauses[1].push_back(doc);
  }
  std::array<float, 2> contributions = {1.0f, 1.0f};

  auto exhaustive = runConstantTopK(
      clauses, contributions, maxDoc, 10, false);
  auto pruned = runConstantTopK(
      clauses, contributions, maxDoc, 10, true);
  auto reversed = runConstantTopK(
      clauses, contributions, maxDoc, 10, true, true);
  expectSameScoreDocs(exhaustive, pruned);
  expectSameScoreDocs(exhaustive, reversed);
  EXPECT_GT(pruned.exactPartitions, 0);
  EXPECT_GT(pruned.deadOuterJumps, 0);
}

TEST_F(MultiTermScorerModesTest,
       requiredIntersectionFindsLateScoreWinnersWithoutRequiredProbes) {
  constexpr int32_t maxDoc = 9000;
  std::vector<std::vector<int32_t>> clauses(2);
  for (int32_t doc = 0; doc < 2000; doc++) {
    clauses[(size_t) (doc & 1)].push_back(doc);
  }
  for (int32_t doc : {5000, 7000}) {
    clauses[0].push_back(doc);
    clauses[1].push_back(doc);
  }
  std::array<float, 2> contributions = {1.0f, 1.0f};

  auto exhaustive = runConstantTopK(
      clauses, contributions, maxDoc, 10, false);
  auto pruned = runConstantTopK(
      clauses, contributions, maxDoc, 10, true);
  auto reversed = runConstantTopK(
      clauses, contributions, maxDoc, 10, true, true);
  expectSameScoreDocs(exhaustive, pruned);
  expectSameScoreDocs(exhaustive, reversed);
  EXPECT_GT(pruned.requiredIntersectionWindows, 0);
  EXPECT_EQ(0, pruned.requiredProbeCandidates);
}

TEST_F(MultiTermScorerModesTest,
       requiredIntersectionMatchesExhaustiveAcrossMultiTermScorerModes) {
  std::map<int32_t, std::string> docs;
  for (int32_t doc = 0; doc < 200; doc++) {
    append(docs, doc, doc % 2 == 0 ? "aalpha" : "bbeta");
  }
  append(docs, 4500, "aalpha");
  append(docs, 4500, "bbeta");
  append(docs, 5000, "other");
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, docs);

  for (ScorerMode mode : {ScorerMode::FORCE_EAGER,
                          ScorerMode::FORCE_WINDOWED,
                          ScorerMode::FORCE_HEAP}) {
    auto exhaustive = runPrefixDisjunctionTopK(ti, mode, false);
    auto pruned = runPrefixDisjunctionTopK(ti, mode, true);
    auto reversed = runPrefixDisjunctionTopK(ti, mode, true, true);
    expectSameScoreDocs(exhaustive, pruned);
    expectSameScoreDocs(exhaustive, reversed);
    EXPECT_GT(pruned.requiredIntersectionWindows, 0);
    EXPECT_EQ(0, pruned.requiredProbeCandidates);
  }
}

TEST_F(MultiTermScorerModesTest,
       constantLatticeUsesRepeatedFloatAdditionAndRejectsMixedValues) {
  constexpr int32_t maxDoc = 9000;
  std::vector<std::vector<int32_t>> clauses(3);
  for (int32_t doc = 0; doc < maxDoc; doc += 7) {
    for (auto& clause : clauses) clause.push_back(doc);
  }
  std::array<float, 3> uniform = {0.1f, 0.1f, 0.1f};
  auto uniformExhaustive = runConstantTopK(
      clauses, uniform, maxDoc, 3, false);
  auto uniformPruned = runConstantTopK(
      clauses, uniform, maxDoc, 3, true);
  expectSameScoreDocs(uniformExhaustive, uniformPruned);
  ASSERT_FALSE(uniformPruned.docs.empty());
  float repeated = 0.0f;
  for (float contribution : uniform) repeated += contribution;
  EXPECT_EQ(std::bit_cast<uint32_t>(repeated),
            std::bit_cast<uint32_t>(uniformPruned.docs[0].score));
  EXPECT_GT(uniformPruned.exactPartitions, 0);

  std::array<float, 3> mixed = {0.1f, 0.1f, 0.2f};
  auto mixedExhaustive = runConstantTopK(
      clauses, mixed, maxDoc, 3, false);
  auto mixedPruned = runConstantTopK(
      clauses, mixed, maxDoc, 3, true);
  expectSameScoreDocs(mixedExhaustive, mixedPruned);
  EXPECT_EQ(0, mixedPruned.exactPartitions);
}

TEST_F(MultiTermScorerModesTest,
       constantDisjunctionOrderAndParallelMergeMatchExhaustive) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 3; segment++) {
    for (int32_t doc = 0; doc < 64; doc++) {
      std::string body;
      if (doc % 2 == 0) body = "alpha";
      if (doc % 3 == 0) body += body.empty() ? "beta" : " beta";
      if (body.empty()) body = "other";
      helper.index(
          flatdoc("id", "s" + std::to_string(segment) + "d"
                            + std::to_string(doc),
                  "body_w", body),
          doc == 63 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
    }
  }

  auto run = [&](bool reverseClauses, bool parallel, bool exhaustive) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    auto alpha = qb::prefix(top.mr(), "body_w", "a");
    auto beta = qb::prefix(top.mr(), "body_w", "b");
    top.rawQuery() = reverseClauses
        ? qb::boolean(top.mr(), {}, {beta, alpha})
        : qb::boolean(top.mr(), {}, {alpha, beta});
    top.fields({"id"}).getScores().limit(10);
    if (exhaustive) top.getNumber();
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->toString();

    std::vector<std::pair<std::string, uint32_t>> result;
    const auto* docs = req->docList("q");
    if (docs == nullptr) return result;
    const auto* idColumn = docs->columns.find("id");
    const auto* scoreColumn = docs->columns.find("_score_");
    if (idColumn == nullptr || scoreColumn == nullptr) return result;
    const auto* ids = std::get_if<luxir::api::ColStr>(&idColumn->kind);
    const auto* scores =
        std::get_if<luxir::api::ColFloat>(&scoreColumn->kind);
    if (ids == nullptr || scores == nullptr) return result;
    EXPECT_EQ(ids->v.size(), scores->v.size());
    for (size_t i = 0; i < std::min(ids->v.size(), scores->v.size()); i++) {
      result.emplace_back(
          std::string(ids->v[i]), std::bit_cast<uint32_t>(scores->v[i]));
    }
    return result;
  };

  auto exhaustive = run(false, false, true);
  ASSERT_EQ(10u, exhaustive.size());
  EXPECT_EQ(exhaustive, run(false, false, false));
  EXPECT_EQ(exhaustive, run(true, false, false));
  EXPECT_EQ(exhaustive, run(false, true, false));
  EXPECT_EQ(exhaustive, run(true, true, false));
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
    Query::PlanContext buildContext =
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
        buildScorerForTests(ti.pool, *supplier, 1));
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
    auto* scorer = buildScorerForTests(
        ti.pool, *supplier, 100);  // driven: finite candidates
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
  Query::PlanContext buildContext{
      .demand = Query::Demand::fromLeadCost(1),
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

    Query::PlanContext modeContext =
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
    Query::PlanContext buildContext =
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
        buildScorerForTests(ti.pool, *firstSupplier, 1));
    ASSERT_NE(first, nullptr);

    auto* secondSupplier = weight->scorerSupplier(ti.pool, segment);
    ASSERT_NE(secondSupplier, nullptr);
    EXPECT_FALSE(secondSupplier->fillExpansionMemo(buildContext));
    auto* second = dynamic_cast<MultiTermQuery::Scorer*>(
        buildScorerForTests(ti.pool, *secondSupplier, 1));
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

TEST_F(MultiTermScorerModesTest,
       supplierCostStaysMaxDocUntilExpansionMemoIsFilled) {
  TestIndex ti;
  TestField field(ti, "body_w");
  buildCorpus(field, {{0, "rare_a"}, {1, "other"}, {2, "rare_b"}});

  auto guard = ti.pool.rewindScopeGuard();
  auto& segment = ti.reader->segments()[0];
  Query::Context context(ti.pool, *ti.reader);
  PrefixQuery prefix("body_w", "rare_");
  auto* weight = prefix.createWeight(context, 0);
  auto* supplier = weight->scorerSupplier(ti.pool, segment);
  ASSERT_NE(supplier, nullptr);

  SkipStatsGuard statsGuard;
  EXPECT_EQ(segment.maxDoc(), supplier->cost());
  EXPECT_EQ(0, SkipStats::multitermExpansions);
  EXPECT_TRUE(supplier->fillExpansionMemo(
      MultiTermQuery::Weight::scorerBuildContext(segment.maxDoc())));
  EXPECT_EQ(2, supplier->cost());
  EXPECT_EQ(1, SkipStats::multitermExpansions);
}

TEST_F(MultiTermScorerModesTest,
       truthfulCostMakesRareMultiTermLeadCommonTermConjunction) {
  constexpr int32_t maxDoc = 1000;
  TestIndex ti;
  TestField field(ti, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string body = "common";
    if (doc == 100) body += " rare_a";
    if (doc == 700) body += " rare_b";
    field.add(doc, body);
  }
  ti.flush();
  field.startReading();

  struct Run {
    int64_t count;
    int64_t advanceCalls;
    int64_t sparseFallbacks;
    int64_t denseWindows;
    std::unique_ptr<DocSet> docs;
  };
  auto run = [&](bool disableTruthfulCost) {
    TruthfulCostGuard costGuard(disableTruthfulCost);
    auto poolGuard = ti.pool.rewindScopeGuard();
    Query::Context context(ti.pool, *ti.reader);
    TermQuery common("body_w", "common");
    PrefixQuery rare("body_w", "rare_");
    std::array<Query*, 2> mandatory = {&common, &rare};
    BooleanQuery query(mandatory, {}, {}, {});
    auto* weight = query.createWeight(context, 0);
    auto& segment = context.topReader.segments()[0];

    SkipStatsGuard statsGuard;
    auto* supplier = weight->scorerSupplier(ti.pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr
        ? nullptr : supplier->bulkScorer(ti.pool);
    EXPECT_NE(bulk, nullptr);
    int64_t count = 0;
    DocSetBuilder docsBuilder(segment.maxDoc());
    int32_t next = bulk == nullptr
        ? PostingsReader::END
        : bulk->countNextWindow(
            count, &docsBuilder, nullptr, 0, segment.maxDoc());
    EXPECT_EQ(PostingsReader::END, next);
    auto docs = docsBuilder.build();
    EXPECT_EQ(count, docs->card());
    return Run{
      count,
      SkipStats::advanceCalls,
      SkipStats::conjCountFallbacks,
      SkipStats::conjDenseCountWindows,
      std::move(docs),
    };
  };

  Run maxDocCostOracle = run(true);
  Run truthfulCost = run(false);
  EXPECT_EQ(2, maxDocCostOracle.count);
  EXPECT_EQ(maxDocCostOracle.count, truthfulCost.count);
  EXPECT_EQ(1, maxDocCostOracle.sparseFallbacks);
  EXPECT_EQ(maxDocCostOracle.sparseFallbacks,
            truthfulCost.sparseFallbacks);
  EXPECT_EQ(0, maxDocCostOracle.denseWindows);
  EXPECT_EQ(maxDocCostOracle.denseWindows, truthfulCost.denseWindows);
  EXPECT_LT(truthfulCost.advanceCalls, maxDocCostOracle.advanceCalls);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    EXPECT_EQ(maxDocCostOracle.docs->get(doc), truthfulCost.docs->get(doc));
  }
  EXPECT_TRUE(truthfulCost.docs->get(100));
  EXPECT_TRUE(truthfulCost.docs->get(700));
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
