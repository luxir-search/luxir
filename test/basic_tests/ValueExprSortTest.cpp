// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"

using namespace luxir;
using namespace luxir::test;

namespace {

std::vector<std::string> ids(const LocalReq& req) {
  const auto* docs = req.docList("q");
  if (docs == nullptr) return {};
  const auto& values = std::get<api::ColStr>(docs->columns.at("id_s").kind).v;
  return {values.begin(), values.end()};
}

} // namespace

class ValueExprSortTest : public LuxirTest {};

TEST_F(ValueExprSortTest, directionsMissingLastDefAndVariable) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "ten", "price_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "five", "price_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "missing"), UpdateMessage::COMMIT);

  auto run = [&](std::string_view expression, qb::SortDir direction,
                 std::optional<int64_t> fallback = std::nullopt) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
    if (fallback) {
      qb::sortVar(top, expression, "fallback", *fallback, direction);
    } else {
      qb::sort(top, expression, direction);
    }
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return ids(*req);
  };

  EXPECT_EQ((std::vector<std::string>{"five", "ten", "missing"}),
            run("add(price_i,2)", qb::ASC));
  EXPECT_EQ((std::vector<std::string>{"ten", "five", "missing"}),
            run("add(price_i,2)", qb::DESC));
  EXPECT_EQ((std::vector<std::string>{"missing", "five", "ten"}),
            run("add(def(price_i,$fallback),1)", qb::ASC, 0));
}

TEST_F(ValueExprSortTest, divisionIsDoubleAndZeroDenominatorSortsMissingLast) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "half", "den_i", 2),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "zero", "den_i", 0),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "quarter", "den_i", 4),
               UpdateMessage::COMMIT);

  auto run = [&](std::string_view expression) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
    qb::sort(top, expression, qb::ASC);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return ids(*req);
  };

  EXPECT_EQ((std::vector<std::string>{"quarter", "half", "zero"}),
            run("1 / den_i"));
  EXPECT_EQ((std::vector<std::string>{"zero", "quarter", "half"}),
            run("def(div(1,den_i),-1)"));
}

TEST_F(ValueExprSortTest, dateScalingDemotesAndFloorSortsEpochDays) {
  constexpr int64_t DAY = 86400000;
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "day2", "when_dt", 2 * DAY + 1000),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "day1", "when_dt", DAY + 2000),
               UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
  qb::sort(top, "floor(when_dt / 86400000)", qb::ASC);
  req->execute(false);

  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"day1", "day2"}), ids(*req));
}

TEST_F(ValueExprSortTest, bareMultiValuedIntSortsByFirstValue) {
  // Multi-valued numeric columns store per-doc values unsorted, so the bare
  // field sort key is the FIRST stored value; min/max are explicit reducers.
  // Data chosen so first-value order, per-doc min order, per-doc max order,
  // and the flat value stream's leading entries all rank the docs differently.
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "values_is", vec_i(9, 4)), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "values_is", vec_i(8, 7)), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "values_is", vec_i(1, 20)), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d"), UpdateMessage::COMMIT);

  auto run = [&](std::string_view expression, qb::SortDir direction) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
    qb::sort(top, expression, direction);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return ids(*req);
  };

  // First values: a=9, b=8, c=1; d missing sorts last both directions.
  EXPECT_EQ((std::vector<std::string>{"c", "b", "a", "d"}), run("values_is", qb::ASC));
  EXPECT_EQ((std::vector<std::string>{"a", "b", "c", "d"}), run("values_is", qb::DESC));
  // Explicit reducers see every value: mins a=4, b=7, c=1; maxes a=9, b=8, c=20.
  EXPECT_EQ((std::vector<std::string>{"c", "a", "b", "d"}), run("min(values_is)", qb::ASC));
  EXPECT_EQ((std::vector<std::string>{"c", "a", "b", "d"}), run("max(values_is)", qb::DESC));
}

