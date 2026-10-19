// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <bit>
#include <format>
#include <limits>
#include <type_traits>
#include <variant>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/Collector.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/value/ValueExprParser.h"
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

  static void checkJsonAndWire(SearchEngine& engine, const char* json,
                               const std::vector<std::string>& expected) {
    for (bool wire : {false, true}) {
      auto req = localReq(engine);
      parseQueryRequest(json, req->rawRequest(), req->mr);
      req->collection("main");
      auto run = localReq(engine);
      std::vector<std::byte> data;  // decoded views point into it
      if (wire) {
        ASSERT_TRUE(api::encode(req->rawRequest(), data));
        size_t size = data.size(); data.resize(size + 16);
        ASSERT_TRUE(api::decode(run->rawRequest(), std::span(data).first(size), run->mr));
      }
      auto& target = wire ? *run : *req;
      target.execute();
      ASSERT_OK((&target));
      EXPECT_EQ(expected, ids(target)) << json << " wire=" << wire;
    }
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

// Elasticsearch-style shorthand bodies as a gateway sends them: a match query
// with a top-level min_score, as a float or an integer, beside ops.
TEST_F(MinScoreTest, jsonShorthandMatchQueryWithOps) {
  CollectionHelper h;
  for (int i = 1; i <= 4; i++) {
    std::string body = "gamma";
    for (int j = 1; j < i; j++) body += " gamma";
    for (int j = i; j < 4; j++) body += " pad";
    h.index(flatdoc("id", "m" + std::to_string(i), "body_w", body,
                    "cat_s", i % 2 == 0 ? "even" : "odd", "pop_i", (int64_t)i));
  }
  h.commit();

  auto run = [&](const std::string& minScore) {
    auto req = localReq(h.getSearchEngine());
    parseQueryRequest(R"json({"query":{"match":{"body_w":"gamma"}},)json" + minScore
        + R"json("get_number":true,"get_scores":true,"fields":["id"],"limit":-1,
        "ops":{"f":{"field_facet":{"field":"cat_s","limit":-1}},"s":"sum(pop_i)"}})json",
        req->rawRequest(), req->mr);
    req->collection("main");
    req->execute();
    EXPECT_TRUE(req->ok()) << minScore << ": " << req->errorMsg();
    return req;
  };

  // Unthresholded: higher term frequency at equal length scores higher.
  auto all = run("");
  ASSERT_EQ((std::vector<std::string>{"m4", "m3", "m2", "m1"}), ids(*all));
  std::vector<double> scores;
  std::visit([&](const auto& c) {
    if constexpr (requires { c.v; }) {
      for (auto v : c.v) {
        if constexpr (std::is_arithmetic_v<std::decay_t<decltype(v)>>) scores.push_back((double)v);
      }
    }
  }, all->docList("q")->columns.find("_score_")->kind);
  ASSERT_EQ(4u, scores.size());

  // A float cut-off between the second and third scores.
  auto cut = run(std::format(R"("min_score":{},)", (scores[1] + scores[2]) / 2));
  EXPECT_EQ((std::vector<std::string>{"m4", "m3"}), ids(*cut));
  EXPECT_EQ(2, cut->getMatchCount());
  EXPECT_EQ((std::vector<int64_t>{1, 1}), facetCounts(*cut));
  EXPECT_DOUBLE_EQ(7.0, sumPop(*cut));

  // Integer spellings: 0 keeps every match, a large one keeps none.
  EXPECT_EQ(4, run(R"("min_score":0,)")->getMatchCount());
  auto none = run(R"("min_score":1000,)");
  EXPECT_EQ(0, none->getMatchCount());
  EXPECT_TRUE(ids(*none).empty());
}


// rescore.min_score and knn.similarity through the JSON dialect, executed
// from JSON and again after a protobuf wire round trip (the gRPC path):
// the cut-offs keep their own tags beside collapse and knn.unit.
TEST_F(MinScoreTest, jsonAndWireRescore) {
  CollectionHelper h;
  indexScored(h);
  checkJsonAndWire(h.getSearchEngine(), R"json({"query":{"rescore":{"query":{"match":{"body_w":"alpha"}},
    "expr":"pop_i","min_score":4.5}},"fields":["id"],"limit":-1})json", {"d6", "d5"});
  checkJsonAndWire(h.getSearchEngine(), R"json({"query":"rescore(body_w:alpha, pop_i)",
    "min_score":3.5,"fields":["id"],"limit":-1})json", {"d6", "d5", "d4"});
}

