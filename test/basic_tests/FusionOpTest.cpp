#include <gtest/gtest.h>

#include <memory>
#include <memory_resource>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "solux/schema/Schema.h"
#include "solux/server/SoluxNode.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

class FusionOpTest : public SoluxTest {
protected:
  void SetUp() override {
    auto col = soluxNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }

  // Override _v with a vector field so the same docs can carry both an
  // analyzed text field (_w) and a dense vector for fusion across both.
  static void installVecSchema(Collection& col, solux::api::VectorMetric metric) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = solux::api::FieldDef_::FieldClass::VECTOR;
    f.column = true;
    f.metric = metric;
    b.set(col);
  }

  // --- arena builders for the Fusion op the OpCursor fluent API doesn't cover ---

  // Copy a by-value Query into the build arena and return a stable pointer for an
  // optional_indirect_view<Query> member to point at.
  static solux::api::Query* arenaQuery(std::pmr::memory_resource& mr, const solux::api::Query& q) {
    auto* p = (solux::api::Query*)mr.allocate(sizeof(solux::api::Query), alignof(solux::api::Query));
    return new (p) solux::api::Query(q);
  }

  // Grow Fusion.sources (map_view<string_view, TopDocs>, TopDocs by value) by one entry and
  // return the fresh slot to fill. Realloc-grow (mirrors LocalReq::appendOp): the prior
  // entries are trivially copyable with their nested data in the arena. The returned
  // reference stays valid until the next addSource() call reallocates the backing array.
  static solux::api::TopDocs& addSource(solux::api::Fusion& fusion, std::string_view name,
                                        std::pmr::memory_resource& mr) {
    using Map = solux::api::map_view<std::string_view, solux::api::TopDocs>;
    using Pair = std::pair<std::string_view, solux::api::TopDocs>;
    auto old = fusion.sources;
    Pair* a = (Pair*)mr.allocate(sizeof(Pair) * (old.size() + 1), alignof(Pair));
    std::uninitialized_value_construct_n(a, old.size() + 1);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()].first = build::arenaStr(mr, name);
    fusion.sources = Map(std::span<const Pair>(a, old.size() + 1));
    return a[old.size()].second;
  }

  // Append one field name to a Fusion.fields span (realloc-grow preserves prior entries).
  static void addField(solux::api::Fusion& fusion, std::string_view field,
                       std::pmr::memory_resource& mr) {
    auto old = fusion.fields;
    std::string_view* a = build::allocArray(fusion.fields, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()] = build::arenaStr(mr, field);
  }

  // Append a named filter (a NamedQuery) to a span<const NamedQuery> (Fusion.filter or
  // TopDocs.filter), pointing the entry at an arena copy of the pre-built query.
  static void addNamedFilter(std::span<const solux::api::NamedQuery>& filter,
                             std::string_view name, const solux::api::Query& q,
                             std::pmr::memory_resource& mr) {
    auto old = filter;
    solux::api::NamedQuery* a = build::allocArray(filter, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()].name = build::arenaStr(mr, name);
    a[old.size()].query = arenaQuery(mr, q);
  }

  // Append a sort spec to a TopDocs source (realloc-grow preserves prior entries).
  static void addSort(solux::api::TopDocs& src, std::string_view expr,
                      solux::api::SortSpec::SortDir dir, std::pmr::memory_resource& mr) {
    auto old = src.sorts;
    solux::api::SortSpec* a = build::allocArray(src.sorts, old.size() + 1, mr);
    for (std::size_t i = 0; i < old.size(); i++) a[i] = old[i];
    a[old.size()].expr = build::arenaStr(mr, expr);
    a[old.size()].dir = dir;
  }

  static void setTextSource(solux::api::TopDocs& src, std::pmr::memory_resource& mr,
                            std::string_view field, std::string_view term, int64_t limit) {
    src.limit = limit;
    src.query = arenaQuery(mr, qb::match(mr, field, term));
  }

  static void setKnnSource(solux::api::TopDocs& src, std::pmr::memory_resource& mr,
                           std::string_view field, std::vector<float> query, int32_t k) {
    src.limit = k;
    src.query = arenaQuery(mr, qb::knn(mr, field, query, k));
  }

  // Set a source's query to a BooleanQuery of several optional kNN clauses, so
  // a single source's Context hosts multiple kNN weights (prepared serially),
  // while many such sources prepare concurrently against the shared request pool.
  static void setBoolKnnSource(solux::api::TopDocs& src, std::pmr::memory_resource& mr,
                               std::string_view field,
                               const std::vector<std::vector<float>>& queryVecs,
                               int32_t k, int64_t limit) {
    src.limit = limit;
    std::vector<solux::api::Query> optional;
    for (const auto& qv : queryVecs) optional.push_back(qb::knn(mr, field, qv, k));
    src.query = arenaQuery(mr, qb::boolean(mr, std::span<const solux::api::Query>{},
                                           std::span<const solux::api::Query>(optional)));
  }

  // Pull "id" out of a Fusion response in fused-rank order.
  static std::vector<std::string> resultIds(LocalReq& req, std::string_view opName = "f") {
    std::vector<std::string> ids;
    const auto* dl = req.docList(opName);
    if (dl == nullptr) return ids;
    const auto* colp = dl->columns.find("id");
    if (colp == nullptr) return ids;
    const auto* col = std::get_if<solux::api::ColStr>(&colp->kind);
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
    const auto* col = std::get_if<solux::api::ColFloat>(&colp->kind);
    if (col == nullptr) return scores;
    for (float v : col->v) scores.push_back(v);
    return scores;
  }
};


