// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/util/NumericUtils.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using FC = api::FieldDef::FieldClass;
using IM = api::FieldDef::IndexMode;
using MM = api::FieldDef::MultiMode;

std::vector<std::string> queryIds(CollectionHelper& helper, std::string_view field,
                                 int kind, std::string_view lo, std::string_view hi = {}) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q").fields({"id"}).limit(100).countLimit();
  if (kind == 0) top.matchQuery(field, lo);
  else if (kind == 1) {
    auto& any = top.rawQuery().kind.emplace<api::AnyOfQuery>();
    any.field = api::build::arenaStr(top.mr(), field);
    auto* sequence = api::build::allocMessage<api::Val>(top.mr());
    auto& values = sequence->kind.emplace<api::ArrVal>();
    auto* stored = api::build::allocArray(values.v, 2, top.mr());
    stored[0].kind = lo;
    stored[1].kind = hi;
    any.values = sequence;
  } else {
    top.rawQuery() = qb::range(top.mr(), field, qb::valStr(top.mr(), lo), nullptr,
                               qb::valStr(top.mr(), hi), nullptr);
  }
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::vector<std::string> ids;
  for (const auto& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
  EXPECT_EQ((int64_t)ids.size(), req->getMatchCount());
  std::sort(ids.begin(), ids.end());
  return ids;
}
} // namespace

class NumericMatchTest : public LuxirTest {};

TEST_F(NumericMatchTest, termOrderAndRoundTrip) {
  std::string previous;
  for (int64_t value : {INT64_MIN, (int64_t)-100, (int64_t)-1, (int64_t)0,
                        (int64_t)1, (int64_t)100, INT64_MAX}) {
    auto bytes = encodeNumericTerm(value);
    std::string term(bytes.data(), bytes.size());
    EXPECT_EQ(value, decodeNumericTerm(term));
    if (!previous.empty()) { EXPECT_LT(previous, term); }
    previous = term;
  }
}

TEST_F(NumericMatchTest, modesAgreeAcrossFlushDeleteAndMerge) {
  for (auto type : {FC::INT, FC::FLOAT, FC::DOUBLE, FC::DATE}) {
    for (auto multi : {MM::SINGLE, MM::SET, MM::ARRAY}) {
      SCOPED_TRACE((int)type);
      SCOPED_TRACE((int)multi);
      CollectionHelper helper;
      helper.clear();
      SchemaBuilder schema;
      for (auto [name, index] : {std::pair{"n", IM::NONE}, {"r", IM::RANGE}, {"m", IM::MATCH}}) {
        auto& f = schema.field(name);
        f.type = type;
        f.index = index;
        f.multi = multi;
        if (type == FC::DATE && multi == MM::ARRAY) f.unit = api::FieldDef::DateUnit::NS;
      }
      schema.set(helper.collection());
      auto writer = helper.getIndexWriter();
      writer->mergePolicy->setMergeFactor(1000);
      bool integral = type == FC::INT || type == FC::DATE;
      const char* negative = integral ? "-2" : "-2.5";
      const char* positive = integral ? "1" : "1.5";
      const double values[] = {integral ? -2 : -2.5, -0.0, 0.0, integral ? 1 : 1.5, 7};
      for (int i = 0; i < 24; i++) {
        Doc doc = flatdoc("id", std::to_string(i));
        if (i % 7 != 0) for (auto name : {"n", "r", "m"}) {
          double value = values[i % 5];
          if (multi == MM::SINGLE) {
            if (integral) doc.push_back({name, (int64_t)value});
            else doc.push_back({name, value});
          } else {
            double other = values[(i + 1) % 5];
            if (integral) doc.push_back({name, vec_i((int64_t)value, (int64_t)other, (int64_t)value)});
            else doc.push_back({name, vec(value, other, value)});
          }
        }
        ASSERT_TRUE(helper.index(doc, i == 11 || i == 23 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT).success);
      }
      for (int stage = 0; stage < 3; stage++) {
        SCOPED_TRACE(stage);
        for (auto value : {"-9", negative, "-0", "0", positive, "7", "99"}) {
          auto expected = queryIds(helper, "n", 0, value);
          EXPECT_EQ(expected, queryIds(helper, "r", 0, value));
          EXPECT_EQ(expected, queryIds(helper, "m", 0, value));
        }
        for (auto [lo, hi] : {std::pair{"-9", "99"}, {"-2", "1"}, {"-0", "0"}, {"2", "6"}}) {
          for (int kind : {1, 2}) {
            auto expected = queryIds(helper, "n", kind, lo, hi);
            EXPECT_EQ(expected, queryIds(helper, "r", kind, lo, hi));
            EXPECT_EQ(expected, queryIds(helper, "m", kind, lo, hi));
          }
        }
        if (stage == 0) { ASSERT_TRUE(helper.deleteById("3", UpdateMessage::COMMIT).success); }
        if (stage == 1) {
          CollectionHelper::UpdateBuilder merge;
          merge.commit(true, 1);
          ASSERT_TRUE(helper.submit(merge).success);
          ASSERT_EQ(1u, helper.durableSegmentCount());
        }
      }
    }
  }
}

