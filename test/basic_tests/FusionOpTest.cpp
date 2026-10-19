// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "luxir/schema/Schema.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/ops/FusionOp.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TaskSpawnCounter.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class FusionOpTest : public LuxirTest {
protected:
  void SetUp() override {
    auto col = luxirNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }

  // Override _v with a vector field so the same docs can carry both an
  // analyzed text field (_w) and a dense vector for fusion across both.
  static void installVecSchema(Collection& col, luxir::api::VectorMetric metric) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = luxir::api::FieldDef_::FieldClass::VECTOR;
    f.column = true;
    f.metric = metric;
    b.set(col);
  }

  // --- arena builders for the Fusion op the OpCursor fluent API doesn't cover ---

  // Copy a by-value Query into the build arena and return a stable pointer for an
  // optional_indirect_view<Query> member to point at.
  static luxir::api::Query* arenaQuery(std::pmr::memory_resource& mr, const luxir::api::Query& q) {
    auto* p = (luxir::api::Query*)mr.allocate(sizeof(luxir::api::Query), alignof(luxir::api::Query));
    return new (p) luxir::api::Query(q);
  }

  // Grow Fusion.sources (map_view<string_view, TopDocs>, TopDocs by value) by one entry and
  // return the fresh slot to fill. Realloc-grow (mirrors LocalReq::appendOp): the prior
  // entries are trivially copyable with their nested data in the arena. The returned
  // reference stays valid until the next addSource() call reallocates the backing array.
  static luxir::api::TopDocs& addSource(luxir::api::Fusion& fusion, std::string_view name,
                                        std::pmr::memory_resource& mr) {
    using Map = luxir::api::map_view<std::string_view, luxir::api::TopDocs>;
    using Pair = std::pair<std::string_view, luxir::api::TopDocs>;
    auto old = fusion.sources;
    Pair* a = (Pair*)mr.allocate(sizeof(Pair) * (old.size() + 1), alignof(Pair));
    std::uninitialized_value_construct_n(a, old.size() + 1);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()].first = build::arenaStr(mr, name);
    fusion.sources = Map(std::span<const Pair>(a, old.size() + 1));
    return a[old.size()].second;
  }

  // Append one field name to a Fusion.fields span (realloc-grow preserves prior entries).
  static void addField(luxir::api::Fusion& fusion, std::string_view field,
                       std::pmr::memory_resource& mr) {
    auto old = fusion.fields;
    std::string_view* a = build::allocArray(fusion.fields, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()] = build::arenaStr(mr, field);
  }

  static void addFilter(std::span<const luxir::api::Filter>& filter,
                        const luxir::api::Query& q,
                        std::pmr::memory_resource& mr,
                        std::initializer_list<std::string_view> exceptOps = {}) {
    auto old = filter;
    luxir::api::Filter* a = build::allocArray(filter, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()].query = arenaQuery(mr, q);
    auto* except = build::allocArray(a[old.size()].except_ops, exceptOps.size(), mr);
    size_t i = 0;
    for (std::string_view op : exceptOps) {
      except[i++] = build::arenaStr(mr, op);
    }
  }

  // Append a sort spec to a TopDocs source (realloc-grow preserves prior entries).
  static void addSort(luxir::api::TopDocs& src, std::string_view expr,
                      luxir::api::SortSpec::SortDir dir, std::pmr::memory_resource& mr) {
    auto old = src.sort;
    luxir::api::SortSpec* a = build::allocArray(src.sort, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()].expr = build::arenaStr(mr, expr);
    a[old.size()].dir = dir;
  }

  static void setTextSource(luxir::api::TopDocs& src, std::pmr::memory_resource& mr,
                            std::string_view field, std::string_view term, int64_t limit) {
    src.limit = limit;
    src.query = arenaQuery(mr, qb::match(mr, field, term));
  }

  static void setKnnSource(luxir::api::TopDocs& src, std::pmr::memory_resource& mr,
                           std::string_view field, std::vector<float> query, int32_t k) {
    src.limit = k;
    src.query = arenaQuery(mr, qb::knn(mr, field, query, k));
  }

  // Set a source's query to a BooleanQuery of several optional kNN clauses, so
  // a single source's Context hosts multiple kNN weights (prepared serially),
  // while many such sources prepare concurrently against the shared request pool.
  static void setBoolKnnSource(luxir::api::TopDocs& src, std::pmr::memory_resource& mr,
                               std::string_view field,
                               const std::vector<std::vector<float>>& queryVecs,
                               int32_t k, int64_t limit) {
    src.limit = limit;
    std::vector<luxir::api::Query> optional;
    for (const auto& qv : queryVecs) optional.push_back(qb::knn(mr, field, qv, k));
    src.query = arenaQuery(mr, qb::boolean(mr, std::span<const luxir::api::Query>{},
                                           std::span<const luxir::api::Query>(optional)));
  }

  // Pull "id" out of a Fusion response in fused-rank order.
  static std::vector<std::string> resultIds(LocalReq& req, std::string_view opName = "f") {
    std::vector<std::string> ids;
    const auto* dl = req.docList(opName);
    if (dl == nullptr) return ids;
    const auto* colp = dl->columns.find("id");
    if (colp == nullptr) return ids;
    const auto* col = std::get_if<luxir::api::ColStr>(&colp->kind);
    if (col == nullptr) return ids;
    for (const auto& s : col->v) ids.emplace_back(s);
    return ids;
  }

  static std::vector<float> resultScores(LocalReq& req, std::string_view opName = "f") {
    std::vector<float> scores;
    const auto* dl = req.docList(opName);
    if (dl == nullptr) return scores;
    const auto* colp = dl->columns.find("_score_");
    if (colp == nullptr) return scores;
    const auto* col = std::get_if<luxir::api::ColFloat>(&colp->kind);
    if (col == nullptr) return scores;
    for (float v : col->v) scores.push_back(v);
    return scores;
  }

  // Bucket ids of the field facet "ids" on fusion "f".
  static std::set<std::string_view> facetIds(LocalReq& req) {
    const auto* ids = req.docList("f")->ops.at("ids")->facetResult();
    const auto& v = std::get<api::ColStr>(ids->bucket_ids->kind).v;
    return {v.begin(), v.end()};
  }
};