// Basic RRF over a text source and a KNN source.  Crafted so each source's
// ranking is unambiguous, so we can assert exact fused scores:
//   - text "apple" matches only "a" (single match -> rank 1).
//   - knn near [1,0,0] under L2 ranks a (d^2=0), b (0.02), c (1.5) at 1..3;
//     d (2.0) falls outside the k=3 cutoff.
TEST_F(FusionOpTest, rrfTextAndKnn) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), solux::api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",  "embedding_v", std::vector<float>{1.0f, 0,    0   }));
  h.index(flatdoc("id", std::string("b"), "foo_w", "orange", "embedding_v", std::vector<float>{0.9f, 0.1f, 0   }));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange", "embedding_v", std::vector<float>{0,    0.5f, 0.5f}));
  h.index(flatdoc("id", std::string("d"), "foo_w", "orange", "embedding_v", std::vector<float>{0,    1.0f, 0   }));
  h.commit({"*"});

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  fusion.get_scores = true;
  addField(fusion, "id", mr);
  fusion.rrf.emplace().k = 60;
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

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  fusion.get_scores = true;
  addField(fusion, "id", mr);
  fusion.rrf.emplace().k = 60;

  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 10);

  addNamedFilter(fusion.filter, "colorRed", qb::match(mr, "color_s", "red"), mr);

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

TEST_F(FusionOpTest, sharedKnnFilter) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), solux::api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "embedding_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple",
                  "embedding_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange",
                  "embedding_v", std::vector<float>{0.0f, 0.9f}));
  h.commit({"*"});

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  addField(fusion, "id", mr);
  fusion.rrf.emplace().k = 60;

  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 10);

  addNamedFilter(fusion.filter, "near", qb::knn(mr, "embedding_v", {0.0f, 1.0f}, 1), mr);

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

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  fusion.get_scores = true;
  addField(fusion, "id", mr);
  fusion.rrf.emplace().k = 60;
  setTextSource(addSource(fusion, "apple", mr), mr, "foo_w", "apple", 10);
  auto& bananaSrc = addSource(fusion, "banana", mr);
  setTextSource(bananaSrc, mr, "foo_w", "banana", 10);
  addSort(bananaSrc, "prio_i", solux::api::SortSpec::SortDir::DESC, mr);

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

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 10;
  fusion.get_number = true;
  addField(fusion, "id", mr);
  fusion.rrf.emplace().k = 60;

  // Shared filter: color_s = red.
  addNamedFilter(fusion.filter, "colorRed", qb::match(mr, "color_s", "red"), mr);

  // Per-source filter: owner_s = alice (applied to the text source only).
  auto& textSrc = addSource(fusion, "text", mr);
  setTextSource(textSrc, mr, "foo_w", "apple", 10);
  addNamedFilter(textSrc.filter, "ownerAlice", qb::match(mr, "owner_s", "alice"), mr);

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


// Validation: missing required pieces and unsupported sub-ops surface as
// errors in the response.
TEST_F(FusionOpTest, validation) {
  CollectionHelper h("main");
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple"), UpdateMessage::COMMIT);

  auto buildBase = [&]() {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->collection("main");
    return lreq;
  };

  // No sources.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
    fusion.rrf.emplace().k = 60;
    ExpectLog quiet("Search request failed:");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("source"), std::string::npos);
    lreq->done();
  }

  // No method.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
    auto& mr = lreq->mr;
    setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);
    ExpectLog quiet("Search request failed:");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("method"), std::string::npos);
    lreq->done();
  }

  // Negative k.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
    auto& mr = lreq->mr;
    fusion.rrf.emplace().k = -1;
    setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);
    ExpectLog quiet("Search request failed:");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("k"), std::string::npos);
    lreq->done();
  }

  // Sub-ops not supported.
  {
    auto* lreq = buildBase();
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
    auto& mr = lreq->mr;
    fusion.rrf.emplace().k = 60;
    setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);
    auto* sub = build::mapSlot<solux::api::SearchOp>(fusion.ops, 1, "facet", mr);
    sub->kind.emplace<solux::api::FieldFacet>().field = build::arenaStr(mr, "color_s");
    ExpectLog quiet("Search request failed:");
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->errorMsg().find("sub-ops"), std::string::npos);
    lreq->done();
  }
}


// Empty index: no segments, no per-source tasks ever run.  The empty-index
// branch should still emit a well-formed (empty) DocList.
TEST_F(FusionOpTest, emptyIndex) {
  CollectionHelper h("main");

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->collection("main");
  auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
  auto& mr = lreq->mr;
  fusion.limit = 5;
  fusion.get_number = true;
  fusion.rrf.emplace().k = 60;
  setTextSource(addSource(fusion, "text", mr), mr, "foo_w", "apple", 5);

  lreq->execute();
  ASSERT_OK(lreq);

  const auto* dl = lreq->docList("f");
  ASSERT_TRUE(dl != nullptr);
  EXPECT_EQ(dl->found.value_or(0), 0);

  lreq->done();
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
  installVecSchema(h.collection(), solux::api::VectorMetric::L2);

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
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->collection("main");
    auto& fusion = lreq->topDocs("f").rawOp().kind.emplace<solux::api::Fusion>();
    auto& mr = lreq->mr;
    fusion.limit = -1;
    fusion.get_number = true;
    addField(fusion, "id", mr);
    fusion.rrf.emplace().k = 60;
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
