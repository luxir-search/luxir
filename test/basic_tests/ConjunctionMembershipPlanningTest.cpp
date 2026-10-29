// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/BooleanWeight.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/TermQuery.h"
#include "test/CollectionHelper.h"
#include "test/ConjunctionPlanControl.h"
#include "test/NumericVerification.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Use = Query::ExecutionUse;
using Consumption = Query::MembershipConsumption;
using Answer = Query::ScorerSupplier::BulkAnswer;

class ConjunctionMembershipPlanningTest : public LuxirTest {
protected:
  static constexpr int32_t N = 131072;
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;

  struct Result {
    std::array<std::vector<size_t>, 2> stages;
    std::vector<int32_t> docs;
    std::map<std::string, int64_t> verified;
  };

  static uint64_t sample(int32_t doc, uint64_t seed) {
    uint64_t value = seed + ((uint64_t)doc + 1) * 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return (value ^ (value >> 31)) & 0x7fffffffffffffffULL;
  }

  static int64_t a(int32_t doc) { return (int64_t)(sample(doc, 0) % 100000); }
  static int64_t b(int32_t doc) { return (int64_t)(sample(doc, 7) % 100000); }
  static bool term(int32_t doc, int32_t percent, uint64_t seed = 1) {
    return sample(doc, seed) % 100 < (uint64_t)percent;
  }
  static bool narrow(int32_t doc) { return a(doc) >= 49995 && a(doc) <= 50004; }
  static int64_t mixed(int32_t doc) {
    return doc < N / 4 ? 0 : doc >= 3 * N / 4 ? 100000 : a(doc);
  }