// Basic RRF over a text source and a KNN source.  Crafted so each source's
// ranking is unambiguous, so we can assert exact fused scores:
//   - text "apple" matches only "a" (single match -> rank 1).
//   - knn near [1,0,0] under L2 ranks a (d^2=0), b (0.02), c (1.5) at 1..3;
//     d (2.0) falls outside the k=3 cutoff.
TEST_F(FusionOpTest, rrfTextAndKnn) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), luxir::api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",  "embedding_v", std::vector<float>{1.0f, 0,    0   }));
  h.index(flatdoc("id", std::string("b"), "foo_w", "orange", "embedding_v", std::vector<float>{0.9f, 0.1f, 0   }));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange", "embedding_v", std::vector<float>{0,    0.5f, 0.5f}));
  h.index(flatdoc("id", std::string("d"), "foo_w", "orange", "embedding_v", std::vector<float>{0,    1.0f, 0   }));
  h.commit({"*"});

  auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  fusion.get_scores = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;
  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);
  setKnnSource(addSource(fusion, "vec", mr), mr, "embedding_v", {1, 0, 0}, 3);

  lreq->execute();
  ASSERT_OK(lreq);

  // Union: a (text+knn), b (knn), c (knn).  d is excluded by k=3.
  auto matches = lreq->getMatchCount("f");
  EXPECT_EQ(matches, 3);

  auto ids = resultIds(*lreq);
  auto scores = resultScores(*lreq);
  ASSERT_EQ(ids.size(), 3u);
  ASSERT_EQ(scores.size(), 3u);

  // Deterministic ranks -> exact fused scores:
  //   a: text rank 1 + knn rank 1 = 1/61 + 1/61
  //   b: knn rank 2                = 1/62
  //   c: knn rank 3                = 1/63
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");
  EXPECT_NEAR(scores[0], 1.0f / 61.0f + 1.0f / 61.0f, 1e-6f);
  EXPECT_NEAR(scores[1], 1.0f / 62.0f, 1e-6f);
  EXPECT_NEAR(scores[2], 1.0f / 63.0f, 1e-6f);

  lreq->done();
}


// Fusion-level filter restricts the docs each source sees, without the caller
// having to repeat the filter on every source.  Distinct foo_w field lengths
// force a deterministic BM25 ordering between the two surviving docs so we
// can assert exact fused scores.
TEST_F(FusionOpTest, sharedFilter) {
  CollectionHelper h("main");

  // All three match "apple".  After the color=red filter, only a and c
  // remain; a is shorter so BM25 ranks it first.
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",       "color_s", "red"));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple x",     "color_s", "blue"));
  h.index(flatdoc("id", std::string("c"), "foo_w", "apple x y",   "color_s", "red"));
  h.commit();

  auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  fusion.get_scores = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;

  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 10);

  addFilter(fusion.filter, qb::match(mr, "color_s", "red"), mr);

  lreq->execute();
  ASSERT_OK(lreq);

  auto ids = resultIds(*lreq);
  auto scores = resultScores(*lreq);
  ASSERT_EQ(ids.size(), 2u);
  ASSERT_EQ(scores.size(), 2u);

  // Source ranks (single source):  a=1, c=2.  Fused = 1/(60+rank).
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "c");
  EXPECT_NEAR(scores[0], 1.0f / 61.0f, 1e-6f);
  EXPECT_NEAR(scores[1], 1.0f / 62.0f, 1e-6f);

  lreq->done();
}

TEST_F(FusionOpTest, routedSharedFilterIsRejected) {
  CollectionHelper helper("main");
  helper.index(flatdoc("foo_w", "apple", "color_s", "red"),
               UpdateMessage::COMMIT);

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.method.emplace<api::RrfFusion>().k = 60;
  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 10);
  addFilter(fusion.filter, qb::match(mr, "color_s", "red"), mr, {"brands"});

  ExpectLog quiet("Search request rejected");
  lreq->execute();
  EXPECT_NE(std::string::npos,
            lreq->errorMsg().find("fusion.filter[0].except_ops is not supported"))
      << lreq->errorMsg();
}

TEST_F(FusionOpTest, routedSourceFilterHasNoTarget) {
  CollectionHelper helper("main");
  helper.index(flatdoc("foo_w", "apple", "color_s", "red"),
               UpdateMessage::COMMIT);

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.method.emplace<api::RrfFusion>().k = 60;
  auto& source = addSource(fusion, "text", mr);
  setTextSource(source, mr, "foo_w", "apple", 10);
  addFilter(
      source.filter, qb::match(mr, "color_s", "red"), mr, {"ignored"});

  ExpectLog quiet("Search request rejected");
  lreq->execute();
  EXPECT_NE(lreq->errorMsg().find("fusion source 'text'.filter[0].except_ops"),
            std::string::npos)
      << lreq->errorMsg();
  EXPECT_NE(lreq->errorMsg().find(
                "filter routing has no target on a fusion source"),
            std::string::npos)
      << lreq->errorMsg();
}

TEST_F(FusionOpTest, pureCountWholeHitComposesSharedDomain) {
  CollectionHelper h("main");
  h.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0});
  ASSERT_TRUE(h.indexAll(
      {flatdoc("id", "a", "foo_w", "alpha beta", "keep_s", "yes"),
       flatdoc("id", "b", "foo_w", "alpha beta", "keep_s", "no"),
       flatdoc("id", "c", "foo_w", "alpha", "keep_s", "yes"),
       flatdoc("id", "d", "foo_w", "alpha beta", "keep_s", "yes")},
      UpdateMessage::COMMIT).success);

  auto countQuery = [](std::pmr::memory_resource& mr) {
    return qb::boolean(
        mr, {qb::match(mr, "foo_w", "alpha"),
             qb::match(mr, "foo_w", "beta")});
  };
  for (int round = 0; round < 3; round++) {
    auto warm = localReq(luxirNode->getSearchEngine());
    warm->collection("main");
    auto& topDocs = warm->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() = countQuery(topDocs.mr());
    warm->execute(false);
    ASSERT_OK(warm);
    EXPECT_EQ(3, warm->getMatchCount("q"));
  }

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion =
      lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;

  auto& countSource = addSource(fusion, "count", mr);
  countSource.limit = 0;
  countSource.get_number = true;
  countSource.query = arenaQuery(mr, countQuery(mr));
  auto& rankedSource = addSource(fusion, "ranked", mr);
  rankedSource.limit = 10;
  rankedSource.query = arenaQuery(mr, countQuery(mr));
  addFilter(fusion.filter, qb::match(mr, "keep_s", "yes"), mr);

  bool saved = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  lreq->execute(false);
  int64_t wholeHits = SkipStats::wholeCountHits;
  int64_t wholeBuilds = SkipStats::wholeCountBuilds;
  int64_t wholeBypasses = SkipStats::wholeCountBypasses;
  SkipStats::enabled = saved;
  ASSERT_OK(lreq);
  EXPECT_EQ(1, wholeHits);
  EXPECT_EQ(0, wholeBuilds);
  EXPECT_EQ(0, wholeBypasses);
  EXPECT_EQ((std::vector<std::string>{"a", "d"}), resultIds(*lreq));
}

