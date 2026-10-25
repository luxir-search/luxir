// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/BooleanWeight.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/NumericVerification.h"
#include "test/SchemaBuilder.h"
#include "test/TopKAssert.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Use = Query::ExecutionUse;
using Consumption = Query::MembershipConsumption;
using Answer = Query::ScorerSupplier::BulkAnswer;
using FilterConsumption = Query::ScorerSupplier::WindowFilterConsumption;

class RangeFilterTest : public LuxirTest {
protected:
  static constexpr int32_t N = 32768;
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;

  void SetUp() override {
    SchemaBuilder schema;
    auto& field = schema.field("points_i");
    field.type = api::FieldDef::FieldClass::INT;
    field.index = api::FieldDef::IndexMode::RANGE;
    schema.set(helper.collection());
    std::vector<Doc> docs;
    for (int32_t doc = 0; doc < N; doc++) {
      int64_t value = 2 * ((doc * 1543) % N);
      std::string body = "pad";
      if (doc % 64 == 0) body += " a";
      if (doc % 2048 == 0) body += " rare";
      if (doc % 256 == 0) body += " threshold";
      if (doc % 3 == 0) body += " b";
      if (doc % 64 == 1 || doc % 128 == 0) body += " c";
      if (doc % 16 == 0) body += " dense_a";
      if (doc % 32 == 0) body += " dense_b";
      if (doc % 4 == 0) body += " excluded";
      docs.push_back(flatdoc("id", std::to_string(doc), "body_w", body,
          "column_i", value, "points_i", value, "cluster_i", doc));
      if (doc % 5 != 0) docs.back().push_back({"multi_is", vec_i(value, value + 1000)});
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    reader = std::make_shared<IndexReader>(helper.getIndexWriter()->dir);
    ASSERT_EQ(1u, reader->segments().size());
  }

  std::vector<int32_t> membership(Query& query, Use use, DocSet* domain = nullptr,
                                  Query* enclosing = nullptr, bool reposition = false) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = reader->segments()[0];
    auto* supplier = query.createWeight(context, 0)->scorerSupplier(pool, segment);
    Query::ScorerSupplier::BulkScorerContext bulkContext;
    std::array<Query::ScorerSupplier*, 1> filters;
    if (enclosing != nullptr) {
      filters[0] = enclosing->createWeight(context, 0)->scorerSupplier(pool, segment);
      bulkContext = {filters[0]->cost(), filters, true};
    }
    auto plan = supplier->planBulk(use, bulkContext);
    EXPECT_EQ(Answer::YES, plan.available);
    if (plan.available != Answer::YES) return {};
    EXPECT_EQ(FilterConsumption::NO, plan.windowFilterConsumption);
    auto* bulk = supplier->buildBulk(pool, plan);
    EXPECT_NE(nullptr, bulk);
    if (bulk == nullptr) return {};
    if (reposition) {
      EXPECT_EQ(Answer::YES, plan.supportsReposition());
      ScoreWindow ignored;
      bulk->matchNextWindow(ignored, nullptr, 0, N / 2);
      bulk->prepareMembership();
      bulk->reposition(0);
    }
    std::vector<int32_t> result;
    DocSetBuilder builder(N);
    int64_t count = 0;
    for (int32_t cursor = 0; cursor < N;) {
      int32_t next;
      if (use == Use::COUNT_WINDOWS) {
        next = bulk->countNextWindow(count, &builder, domain, cursor, N);
      } else {
        ScoreWindow out;
        next = bulk->matchNextWindow(out, domain, cursor, N);
        result.insert(result.end(), out.docs.begin(), out.docs.begin() + out.size);
      }
      EXPECT_GT(next, cursor);
      if (next <= cursor) break;
      cursor = next;
    }
    if (use == Use::COUNT_WINDOWS) {
      auto docs = builder.build();
      EXPECT_EQ(count, docs->card());
      for (int32_t doc = 0; doc < N; doc++) if (docs->get(doc)) result.push_back(doc);
    }
    return result;
  }
};

