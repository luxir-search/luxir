#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "protos/solux_types.pb.h"
#include "solux/schema/Schema.h"
#include "solux/server/SoluxNode.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
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
  static void installVecSchema(Collection& col, proto::VectorParams::Metric metric) {
    proto::SchemaDef def;
    auto* f = def.add_fields();
    f->set_name("_v");
    f->set_field_class(proto::FieldDef::VECTOR);
    f->set_abstract(true);
    f->set_column_stored(true);
    f->mutable_vector()->set_metric(metric);
    auto base = col.getSchema();
    col.setSchema(Schema::fromProto(def, base.get()));
  }

  static void setTextSource(proto::TopDocs& src, std::string_view field,
                            std::string_view term, int64_t limit) {
    src.set_limit(limit);
    auto& m = *src.mutable_query()->mutable_match();
    m.set_field(field);
    m.mutable_val()->set_s(term);
  }

  static void setKnnQuery(proto::Query& query, std::string_view field,
                          const std::vector<float>& queryVec, int32_t k) {
    auto& knn = *query.mutable_knn();
    knn.set_field(field);
    knn.set_k(k);
    auto& f32 = *knn.mutable_query()->mutable_f32();
    for (float v : queryVec) f32.add_v(v);
  }

  static void setKnnSource(proto::TopDocs& src, std::string_view field,
                           std::vector<float> query, int32_t k) {
    src.set_limit(k);
    setKnnQuery(*src.mutable_query(), field, query, k);
  }

  // Set a source's query to a BooleanQuery of several optional kNN clauses, so
  // a single source's Context hosts multiple kNN weights (prepared serially),
  // while many such sources prepare concurrently against the shared request pool.
  static void setBoolKnnSource(proto::TopDocs& src, std::string_view field,
                               const std::vector<std::vector<float>>& queryVecs,
                               int32_t k, int64_t limit) {
    src.set_limit(limit);
    auto& boolean = *src.mutable_query()->mutable_boolean();
    for (const auto& qv : queryVecs) {
      setKnnQuery(*boolean.add_optional(), field, qv, k);
    }
  }

  // Pull "id" out of a Fusion response in fused-rank order.
  static std::vector<std::string> resultIds(LocalReq& req, std::string_view opName = "f") {
    std::vector<std::string> ids;
    if (req.responses.empty()) return ids;
    auto it = req.responses[0]->proto.ops().find(std::string(opName));
    if (it == req.responses[0]->proto.ops().end() || !it->second.has_docs()) return ids;
    const auto& cols = it->second.docs().columns();
    auto idIt = cols.find("id");
    if (idIt == cols.end()) return ids;
    for (const auto& s : idIt->second.col_s().v()) ids.emplace_back(s);
    return ids;
  }

  static std::vector<float> resultScores(LocalReq& req, std::string_view opName = "f") {
    std::vector<float> scores;
    if (req.responses.empty()) return scores;
    auto it = req.responses[0]->proto.ops().find(std::string(opName));
    if (it == req.responses[0]->proto.ops().end() || !it->second.has_docs()) return scores;
    const auto& cols = it->second.docs().columns();
    auto sIt = cols.find("_score_");
    if (sIt == cols.end()) return scores;
    for (float v : sIt->second.col_f().v()) scores.push_back(v);
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
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",  "embedding_v", std::vector<float>{1.0f, 0,    0   }));
  h.index(flatdoc("id", std::string("b"), "foo_w", "orange", "embedding_v", std::vector<float>{0.9f, 0.1f, 0   }));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange", "embedding_v", std::vector<float>{0,    0.5f, 0.5f}));
  h.index(flatdoc("id", std::string("d"), "foo_w", "orange", "embedding_v", std::vector<float>{0,    1.0f, 0   }));
  h.commit({"*"});

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
  fusion.set_limit(10);
  fusion.set_get_number(true);
  fusion.set_get_scores(true);
  fusion.mutable_fields()->Add("id");
  fusion.mutable_rrf()->set_k(60);
  setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 5);
  setKnnSource((*fusion.mutable_sources())["vec"], "embedding_v", {1, 0, 0}, 3);

  lreq->execute();

  // Union: a (text+knn), b (knn), c (knn).  d is excluded by k=3.
  auto matches = lreq->responses[0]->proto.ops().at("f").docs().matches();
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
  h.clear();

  // All three match "apple".  After the color=red filter, only a and c
  // remain; a is shorter so BM25 ranks it first.
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",       "color_s", "red"));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple x",     "color_s", "blue"));
  h.index(flatdoc("id", std::string("c"), "foo_w", "apple x y",   "color_s", "red"));
  h.commit();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
  fusion.set_limit(10);
  fusion.set_get_number(true);
  fusion.set_get_scores(true);
  fusion.mutable_fields()->Add("id");
  fusion.mutable_rrf()->set_k(60);

  setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 10);

  auto& nf = *fusion.add_filter();
  nf.set_name("colorRed");
  auto& m = *nf.mutable_query()->mutable_match();
  m.set_field("color_s");
  m.mutable_val()->set_s("red");

  lreq->execute();

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
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "embedding_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple",
                  "embedding_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange",
                  "embedding_v", std::vector<float>{0.0f, 0.9f}));
  h.commit({"*"});

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
  fusion.set_limit(10);
  fusion.set_get_number(true);
  fusion.mutable_fields()->Add("id");
  fusion.mutable_rrf()->set_k(60);

  setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 10);

  auto& nf = *fusion.add_filter();
  nf.set_name("near");
  setKnnQuery(*nf.mutable_query(), "embedding_v", {0.0f, 1.0f}, 1);

  lreq->execute();

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
  h.clear();

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
  lreq->proto.mutable_collection()->add_name("main");
  auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
  fusion.set_limit(10);
  fusion.set_get_number(true);
  fusion.set_get_scores(true);
  fusion.mutable_fields()->Add("id");
  fusion.mutable_rrf()->set_k(60);
  setTextSource((*fusion.mutable_sources())["apple"],  "foo_w", "apple",  10);
  auto& bananaSrc = (*fusion.mutable_sources())["banana"];
  setTextSource(bananaSrc, "foo_w", "banana", 10);
  auto* bananaSort = bananaSrc.add_sorts();
  bananaSort->set_field("prio_i");
  bananaSort->set_dir(proto::SortSpec::DESC);

  lreq->execute();

  // Union: a, b, c, d, e.
  auto matches = lreq->responses[0]->proto.ops().at("f").docs().matches();
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
  h.clear();

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
  lreq->proto.mutable_collection()->add_name("main");
  auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
  fusion.set_limit(10);
  fusion.set_get_number(true);
  fusion.mutable_fields()->Add("id");
  fusion.mutable_rrf()->set_k(60);

  // Shared filter: color_s = red.
  {
    auto& nf = *fusion.add_filter();
    nf.set_name("colorRed");
    auto& m = *nf.mutable_query()->mutable_match();
    m.set_field("color_s");
    m.mutable_val()->set_s("red");
  }

  // Per-source filter: owner_s = alice (applied to the text source only).
  auto& textSrc = (*fusion.mutable_sources())["text"];
  setTextSource(textSrc, "foo_w", "apple", 10);
  {
    auto& nf = *textSrc.add_filter();
    nf.set_name("ownerAlice");
    auto& m = *nf.mutable_query()->mutable_match();
    m.set_field("owner_s");
    m.mutable_val()->set_s("alice");
  }

  lreq->execute();

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
  h.clear();
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple"), UpdateMessage::COMMIT);

  auto buildBase = [&]() {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    return lreq;
  };

  // No sources.
  {
    auto* lreq = buildBase();
    auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
    fusion.mutable_rrf()->set_k(60);
    LogLevelGuard quiet;
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->responses[0]->proto.error().find("source"), std::string::npos);
    lreq->done();
  }

  // No method.
  {
    auto* lreq = buildBase();
    auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
    setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 5);
    LogLevelGuard quiet;
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->responses[0]->proto.error().find("method"), std::string::npos);
    lreq->done();
  }

  // Negative k.
  {
    auto* lreq = buildBase();
    auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
    fusion.mutable_rrf()->set_k(-1);
    setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 5);
    LogLevelGuard quiet;
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->responses[0]->proto.error().find("k"), std::string::npos);
    lreq->done();
  }

  // Sub-ops not supported.
  {
    auto* lreq = buildBase();
    auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
    fusion.mutable_rrf()->set_k(60);
    setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 5);
    auto& sub = (*fusion.mutable_ops())["facet"];
    sub.mutable_field_facet()->set_field("color_s");
    LogLevelGuard quiet;
    lreq->execute();
    ASSERT_FALSE(lreq->responses.empty());
    EXPECT_NE(lreq->responses[0]->proto.error().find("sub-ops"), std::string::npos);
    lreq->done();
  }
}