// No fusion-level fields: the default projection applies to the fused list
// too - every retrievable field, as per-document rows.
TEST_F(FusionOpTest, defaultProjectionWhenNoFieldsNamed) {
  CollectionHelper h("main");
  ASSERT_TRUE(h.indexAll(
      {flatdoc("id", "a", "foo_w", "alpha beta"),
       flatdoc("id", "b", "foo_w", "alpha")},
      UpdateMessage::COMMIT).success);

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion =
      lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.method.emplace<api::RrfFusion>().k = 60;
  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "alpha", 10);
  lreq->execute(false);
  ASSERT_OK(lreq);

  const auto* dl = lreq->docList("f");
  ASSERT_NE(dl, nullptr);
  EXPECT_TRUE(dl->columns.empty());
  ASSERT_EQ(2u, dl->docs.size());
  auto docs = lreq->getDocs("f");
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", "a", "foo_w", "alpha beta"));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", "b", "foo_w", "alpha"));
}

TEST_F(FusionOpTest, topKCountWholeHitComposesSharedDomainOnce) {
  CollectionHelper h("main");
  h.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0});
  ASSERT_TRUE(h.indexAll(
      {flatdoc("id", "a", "foo_w", "alpha beta", "keep_s", "yes"),
       flatdoc("id", "b", "foo_w", "alpha beta", "keep_s", "no"),
       flatdoc("id", "c", "foo_w", "alpha", "keep_s", "yes"),
       flatdoc("id", "d", "foo_w", "alpha beta", "keep_s", "yes")},
      UpdateMessage::COMMIT).success);

  auto rankedQuery = [](std::pmr::memory_resource& mr) {
    return qb::boolean(
        mr, {qb::match(mr, "foo_w", "alpha"),
             qb::match(mr, "foo_w", "beta")});
  };
  for (int round = 0; round < 3; round++) {
    auto warm = localReq(luxirNode->getSearchEngine());
    warm->collection("main");
    auto& topDocs = warm->topDocs("q").getNumber().getScores()
        .fields({"id"}).limit(10);
    topDocs.rawQuery() = rankedQuery(topDocs.mr());
    warm->execute(false);
    ASSERT_OK(warm);
    EXPECT_EQ(3, warm->getMatchCount("q"));
  }

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion =
      lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;

  auto& source = addSource(fusion, "ranked", mr);
  source.limit = 10;
  source.get_number = true;
  source.get_scores = true;
  source.query = arenaQuery(mr, rankedQuery(mr));
  addFilter(fusion.filter, qb::match(mr, "keep_s", "yes"), mr);

  bool saved = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  lreq->execute(false);
  int64_t wholeHits = SkipStats::wholeTopKCountHits;
  int64_t wholeBuilds = SkipStats::wholeTopKCountBuilds;
  int64_t wholeBypasses = SkipStats::wholeTopKCountBypasses;
  SkipStats::enabled = saved;
  ASSERT_OK(lreq);
  EXPECT_EQ(1, wholeHits);
  EXPECT_EQ(0, wholeBuilds);
  EXPECT_EQ(0, wholeBypasses);
  EXPECT_EQ((std::vector<std::string>{"a", "d"}), resultIds(*lreq));
}

TEST_F(FusionOpTest, sharedKnnFilter) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), luxir::api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "embedding_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple",
                  "embedding_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange",
                  "embedding_v", std::vector<float>{0.0f, 0.9f}));
  h.commit({"*"});

  auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;

  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 10);

  addFilter(fusion.filter, qb::knn(mr, "embedding_v", {0.0f, 1.0f}, 1), mr);

  lreq->execute();
  ASSERT_OK(lreq);

  auto ids = resultIds(*lreq);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "b");

  lreq->done();
}


// Multi-segment: exercises the AtomicMerger path where per-source results
// are produced across segments and merged before fusion fires.  Three
// commits => three segments.  Also exercises the field-sort path for a
// source: "banana" sorts by prio_i DESC instead of by BM25, so its rank
// order is driven by an indexed value rather than by score.
//
//   apple matchers (BM25, field length): a(1), b(2), d(3), e(4)
//                                        -> apple ranks 1..4
//   banana matchers (prio_i DESC):       d(50), c(40), e(20), b(10)
//                                        -> banana ranks 1..4
TEST_F(FusionOpTest, rrfMultiSegment) {
  CollectionHelper h("main");

  // Seg 0: a (apple only), b (both).
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",              "prio_i", (int64_t)50));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple banana",       "prio_i", (int64_t)10));
  h.commit();

  // Seg 1: d (both), c (banana only).
  h.index(flatdoc("id", std::string("d"), "foo_w", "apple banana d1",    "prio_i", (int64_t)50));
  h.index(flatdoc("id", std::string("c"), "foo_w", "banana c1 c2 c3 c4", "prio_i", (int64_t)40));
  h.commit();

  // Seg 2: e (both).
  h.index(flatdoc("id", std::string("e"), "foo_w", "apple banana e1 e2", "prio_i", (int64_t)20));
  h.commit();

  auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  fusion.get_scores = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;
  setTextSource(addSource(fusion, "apple", mr), mr, "foo_w", "apple", 10);
  auto& bananaSrc = addSource(fusion, "banana", mr);
  setTextSource(bananaSrc, mr, "foo_w", "banana", 10);
  addSort(bananaSrc, "prio_i", luxir::api::SortSpec::SortDir::DESC, mr);

  lreq->execute();
  ASSERT_OK(lreq);

  // Union: a, b, c, d, e.
  auto matches = lreq->getMatchCount("f");
  EXPECT_EQ(matches, 5);

  auto ids = resultIds(*lreq);
  auto scores = resultScores(*lreq);
  ASSERT_EQ(ids.size(), 5u);
  ASSERT_EQ(scores.size(), 5u);

  // Expected fused scores (apple rank + banana rank, 1/(60+r) per contribution):
  //   a: apple=1                -> 1/61
  //   b: apple=2, banana=4      -> 1/62 + 1/64
  //   c: banana=2               -> 1/62
  //   d: apple=3, banana=1      -> 1/63 + 1/61
  //   e: apple=4, banana=3      -> 1/64 + 1/63
  // Sorted desc: d > b > e > a > c.
  EXPECT_EQ(ids[0], "d");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "e");
  EXPECT_EQ(ids[3], "a");
  EXPECT_EQ(ids[4], "c");

  EXPECT_NEAR(scores[0], 1.0f / 63.0f + 1.0f / 61.0f, 1e-6f);
  EXPECT_NEAR(scores[1], 1.0f / 62.0f + 1.0f / 64.0f, 1e-6f);
  EXPECT_NEAR(scores[2], 1.0f / 64.0f + 1.0f / 63.0f, 1e-6f);
  EXPECT_NEAR(scores[3], 1.0f / 61.0f, 1e-6f);
  EXPECT_NEAR(scores[4], 1.0f / 62.0f, 1e-6f);

  lreq->done();
}