  void SetUp() override {
    SchemaBuilder schema;
    auto& points = schema.field("points_i");
    points.type = api::FieldDef::FieldClass::INT;
    points.index = api::FieldDef::IndexMode::RANGE;
    schema.set(helper.collection());
    std::vector<Doc> docs;
    for (int32_t doc = 0; doc < N; doc++) {
      std::string body = "pad";
      if (term(doc, 10)) body += " t10";
      if (term(doc, 40)) body += " t40";
      if (term(doc, 45)) body += " t45";
      if (term(doc, 50, 2)) body += " u50";
      docs.push_back(flatdoc("id", std::to_string(doc), "body_w", body,
          "a_i", a(doc), "b_i", b(doc), "mixed_i", mixed(doc), "cluster_i", doc,
          "points_i", a(doc), "multi_is", vec_i(a(doc), b(doc)),
          "under_i", doc % 64 == 0 ? (int64_t)49900 : a(doc)));
      if (doc % 3 != 0) docs.back().push_back({"missing_i", a(doc)});
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    reader = std::make_shared<IndexReader>(helper.getIndexWriter()->dir);
    ASSERT_EQ(1u, reader->segments().size());
  }

  template <class Match>
  static std::vector<int32_t> ids(Match matches, int32_t min = 0, int32_t max = N) {
    std::vector<int32_t> result;
    for (int32_t doc = min; doc < max; doc++) if (matches(doc)) result.push_back(doc);
    return result;
  }

  Result run(Query& query, Use use, std::span<const size_t> order = {},
             std::span<const Consumption> consumption = {}, bool reposition = false,
             int32_t min = 0, int32_t max = INT32_MAX, int32_t bound = INT32_MAX) {
    Result result;
    BooleanQuery::ConjunctionControlForTests control{order, consumption, true,
        [&](size_t ordinal, Consumption stage, size_t position, Query::ScorerSupplier&,
            const Query::PlanContext&, const Query::ScorerShape& shape) {
          auto& sequence = result.stages[stage == Consumption::CANDIDATES];
          EXPECT_EQ(sequence.size(), position);
          EXPECT_EQ(shape.membershipConsumption, stage);
          sequence.push_back(ordinal);
        }};
    ConjunctionPlanControl observe(control);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = reader->segments()[0];
    max = std::min(max, segment.maxDoc());
    auto* supplier = query.createWeight(context, 0)->scorerSupplier(pool, segment);
    auto plan = supplier->planBulk(use, {});
    EXPECT_EQ(Answer::YES, plan.available);
    if (plan.available != Answer::YES) return result;
    auto* bulk = supplier->buildBulk(pool, plan);
    if (reposition) {
      EXPECT_EQ(Answer::YES, plan.supportsReposition());
      ScoreWindow ignored;
      bulk->matchNextWindow(ignored, nullptr, 0, max / 2);
      bulk->prepareMembership();
      bulk->reposition(0);
    }
    int32_t verifyMin = min, verifyMax = max;
    NumericVerificationObserver verification([&](const NumericPredicateQuery& leaf, int32_t doc) {
      EXPECT_GE(doc, verifyMin);
      EXPECT_LT(doc, verifyMax);
      result.verified[std::string(leaf.getField())]++;
    });
    DocSetBuilder builder(segment.maxDoc());
    int64_t count = 0;
    for (int32_t cursor = min; cursor < max;) {
      int32_t end = (int32_t)std::min((int64_t)max, (int64_t)cursor + bound);
      verifyMin = cursor;
      verifyMax = end;
      int32_t next;
      if (use == Use::COUNT_WINDOWS) {
        next = bulk->countNextWindow(count, &builder, nullptr, cursor, end);
      } else {
        ScoreWindow out;
        next = bulk->matchNextWindow(out, nullptr, cursor, end);
        for (int32_t doc : out.docs.first((size_t)out.size)) {
          EXPECT_GE(doc, cursor);
          EXPECT_LT(doc, end);
          result.docs.push_back(doc);
        }
      }
      EXPECT_GT(next, cursor);
      if (next <= cursor) break;
      cursor = std::min(next, end);
    }
    if (use == Use::COUNT_WINDOWS) {
      auto set = builder.build();
      EXPECT_EQ(count, set->card());
      for (int32_t doc = min; doc < max; doc++) if (set->get(doc)) result.docs.push_back(doc);
    }
    return result;
  }
};

TEST_F(ConjunctionMembershipPlanningTest, SelectiveScanLeadsInBothOrders) {
  NumericPredicateQuery selective("a_i", 49500, 50499), broad("b_i", 40000, 59999);
  auto matches = [](int32_t doc) { return a(doc) >= 49500 && a(doc) <= 50499 && b(doc) >= 40000 && b(doc) <= 59999; };
  for (bool reverse : {false, true}) {
    std::array<Query*, 2> clauses{&broad, &selective};
    if (reverse) std::swap(clauses[0], clauses[1]);
    BooleanQuery query{std::span(clauses).first(1), {}, {}, std::span(clauses).last(1)};
    for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
      auto result = run(query, use, {}, {}, true, 13, N - 19, 997);
      EXPECT_EQ(ids(matches, 13, N - 19), result.docs);
      EXPECT_EQ((std::vector<size_t>{reverse ? 0u : 1u}), result.stages[0]);
      EXPECT_EQ((std::vector<size_t>{reverse ? 1u : 0u}), result.stages[1]);
    }
    if (!reverse) {
      std::array<size_t, 2> order{0, 1};
      std::array<Consumption, 2> consumption{Consumption::WINDOWS, Consumption::CANDIDATES};
      auto incumbent = run(query, Use::COUNT_WINDOWS, order, consumption);
      auto selected = run(query, Use::COUNT_WINDOWS);
      EXPECT_EQ(incumbent.docs, selected.docs);
#ifndef NDEBUG
      EXPECT_EQ(ids([](int32_t doc) { return a(doc) >= 49500 && a(doc) <= 50499; }).size(),
          (size_t)selected.verified["b_i"]);
      EXPECT_GT(incumbent.verified["b_i"], selected.verified["b_i"] * 10);
#endif
    }
  }
}