TEST_F(RangeFilterTest, CountAndMatchVerifyOnlyExactSurvivors) {
  TermQuery a("body_w", "a"), b("body_w", "b");
  NumericPredicateQuery range("column_i", 0, 59000);
  std::array<Query*, 2> required{&a, &b};
  std::array<Query*, 1> filters{&range};
  BooleanQuery query(required, {}, {}, filters);
  std::vector<int32_t> expected;
  for (int32_t doc = 0; doc < N; doc += 192) {
    if (2 * ((doc * 1543) % N) <= 59000) expected.push_back(doc);
  }
  for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
    int64_t verified = 0;
    NumericVerificationObserver observe([&](int32_t doc) {
      verified++;
      EXPECT_EQ(0, doc % 192);
    });
    EXPECT_EQ(expected, membership(query, use));
    EXPECT_LE(verified, N / 192 + 1);
#ifndef NDEBUG
    EXPECT_GT(verified, 0);
#endif
  }
}

TEST_F(RangeFilterTest, CandidateFiltersFollowExclusionsAndDomain) {
  TermQuery a("body_w", "a"), b("body_w", "b"), excluded("body_w", "excluded");
  NumericPredicateQuery range("column_i", 0, 59000);
  std::array<Query*, 2> required{&a, &b};
  std::array<Query*, 1> filters{&range}, prohibited{&excluded};
  BooleanQuery positive(required, {}, {}, filters), negative(required, {}, prohibited, filters);
  RAMBitDocSet domain(N);
  domain.mutableBits().set(1);
  int64_t verified = 0;
  NumericVerificationObserver observe([&](int32_t) { verified++; });
  for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
    EXPECT_TRUE(membership(positive, use, &domain).empty());
    EXPECT_TRUE(membership(negative, use).empty());
  }
  EXPECT_EQ(0, verified);
}

TEST_F(RangeFilterTest, SparseCandidateFiltersFollowDomain) {
  TermQuery a("body_w", "rare"), b("body_w", "b");
  NumericPredicateQuery range("column_i", 0, 59000);
  std::array<Query*, 2> required{&a, &b};
  BooleanQuery body(required, {}, {}, {});
  RAMBitDocSet domain(N);
  domain.mutableBits().set(1);
  int64_t verified = 0;
  NumericVerificationObserver observe([&](int32_t) { verified++; });
  for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
    EXPECT_TRUE(membership(body, use, &domain, &range).empty());
  }
  EXPECT_EQ(0, verified);
}

TEST_F(RangeFilterTest, LeadOnlyAndMixedThresholdPlansResetWithFilters) {
  TermQuery a("body_w", "threshold"), b("body_w", "b");
  NumericPredicateQuery range("column_i", 0, 59000);
  std::array<Query*, 2> required{&a, &b};
  std::array<Query*, 1> filters{&range};
  for (size_t size : {1u, 2u}) {
    BooleanQuery query(std::span<Query*>(required).first(size), {}, {}, filters);
    for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
      SkipStatsScope stats;
      auto expected = membership(query, use);
      EXPECT_FALSE(expected.empty());
      EXPECT_EQ(expected, membership(query, use, nullptr, nullptr, true));
      EXPECT_GT(SkipStats::conjDenseCountWindows + SkipStats::conjDenseMatchWindows, 0);
    }
  }
}

TEST_F(RangeFilterTest, NumericSelectionPricesDenseAndSettledProduction) {
  // Scanning a random column costs about as much as verifying most docs.
  // Settled zones keep production for clustered fields, even for few candidates.
  struct Case {
    const char* field;
    int64_t candidates;
    bool verifies;
  };
  for (auto [field, candidates, verifies] : {Case{"column_i", 64, true},
           Case{"column_i", N * 3 / 10, true}, Case{"column_i", N * 9 / 10, false},
           Case{"cluster_i", 64, false}, Case{"cluster_i", N * 3 / 10, false},
           Case{"cluster_i", N * 9 / 10, false}}) {
    SCOPED_TRACE(::testing::Message() << field << " " << candidates);
    NumericPredicateQuery range(field, 4096, 28671);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* supplier = range.createWeight(context, 0)->scorerSupplier(pool, reader->segments()[0]);
    auto build = supplier->makePlanContext(Query::Demand::fromCandidatesAndSpan(
        candidates, N, Use::COUNT_WINDOWS));
    build.membershipConsumption = Consumption::EITHER;
    auto shape = supplier->describeScorer(build);
    auto* plan = supplier->resolve(pool, build);
    EXPECT_EQ(shape.membershipConsumption, plan->shape().membershipConsumption);
    EXPECT_EQ(verifies ? Consumption::CANDIDATES : Consumption::WINDOWS,
              shape.membershipConsumption);
    auto* scorer = plan->build(pool);
    if (verifies) {
      EXPECT_NE(nullptr, dynamic_cast<NumericPredicateQuery::PredicateScorer<
          IntColReader::SparseIterator>*>(scorer));
    } else if (std::string_view(field) == "cluster_i") {
      EXPECT_NE(nullptr, dynamic_cast<NumericPredicateQuery::ZoneMapScorer*>(scorer));
    } else {
      EXPECT_NE(nullptr, dynamic_cast<NumericPredicateQuery::PredicateScorer<
          IntColReader::Iterator>*>(scorer));
    }
  }
}