TEST_F(FusionOpTest, subOpsUseFusedCandidates) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  auto index = [&](const char* id, const char* text, const char* color,
                   int price, float distance, const char* keep = "yes",
                   const char* owner = "yes") {
    h.index(flatdoc("id", id, "foo_w", text, "color_s", color,
                   "price_i", price, "embedding_v", std::vector<float>{distance},
                   "keep_s", keep, "owner_s", owner));
  };
  // Text ranks b, a; kNN ranks a, c. The union is a, b, c (sum = 60).
  index("a", "apple", "red", 10, 0);
  index("b", "apple", "red", 20, 10);
  h.commit();
  index("c", "banana", "blue", 30, 1);
  h.commit();
  // This entire segment contributes no candidates. d matches text but is
  // below its limit; e is outside kNN k. f and g test the two filter scopes.
  index("d", "apple", "excluded", 1, 20);
  index("e", "banana", "excluded", 100, 30);
  index("f", "apple", "excluded", 200, 0.1f, "no");
  index("g", "apple", "excluded", 300, 40, "yes", "no");
  h.commit();
  ASSERT_EQ(3u, h.getIndexWriter()->snapshots.readers.getReader()->segments().size());

  for (int maxParallel : {0, 1, -1}) {
    for (int limit : {0, 1, 10}) {
      auto req = localReq(luxirNode->getSearchEngine());
      parseQueryRequest(R"json({"ops":{"f":{"fusion":{
        "sources":{
          "text":{"query":{"match":{"foo_w":"apple"}},"limit":2,
                  "sort":[{"expr":"price_i","dir":"desc"}],
                  "filter":[{"match":{"owner_s":"yes"}}]},
          "knn":{"query":{"knn":{"field":"embedding_v","query":[0],"k":2}},
                 "limit":2}},
        "filter":[{"match":{"keep_s":"yes"}}],"rrf":{},
        "get_number":true,"fields":["id"],
        "ops":{
          "colors":{"field_facet":{"field":"color_s","ops":{
            "ids":{"field_facet":{"field":"id"}}}}},
          "total":{"expr_op":"sum(price_i)"}}
      }}}})json", req->rawRequest(), req->mr);
      auto& fusion = std::get<api::Fusion>(
          const_cast<api::SearchOp&>(*req->rawRequest().ops.at("f")).kind);
      fusion.limit = limit;
      fusion.offset = limit == 1 ? 1 : 0;
      req->execute(maxParallel);
      ASSERT_OK(req);
      const auto* docs = req->docList("f");
      ASSERT_NE(nullptr, docs);
      EXPECT_EQ(3, docs->found.value_or(-1));
      EXPECT_EQ((size_t)std::min(limit, 3), resultIds(*req).size());
      EXPECT_EQ(60, std::get<int64_t>(docs->ops.at("total")->kind));
      const auto* colors = docs->ops.at("colors")->facetResult();
      ASSERT_NE(nullptr, colors);
      const auto& names = std::get<api::ColStr>(colors->bucket_ids->kind).v;
      ASSERT_EQ(2u, names.size());
      std::map<std::string, int64_t> counts;
      const auto& nested = std::get<api::ArrVal>(colors->ops.at("ids")->kind).v;
      ASSERT_EQ(names.size(), nested.size());
      for (size_t i = 0; i < names.size(); i++) {
        counts[std::string(names[i])] = colors->counts[i];
        const auto* ids = nested[i].facetResult();
        ASSERT_NE(nullptr, ids);
        const auto& values = std::get<api::ColStr>(ids->bucket_ids->kind).v;
        std::set<std::string_view> actual(values.begin(), values.end());
        EXPECT_EQ(names[i] == "red" ? (std::set<std::string_view>{"a", "b"})
                                    : (std::set<std::string_view>{"c"}), actual);
        for (auto count : ids->counts) EXPECT_EQ(1, count);
      }
      EXPECT_EQ((std::map<std::string, int64_t>{{"red", 2}, {"blue", 1}}), counts);
    }
  }
}

TEST_F(FusionOpTest, sourceOpsRejected) {
  CollectionHelper h("main");
  auto req = localReq(luxirNode->getSearchEngine());
  parseQueryRequest(R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{
    "text":{"query":{"all":true},"ops":{"colors":{
      "field_facet":{"field":"color_s"}}}}}}}}})",
      req->rawRequest(), req->mr);
  ExpectLog quiet("Search request rejected");
  req->execute();
  EXPECT_NE(std::string::npos, req->errorMsg().find(
      "fusion source 'text': ops are not executed under Fusion; put them in Fusion.ops"))
      << req->errorMsg();
}


// Per-source filter intersected with the fusion-level filter.  Each filter
// should be applied; only docs satisfying both pass through.
TEST_F(FusionOpTest, sharedAndPerSourceFilter) {
  CollectionHelper h("main");

  // Docs: id, foo_w, color_s, owner_s
  //   a: apple, red,  alice  - passes shared (red) and per-source (alice)
  //   b: apple, red,  bob    - passes shared, fails per-source
  //   c: apple, blue, alice  - fails shared
  //   d: apple, blue, bob    - fails both
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple", "color_s", "red",  "owner_s", "alice"));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple", "color_s", "red",  "owner_s", "bob"));
  h.index(flatdoc("id", std::string("c"), "foo_w", "apple", "color_s", "blue", "owner_s", "alice"));
  h.index(flatdoc("id", std::string("d"), "foo_w", "apple", "color_s", "blue", "owner_s", "bob"));
  h.commit();

  auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  addField(fusion, "id", mr);
  fusion.method.emplace<api::RrfFusion>().k = 60;

  // Shared filter: color_s = red.
  addFilter(fusion.filter, qb::match(mr, "color_s", "red"), mr);

  // Per-source filter: owner_s = alice (applied to the text source only).
  auto& textSrc = addSource(fusion, "text", mr);
  setTextSource(textSrc, mr, "foo_w", "apple", 10);
  addFilter(textSrc.filter, qb::match(mr, "owner_s", "alice"), mr);

  lreq->execute();
  ASSERT_OK(lreq);

  std::set<std::string> got;
  for (auto& id : resultIds(*lreq)) got.insert(id);
  EXPECT_EQ(got.size(), 1u);
  EXPECT_TRUE(got.count("a"));
  EXPECT_FALSE(got.count("b"));
  EXPECT_FALSE(got.count("c"));
  EXPECT_FALSE(got.count("d"));

  lreq->done();
}


// Validation: missing required pieces surface as
// errors in the response.
TEST_F(FusionOpTest, validation) {
  CollectionHelper h("main");
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple"), UpdateMessage::COMMIT);

  auto buildBase = [&]() {
    auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
    lreq->collection("main");
    return lreq;
  };

  // No sources.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
    fusion.method.emplace<api::RrfFusion>().k = 60;
    ExpectLog quiet("Search request rejected");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("fusion 'f': requires at least one source"),
              std::string::npos) << lreq->errorMsg();
    lreq->done();
  }

  // No method.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
    auto& mr = lreq->mr;
    setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);
    ExpectLog quiet("Search request rejected");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("method"), std::string::npos);
    lreq->done();
  }

  // Negative k.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
    auto& mr = lreq->mr;
    fusion.method.emplace<api::RrfFusion>().k = -1;
    setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);
    ExpectLog quiet("Search request rejected");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("k"), std::string::npos);
    lreq->done();
  }

}


