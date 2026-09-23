// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct DenseClauseGuard {
  bool saved = AllQuery::disableDenseClauseForTests;

  explicit DenseClauseGuard(bool disabled) {
    AllQuery::disableDenseClauseForTests = disabled;
  }

  ~DenseClauseGuard() {
    AllQuery::disableDenseClauseForTests = saved;
  }
};

struct SkipStatsGuard {
  bool saved = SkipStats::enabled;

  SkipStatsGuard() {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
    SkipStats::reset();
  }
};

void expectExactAllShape(const Query::ScorerShape& shape,
                         Query::ClauseShape windowFillClause) {
  EXPECT_EQ(Query::MatchState::NONEMPTY, shape.matchState);
  EXPECT_EQ(Query::DirectScorerKind::OTHER, shape.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::NO, shape.reportedTwoPhase);
  EXPECT_EQ(windowFillClause, shape.windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE, shape.termDisjunctionClause);
  EXPECT_EQ(Query::IndependentTermAccess::UNSUPPORTED,
            shape.independentTerm);
  EXPECT_EQ(Query::DocsOnlyAccess::UNSUPPORTED, shape.docsOnly);
  EXPECT_EQ(Query::DirectDocSetAccess::UNSUPPORTED, shape.directDocSet);
}

} // namespace

class AllQueryCapabilityShapeTest : public LuxirTest {
public:
  CollectionHelper helper;
};