TEST_F(ConjunctionMembershipPlanningTest, SelectiveVerifierKeepsTermLead) {
  TermQuery t40("body_w", "t40");
  NumericPredicateQuery broad("b_i", 0, 89999), selective("a_i", 49995, 50004), correlated("mixed_i", 0, 89999);
  for (bool many : {false, true}) {
    auto matches = [&](int32_t doc) { return term(doc, 40) && narrow(doc) && b(doc) < 90000 && (!many || mixed(doc) < 90000); };
    for (bool reverse : {false, true}) {
      std::vector<Query*> clauses{&t40, &broad};
      if (many) clauses.push_back(&correlated);
      clauses.push_back(&selective);
      if (reverse) std::swap(clauses[1], clauses.back());
      BooleanQuery query{std::span(clauses).first(clauses.size() - 1), {}, {}, std::span(clauses).last(1)};
      // The ranges verify behind the term; the planner orders that split.
      std::vector<Consumption> consumption(clauses.size(), Consumption::CANDIDATES);
      consumption[0] = Consumption::WINDOWS;
      for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
        auto result = run(query, use, {}, consumption, true, 7, N - 11, 1531);
        EXPECT_EQ(ids(matches, 7, N - 11), result.docs);
        EXPECT_EQ((std::vector<size_t>{0}), result.stages[0]);
        ASSERT_FALSE(result.stages[1].empty());
        EXPECT_EQ(reverse ? 1u : clauses.size() - 1, result.stages[1][0]);
      }
      if (!reverse) {
        std::vector<size_t> order = many ? std::vector<size_t>{0, 2, 1, 3} : std::vector<size_t>{0, 1, 2};
        auto incumbent = run(query, Use::COUNT_WINDOWS, order, consumption);
        auto selected = run(query, Use::COUNT_WINDOWS, {}, consumption);
        EXPECT_EQ(incumbent.docs, selected.docs);
#ifndef NDEBUG
        EXPECT_EQ(ids([&](int32_t doc) { return term(doc, 40) && narrow(doc) && (!many || doc < 3 * N / 4); }).size(),
            (size_t)selected.verified["b_i"]);
        EXPECT_GT(incumbent.verified["b_i"], selected.verified["b_i"] * 100);
        if (many) {
          EXPECT_GT(incumbent.verified["mixed_i"], selected.verified["mixed_i"] * 100);
        }
#endif
      }
    }
  }
}

TEST_F(ConjunctionMembershipPlanningTest, TwoTermServerShapeOrdersBothFilters) {
  TermQuery t10("body_w", "t10"), u50("body_w", "u50");
  NumericPredicateQuery broad("b_i", 0, 89999), selective("a_i", 49500, 50499);
  auto expected = ids([](int32_t doc) { return term(doc, 10) && term(doc, 50, 2) && b(doc) < 90000 && a(doc) >= 49500 && a(doc) <= 50499; });
  for (bool reverse : {false, true}) {
    std::array<Query*, 4> clauses{&t10, &u50, &broad, &selective};
    if (reverse) std::swap(clauses[2], clauses[3]);
    BooleanQuery query{std::span(clauses).first(2), {}, {}, std::span(clauses).last(2)};
    for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
      auto result = run(query, use);
      EXPECT_EQ(expected, result.docs);
      EXPECT_EQ((std::vector<size_t>{0, 1}), result.stages[0]);
      EXPECT_EQ((reverse ? std::vector<size_t>{2, 3} : std::vector<size_t>{3, 2}), result.stages[1]);
    }
  }
  std::array<Query*, 2> terms{&t10, &u50};
  std::array<Query*, 1> filters{&selective};
  BooleanQuery single(terms, {}, {}, filters);
  auto result = run(single, Use::COUNT_WINDOWS);
  EXPECT_EQ((std::vector<size_t>{0, 1}), result.stages[0]);
  EXPECT_EQ((std::vector<size_t>{2}), result.stages[1]);
}

TEST_F(ConjunctionMembershipPlanningTest, UnknownCandidateIsAnOrderingBarrier) {
  TermQuery t10("body_w", "t10");
  NumericPredicateQuery settled("cluster_i", 0, N / 5), missing("missing_i", 0, 89999), selective("a_i", 49995, 50004);
  std::array<Query*, 4> clauses{&t10, &settled, &missing, &selective};
  BooleanQuery query{std::span(clauses).first(3), {}, {}, std::span(clauses).last(1)};
  std::array<Consumption, 4> consumption{Consumption::WINDOWS, Consumption::CANDIDATES,
      Consumption::CANDIDATES, Consumption::CANDIDATES};
  auto result = run(query, Use::COUNT_WINDOWS, {}, consumption);
  EXPECT_EQ((std::vector<size_t>{0}), result.stages[0]);
  EXPECT_EQ((std::vector<size_t>{1, 2, 3}), result.stages[1]);
  EXPECT_EQ(ids([](int32_t doc) { return term(doc, 10) && doc <= N / 5 && doc % 3 != 0 && narrow(doc); }), result.docs);
}