TEST_F(ValueExprSortTest, multiSortFallsThroughToSegmentDocOrder) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "first", "x_i", 1, "y_i", 2),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "tie0", "x_i", 1, "y_i", 1),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "tie1", "x_i", 1, "y_i", 1),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "lower", "x_i", 0, "y_i", 9),
               UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
  qb::sort(top, "add(x_i,0)", qb::ASC);
  qb::sort(top, "mul(y_i,1)", qb::DESC);
  req->execute(true);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"lower", "first", "tie0", "tie1"}),
            ids(*req));
}

TEST_F(ValueExprSortTest, nestedDocidUsesReaderGlobalOrder) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c"), UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
  qb::sort(top, "add(_docid_,0)", qb::DESC);
  req->execute(true);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"c", "b", "a"}), ids(*req));
}

TEST_F(ValueExprSortTest, scoreColumnExpressionAndGetScoresOutput) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "popular", "popularity_i", 100,
                       "body_w", "term"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "dense", "popularity_i", 0,
                       "body_w", "term term term term"), UpdateMessage::COMMIT);

  auto run = [&](bool getScores) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").matchQuery("body_w", "term")
        .limit(10).fields({"id_s"}).getScores(getScores);
    qb::sort(top, "add(score,popularity_i)", qb::DESC);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ((std::vector<std::string>{"popular", "dense"}), ids(*req));
    const auto* docs = req->docList("q");
    EXPECT_EQ(getScores, docs->columns.find("_score_") != nullptr);
    return ids(*req);
  };

  EXPECT_EQ(run(false), run(true));
}

TEST_F(ValueExprSortTest, reducersAndRootDiagnostics) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "wide", "values_is", vec_i(2, 8)),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "low", "values_is", vec_i(-3, 10)),
               UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s"});
  qb::sort(top, "min(values_is)", qb::ASC);
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"low", "wide"}), ids(*req));

  auto expectError = [&](std::string_view expression, std::string_view expected) {
    auto bad = localReq(luxirNode->getSearchEngine());
    bad->collection("main");
    auto& badTop = bad->topDocs("q").allQuery().limit(10);
    qb::sort(badTop, expression, qb::ASC);
    bad->execute(false);
    ASSERT_FALSE(bad->ok());
    EXPECT_NE(bad->errorMsg().find(expected), std::string::npos) << bad->errorMsg();
  };
  expectError("neg(values_is)", "explicit reducer");
  expectError("123", "col(\"123\")");
}