TEST_F(RangeFilterTest, NestedRangesRetainPointsMissingAndMultivalueSemantics) {
  TermQuery a("body_w", "a"), b("body_w", "b");
  std::array<Query*, 2> terms{&a, &b};
  BooleanQuery body(terms, {}, {}, {});
  std::array<Query*, 1> required{&body};
  for (auto field : {"column_i", "points_i", "multi_is"}) {
    for (auto [lo, hi] : {std::pair<int64_t, int64_t>{0, 2047}, {30000, 59000}}) {
      NumericPredicateQuery range(field, lo, hi);
      std::array<Query*, 1> filters{&range};
      BooleanQuery query(required, {}, {}, filters);
      std::vector<int32_t> expected;
      for (int32_t doc = 0; doc < N; doc += 192) {
        int64_t value = 2 * ((doc * 1543) % N);
        bool matches = lo <= value && value <= hi;
        if (std::string_view(field) == "multi_is") {
          matches = doc % 5 != 0 && (matches
              || (lo <= value + 1000 && value + 1000 <= hi));
        }
        if (matches) expected.push_back(doc);
      }
      for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
        EXPECT_EQ(expected, membership(query, use));
      }
    }
  }
}

TEST_F(RangeFilterTest, EmptyNestedRangesReturnNoScoredMatches) {
  TermQuery a("body_w", "a"), b("body_w", "b");
  std::array<Query*, 2> terms{&a, &b};
  BooleanQuery body(terms, {}, {}, {});
  std::array<Query*, 1> required{&body};
  for (auto field : {"column_i", "points_i"}) {
    NumericPredicateQuery range(field, 1, 1);
    std::array<Query*, 1> filters{&range};
    BooleanQuery query(required, {}, {}, filters);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* supplier = query.createWeight(context, Query::NEED_SCORES)
        ->scorerSupplier(pool, reader->segments()[0]);
    if (supplier == nullptr) continue;
    auto* bulk = supplier->bulkScorer(pool);
    TopDocsCollector collector(10);
    if (bulk != nullptr) {
      collectTopKWindowed(0, bulk, nullptr, collector, nullptr, N, true);
    } else if (auto* scorer = buildScorerForTests(pool, *supplier, INT64_MAX)) {
      collectTopK(0, scorer, nullptr, nullptr, collector, true);
    }
    EXPECT_EQ(0, collector.totalHits());
  }
}