// Empty index: no segments, no per-source tasks ever run.  The empty-index
// branch should still emit a well-formed (empty) DocList.
TEST_F(FusionOpTest, emptyIndex) {
  CollectionHelper h("main");

  auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 5;
  fusion.get_number = true;
  fusion.method.emplace<api::RrfFusion>().k = 60;
  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);

  auto* facet = build::mapSlot<api::SearchOp>(fusion.ops, 1, "colors", mr);
  facet->kind.emplace<api::FieldFacet>().field = "color_s";

  lreq->execute();
  ASSERT_OK(lreq);

  const auto* dl = lreq->docList("f");
  ASSERT_TRUE(dl != nullptr);
  EXPECT_EQ(dl->found.value_or(0), 0);
  const auto* colors = dl->ops.at("colors")->facetResult();
  ASSERT_NE(nullptr, colors);
  EXPECT_TRUE(colors->counts.empty());

  lreq->done();
}


TEST_F(FusionOpTest, emptyIndexWithTwoSourcesSpawnsNoTasks) {
  CollectionHelper h("main");
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& fusion = req->topDocs("f").rawOp().kind.emplace<api::Fusion>();
  fusion.limit = 5;
  fusion.get_number = true;
  fusion.method.emplace<api::RrfFusion>().k = 60;
  setTextSource(addSource(fusion, "text", req->mr), req->mr, "foo_w", "apple", 5);
  setTextSource(addSource(fusion, "other", req->mr), req->mr, "foo_w", "banana", 5);
  auto* facet = build::mapSlot<api::SearchOp>(fusion.ops, 1, "colors", req->mr);
  facet->kind.emplace<api::FieldFacet>().field = "color_s";
  int64_t spawns;
  {
    TaskSpawnCounter counter;
    luxirNode->getSearchEngine().dispatch(*req.get(), -1);
    spawns = counter.count();
  }
  ASSERT_OK(req);
  EXPECT_EQ(0, spawns);
  ASSERT_NE(nullptr, req->docList("f"));
  EXPECT_EQ(0, req->docList("f")->row_count);
  EXPECT_EQ(0, req->docList("f")->found.value_or(-1));
  EXPECT_TRUE(req->docList("f")->ops.at("colors")->facetResult()->counts.empty());
}

TEST_F(FusionOpTest, unarySubOpDoesNotSpawn) {
  CollectionHelper h("main");
  h.indexAll(std::array{flatdoc("id", "1", "foo_w", "apple", "color_s", "red"),
                        flatdoc("id", "2", "foo_w", "banana", "color_s", "blue")},
             UpdateMessage::COMMIT);
  for (int profile : {0, -1}) {
    for (bool loadFields : {false, true}) {
      for (int64_t offset : {0, 10}) {
        SCOPED_TRACE(::testing::Message() << "profile=" << profile
                     << ", fields=" << loadFields << ", offset=" << offset);
        auto req = localReq(luxirNode->getSearchEngine());
        req->collection("main");
        auto& fusion = req->topDocs("f").rawOp().kind.emplace<api::Fusion>();
        fusion.limit = 10;
        fusion.offset = offset;
        fusion.method.emplace<api::RrfFusion>().k = 60;
        addField(fusion, loadFields ? "id" : "_score_", req->mr);
        setTextSource(addSource(fusion, "text", req->mr), req->mr, "foo_w", "apple", 5);
        setTextSource(addSource(fusion, "other", req->mr), req->mr, "foo_w", "banana", 5);
        auto* facet = build::mapSlot<api::SearchOp>(fusion.ops, 1, "colors", req->mr);
        facet->kind.emplace<api::FieldFacet>().field = "color_s";
        int64_t spawns;
        {
          TaskSpawnCounter counter;
          luxirNode->getSearchEngine().dispatch(*req.get(), profile);
          spawns = counter.count();
        }
        ASSERT_OK(req);
        EXPECT_EQ(profile == 0 ? 0 : 1, spawns);
        ASSERT_NE(nullptr, req->docList("f"));
        EXPECT_EQ(offset == 0 ? 2 : 0, req->docList("f")->row_count);
        EXPECT_EQ(2u, req->docList("f")->ops.at("colors")->facetResult()->counts.size());
      }
    }
  }
}

// Concurrency coverage for the kNN prepare() path.
//
// prepare() runs during the parallel execution phase.  It used to allocate scratch
// memory from the context pool, which is the same as the request pool and shared
// across query contexts in the same request. MemPool is not thread-safe and
// hence allocations should not be done concurrently.
//
TEST_F(FusionOpTest, concurrentKnnPrepareSharesRequestPool) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), luxir::api::VectorMetric::L2);

  // ~40 docs across 5 segments; ~1/3 lack the vector field so the sparse
  // single-valued valueRank->docId selector path runs.  Distinct vectors keep
  // kNN ordering unambiguous so the matched set is deterministic across runs.
  constexpr int kSegments = 5;
  constexpr int kPerSegment = 8;
  int next = 0;
  for (int seg = 0; seg < kSegments; seg++) {
    for (int j = 0; j < kPerSegment; j++) {
      if (next % 3 == 0) {
        h.index(flatdoc("id", std::to_string(next)));  // no vector -> sparse
      } else {
        float x = (float)next / (float)(kSegments * kPerSegment);
        h.index(flatdoc("id", std::to_string(next),
                        "embedding_v", std::vector<float>{x, 1.0f - x}));
      }
      next++;
    }
    // Flush a segment per batch; build the shard kNN index on the last commit.
    if (seg + 1 < kSegments) h.commit();
    else h.commit({"*"});
  }

  auto buildReq = [&]() {
    auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
    lreq->collection("main");
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<luxir::api::Fusion>();
    auto& mr = lreq->mr;
    fusion.limit = -1;
    fusion.get_number = true;
    addField(fusion, "id", mr);
    fusion.method.emplace<api::RrfFusion>().k = 60;
    // 10 sources, each a boolean of 2 optional kNN clauses => 20 kNN weights,
    // 10 source prepares running concurrently on the shared request pool.
    for (int s = 0; s < 10; s++) {
      float a = (float)s / 10.0f;
      setBoolKnnSource(addSource(fusion, "vec" + std::to_string(s), mr),
                       mr, "embedding_v", {{a, 1.0f - a}, {1.0f - a, a}}, 5, -1);
    }
    return lreq;
  };

  auto resultIdSet = [](LocalReq& req) {
    auto ids = resultIds(req);
    return std::set<std::string>(ids.begin(), ids.end());
  };

  std::set<std::string> baseline;
  {
    auto* lreq = buildReq();
    lreq->execute();
    ASSERT_OK(lreq);
    baseline = resultIdSet(*lreq);
    lreq->done();
  }
  ASSERT_FALSE(baseline.empty());

  for (int iter = 0; iter < 50; iter++) {
    auto* lreq = buildReq();
    lreq->execute();
    ASSERT_OK(lreq);
    auto ids = resultIdSet(*lreq);
    ASSERT_EQ(ids, baseline) << "divergent/garbage fused result set at iteration " << iter;
    lreq->done();
  }
}