// Empty index: no segments, no per-source tasks ever run.  The empty-index
// branch should still emit a well-formed (empty) DocList.
TEST_F(FusionOpTest, emptyIndex) {
  CollectionHelper h("main");
  h.clear();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
  fusion.set_limit(5);
  fusion.set_get_number(true);
  fusion.mutable_rrf()->set_k(60);
  setTextSource((*fusion.mutable_sources())["text"], "foo_w", "apple", 5);

  lreq->execute();

  ASSERT_FALSE(lreq->responses.empty());
  auto& opVal = lreq->responses[0]->proto.ops().at("f");
  ASSERT_TRUE(opVal.has_docs());
  EXPECT_EQ(opVal.docs().matches(), 0);

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
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

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
    lreq->proto.mutable_collection()->add_name("main");
    auto& fusion = *(*lreq->proto.mutable_ops())["f"].mutable_fusion();
    fusion.set_limit(-1);
    fusion.set_get_number(true);
    fusion.mutable_fields()->Add("id");
    fusion.mutable_rrf()->set_k(60);
    // 10 sources, each a boolean of 2 optional kNN clauses => 20 kNN weights,
    // 10 source prepares running concurrently on the shared request pool.
    for (int s = 0; s < 10; s++) {
      float a = (float)s / 10.0f;
      setBoolKnnSource((*fusion.mutable_sources())["vec" + std::to_string(s)],
                       "embedding_v", {{a, 1.0f - a}, {1.0f - a, a}}, 5, -1);
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
    baseline = resultIdSet(*lreq);
    lreq->done();
  }
  ASSERT_FALSE(baseline.empty());

  for (int iter = 0; iter < 50; iter++) {
    auto* lreq = buildReq();
    lreq->execute();
    auto ids = resultIdSet(*lreq);
    ASSERT_EQ(ids, baseline) << "divergent/garbage fused result set at iteration " << iter;
    lreq->done();
  }
}