TEST_F(ConjunctionMembershipPlanningTest, QuotesRespectScopeAndProofProjection) {
  MemPool pool;
  Query::Context context(pool, *reader);
  auto& segment = reader->segments()[0];
  auto demand = Query::PlanContext::fromLeadCost(N, Use::COUNT_WINDOWS);
  for (auto field : {"a_i", "missing_i", "multi_is", "points_i"}) {
    NumericPredicateQuery query(field, 40000, 59999);
    auto* supplier = query.createWeight(context, 0)->scorerSupplier(pool, segment);
    for (auto consumption : {Consumption::WINDOWS, Consumption::CANDIDATES}) {
      demand.membershipConsumption = consumption;
      EXPECT_EQ(std::string_view(field) == "a_i", supplier->quoteMembership(demand).has_value());
    }
    demand.demand.horizon = Use::PULL;
    EXPECT_FALSE(supplier->quoteMembership(demand));
    demand.demand.horizon = Use::COUNT_WINDOWS;
  }
  std::array<int64_t, 2> values{40000, 59999};
  std::array<PointsReader::ValueRange, 2> ranges{{{40000, 40000}, {59999, 59999}}};
  NumericPredicateQuery exact("a_i", values, ranges);
  auto* exactSupplier = exact.createWeight(context, 0)->scorerSupplier(pool, segment);
  EXPECT_FALSE(exactSupplier->quoteMembership(demand));
  NumericPredicateQuery settled("cluster_i", 0, N / 4 - 1);
  auto* supplier = settled.createWeight(context, 0)->scorerSupplier(pool, segment);
  demand.demand = Query::Demand::fromCandidatesAndSpan(1, 1, Use::COUNT_WINDOWS);
  auto quote = supplier->quoteMembership(demand);
  ASSERT_TRUE(quote);
  EXPECT_DOUBLE_EQ(1, quote->matches);
  demand.membershipConsumption = Consumption::WINDOWS;
  demand.demand = Query::Demand::fromCandidatesAndSpan(N, N, Use::COUNT_WINDOWS);
  EXPECT_FALSE(supplier->quoteMembership(demand));
}

TEST_F(ConjunctionMembershipPlanningTest, UnderestimatedYieldDoesNotClearLeadMargin) {
  TermQuery t45("body_w", "t45");
  NumericPredicateQuery range("under_i", 49995, 50004);
  std::array<Query*, 1> terms{&t45}, filters{&range};
  BooleanQuery query(terms, {}, {}, filters);
  auto matches = [](int32_t doc) { return doc % 64 != 0 && narrow(doc); };
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* supplier = range.createWeight(context, 0)->scorerSupplier(pool, reader->segments()[0]);
  auto quote = supplier->quoteMembership(Query::PlanContext::fromLeadCost(N, Use::COUNT_WINDOWS));
  ASSERT_TRUE(quote);
  EXPECT_LT(quote->matches, (double)ids(matches).size() / 2);
  std::array<Consumption, 2> split{Consumption::WINDOWS, Consumption::CANDIDATES};
  for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
    auto result = run(query, use, {}, split);
    EXPECT_EQ((std::vector<size_t>{0}), result.stages[0]);
    EXPECT_EQ(ids([&](int32_t doc) { return term(doc, 45) && matches(doc); }), result.docs);
  }
}

TEST_F(ConjunctionMembershipPlanningTest, AdverseYieldRejectsAnOtherwiseCheaperLead) {
  NumericPredicateQuery incumbent("b_i", 10000, 89999), challenger("a_i", 25000, 74999);
  std::array<Query*, 1> mandatory{&incumbent}, filters{&challenger};
  BooleanQuery query(mandatory, {}, {}, filters);
  MemPool pool;
  Query::Context context(pool, *reader);
  auto& segment = reader->segments()[0];
  auto* oldLead = incumbent.createWeight(context, 0)->scorerSupplier(pool, segment);
  auto* newLead = challenger.createWeight(context, 0)->scorerSupplier(pool, segment);
  auto demand = Query::PlanContext::fromLeadCost(N, Use::COUNT_WINDOWS);
  auto oldProduction = oldLead->quoteMembership(demand);
  auto newProduction = newLead->quoteMembership(demand);
  ASSERT_TRUE(oldProduction);
  ASSERT_TRUE(newProduction);
  demand.membershipConsumption = Consumption::CANDIDATES;
  demand.demand.candidates = (int64_t)std::ceil(oldProduction->matches);
  auto oldPlan = oldProduction->work + newLead->quoteMembership(demand)->work;
  demand.demand.candidates = (int64_t)std::ceil(newProduction->matches);
  auto newPlan = newProduction->work + oldLead->quoteMembership(demand)->work;
  ASSERT_LE(newPlan, 0.75 * oldPlan);
  std::array<Consumption, 2> split{Consumption::WINDOWS, Consumption::CANDIDATES};
  for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
    auto result = run(query, use, {}, split);
    EXPECT_EQ((std::vector<size_t>{0}), result.stages[0]);
    EXPECT_EQ(ids([](int32_t doc) { return b(doc) >= 10000 && b(doc) < 90000 && a(doc) >= 25000 && a(doc) < 75000; }), result.docs);
  }
}