TEST_F(AllQueryCapabilityShapeTest,
       supplierShapeIsExactIncludingZeroMaxDoc) {
  ASSERT_TRUE(helper.index(
      flatdoc("id", "0", "body_w", "keep"),
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& sourceSegment = reader->segments()[0];

  auto postings = std::shared_ptr<PostingsReader>(
      &sourceSegment.postingsReader(), [](PostingsReader*) {});
  Segment::SegmentInfo info{
    .seg_id = 0,
    .live_gen = 0,
    .min_version = 0,
    .max_version = 0,
    .commit_time = 0,
    .max_doc = 0,
    .live_docs = 0,
  };
  Segment zeroSegment(
      std::move(postings), std::shared_ptr<LiveDocs>{},
      std::vector<std::shared_ptr<AuxReader>>{}, info, 0, 0);
  AllQuery::Supplier supplier(zeroSegment, 0.0f);
  MemPool pool;

  EXPECT_EQ(0, supplier.cost());
  expectExactAllShape(
      supplier.describeScorer({}), Query::ClauseShape::DIRECT);
  EXPECT_NE(nullptr, buildScorerForTests(
      pool, supplier, std::numeric_limits<int64_t>::max()));
}

TEST_F(AllQueryCapabilityShapeTest, disableSwitchGatesDenseClauseProtocol) {
  ASSERT_TRUE(helper.index(
      flatdoc("id", "0", "body_w", "keep"),
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  MemPool pool;

  {
    DenseClauseGuard enabled(false);
    AllQuery::Supplier supplier(segment, 0.0f);
    expectExactAllShape(
        supplier.describeScorer({}), Query::ClauseShape::DIRECT);
    EXPECT_EQ(Query::ClauseShape::DIRECT,
              resolveScorerPlanForTests(pool, supplier, segment.maxDoc())
                  ->shape().windowFillClause);
  }
  {
    DenseClauseGuard disabled(true);
    AllQuery::Supplier supplier(segment, 0.0f);
    expectExactAllShape(
        supplier.describeScorer({}), Query::ClauseShape::NONE);
    EXPECT_EQ(Query::ClauseShape::NONE,
              resolveScorerPlanForTests(pool, supplier, segment.maxDoc())
                  ->shape().windowFillClause);
  }
}

TEST_F(AllQueryCapabilityShapeTest,
       pureNegativeCountUsesDenseNegatedRoute) {
  const int32_t numDocs = DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  docs.reserve((size_t) numDocs);
  int64_t expectedCount = 0;
  for (int32_t doc = 0; doc < numDocs; doc++) {
    bool excluded = (doc % 7) == 0;
    std::string body = excluded ? "excluded" : "keep";
    docs.push_back(flatdoc(
        "id", std::to_string(doc), "body_w", body));
    expectedCount += !excluded;
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto runCount = [&]() {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").getNumber().limit(0);
    top.rawQuery() = qb::boolean(
        top.mr(), {}, {},
        {qb::match(top.mr(), "body_w", "excluded")});
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount();
  };

  int64_t oracle;
  {
    DenseClauseGuard disabled(true);
    oracle = runCount();
  }
  EXPECT_EQ(expectedCount, oracle);

  SkipStatsGuard stats;
  {
    DenseClauseGuard enabled(false);
    EXPECT_EQ(oracle, runCount());
  }
  EXPECT_GT(SkipStats::conjDenseCountWindows, 0);
  EXPECT_GT(SkipStats::negatedCountWindows, 0);
  EXPECT_GT(SkipStats::negatedCountExclFills, 0);
  EXPECT_EQ(SkipStats::conjDirectDenseEngagements, 0);
}

TEST_F(AllQueryCapabilityShapeTest,
       limitOnlyConstantWindowDrainStopsAfterTopK) {
  constexpr int64_t topCount = 10;
  const int32_t numDocs = DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  docs.reserve((size_t) numDocs);
  for (int32_t doc = 0; doc < numDocs; doc++) {
    docs.push_back(flatdoc(
        "id", std::to_string(doc), "body_w", "keep"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];

  auto collect = [&](ConstantScoreDrain drain) -> int64_t {
    MemPool pool;
    AllQuery::Supplier supplier(segment, 1.0f);
    Query::ScorerSupplier::BulkScorerContext context;
    auto plan = supplier.planBulk(
        Query::ExecutionUse::SCORED_WINDOWS, context);
    EXPECT_EQ(Query::ScorerSupplier::BulkAnswer::YES, plan.available);
    if (plan.available != Query::ScorerSupplier::BulkAnswer::YES) return -1;
    auto* bulk = supplier.buildBulk(pool, plan);
    EXPECT_NE(nullptr, bulk);
    if (bulk == nullptr) return -1;
    TopDocsCollector collector(topCount);
    collectFirstKConstantWindowed(
        0, bulk, nullptr, nullptr, collector, segment.maxDoc(), drain);
    return collector.totalHits();
  };

  EXPECT_EQ(topCount, collect(ConstantScoreDrain::LIMIT_ONLY));
  EXPECT_EQ(numDocs, collect(ConstantScoreDrain::COMPLETE));
}

TEST_F(AllQueryCapabilityShapeTest,
       pureNegativeExactCountTopKComposesNegatedCountAndFirstK) {
  const int32_t numDocs = DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  std::vector<std::string> expectedTop;
  docs.reserve((size_t) numDocs);
  for (int32_t doc = 0; doc < numDocs; doc++) {
    bool excluded = (doc % 7) == 0;
    docs.push_back(flatdoc(
        "id", std::to_string(doc), "body_w",
        excluded ? "excluded" : "keep"));
    if (!excluded && expectedTop.size() < 100) {
      expectedTop.push_back(std::to_string(doc));
    }
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q")
      .getNumber().fields({"id"}).limit(100);
  top.rawQuery() = qb::boolean(
      top.mr(), {}, {},
      {qb::match(top.mr(), "body_w", "excluded")});

  SkipStatsGuard stats;
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  std::vector<std::string> actualTop;
  for (const Doc& doc : req->getDocs()) {
    const FieldVal* id = luxir::test::find(doc, "id");
    ASSERT_NE(nullptr, id);
    actualTop.push_back(std::get<std::string>(*id));
  }
  EXPECT_EQ(numDocs - (numDocs + 6) / 7, req->getMatchCount());
  EXPECT_EQ(expectedTop, actualTop);
  EXPECT_GT(SkipStats::exactCountTopKCompositions, 0);
  EXPECT_GT(SkipStats::negatedCountWindows, 0);
  EXPECT_GT(SkipStats::negatedCountExclFills, 0);
  EXPECT_EQ(SkipStats::bulkExclusionShapeFallbacks, 0);
}