TEST_F(FusionOpTest, offsetPagesSourceWindows) {
  CollectionHelper helper;
  for (int i = 0; i < 10; i++) {
    helper.index(flatdoc("id", std::to_string(i), "rank_i", (int64_t)i));
  }
  helper.commit();
  auto run = [&](int64_t offset, int64_t limit, int64_t sourceOffset = 0) {
    auto req = localReq(helper.getSearchEngine());
    auto& cursor = req->collection("main").topDocs("f");
    auto& fusion = cursor.rawOp().kind.emplace<api::Fusion>();
    cursor.offset(offset).limit(limit);
    fusion.get_number = true;
    fusion.get_scores = true;
    fusion.method.emplace<api::RrfFusion>();
    addField(fusion, "id", req->mr);
    for (bool descending : {false, true}) {
      auto& src = addSource(fusion, descending ? "desc" : "asc", req->mr);
      src.limit = 3;
      src.offset = sourceOffset;
      addSort(src, "rank_i", descending ? api::SortSpec::SortDir::DESC
                                      : api::SortSpec::SortDir::ASC, req->mr);
    }
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    return req;
  };
  auto all = run(0, 100);
  auto expected = resultIds(*all);
  ASSERT_EQ(6u, expected.size());
  std::vector<std::string> pages;
  std::vector<float> scores;
  for (int64_t offset = 0; offset < 6; offset += 2) {
    auto page = run(offset, 2);
    ASSERT_OK(page);
    EXPECT_EQ(6, page->docList("f")->found.value_or(-1));
    EXPECT_EQ(offset, page->docList("f")->offset);
    auto ids = resultIds(*page);
    pages.insert(pages.end(), ids.begin(), ids.end());
    auto pageScores = resultScores(*page);
    scores.insert(scores.end(), pageScores.begin(), pageScores.end());
  }
  EXPECT_EQ(expected, pages);
  EXPECT_EQ(resultScores(*all), scores);
  EXPECT_EQ(expected, resultIds(*run(0, 100, INT64_MAX)));
  auto rest = run(2, -1);
  EXPECT_EQ((std::vector<std::string>(expected.begin() + 2, expected.end())), resultIds(*rest));
  EXPECT_EQ(resultIds(*rest), resultIds(*run(2, INT64_MAX)));
  for (auto [offset, limit] : std::array<std::pair<int64_t, int64_t>, 4>{
           {{6, 2}, {7, 2}, {INT64_MAX, INT64_MAX}, {2, 0}}}) {
    auto empty = run(offset, limit);
    ASSERT_OK(empty);
    ASSERT_EQ(1u, empty->responses.size());
    const auto* docs = empty->docList("f");
    ASSERT_NE(nullptr, docs);
    EXPECT_EQ(6, docs->found.value_or(-1));
    EXPECT_EQ(offset, docs->offset);
    EXPECT_EQ(0, docs->row_count);
    EXPECT_FALSE(docs->more);
    EXPECT_FALSE(empty->responses[0]->proto.more);
  }
}

TEST_F(FusionOpTest, offsetNegativeRejected) {
  CollectionHelper helper;
  auto req = localReq(helper.getSearchEngine());
  auto& cursor = req->collection("main").topDocs("f");
  auto& fusion = cursor.rawOp().kind.emplace<api::Fusion>();
  cursor.offset(-2);
  fusion.method.emplace<api::RrfFusion>();
  addSource(fusion, "all", req->mr).limit = 3;
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("fusion 'f': offset must be >= 0"))
      << req->errorMsg();
}

// Weighted RRF.  The corpus (three segments) gives deterministic ranks:
//   apple matchers (BM25, field length): a, b, d, e -> ranks 1..4
//   banana matchers (prio_i DESC):       d, c, e, b -> ranks 1..4
class WeightedRrfTest : public FusionOpTest {
protected:
  void indexCorpus(CollectionHelper& h) {
    h.index(flatdoc("id", "a", "foo_w", "apple", "prio_i", 50));
    h.index(flatdoc("id", "b", "foo_w", "apple banana", "prio_i", 10));
    h.commit();
    h.index(flatdoc("id", "d", "foo_w", "apple banana d1", "prio_i", 50));
    h.index(flatdoc("id", "c", "foo_w", "banana c1 c2 c3 c4", "prio_i", 40));
    h.commit();
    h.index(flatdoc("id", "e", "foo_w", "apple banana e1 e2", "prio_i", 20));
    h.commit();
  }

  // `extra` is spliced into the fusion object (weights, rrf) and
  // `moreSources` into its sources.
  LocalReqHandle run(std::string_view extra, std::string_view moreSources = "") {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(R"({"ops":{"f":{"fusion":{"sources":{)" + std::string(moreSources) + R"(
        "apple":{"query":{"match":{"foo_w":"apple"}}},
        "banana":{"query":{"match":{"foo_w":"banana"}},
                  "sort":[{"expr":"prio_i","dir":"desc"}]}},
        "get_number":true,"get_scores":true,"fields":["id"],
        "ops":{"ids":{"field_facet":{"field":"id"}}},)"
        + std::string(extra) + "}}}}", req->rawRequest(), req->mr);
    req->execute();
    return req;
  }

  void expectError(std::string_view extra, std::string_view msg) {
    ExpectLog quiet("Search request rejected");
    auto req = run(extra);
    EXPECT_NE(std::string::npos, req->errorMsg().find(msg)) << req->errorMsg();
  }
};

TEST_F(WeightedRrfTest, weightedScores) {
  CollectionHelper h("main");
  indexCorpus(h);
  // Unlisted sources default to weight 1, and duplicate keys are last-wins.
  for (auto weights : {R"("weights":{"apple":2,"banana":0.5},"rrf":{"k":60})",
                       R"("weights":{"banana":1,"apple":2,"banana":0.5},"rrf":{})"}) {
    auto req = run(weights);
    ASSERT_OK(req);
    EXPECT_EQ((std::vector<std::string>{"b", "d", "e", "a", "c"}), resultIds(*req));
    auto s = resultScores(*req);
    ASSERT_EQ(5u, s.size());
    EXPECT_FLOAT_EQ(2.0f / 62 + 0.5f / 64, s[0]);
    EXPECT_FLOAT_EQ(2.0f / 63 + 0.5f / 61, s[1]);
    EXPECT_FLOAT_EQ(2.0f / 64 + 0.5f / 63, s[2]);
    EXPECT_FLOAT_EQ(2.0f / 61, s[3]);
    EXPECT_FLOAT_EQ(0.5f / 62, s[4]);
  }
  auto req = run(R"("weights":{"banana":1},"rrf":{})");
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<std::string>{"d", "b", "e", "a", "c"}), resultIds(*req));
  EXPECT_FLOAT_EQ(1.0f / 63 + 1.0f / 61, resultScores(*req)[0]);
}