TEST_F(ConjunctionMembershipPlanningTest, UnquotedSuppliersKeepCostOrder) {
  TermQuery t10("body_w", "t10");
  NumericPredicateQuery missing("missing_i", 0, 89999), multi("multi_is", 0, 89999), points("points_i", 0, 89999);
  std::array<int64_t, 2> values{40000, 59999};
  std::array<PointsReader::ValueRange, 2> ranges{{{40000, 40000}, {59999, 59999}}};
  NumericPredicateQuery exact("a_i", values, ranges);
  for (auto* range : {&missing, &multi, &points, &exact}) {
    std::array<Query*, 1> terms{&t10}, filters{range};
    BooleanQuery query(terms, {}, {}, filters);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = reader->segments()[0];
    auto termCost = t10.createWeight(context, 0)->scorerSupplier(pool, segment)->cost();
    auto rangeCost = range->createWeight(context, 0)->scorerSupplier(pool, segment)->cost();
    std::array<size_t, 2> order = termCost <= rangeCost
        ? std::array<size_t, 2>{0, 1} : std::array<size_t, 2>{1, 0};
    auto expected = ids([&](int32_t doc) {
      if (!term(doc, 10)) return false;
      if (range == &missing) return doc % 3 != 0 && a(doc) < 90000;
      if (range == &multi) return a(doc) < 90000 || b(doc) < 90000;
      if (range == &points) return a(doc) < 90000;
      return a(doc) == 40000 || a(doc) == 59999;
    });
    for (auto use : {Use::COUNT_WINDOWS, Use::MATCH_WINDOWS}) {
      auto incumbent = run(query, use, order);
      auto normal = run(query, use);
      EXPECT_EQ(incumbent.stages, normal.stages);
      EXPECT_EQ(expected, normal.docs);
      EXPECT_EQ(expected, incumbent.docs);
    }
  }
}

TEST_F(ConjunctionMembershipPlanningTest, AdmissionSketchAliasKeepsTheIncumbent) {
  // Scale the admission fixture's prefix to get past the exact 16k sketch
  // tail without building millions of documents in a unit test.
  constexpr int32_t M = 524288;
  CollectionHelper alias("membership_alias");
  auto writer = alias.getIndexWriter();
  auto& inverter = writer->obtainInverter();
  auto& body = inverter.getIndexHandler("body_w");
  auto& field = inverter.getIndexHandler("alias_i");
  auto value = [](int32_t doc) { return doc < M / 2 ? 10000 + a(doc) % 90000 : doc % 512 == 0 ? 0 : a(doc); };
  for (int32_t doc = 0; doc < M; doc++) {
    inverter.startDoc();
    body.index(inverter, term(doc, 10) ? (term(doc, 50, 2) ? "t10 u50" : "t10") : (term(doc, 50, 2) ? "u50" : "pad"));
    field.index(inverter, value(doc));
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter, true);
  alias.commit();
  reader = std::make_shared<IndexReader>(writer->dir);
  ASSERT_EQ(1u, reader->segments().size());
  ASSERT_EQ(M, reader->segments()[0].maxDoc());
  TermQuery t10("body_w", "t10"), u50("body_w", "u50");
  NumericPredicateQuery range("alias_i", 0, 9999);
  std::array<Query*, 2> terms{&t10, &u50};
  std::array<Query*, 1> filters{&range};
  BooleanQuery query(terms, {}, {}, filters);
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* supplier = range.createWeight(context, 0)->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, supplier);
  auto truth = ids([&](int32_t doc) { return value(doc) < 10000; }, 0, M);
  EXPECT_GT(supplier->membershipWork(M).matches, (double)truth.size() * 3);
  auto result = run(query, Use::COUNT_WINDOWS);
  EXPECT_EQ((std::vector<size_t>{0, 1}), result.stages[0]);
  EXPECT_EQ((std::vector<size_t>{2}), result.stages[1]);
  EXPECT_EQ(ids([&](int32_t doc) { return term(doc, 10) && term(doc, 50, 2) && value(doc) < 10000; }, 0, M), result.docs);
}

