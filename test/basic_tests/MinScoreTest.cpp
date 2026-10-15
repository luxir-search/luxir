// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;


// Minimum-score cut-offs: TopDocs.min_score, RescoreQuery.min_score, and
// KnnQuery.similarity. A doc below the threshold is not a match: it leaves
// the ranked docs, the found count, and the domain of every sub-op.
class MinScoreTest : public LuxirTest {
protected:
  // Docs 1..6 score their popularity under the rescore below, split over two
  // segments so serial and parallel execution differ in scheduling.
  static void indexScored(CollectionHelper& h) {
    for (int i = 1; i <= 6; i++) {
      h.index(flatdoc("id", "d" + std::to_string(i), "body_w", "alpha",
                      "cat_s", i % 2 == 0 ? "even" : "odd",
                      "pop_i", (int64_t)i),
              i == 3 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
    }
    h.index(flatdoc("id", "other", "body_w", "beta", "cat_s", "odd",
                    "pop_i", (int64_t)100),
            UpdateMessage::COMMIT);
  }

  static std::vector<std::string> ids(const LocalReq& req,
                                      std::string_view op = "q") {
    std::vector<std::string> out;
    const auto* docs = req.docList(op);
    if (docs == nullptr) return out;
    const auto* col = docs->columns.find("id");
    if (col == nullptr) return out;
    for (auto id : std::get<api::ColStr>(col->kind).v) out.emplace_back(id);
    return out;
  }

  static std::vector<int64_t> facetCounts(const LocalReq& req) {
    const auto* result = req.docList("q")->ops.at("f")->facetResult();
    std::vector<int64_t> out;
    auto names = std::get<api::ColStr>(result->bucket_ids->kind).v;
    for (size_t i = 0; i < names.size(); i++) {
      out.push_back(result->counts[i]);
    }
    return out;
  }

  static double sumPop(const LocalReq& req) {
    const auto& kind = req.docList("q")->ops.at("s")->kind;
    if (const auto* i = std::get_if<int64_t>(&kind)) return (double)*i;
    return std::get<double>(kind);
  }

  static api::Query exprQuery(std::pmr::memory_resource& mr,
                              std::string_view text) {
    api::Query q;
    q.kind.emplace<api::ExprQuery>().q = api::build::arenaStr(mr, text);
    return q;
  }

  static void installVec(Collection& col, api::VectorMetric metric) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.column = true;
    f.metric = metric;
    b.set(col);
  }
};

TEST_F(MinScoreTest, topDocsExcludesHitsCountsAndFacets) {
  CollectionHelper h;
  indexScored(h);
  for (int32_t maxParallel : {1, -1}) {
    auto req = localReq(h.getSearchEngine());
    auto& top = req->collection("main").topDocs("q")
        .exprQuery("rescore(body_w:alpha, pop_i)")
        .minScore(3.5f).fields({"id"}).getNumber().limit(-1);
    top.facet("f", "cat_s").limit(-1);
    top.sum("s", "pop_i");
    req->execute(maxParallel);
    ASSERT_OK(req);
    EXPECT_EQ((std::vector<std::string>{"d6", "d5", "d4"}), ids(*req));
    EXPECT_EQ(3, req->getMatchCount());
    // even: d4, d6; odd: d5 ("other" fails the query).
    EXPECT_EQ((std::vector<int64_t>{2, 1}), facetCounts(*req));
    EXPECT_DOUBLE_EQ(15.0, sumPop(*req));
  }
}