TEST_F(NumericMatchTest, rareLeadUsesPostingsAndCandidateFilterUsesColumn) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& f = schema.field("m");
  f.type = FC::INT;
  f.index = IM::MATCH;
  f.multi = MM::SINGLE;
  schema.set(helper.collection());
  std::vector<Doc> docs;
  for (int i = 0; i < 1024; i++) docs.push_back(flatdoc("m", i % 400 == 0 ? 9 : i % 400 == 1 ? 8 : 0));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  MemPool pool;
  Query::Context context(pool, *reader);
  NumericPredicateQuery query("m", 9, 9);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);
  EXPECT_EQ(3, supplier->cost());
  EXPECT_EQ(3, QueryPrep::supplierCount(supplier));
  EXPECT_TRUE(supplier->membershipWork(segment.maxDoc()).knownMatches);
  for (bool lead : {true, false}) {
    SkipStatsScope stats;
    auto planContext = supplier->makePlanContext(Query::Demand::fromLeadCost(lead ? INT64_MAX : 1));
    if (!lead) planContext.membershipConsumption = Query::MembershipConsumption::CANDIDATES;
    auto* plan = supplier->resolve(pool, planContext);
    EXPECT_EQ(Query::ClauseShape::DIRECT, plan->shape().windowFillClause);
    EXPECT_EQ(lead ? Query::ReportedTwoPhase::NO : Query::ReportedTwoPhase::YES,
              plan->shape().reportedTwoPhase);
    auto* scorer = plan->build(pool);
    EXPECT_EQ(lead ? 1 : 0, SkipStats::numericPredicatePostingsArms);
    EXPECT_EQ(lead ? 0 : 1, SkipStats::numericPredicateSparseVerifyArms);
    if (lead) { EXPECT_NE(nullptr, dynamic_cast<NumericPredicateQuery::PostingsScorer*>(scorer)); }
    EXPECT_EQ(0, scorer->next());
    EXPECT_FLOAT_EQ(1, scorer->score());
    EXPECT_EQ(400, scorer->advance(1));
    if (lead) {
      scorer->reset();
      std::array<uint64_t, 8> words{};
      scorer->fillScoredWindowBits(words, 0, 512);
      EXPECT_EQ(1ULL, words[0]);
      EXPECT_EQ(1ULL << 16, words[6]);
      words.fill(0);
      scorer->fillWindowBits(words, 512, 1024);
      EXPECT_EQ(1ULL << 32, words[4]);
    }
  }
  for (auto [lo, hi] : {std::pair<int64_t, int64_t>{8, 9}, {0, 8}}) {
    NumericPredicateQuery range("m", lo, hi);
    auto* rangeWeight = range.createWeight(context, Query::NEED_SCORES);
    auto* rangeSupplier = rangeWeight->scorerSupplier(pool, segment);
    ASSERT_NE(nullptr, rangeSupplier);
    SkipStatsScope stats;
    auto* scorer = buildScorerForTests(pool, *rangeSupplier, INT64_MAX);
    EXPECT_EQ(1, SkipStats::numericPredicatePostingsArms);
    EXPECT_EQ(lo == 8, dynamic_cast<NumericPredicateQuery::PointsArrayScorer*>(scorer) != nullptr);
    EXPECT_EQ(lo == 0, dynamic_cast<NumericPredicateQuery::PointsBitScorer*>(scorer) != nullptr);
    int count = 0;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      EXPECT_FLOAT_EQ(1, scorer->score());
      count++;
    }
    EXPECT_EQ(lo == 8 ? 6 : 1021, count);
  }

}