TEST_F(MinScoreTest, jsonAndWireKnnSimilarity) {
  CollectionHelper v("main");
  installVec(v.collection(), api::VectorMetric::L2);
  for (int i = 0; i < 4; i++) {
    v.index(flatdoc("id", "v" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0}));
  }
  v.commit();
  checkJsonAndWire(v.getSearchEngine(), R"json({"query":{"knn":{"field":"embedding_v","query":[0,0],
    "k":4,"unit":"docs","similarity":1.5}},"fields":["id"],"limit":-1})json", {"v0", "v1"});
}

TEST_F(MinScoreTest, bulkTopKCountAndDomain) {
  CollectionHelper h;
  constexpr int32_t nDocs = 8192;
  const char* bodies[] = {"alpha pad pad pad", "beta beta pad pad",
                         "alpha beta pad pad", "alpha alpha alpha beta"};
  for (int32_t i = 0; i < nDocs; i++) {
    h.index(flatdoc("id", std::to_string(i), "body_w", bodies[i % 4],
                    "pop_i", (int64_t)i, "tag_s", i % 3 == 0 ? "x" : "y"));
  }
  h.commit();

  auto run = [&](std::optional<float> floor, int32_t limit, bool exact,
                  bool filtered, bool domain = false) {
    auto req = localReq(h.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q")
        .fields({"id"}).getScores().getNumber(exact)
        .limit(limit < 0 ? nDocs : limit).batchSize(nDocs);
    cur.rawQuery() = qb::boolean(cur.mr(), {}, {
        qb::match(cur.mr(), "body_w", "alpha"),
        qb::match(cur.mr(), "body_w", "beta")});
    if (floor) cur.minScore(*floor);
    if (filtered) cur.matchFilter("tag_s", "x");
    if (domain) cur.sum("s", "pop_i");
    req->execute(1);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  auto results = [](const LocalReq& req) {
    std::pair<std::vector<std::string>, std::vector<float>> out;
    // Emission can split even an explicitly large batch into responses.
    for (auto* response : req.responses) {
      const auto* docs = response->proto.ops.at("q")->docList();
      const auto* ids = docs->columns.find("id");
      if (ids == nullptr) continue;
      for (auto id : std::get<api::ColStr>(ids->kind).v) out.first.emplace_back(id);
      const auto& scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
      out.second.insert(out.second.end(), scores.begin(), scores.end());
    }
    return out;
  };

  SkipStatsScope stats;
  for (bool filtered : {false, true}) {
    SCOPED_TRACE(filtered);
    SkipStats::reset();
    auto all = run(std::nullopt, -1, true, filtered);
    ASSERT_OK(all);
    EXPECT_GT(SkipStats::maxScoreInnerWindows, 0);
    auto [allIds, allScores] = results(*all);
    ASSERT_EQ(filtered ? (nDocs + 2) / 3 : nDocs, allScores.size());
    // Include the entire tied score group at the boundary, then exclude it.
    for (float floor : {allScores[allScores.size() / 2], std::nextafter(
             allScores[allScores.size() / 2], std::numeric_limits<float>::infinity())}) {
      std::vector<std::string> expectedIds;
      std::vector<float> expectedScores;
      int64_t expectedSum = 0;
      for (size_t i = 0; i < allScores.size(); i++) {
        if (allScores[i] < floor) continue;
        expectedIds.push_back(allIds[i]);
        expectedScores.push_back(allScores[i]);
        expectedSum += std::stoll(allIds[i]);
      }
      ASSERT_FALSE(expectedIds.empty());
      ASSERT_LT(expectedIds.size(), allIds.size());
      for (auto [limit, exact] : {std::pair{-1, true}, {17, false},
                                  {17, true}, {0, true}}) {
        SCOPED_TRACE(std::format("floor={} limit={} exact={}", floor, limit, exact));
        SkipStats::reset();
        auto cut = run(floor, limit, exact, filtered, limit == 0);
        ASSERT_OK(cut);
        // Filtered count-only uses the root's exhaustive conjunction.
        if (!filtered || limit != 0) {
          EXPECT_GT(SkipStats::minScoreBulkWindows, 0);
        }
        if (exact) {
          EXPECT_EQ(expectedIds.size(), cut->getMatchCount());
        }
        size_t size = limit < 0 ? expectedIds.size()
            : std::min((size_t)limit, expectedIds.size());
        auto [actualIds, actualScores] = results(*cut);
        EXPECT_EQ(std::vector<std::string>(expectedIds.begin(), expectedIds.begin() + size),
                  actualIds);
        if (limit != 0) {
          ASSERT_EQ(size, actualScores.size());
          for (size_t i = 0; i < size; i++) {
            EXPECT_EQ(std::bit_cast<uint32_t>(expectedScores[i]),
                      std::bit_cast<uint32_t>(actualScores[i])) << i;
          }
        } else {
          EXPECT_EQ((double)expectedSum, sumPop(*cut));
          if (!filtered) {
            EXPECT_GT(SkipStats::bulkDomainWindowsFed, 0);
          }
        }
      }
    }
  }
}

TEST_F(MinScoreTest, selectiveConjunctionAndProhibitionBoundVerification) {
  CollectionHelper h;
  constexpr int32_t nDocs = 4096;
  int32_t candidates = 0;
  std::vector<std::string> accepted;
  std::vector<std::string> rejected;
  for (int32_t i = 0; i < nDocs; i++) {
    auto doc = flatdoc("id", std::to_string(i), "body_w", "alpha");
    if (i % 257 == 0) {
      candidates++;
      doc.push_back({"tag_s", "rare"});
      doc.push_back({"pop_i", (int64_t)(i % 4)});
      (i % 4 >= 2 ? accepted : rejected).push_back(std::to_string(i));
    }
    // Scoring any non-candidate throws: the expression is missing there.
    h.index(doc);
  }
  h.commit();

  for (bool prohibited : {false, true}) {
    SkipStatsScope stats;
    auto req = localReq(h.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q")
        .fields({"id"}).getNumber().limit(-1);
    auto cutoff = exprQuery(cur.mr(),
        "rescore(body_w:alpha, pop_i, min_score=2)");
    auto rare = qb::match(cur.mr(), "tag_s", "rare");
    cur.rawQuery() = prohibited
        ? qb::boolean(cur.mr(), {rare}, {}, {cutoff})
        : qb::boolean(cur.mr(), {rare, cutoff});
    req->execute(1);
    ASSERT_OK(req);
    auto actual = ids(*req);
    auto expected = prohibited ? rejected : accepted;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(expected.size(), req->getMatchCount());
    EXPECT_GT(SkipStats::minScoreChecks, 0);
    EXPECT_LE(SkipStats::minScoreChecks, 2 * candidates);
  }
}

TEST_F(MinScoreTest, boundedScorerProtocolsKeepChildScores) {
  CollectionHelper h;
  for (int32_t i = 0; i < 6; i++) {
    auto doc = flatdoc("body_w", "alpha beta");
    if (i % 2 == 0) doc.push_back({"pop_i", (int64_t)i});
    h.index(doc);
  }
  h.commit();
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  auto schema = h.collection().getSchema();
  google::protobuf::Arena arena;
  auto* program = ValueExprParser({schema.get(), {}}, arena).parse("def(pop_i, 0)");
  std::string_view terms[] = {"alpha", "beta"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  RescoreQuery rescore(&phrase, program);
  MinScoreQuery query(&rescore, 2.0f);
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* weight = query.createWeight(context, Query::NEED_SCORES, 0.5f);
  auto fresh = [&] { return weight->createScorer(pool, reader->segments()[0]); };
  {
    auto* scorer = fresh();
    SkipStatsScope stats;
    auto enums = scorer->approximationEnums();
    ASSERT_FALSE(enums.empty());
    for (int32_t doc : {0, 2, 4}) {
      for (auto* e : enums) EXPECT_EQ(doc, e->advance(doc));
      EXPECT_EQ(doc >= 2, scorer->matchesAt(doc));
      if (doc >= 2) {
        EXPECT_EQ(doc, scorer->docId());
        EXPECT_EQ((float)doc * 0.5f, scorer->score());
      }
    }
    EXPECT_EQ(3, SkipStats::minScoreChecks);
  }
  for (bool required : {false, true}) {
    auto* scorer = fresh();
    SkipStatsScope stats;
    scorer->prepareMembership();
    int32_t docs[] = {0, 2, 4};
    float scores[] = {7, 7, 7};
    EXPECT_EQ(required ? 2 : 3,
              scorer->applyToCandidates(docs, scores, 3, required));
    if (required) {
      EXPECT_EQ(2, docs[0]); EXPECT_EQ(4, docs[1]);
      EXPECT_EQ(8.0f, scores[0]); EXPECT_EQ(9.0f, scores[1]);
    } else {
      EXPECT_EQ(0, docs[0]); EXPECT_EQ(2, docs[1]); EXPECT_EQ(4, docs[2]);
      EXPECT_EQ(7.0f, scores[0]); EXPECT_EQ(8.0f, scores[1]); EXPECT_EQ(9.0f, scores[2]);
    }
    EXPECT_EQ(3, SkipStats::minScoreChecks);
  }
  {
    auto* scorer = fresh();
    SkipStatsScope stats;
    scorer->prepareMembership();
    EXPECT_EQ(0, scorer->advanceApproximationWindow(0, 1));
    EXPECT_EQ(0, SkipStats::minScoreChecks);
    EXPECT_GE(scorer->advanceWindow(0, 1), 1);
    EXPECT_EQ(1, SkipStats::minScoreChecks);
    uint64_t words[] = {0};
    scorer->fillWindowBits(words, 2, 3);
    EXPECT_EQ(1u, words[0]);
    EXPECT_EQ(2, SkipStats::minScoreChecks);
    EXPECT_GE(scorer->advanceWindow(3, 4), 4);
    EXPECT_EQ(3, SkipStats::minScoreChecks);
  }
  {
    auto* scorer = fresh();
    EXPECT_EQ(2, scorer->next());
    int32_t docs[1]; float scores[1];
    EXPECT_EQ(1, scorer->fillScoreBlock(docs, scores, 1, 3));
    EXPECT_EQ(2, docs[0]); EXPECT_EQ(1.0f, scores[0]);
    EXPECT_EQ(4, scorer->docId());
    EXPECT_EQ(2.0f, scorer->score());
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }
  {
    auto* scorer = fresh();
    uint64_t words[] = {0};
    scorer->fillScoredWindowBits(words, 0, 3);
    EXPECT_EQ(1u << 2, words[0]);
    EXPECT_EQ(4, scorer->docId());
    EXPECT_EQ(2.0f, scorer->score());
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }
}

TEST_F(MinScoreTest, bulkPlansAndScoreScalingMatchPull) {
  CollectionHelper h;
  for (int32_t i = 0; i < 256; i++) {
    h.index(flatdoc("body_w", i % 2 ? "alpha alpha" : "alpha pad"));
  }
  h.commit();
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  TermQuery term("body_w", "alpha");
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* childWeight = term.createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  auto* child = childWeight->createScorer(pool, segment);
  ASSERT_EQ(0, child->next());
  MinScoreQuery query(&term, child->score());
  ASSERT_EQ(1, child->next());
  float highScore = child->score();
  using Use = Query::ExecutionUse;
  using Answer = Query::ScorerSupplier::BulkAnswer;
  auto childPlan = childWeight->scorerSupplier(pool, segment)->planBulk(Use::SCORED_WINDOWS, {});
  for (bool scored : {false, true}) {
    for (float multiplier : {0.0f, 0.37f, 2.0f}) {
      auto* supplier = query.createWeight(
          context, scored ? Query::NEED_SCORES : 0, multiplier)
          ->scorerSupplier(pool, segment);
      EXPECT_EQ(Answer::NO, supplier->planBulk(
          Use::EXACT_CANDIDATE_SCORING, {}).available);
      Query::ScorerSupplier::BulkScorerContext countContext;
      countContext.requireConstantCount = true;
      EXPECT_EQ(Answer::NO, supplier->planBulk(
          Use::COUNT_WINDOWS, countContext).available);
      for (Use use : {Use::SCORED_WINDOWS, Use::MATCH_WINDOWS, Use::COUNT_WINDOWS}) {
        auto plan = supplier->planBulk(use, {});
        ASSERT_EQ(Answer::YES, plan.available);
        EXPECT_EQ(Answer::YES, plan.supportsMatchWindows);
        EXPECT_EQ(Answer::NO, plan.supportsExactCandidateScoring);
        EXPECT_EQ(childPlan.acceptsWindowFilter, plan.acceptsWindowFilter);
        EXPECT_EQ(Answer::NO, plan.supportsReposition());
        EXPECT_EQ(-1, plan.constantCount);
        auto* bulk = supplier->buildBulk(pool, plan);
        ASSERT_NE(nullptr, bulk);
        auto* pull = supplier->resolve(pool, supplier->makePlanContext(
            Query::Demand::fromLeadCost(segment.maxDoc())))->build(pool);
        ASSERT_NE(nullptr, pull);
        if (use != Use::SCORED_WINDOWS) {
          bulk->prepareMembership();
          pull->prepareMembership();
        }
        if (use == Use::COUNT_WINDOWS) {
          EXPECT_EQ(256, countMatchesWindowed(
              bulk, nullptr, nullptr, segment.maxDoc()));
          continue;
        }
        // Small bounded ranges exercise continuation after a bounded END.
        for (int32_t min = 0; min < segment.maxDoc();) {
          int32_t max = std::min(min + 7, segment.maxDoc());
          ScoreWindow out;
          int32_t next = use == Use::MATCH_WINDOWS
              ? bulk->matchNextWindow(out, nullptr, min, max)
              : bulk->scoreNextWindow(out, nullptr, min, max,
                                      std::numeric_limits<float>::lowest());
          for (int32_t i = 0; i < out.size; i++) {
            EXPECT_EQ(pull->next(), out.docs[(size_t)i]);
            EXPECT_EQ(std::bit_cast<uint32_t>(pull->score()),
                      std::bit_cast<uint32_t>(out.scores[(size_t)i]));
          }
          min = std::min(next, max);
        }
        EXPECT_EQ(PostingsReader::END, pull->next());
        if (use == Use::SCORED_WINDOWS && scored && multiplier != 0.0f) {
          auto* competitive = supplier->buildBulk(pool, supplier->planBulk(use, {}));
          competitive->setTopKDepth(1, true);
          float theta = highScore * multiplier;
          int32_t count = 0;
          for (int32_t min = 0; min < segment.maxDoc();) {
            ScoreWindow out;
            min = competitive->scoreNextWindow(out, nullptr, min, segment.maxDoc(), theta);
            count += out.size;
            for (int32_t i = 0; i < out.size; i++) {
              EXPECT_EQ(1, out.docs[(size_t)i] % 2);
              EXPECT_EQ(std::bit_cast<uint32_t>(theta),
                        std::bit_cast<uint32_t>(out.scores[(size_t)i]));
            }
          }
          EXPECT_EQ(128, count);
        }
      }
    }
  }
}

TEST_F(MinScoreTest, unreachableTermFloorSkipsImpactsForCount) {
  CollectionHelper h;
  constexpr int32_t nDocs = 8192;
  for (int32_t i = 0; i < nDocs; i++) h.index(flatdoc("body_w", "alpha pad"));
  h.commit();
  SkipStatsScope stats;
  // Check constructor seeding independently of the bulk adapter's floor.
  auto reader = h.getIndexWriter()->snapshots.readers.getReader();
  MemPool pool;
  Query::Context context(pool, *reader);
  TermQuery term("body_w", "alpha");
  MinScoreQuery query(&term, 100000.0f);
  auto* scorer = query.createWeight(context, 0)->createScorer(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, scorer);
  EXPECT_EQ(PostingsReader::END, scorer->next());
  EXPECT_GT(SkipStats::impactCompetitiveColdLookups, 0);
  EXPECT_LT(SkipStats::minScoreChecks, nDocs / 100);
  SkipStats::reset();
  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("body_w", "alpha")
      .minScore(100000.0f).getNumber().limit(0);
  req->execute(1);
  ASSERT_OK(req);
  EXPECT_EQ(0, req->getMatchCount());
  EXPECT_GT(SkipStats::impactCompetitiveColdLookups, 0);
  EXPECT_LT(SkipStats::docBlocksDecoded, nDocs / Postings::DOCS_BLOCK_SIZE / 4);
  EXPECT_LT(SkipStats::minScoreChecks, nDocs / 100);
}