TEST_F(MinScoreTest, appliesWithoutScoredRanking) {
  CollectionHelper h;
  indexScored(h);

  // Count-only: no ranking pass reads scores.
  auto count = localReq(h.getSearchEngine());
  count->collection("main").topDocs("q")
      .exprQuery("rescore(body_w:alpha, pop_i)")
      .minScore(5.0f).getNumber().limit(0);
  count->execute();
  ASSERT_OK(count);
  EXPECT_EQ(2, count->getMatchCount());

  // Field sort with a folded filter: scores are computed for the cut-off only.
  auto sorted = localReq(h.getSearchEngine());
  auto& cur = sorted->collection("main").topDocs("q")
      .exprQuery("rescore(body_w:alpha, pop_i)")
      .minScore(2.0f).matchFilter("cat_s", "even")
      .fields({"id"}).getNumber().limit(-1);
  auto& td = std::get<api::TopDocs>(cur.rawOp().kind);
  auto* sorts = api::build::allocArray(td.sort, 1, cur.mr());
  sorts[0].expr = api::build::arenaStr(cur.mr(), "pop_i");
  sorts[0].dir = api::SortSpec::SortDir::ASC;
  sorted->execute();
  ASSERT_OK(sorted);
  EXPECT_EQ((std::vector<std::string>{"d2", "d4", "d6"}), ids(*sorted));
  EXPECT_EQ(3, sorted->getMatchCount());

  // Match-all scores 1, so a cut-off above it matches nothing.
  auto none = localReq(h.getSearchEngine());
  none->collection("main").topDocs("q").allQuery().minScore(1.5f)
      .fields({"id"}).getNumber().limit(-1);
  none->execute();
  ASSERT_OK(none);
  EXPECT_EQ(0, none->getMatchCount());
  EXPECT_TRUE(ids(*none).empty());
}

TEST_F(MinScoreTest, rescoreMinScoreFiltersInScoredAndFilterPositions) {
  CollectionHelper h;
  indexScored(h);

  auto scored = localReq(h.getSearchEngine());
  scored->collection("main").topDocs("q")
      .exprQuery("boost(rescore(body_w:alpha, pop_i, min_score=4), boost=2)")
      .fields({"id"}).getScores().getNumber().limit(-1);
  scored->execute();
  ASSERT_OK(scored);
  EXPECT_EQ((std::vector<std::string>{"d6", "d5", "d4"}), ids(*scored));
  EXPECT_EQ(3, scored->getMatchCount());

  // As a filter the rescore is unscored, but its cut-off still applies.
  auto filtered = localReq(h.getSearchEngine());
  auto& cur = filtered->collection("main").topDocs("q").allQuery()
      .fields({"id"}).getNumber().limit(-1);
  cur.filter(exprQuery(cur.mr(), "rescore(body_w:alpha, pop_i, min_score=5)"));
  filtered->execute();
  ASSERT_OK(filtered);
  EXPECT_EQ(2, filtered->getMatchCount());
}

TEST_F(MinScoreTest, rejectsNonFiniteMinScore) {
  CollectionHelper h;
  indexScored(h);
  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .minScore(std::numeric_limits<float>::infinity()).limit(-1);
  req->execute();
  EXPECT_FALSE(req->ok());
}

