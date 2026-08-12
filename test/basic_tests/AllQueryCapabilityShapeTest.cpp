#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

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

class AllQueryCapabilityShapeTest : public SoluxTest {
public:
  CollectionHelper helper;
};

TEST_F(AllQueryCapabilityShapeTest,
       supplierShapeIsExactIncludingZeroMaxDoc) {
  ASSERT_TRUE(helper.index(
      flatdoc("id", "0", "body_w", "keep"),
      UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
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
  auto reader = helper.getIndexWriter()->getIndexReader();
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