TEST_F(ValueExprSortTest, collectorReuseAndPairwiseMergeKeepOnlyValues) {
  CollectionHelper helper;
  for (int64_t value : {40, 10, 30, 20}) {
    helper.index(flatdoc("id_s", std::to_string(value), "key_i", value),
                 UpdateMessage::COMMIT);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena* arena = createArena();
  ValueExprOptions options{schema.get(), {}};
  ValueProgram* program = ValueExprParser(options, *arena).parse("add(key_i,0)");
  std::vector<SortClause> clauses;
  clauses.emplace_back(*program, SortField::ASC);

  auto run = [&](bool reverseMerge) {
    auto left = std::make_unique<FieldSortCollector>(3, clauses, reader.get(), false);
    auto right = std::make_unique<FieldSortCollector>(3, clauses, reader.get(), false);
    auto collectSegment = [&](FieldSortCollector& collector, int32_t segment) {
      auto& readerSegment = reader->segments()[(size_t)segment];
      auto& postings = readerSegment.postingsReader();
      MemPool pool;
      collector.setSegment(segment, &postings);
      {
        FieldSortCollector::ExpressionBindings bindings(
            collector, pool, readerSegment);
        for (int32_t doc = 0; doc < postings.maxDoc(); doc++) {
          collector.collect(segment, doc, 0.0f);
        }
      }
      EXPECT_EQ(nullptr, collector.clauses[0].expr->expression);
    };
    collectSegment(*left, 0);
    collectSegment(*left, 2);
    collectSegment(*right, 1);
    collectSegment(*right, 3);
    auto& destination = reverseMerge ? right : left;
    auto& source = reverseMerge ? left : right;
    destination->merge(*source);
    source.reset();
    std::vector<segdoc> result;
    for (const auto& doc : destination->sort()) result.push_back(doc.doc);
    return result;
  };

  std::vector<segdoc> expected{segdoc(1, 0), segdoc(3, 0), segdoc(2, 0)};
  EXPECT_EQ(expected, run(false));
  EXPECT_EQ(expected, run(true));
  releaseArena(arena);
}

TEST_F(ValueExprSortTest, randomizedDifferentialOracle) {
  struct ModelDoc {
    std::string id;
    segdoc doc;
    std::optional<int64_t> integer;
    std::optional<double> floating;
  };

  CollectionHelper helper;
  std::vector<ModelDoc> model;
  uint64_t state = 0x9e3779b97f4a7c15ULL;
  auto random = [&]() {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dULL;
  };
  constexpr std::array<int64_t, 9> integerKeys{
      -9007199254740995LL, -11, -3, 0, 0, 4, 9, 9007199254740993LL,
      9007199254740995LL};
  constexpr std::array<double, 9> doubleKeys{
      -100.25, -8.0, -1.5, 0.0, 0.0, 2.25, 8.0, 8.0, 99.5};
  int32_t segment = 0;
  int32_t segmentDoc = 0;
  constexpr int32_t DOC_COUNT = 137;
  for (int32_t i = 0; i < DOC_COUNT; i++) {
    uint64_t bits = random();
    ModelDoc entry;
    entry.id = "d" + std::to_string(i);
    entry.doc = segdoc(segment, segmentDoc++);
    if ((bits & 3) != 0) entry.integer = integerKeys[(bits >> 8) % integerKeys.size()];
    if ((bits & 12) != 0) entry.floating = doubleKeys[(bits >> 16) % doubleKeys.size()];
    Doc doc = flatdoc("id_s", entry.id);
    if (entry.integer) doc.push_back({"key_i", *entry.integer});
    if (entry.floating) doc.push_back({"key_d", *entry.floating});
    bool commit = i % 19 == 18 || i + 1 == DOC_COUNT;
    helper.index(doc, commit ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
    model.push_back(std::move(entry));
    if (commit) {
      segment++;
      segmentDoc = 0;
    }
  }

  auto actual = [&](std::string_view expression, qb::SortDir direction) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(-1).batchSize(DOC_COUNT)
        .fields({"id_s"});
    qb::sort(top, expression, direction);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return ids(*req);
  };
  auto expected = [&](auto member, qb::SortDir direction) {
    auto copy = model;
    std::sort(copy.begin(), copy.end(), [&](const ModelDoc& left,
                                            const ModelDoc& right) {
      const auto& a = left.*member;
      const auto& b = right.*member;
      if (a.has_value() != b.has_value()) return a.has_value();
      if (a && *a != *b) {
        return direction == qb::ASC ? *a < *b : *a > *b;
      }
      return left.doc < right.doc;
    });
    std::vector<std::string> result;
    for (const auto& entry : copy) result.push_back(entry.id);
    return result;
  };
  auto expectOrder = [](const std::vector<std::string>& wanted,
                        const std::vector<std::string>& got,
                        std::string_view label) {
    ASSERT_EQ(wanted.size(), got.size()) << label;
    auto mismatch = std::mismatch(wanted.begin(), wanted.end(), got.begin());
    if (mismatch.first != wanted.end()) {
      size_t index = (size_t)(mismatch.first - wanted.begin());
      ADD_FAILURE() << label << " first mismatch at " << index
                    << ": expected " << *mismatch.first
                    << ", got " << *mismatch.second;
    }
  };

  for (qb::SortDir direction : {qb::ASC, qb::DESC}) {
    expectOrder(expected(&ModelDoc::integer, direction),
                actual("add(key_i,0)", direction), "integer");
    expectOrder(expected(&ModelDoc::floating, direction),
                actual("mul(key_d,1.0)", direction), "double");
  }
}