// L2 similarity is a maximum (non-squared) distance; the threshold trims the
// k nearest and the facet domain alike, on every execution path.
TEST_F(MinScoreTest, knnSimilarityL2IsMaxDistance) {
  CollectionHelper h("main");
  installVec(h.collection(), api::VectorMetric::L2);
  for (int i = 0; i < 4; i++) {
    h.index(flatdoc("id", "v" + std::to_string(i), "cat_s", "x",
                    "embedding_v", std::vector<float>{(float)i, 0}),
            i == 1 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  h.commit({"*"});

  for (bool exact : {false, true}) {
    for (int32_t maxParallel : {1, -1}) {
      auto req = localReq(h.getSearchEngine());
      auto& cur = req->collection("main").topDocs("q")
          .fields({"id"}).getNumber().limit(-1);
      cur.rawQuery() = qb::knn(cur.mr(), "embedding_v", {0, 0}, 4, 0, exact,
                               0, 0, 2.0f);
      cur.facet("f", "cat_s").limit(-1);
      req->execute(maxParallel);
      ASSERT_OK(req);
      // Distances 0, 1, 2 are within 2.0 (the boundary is inclusive); 3 is not.
      EXPECT_EQ((std::vector<std::string>{"v0", "v1", "v2"}), ids(*req));
      EXPECT_EQ(3, req->getMatchCount());
      EXPECT_EQ((std::vector<int64_t>{3}), facetCounts(*req));
    }
  }
}

// IP/cosine similarity is the raw similarity, compared before boost; a
// thresholded knn is also a cacheable filter.
TEST_F(MinScoreTest, knnSimilarityCosineBeforeBoostAndAsFilter) {
  CollectionHelper h("main");
  installVec(h.collection(), api::VectorMetric::COSINE);
  h.index(flatdoc("id", "same", "embedding_v", std::vector<float>{1, 0}));
  h.index(flatdoc("id", "near", "embedding_v", std::vector<float>{1, 1}));
  h.index(flatdoc("id", "ortho", "embedding_v", std::vector<float>{0, 1}));
  h.index(flatdoc("id", "away", "embedding_v", std::vector<float>{-1, 0}));
  h.commit({"*"});

  auto boosted = localReq(h.getSearchEngine());
  auto& cur = boosted->collection("main").topDocs("q")
      .fields({"id"}).getNumber().limit(-1);
  auto knn = qb::knn(cur.mr(), "embedding_v", {1, 0}, 4, 0, false, 0, 0, 0.5f);
  cur.rawQuery() = qb::boost(cur.mr(), knn, 0.1f);
  boosted->execute();
  ASSERT_OK(boosted);
  // cos = 1, 0.707, 0, -1: the boosted scores are all below 0.5, but the
  // threshold is on the cosine itself.
  EXPECT_EQ((std::vector<std::string>{"same", "near"}), ids(*boosted));

  for (int run = 0; run < 2; run++) {
    auto filtered = localReq(h.getSearchEngine());
    auto& fcur = filtered->collection("main").topDocs("q").allQuery()
        .getNumber().limit(0);
    fcur.filter(qb::knn(fcur.mr(), "embedding_v", {1, 0}, 4, 0, false, 0, 0,
                        -0.5f));
    filtered->execute();
    ASSERT_OK(filtered);
    EXPECT_EQ(3, filtered->getMatchCount()) << "run " << run;
  }
}

TEST_F(MinScoreTest, knnRejectsNegativeL2Similarity) {
  CollectionHelper h("main");
  installVec(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", "a", "embedding_v", std::vector<float>{0, 0}));
  h.commit();
  auto req = localReq(h.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q").limit(-1);
  cur.rawQuery() = qb::knn(cur.mr(), "embedding_v", {0, 0}, 1, 0, false, 0, 0,
                           -1.0f);
  req->execute();
  EXPECT_FALSE(req->ok());
}

// The JSON request shorthand accepts min_score, and a fusion source applies
// its own cut-off before fusion.
TEST_F(MinScoreTest, jsonShorthandAndFusionSource) {
  CollectionHelper h;
  indexScored(h);

  auto shorthand = localReq(h.getSearchEngine());
  parseQueryRequest(R"json({"query":"rescore(body_w:alpha, pop_i)",
    "min_score":4,"get_number":true,"fields":["id"],"limit":-1})json",
      shorthand->rawRequest(), shorthand->mr);
  shorthand->collection("main");
  shorthand->execute();
  ASSERT_OK(shorthand);
  EXPECT_EQ((std::vector<std::string>{"d6", "d5", "d4"}), ids(*shorthand));
  EXPECT_EQ(3, shorthand->getMatchCount());

  auto fused = localReq(h.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"q":{"fusion":{
    "sources":{"a":{"query":"rescore(body_w:alpha, pop_i)","min_score":5,"limit":10}},
    "rrf":{},"limit":10,"get_number":true,"fields":["id"]}}}})json",
      fused->rawRequest(), fused->mr);
  fused->collection("main");
  fused->execute();
  ASSERT_OK(fused);
  EXPECT_EQ((std::vector<std::string>{"d6", "d5"}), ids(*fused));
  EXPECT_EQ(2, fused->getMatchCount());
}