TEST_F(ConjunctionMembershipPlanningTest, OpaqueDomainsScoredAndPullDoNotUseThePlanner) {
  TermQuery t40("body_w", "t40");
  NumericPredicateQuery broad("b_i", 0, 89999), selective("a_i", 49995, 50004);
  std::array<Query*, 2> terms{&t40, &broad};
  std::array<Query*, 1> filters{&selective};
  BooleanQuery query(terms, {}, {}, filters);
  BooleanQuery::ConjunctionControlForTests control{{}, {}, true,
      [](size_t, Consumption, size_t, Query::ScorerSupplier&, const Query::PlanContext&, const Query::ScorerShape&) {
        ADD_FAILURE() << "non-membership planning entered exhaustive membership ordering";
      }};
  ConjunctionPlanControl observe(control);
  auto expected = ids([](int32_t doc) { return term(doc, 40) && b(doc) < 90000 && narrow(doc); });
  for (int32_t flags : {0, Query::NEED_SCORES | Query::ALLOW_PRUNING}) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* supplier = query.createWeight(context, flags)->scorerSupplier(pool, reader->segments()[0]);
    Query::ScorerSupplier::BulkScorerContext opaque;
    opaque.filterCost = N / 2;
    EXPECT_EQ(Answer::NO, supplier->planBulk(Use::COUNT_WINDOWS, opaque).available);
    auto* scorer = buildScorerForTests(pool, *supplier, N);
    std::vector<int32_t> docs;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) docs.push_back(doc);
    EXPECT_EQ(expected, docs);
    if (flags != 0) {
      auto plan = supplier->planBulk(Use::SCORED_WINDOWS, {});
      if (plan.available == Answer::YES) {
        auto* bulk = supplier->buildBulk(pool, plan);
        docs.clear();
        for (int32_t cursor = 0; cursor < N;) {
          ScoreWindow out;
          cursor = bulk->scoreNextWindow(out, nullptr, cursor, N, 0);
          docs.insert(docs.end(), out.docs.begin(), out.docs.begin() + out.size);
        }
        EXPECT_EQ(expected, docs);
      }
    }
  }
}


TEST_F(ConjunctionMembershipPlanningTest, VerifyingRangeLeadCostIsItsApproximation) {
  // A verifying column range estimates its matches but iterates every
  // document with a value; its plan costs the latter, which conjunctions
  // order their clauses by.
  NumericPredicateQuery range("a_i", 0, 9999);
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* supplier = range.createWeight(context, 0)->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, supplier);
  auto* plan = resolveScorerPlanForTests(pool, *supplier, 1);
  ASSERT_EQ(Query::ReportedTwoPhase::YES, plan->shape().reportedTwoPhase);
  EXPECT_LT(supplier->cost(), N);
  EXPECT_EQ(N, plan->cost());

  // A Boolean costs what it iterates: its term lead, ahead of the range.
  TermQuery t10("body_w", "t10");
  std::array<Query*, 1> terms{&t10};
  std::array<Query*, 1> filters{&range};
  BooleanQuery query(terms, {}, {}, filters);
  auto* boolean = query.createWeight(context, 0)->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, boolean);
  auto* booleanPlan = resolveScorerPlanForTests(pool, *boolean, N);
  EXPECT_EQ((int64_t)ids([](int32_t doc) { return term(doc, 10); }).size(), booleanPlan->cost());
  auto* scorer = booleanPlan->build(pool);
  std::vector<int32_t> docs;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) docs.push_back(doc);
  EXPECT_EQ(ids([](int32_t doc) { return term(doc, 10) && a(doc) < 10000; }), docs);
}

} // namespace