TEST_F(NumericMatchTest, mixedIndexModesKeepCompleteResultsAfterMerge) {
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(1000);
  int value = 0;
  for (auto mode : {IM::NONE, IM::MATCH, IM::MATCH}) {
    SchemaBuilder schema;
    auto& f = schema.field("m");
    f.type = FC::INT;
    f.index = mode;
    schema.set(helper.collection());
    value++;
    ASSERT_TRUE(helper.index(flatdoc("id", std::to_string(value), "m", value), UpdateMessage::COMMIT).success);
  }
  auto before = queryIds(helper, "m", 2, "1", "2");
  ASSERT_EQ(2u, before.size());
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  EXPECT_EQ(before, queryIds(helper, "m", 2, "1", "2"));
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(0, reader->field("m").at(0)->flags & FieldType::INDEX_DOCS);
}

TEST_F(NumericMatchTest, dictionaryResolutionIsBoundedByColumnScan) {
  constexpr int N = 4096;
  CollectionHelper helper;
  SchemaBuilder schema;
  for (auto [name, mode] : {std::pair{"n", IM::NONE}, {"m", IM::MATCH}}) {
    auto& f = schema.field(name);
    f.type = FC::INT;
    f.index = mode;
    f.multi = MM::SINGLE;
  }
  schema.set(helper.collection());
  std::vector<Doc> docs;
  for (int i = 0; i < N; i++) docs.push_back(flatdoc("n", i, "m", i));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  auto collect = [](Query::Scorer* scorer) {
    std::vector<int32_t> result;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      result.push_back(doc);
    }
    return result;
  };
  for (auto [lo, hi] : {std::pair<int64_t, int64_t>{0, N - 2}, {-1, N}, {100, 100}}) {
    SCOPED_TRACE(lo);
    SCOPED_TRACE(hi);
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();
    Query::Context context(pool, *reader);
    NumericPredicateQuery match("m", lo, hi), none("n", lo, hi);
    SkipStatsScope stats;
    auto* supplier = match.createWeight(context, 0)->scorerSupplier(pool, segment);
    auto* twin = none.createWeight(context, 0)->scorerSupplier(pool, segment);
    ASSERT_NE(nullptr, supplier);
    ASSERT_NE(nullptr, twin);
    bool narrow = lo == hi;
    if (lo < 0) {
      EXPECT_EQ(0, SkipStats::numericPredicatePostingsTerms);
    } else if (narrow) {
      EXPECT_EQ(1, SkipStats::numericPredicatePostingsTerms);
    } else {
      // The metadata term bound rejects this range before opening terms.
      EXPECT_EQ(0, SkipStats::numericPredicatePostingsTerms);
    }
    auto actual = collect(buildScorerForTests(pool, *supplier, INT64_MAX));
    EXPECT_EQ(narrow ? 1 : 0, SkipStats::numericPredicatePostingsArms);
    EXPECT_EQ(collect(buildScorerForTests(pool, *twin, INT64_MAX)), actual);
    EXPECT_EQ((int64_t)actual.size(), QueryPrep::supplierCount(supplier));
    if (!narrow) { EXPECT_EQ(twin->cost(), supplier->cost()); }
  }
}