// A zero-weight source is not parsed or executed: its documents are absent
// from the fused list, found, and the fusion ops domain.  The disabled
// "bad" source would be rejected if it were parsed.
TEST_F(WeightedRrfTest, zeroWeightDisablesSource) {
  CollectionHelper h("main");
  indexCorpus(h);
  auto req = run(R"("weights":{"banana":0,"bad":0},"rrf":{})",
                 R"("bad":{"ops":{"x":{"field_facet":{"field":"id"}}}},)");
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<std::string>{"a", "b", "d", "e"}), resultIds(*req));
  EXPECT_EQ((std::vector<float>{1.0f / 61, 1.0f / 62, 1.0f / 63, 1.0f / 64}),
            resultScores(*req));
  EXPECT_EQ(4, req->docList("f")->found.value_or(-1));
  EXPECT_EQ((std::set<std::string_view>{"a", "b", "d", "e"}), facetIds(*req));

  auto both = run(R"("rrf":{})");
  ASSERT_OK(both);
  EXPECT_EQ(5, both->docList("f")->found.value_or(-1));
  EXPECT_EQ((std::set<std::string_view>{"a", "b", "c", "d", "e"}), facetIds(*both));
}

TEST_F(WeightedRrfTest, optionalK) {
  CollectionHelper h("main");
  indexCorpus(h);
  auto omitted = run(R"("weights":{"banana":0},"rrf":{})");
  ASSERT_OK(omitted);
  EXPECT_FLOAT_EQ(1.0f / 61, resultScores(*omitted)[0]);
  auto zero = run(R"("weights":{"banana":0},"rrf":{"k":0})");
  ASSERT_OK(zero);
  EXPECT_EQ((std::vector<float>{1.0f, 1.0f / 2, 1.0f / 3, 1.0f / 4}), resultScores(*zero));
  expectError(R"("rrf":{"k":-1})", "fusion 'f': RrfFusion.k must be >= 0");
}

TEST_F(WeightedRrfTest, validation) {
  CollectionHelper h("main");
  indexCorpus(h);
  expectError(R"("weights":{"pear":1},"rrf":{})",
              "fusion 'f': weights names unknown source 'pear'");
  expectError(R"("weights":{"apple":-0.5},"rrf":{})",
              "fusion 'f': weight for source 'apple' must be finite, >= 0, "
              "and within float range");
  expectError(R"("weights":{"apple":0,"banana":0},"rrf":{})",
              "fusion 'f': at least one source weight must be > 0");
  for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity(), 1e300}) {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& fusion = req->collection("main").topDocs("f").rawOp().kind.emplace<api::Fusion>();
    fusion.method.emplace<api::RrfFusion>();
    setTextSource(addSource(fusion, "apple", req->mr), req->mr, "foo_w", "apple", 5);
    std::array<std::pair<std::string_view, double>, 1> weights{{{"apple", bad}}};
    fusion.weights = api::map_view<std::string_view, double>(
        std::span<const std::pair<std::string_view, double>>(weights));
    ExpectLog quiet("Search request rejected");
    req->execute();
    EXPECT_NE(std::string::npos, req->errorMsg().find(
        "weight for source 'apple' must be finite, >= 0, and within float range"))
        << req->errorMsg();
  }
}

// Linear fusion with min-max normalization.  Scores come from rescore (p_i)
// and inner-product kNN (negative dot products), so they do not depend on
// how documents fall into segments:
//   lex = rescore(apple, p_i): b 30, c 20, a 10           -> b 1, c 0.5, a 0
//   vec = knn IP [1,0,0], k 4: a -0.5, b -1, d -2, c -3   -> a 1, b 0.8, d 0.4, c 0
class LinearFusionTest : public FusionOpTest {
protected:
  static constexpr std::string_view LEX =
      R"("lex":{"query":{"rescore":{"query":{"match":{"foo_w":"apple"}},"expr":"p_i"}}})";
  static constexpr std::string_view VEC =
      R"("vec":{"query":{"knn":{"field":"embedding_v","query":[1,0,0],"k":4}},"limit":4})";

  // Commit after every `perSegment` documents.
  static void indexCorpus(std::string_view collection, int perSegment) {
    CollectionHelper h(collection);
    installVecSchema(h.collection(), api::VectorMetric::IP);
    struct Doc { const char* id; const char* text; int p; float x; };
    std::array<Doc, 5> docs{{{"a", "apple", 10, -0.5f}, {"b", "apple", 30, -1},
                             {"c", "apple", 20, -3}, {"d", "pear", 5, -2},
                             {"e", "pear", 15, -4}}};
    for (size_t i = 0; i < docs.size(); i++) {
      h.index(flatdoc("id", std::string(docs[i].id), "foo_w", docs[i].text, "p_i", docs[i].p,
                      "embedding_v", std::vector<float>{docs[i].x, 0, 0}));
      if ((i + 1) % (size_t)perSegment == 0 || i + 1 == docs.size()) h.commit();
    }
  }

  // `sources` is the body of the sources object; `extra` is spliced into the
  // fusion object (method, weights).
  LocalReqHandle run(std::string_view sources, std::string_view extra = R"("linear":{})",
                     std::string_view collection = "main") {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(R"({"ops":{"f":{"fusion":{"sources":{)" + std::string(sources)
        + R"(},"get_number":true,"get_scores":true,"fields":["id"],
        "ops":{"ids":{"field_facet":{"field":"id"}}},)" + std::string(extra) + "}}}}",
        req->rawRequest(), req->mr);
    req->collection(collection);
    req->execute();
    return req;
  }

  static std::string join(std::string_view a, std::string_view b) {
    return std::string(a) + "," + std::string(b);
  }

  void expectResult(LocalReq& req, std::vector<std::string> ids, std::vector<float> scores) {
    ASSERT_OK(&req);
    EXPECT_EQ(ids, resultIds(req));
    auto got = resultScores(req);
    ASSERT_EQ(scores.size(), got.size());
    for (size_t i = 0; i < scores.size(); i++) EXPECT_FLOAT_EQ(scores[i], got[i]) << i;
  }
};