TEST_F(RangeFilterTest, ScoredConsumersHonorDeclaredFilterConsumption) {
  TermQuery a("body_w", "a"), b("body_w", "c");
  TermQuery da("body_w", "dense_a"), db("body_w", "dense_b");
  NumericPredicateQuery range("column_i", 0, 59000);
  NumericPredicateQuery points("points_i", 0, 59000);
  std::array<Query*, 1> required{&a}, optional{&b}, filters{&range};
  std::array<Query*, 1> knownFilters{&points};
  std::array<Query*, 2> disjunction{&a, &b}, conjunction{&da, &db};
  BooleanQuery term(required, {}, {}, filters), mandopt(required, optional, {}, knownFilters);
  BooleanQuery maxscore({}, disjunction, {}, knownFilters, 1), dense(conjunction, {}, {}, filters);
  for (Query* query : {&term, &dense, &mandopt, &maxscore}) {
    std::vector<TopDocsCollector::ScoreDoc> expected;
    for (bool pull : {true, false}) {
      MemPool pool;
      Query::Context context(pool, *reader);
      auto* supplier = query->createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING)
          ->scorerSupplier(pool, reader->segments()[0]);
      TopDocsCollector collector(1000);
      SkipStatsScope stats;
      if (pull) {
        auto* scorer = buildScorerForTests(pool, *supplier, INT64_MAX);
        collectTopK(0, scorer, nullptr, nullptr, collector, true);
        auto hits = collector.sort();
        expected.assign(hits.begin(), hits.end());
      } else {
        auto plan = supplier->planBulk(Use::SCORED_WINDOWS, {});
        ASSERT_EQ(Answer::YES, plan.available);
        bool probes = query == &term || query == &dense;
        EXPECT_EQ(probes ? FilterConsumption::FILL_OR_PROBE : FilterConsumption::FILL,
                  plan.windowFilterConsumption);
        auto* bulk = supplier->buildBulk(pool, plan);
        ASSERT_NE(nullptr, bulk);
        int64_t verified = 0;
        NumericVerificationObserver observe([&](int32_t) { verified++; });
        collectTopKWindowed(0, bulk, nullptr, collector, nullptr, N, true);
        assertTopKEquivalent(expected, collector.sort());
        if (probes) {
          EXPECT_LE(verified, 1024);
        } else {
          EXPECT_EQ(0, SkipStats::numericPredicateSparseVerifyArms);
        }
        if (query == &dense) {
          EXPECT_GT(SkipStats::conjDenseScoredWindows, 0);
        }
      }
    }
  }
}

TEST_F(RangeFilterTest, FillOnlyBodiesKeepUnknownFiltersOnPull) {
  TermQuery a("body_w", "a"), b("body_w", "c");
  NumericPredicateQuery range("column_i", 0, 59000);
  std::array<Query*, 1> required{&a}, optional{&b}, filters{&range};
  std::array<Query*, 2> disjunction{&a, &b};
  BooleanQuery mandopt(required, optional, {}, {}), maxscore({}, disjunction, {}, {}, 1);
  for (Query* body : {&mandopt, &maxscore}) {
    std::array<Query*, 1> bodies{body};
    BooleanQuery query(bodies, {}, {}, filters);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* supplier = query.createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING)
        ->scorerSupplier(pool, reader->segments()[0]);
    EXPECT_EQ(Answer::NO, supplier->planBulk(Use::SCORED_WINDOWS, {}).available);
    auto* scorer = buildScorerForTests(pool, *supplier, INT64_MAX);
    int32_t count = 0, expected = 0;
    while (scorer->next() != PostingsReader::END) count++;
    for (int32_t doc = 0; doc < N; doc++) {
      bool positive = doc % 64 == 0 || (body == &maxscore && doc % 64 == 1);
      expected += positive && 2 * ((doc * 1543) % N) <= 59000;
    }
    EXPECT_EQ(expected, count);
  }
}

TEST_F(RangeFilterTest, ProbePreparationDoesNotFillAndObserversRestore) {
  NumericPredicateQuery range("column_i", 0, 59000);
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* supplier = range.createWeight(context, 0)->scorerSupplier(pool, reader->segments()[0]);
  auto build = supplier->makePlanContext(Query::Demand::fromCandidatesAndSpan(2, N));
  build.membershipConsumption = Consumption::CANDIDATES;
  auto* plan = supplier->resolve(pool, build);
  ASSERT_EQ(Consumption::CANDIDATES, plan->shape().membershipConsumption);
  std::array<Query::Scorer*, 1> scorers{plan->build(pool)};
  WindowFilter filter(pool, scorers, true);
  int64_t verified = 0;
  NumericVerificationObserver outer([&](int32_t) { verified++; });
  EXPECT_GT(filter.prepare(0, 128), 0);
  EXPECT_EQ(0, verified);
  {
    NumericVerificationObserver inner([](int32_t) { FAIL() << "empty mask verified"; });
    std::array<uint64_t, 2> empty{};
    filter.intersect(empty, 0);
  }
  std::array<uint64_t, 2> bits{1, 1};
  filter.intersect(bits, 0);
#ifndef NDEBUG
  EXPECT_EQ(2, verified);
#endif
}

} // namespace