TEST_F(LinearFusionTest, overlappingSources) {
  indexCorpus("main", 5);
  for (auto method : {R"("linear":{})", R"("linear":{"normalization":"min_max"})"}) {
    auto req = run(join(LEX, VEC), method);
    expectResult(*req, {"b", "a", "c", "d"}, {1 + 0.8f, 0 + 1, 0.5f + 0, 0.4f});
    EXPECT_EQ(4, req->docList("f")->found.value_or(-1));
    EXPECT_EQ((std::set<std::string_view>{"a", "b", "c", "d"}), facetIds(*req));
  }
}

TEST_F(LinearFusionTest, weights) {
  indexCorpus("main", 5);
  auto req = run(join(LEX, VEC), R"("weights":{"lex":0.25,"vec":2},"linear":{})");
  expectResult(*req, {"a", "b", "d", "c"}, {2, 0.25f + 1.6f, 0.8f, 0.125f});
  // A zero weight disables vec: d leaves the list, found, and the ops domain.
  auto off = run(join(LEX, VEC), R"("weights":{"vec":0},"linear":{})");
  expectResult(*off, {"b", "c", "a"}, {1, 0.5f, 0});
  EXPECT_EQ(3, off->docList("f")->found.value_or(-1));
  EXPECT_EQ((std::set<std::string_view>{"a", "b", "c"}), facetIds(*off));
}

// Missing-source contribution is zero; a document scored 0 by its only
// source is still a candidate.
TEST_F(LinearFusionTest, disjointSources) {
  indexCorpus("main", 5);
  auto pear = R"("pear":{"query":{"rescore":{"query":{"match":{"foo_w":"pear"}},"expr":"p_i"}}})";
  auto req = run(join(LEX, pear), R"("weights":{"pear":0.6},"linear":{})");
  expectResult(*req, {"b", "e", "c", "a", "d"}, {1, 0.6f, 0.5f, 0, 0});
  EXPECT_EQ(5, req->docList("f")->found.value_or(-1));
}

// Constant-score and single-document lists normalize every document to 1;
// an empty list contributes nothing.
TEST_F(LinearFusionTest, constantSingletonAndEmptySources) {
  indexCorpus("main", 5);
  auto constant =
      R"("lex":{"query":{"constant_score":{"query":{"match":{"foo_w":"apple"}},"score":3}}})";
  expectResult(*run(join(constant, VEC)), {"a", "b", "c", "d"}, {2, 1.8f, 1, 0.4f});
  auto single = R"("one":{"query":{"rescore":{"query":{"match":{"foo_w":"pear"}},"expr":"p_i"}},"limit":1})";
  expectResult(*run(single), {"e"}, {1});
  auto empty = run(join(LEX, R"("none":{"query":{"match":{"foo_w":"zzz"}}})"));
  expectResult(*empty, {"b", "c", "a"}, {1, 0.5f, 0});
  EXPECT_EQ(3, empty->docList("f")->found.value_or(-1));
}

// Normalization runs over each source's merged list, so splitting the same
// documents across segments changes neither scores nor order.
TEST_F(LinearFusionTest, segmentPartitioningInvariant) {
  indexCorpus("main", 5);
  indexCorpus("split", 1);
  indexCorpus("pairs", 2);
  auto extra = R"("weights":{"lex":0.25,"vec":2},"linear":{})";
  auto one = run(join(LEX, VEC), extra, "main");
  ASSERT_OK(one);
  for (auto collection : {"split", "pairs"}) {
    auto req = run(join(LEX, VEC), extra, collection);
    ASSERT_OK(req);
    EXPECT_EQ(resultIds(*one), resultIds(*req)) << collection;
    EXPECT_EQ(resultScores(*one), resultScores(*req)) << collection;
  }
}

TEST_F(LinearFusionTest, minMaxNormalizeEdgeCases) {
  auto norm = [](std::vector<float> scores) {
    FusionOp::minMaxNormalize(scores);
    return scores;
  };
  constexpr float inf = std::numeric_limits<float>::infinity();
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  constexpr float big = std::numeric_limits<float>::max();
  EXPECT_EQ((std::vector<float>{1, 0, 0, 1, 0, 0.5f}), norm({3, nan, 1, inf, -inf, 2}));
  EXPECT_EQ((std::vector<float>{1, 1}), norm({7, 7}));
  EXPECT_EQ((std::vector<float>{1}), norm({-5}));
  EXPECT_EQ((std::vector<float>{0, 1, 0}), norm({nan, inf, -inf}));
  EXPECT_EQ((std::vector<float>{0, 1, 0.5f}), norm({-big, big, 0}));
  EXPECT_EQ((std::vector<float>{}), norm({}));
}

TEST_F(LinearFusionTest, validation) {
  indexCorpus("main", 5);
  auto sorted = [](std::string_view sort, std::string_view name = "lex") {
    return "\"" + std::string(name) + R"(":{"query":{"match":{"foo_w":"apple"}},"sort":[)"
        + std::string(sort) + "]}";
  };
  {
    ExpectLog quiet("Search request rejected");
    auto req = run(sorted(R"({"expr":"p_i","dir":"desc"})"));
    EXPECT_NE(std::string::npos, req->errorMsg().find(
        "fusion 'f': linear fusion source 'lex' must rank by score")) << req->errorMsg();
  }
  // Score order, and field sorts under RRF or on a disabled source, are fine.
  auto byScore = run(sorted(R"({"expr":"score"})"));
  ASSERT_OK(byScore);
  auto rrf = run(sorted(R"({"expr":"p_i"})"), R"("rrf":{})");
  ASSERT_OK(rrf);
  auto disabled = run(join(LEX, sorted(R"({"expr":"p_i"})", "srt")),
                      R"("weights":{"srt":0},"linear":{})");
  ASSERT_OK(disabled);
  // Exactly one method.
  {
    ExpectLog quiet("Search request rejected");
    auto req = run(LEX, R"("limit":10)");
    EXPECT_NE(std::string::npos, req->errorMsg().find(
        "fusion 'f': requires a fusion method (rrf or linear)")) << req->errorMsg();
  }
  try {
    run(LEX, R"("rrf":{},"linear":{})");
    ADD_FAILURE() << "a request naming both methods was accepted";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string::npos,
              std::string(e.what()).find("fusion takes exactly one method: rrf or linear"))
        << e.what();
  }
  EXPECT_ANY_THROW(run(LEX, R"("linear":{"normalization":"l2"})"));
  {
    auto req = localReq(luxirNode->getSearchEngine());
    auto& fusion = req->collection("main").topDocs("f").rawOp().kind.emplace<api::Fusion>();
    fusion.method.emplace<api::LinearFusion>().normalization =
        (api::LinearFusion::Normalization)7;
    setTextSource(addSource(fusion, "lex", req->mr), req->mr, "foo_w", "apple", 5);
    ExpectLog quiet("Search request rejected");
    req->execute();
    EXPECT_NE(std::string::npos, req->errorMsg().find(
        "fusion 'f': unsupported LinearFusion.normalization 7")) << req->errorMsg();
  }
}
