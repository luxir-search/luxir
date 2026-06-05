#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "protos/solux_types.pb.h"
#include "solux/index/VectorIndexBuilder.h"
#include "solux/query/KnnQuery.h"
#include "solux/query/VectorEngine.h"
#include "solux/schema/Schema.h"
#include "solux/server/SoluxNode.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

// Test engines wrapping the production flat engine via
// KnnQuery::engineWrapperForTests.  Real VectorEngine implementations (scores
// come from real searches over the real index), reshaped into result patterns
// future engines will produce, so the host deepen loop's union / rescore /
// sort / collapse logic is exercised without an IVF build.

// IVF-flat shape: exact scores, breadth-limited search.  Splits the index into
// two "lists" by valueRank (< split -> list 0); breadth 0 or 1 probes list 0
// only, breadth >= 2 probes both.  A breadth round can therefore surface hits
// that interleave in score with candidates absorbed earlier.  Single-segment
// only (valueRank is used as the shard-global rank).
class BreadthSplitEngine final : public VectorEngine {
  VectorEngine& inner;
  int64_t ntotal;
  int64_t split;

public:
  BreadthSplitEngine(VectorEngine& inner, int64_t ntotal, int64_t split) noexcept
    : inner(inner), ntotal(ntotal), split(split) {}

  VectorSearchResult search(const VectorSearchRequest& request) override {
    VectorSearchRequest full = request;
    full.candidates = ntotal;
    VectorSearchResult all = inner.search(full);  // exact, score-desc, domain-filtered

    int32_t probedLists = request.breadth <= 1 ? 1 : 2;
    VectorSearchResult result;
    int64_t regionLive = 0;
    for (const auto& hit : all.hits) {
      if (probedLists < 2 && hit.valueRank >= split) continue;
      regionLive++;
      if ((int64_t)result.hits.size() < request.candidates) result.hits.push_back(hit);
    }
    result.scoresAreExact = true;
    result.poolExhausted = (int64_t)result.hits.size() < request.candidates
                           || request.candidates >= regionLive;
    result.breadthExhausted = probedLists >= 2;
    result.nextBreadth = 2;
    return result;
  }
};

// PQ shape: approximate scores.  Quantizes each exact score down to a coarse
// half-unit bucket and inverts the order within each bucket (ascending true
// score), then marks the result approximate.  Any host path that trusts the
// engine order or skips the full-precision column rescore returns wrong
// docs / scores.
class QuantizingEngine final : public VectorEngine {
  VectorEngine& inner;

public:
  explicit QuantizingEngine(VectorEngine& inner) noexcept : inner(inner) {}

  VectorSearchResult search(const VectorSearchRequest& request) override {
    VectorSearchResult result = inner.search(request);
    for (auto& hit : result.hits) {
      hit.score = std::floor(hit.score * 2.0f) / 2.0f;
    }
    // Reverse then stable-sort by bucket: still engine-score-desc as the
    // contract requires, but ascending-by-exact-score within each bucket.
    std::reverse(result.hits.begin(), result.hits.end());
    std::stable_sort(result.hits.begin(), result.hits.end(),
                     [](const VectorEngineHit& a, const VectorEngineHit& b) {
                       return a.score > b.score;
                     });
    result.scoresAreExact = false;
    return result;
  }
};

class KnnQueryTest : public SoluxTest {
protected:
  struct MaxKnnCandidatesGuard {
    int64_t saved;
    explicit MaxKnnCandidatesGuard(int64_t value) : saved(KnnQuery::maxKnnCandidates) {
      KnnQuery::maxKnnCandidates = value;
    }
    ~MaxKnnCandidatesGuard() {
      KnnQuery::maxKnnCandidates = saved;
    }
  };

  struct EngineWrapperGuard {
    explicit EngineWrapperGuard(
        std::function<std::unique_ptr<VectorEngine>(VectorEngine&, int64_t)> f) {
      KnnQuery::engineWrapperForTests = std::move(f);
    }
    ~EngineWrapperGuard() {
      KnnQuery::engineWrapperForTests = nullptr;
    }
  };

  struct FaissFlatAuxGuard {
    explicit FaissFlatAuxGuard() {
      VectorIndexBuilder::buildFaissFlatAuxIndexes = true;
    }
    ~FaissFlatAuxGuard() {
      VectorIndexBuilder::buildFaissFlatAuxIndexes = false;
    }
  };

  void SetUp() override {
    auto col = soluxNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }

  // Install a schema where _v is a vector field with the given metric.
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

  // Install a schema where _v is a COSINE vector field that stores RAW
  // (unnormalized) vectors in the column - the normalize-on-rescore mode.
  static void installVecSchemaCosineRaw(Collection& col) {
    proto::SchemaDef def;
    auto* f = def.add_fields();
    f->set_name("_v");
    f->set_field_class(proto::FieldDef::VECTOR);
    f->set_abstract(true);
    f->set_column_stored(true);
    f->mutable_vector()->set_metric(proto::VectorParams::COSINE);
    f->mutable_vector()->set_normalize_on_write(false);
    auto base = col.getSchema();
    col.setSchema(Schema::fromProto(def, base.get()));
  }

  // Install a schema where _vs is a multi-valued vector field with the given metric.
  static void installMultiVecSchema(Collection& col, proto::VectorParams::Metric metric) {
    proto::SchemaDef def;
    auto* f = def.add_fields();
    f->set_name("_vs");
    f->set_field_class(proto::FieldDef::VECTOR);
    f->set_abstract(true);
    f->set_column_stored(true);
    f->set_multi_valued(true);
    f->mutable_vector()->set_metric(metric);
    auto base = col.getSchema();
    col.setSchema(Schema::fromProto(def, base.get()));
  }

  // Build a TopDocs request with a KNN query for the given field + query
  // vector + k.  Always pulls back "id" so tests can assert ordering.
  static LocalReq* makeKnnReq(SoluxNode& node, std::string_view field,
                              std::vector<float> queryVec, int32_t k) {
    auto* lreq = LocalReq::create(node.getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
    topDocs.set_get_scores(true);
    topDocs.set_get_number(true);
    topDocs.mutable_fields()->Add("id");

    setKnnQuery(*topDocs.mutable_query(), field, queryVec, k);
    return lreq;
  }

  static void setKnnQuery(proto::Query& query, std::string_view field,
                          const std::vector<float>& queryVec, int32_t k) {
    auto& knn = *query.mutable_knn();
    knn.set_field(field);
    knn.set_k(k);
    auto& f32 = *knn.mutable_query()->mutable_f32();
    for (float v : queryVec) f32.add_v(v);
  }

  // Pull "id" out of a search response in result order.
  static std::vector<std::string> resultIds(LocalReq& req, std::string_view opName = "q") {
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

  // Pull "_score_" out of a search response in result order (parallel to
  // resultIds).  Empty if the request didn't set get_scores.
  static std::vector<float> resultScores(LocalReq& req, std::string_view opName = "q") {
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

// Basic ordering: query nearest the first stored vector -> that doc lands at
// the top, scores monotonically decreasing for further-away docs.
//
// Interleaves docs *without* the vector field so the column is sparse -
// exercises valueRank -> docRank lookup (without the lookup, the FAISS-id of
// the second vector would map to docRank 1, which is a no-vector doc).
TEST_F(KnnQueryTest, basicOrdering) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // Mix of vector docs ("a","b","c","d") and bare-id docs ("g1","g2","g3").
  // Doc-rank order: a, g1, b, g2, c, g3, d.  Value-rank order (ranks of docs
  // with vectors): a=0, b=1, c=2, d=3.  If the code mistakenly used valueRank
  // as docRank, it would return ids "a","b","c" - which happen to be wrong
  // (the doc at rank 1 is "g1", not "b").
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0, 0}));
  h.index(flatdoc("id", std::string("g1")));
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0, 1, 0, 0}));
  h.index(flatdoc("id", std::string("g2")));
  h.index(flatdoc("id", std::string("c"), "embedding_v", std::vector<float>{0, 0, 1, 0}));
  h.index(flatdoc("id", std::string("g3")));
  h.index(flatdoc("id", std::string("d"), "embedding_v", std::vector<float>{0, 0, 0, 1}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1, 0, 0, 0}, 3);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "a");  // exact match - distance 0
  // The other three vector docs are equidistant (d^2 = 2 each).  Just check
  // they're all from the vector set, not a no-vector doc.
  std::set<std::string> withVec{"a", "b", "c", "d"};
  for (const auto& id : ids) {
    EXPECT_TRUE(withVec.count(id)) << "result '" << id << "' must be a vector doc";
  }

  req->done();
}

// Multi-segment: each segment contributes its share to the top-K, and the
// FAISS-id -> (segment, docRank) mapping is correct.  Interleaves no-vector
// docs in seg0 and seg1 so the per-segment columns are sparse - surfaces
// off-by-one if the valueRank -> docRank lookup is wrong on a per-segment
// basis (different segments have different sparse layouts).
TEST_F(KnnQueryTest, multiSegment) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // Segment 0: vec, no-vec, vec.  s0_a at docRank 0, s0_b at docRank 2.
  h.index(flatdoc("id", std::string("s0_a"), "embedding_v", std::vector<float>{1.0f, 0, 0}));
  h.index(flatdoc("id", std::string("s0_g")));
  h.index(flatdoc("id", std::string("s0_b"), "embedding_v", std::vector<float>{0.9f, 0.1f, 0}));
  h.commit();

  // Segment 1: no-vec, vec, vec.  s1_a at docRank 1, s1_b at docRank 2.
  h.index(flatdoc("id", std::string("s1_g")));
  h.index(flatdoc("id", std::string("s1_a"), "embedding_v", std::vector<float>{0, 1.0f, 0}));
  h.index(flatdoc("id", std::string("s1_b"), "embedding_v", std::vector<float>{0.1f, 0.9f, 0}));
  h.commit();

  // Segment 2: dense (only vector docs).
  h.index(flatdoc("id", std::string("s2_a"), "embedding_v", std::vector<float>{0, 0, 1.0f}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1, 0, 0}, 3);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  // The two closest are seg0's docs; third is the seg1 [0.1, 0.9, 0] (closer
  // than [0, 1, 0] or seg2's vector).  We assert the set rather than order
  // since within-segment results land in docId order, not score order, in
  // the response (TopDocsReq sorts by score globally - verify "s0_a" is #1).
  EXPECT_EQ(ids[0], "s0_a");
  std::set<std::string> got(ids.begin(), ids.end());
  EXPECT_TRUE(got.count("s0_a"));
  EXPECT_TRUE(got.count("s0_b"));
  EXPECT_TRUE(got.count("s1_b"));

  req->done();
}

// Multi-valued: a doc owns several vectors; FAISS hits must be grouped back to
// the owning doc (one hit per doc, best score) via the valueRank->docId column.
// An interleaved no-vector doc ("g") makes identity (valueRank==docId) resolve a
// vector to the wrong doc, so its absence from the results proves the reverse map
// is used.  A delete phase then exercises the multi-valued is_member resolve path.
TEST_F(KnnQueryTest, multiValuedGrouping) {
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  // docRank 0: "a" owns the two closest chunks to the query.
  // docRank 1: "g" has no vector (forces real-docId resolution).
  // docRank 2: "b" owns one far chunk.
  // value ranks -> docId: 0->0(a), 1->0(a), 2->2(b).  Identity would mis-map
  // value rank 1 to docId 1 ("g").
  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}}));
  h.index(flatdoc("id", std::string("g")));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0, 1, 0}}));
  h.commit({"*"});

  // Only two live docs have vectors, so k=3 returns both distinct docs after
  // collapsing "a"'s two top chunks into one hit.
  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req->execute();
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 2u) << "a's two top vectors must collapse to one hit";
  EXPECT_EQ(ids[0], "a");  // best chunk is the exact match
  EXPECT_EQ(ids[1], "b");
  for (const auto& id : ids)
    EXPECT_NE(id, "g") << "no-vector doc must not appear (reverse map mis-resolved?)";
  req->done();

  // Delete "a" (owns the top vectors): its vectors must be filtered inside FAISS
  // via the map + liveDocs, leaving only "b".
  std::vector<std::string> del{"a"};
  h.deleteByIds(del, UpdateMessage::COMMIT);

  auto* req2 = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req2->execute();
  auto ids2 = resultIds(*req2);
  ASSERT_EQ(ids2.size(), 1u);
  EXPECT_EQ(ids2[0], "b");
  req2->done();
}

TEST_F(KnnQueryTest, multiValuedGuaranteesKDocs) {
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  // "a" owns the top 3 vectors.  A k-vectors implementation returns only "a";
  // over-requesting should continue far enough to fill k distinct docs.
  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");

  req->done();
}

TEST_F(KnnQueryTest, multiValuedKExceedsDistinctDocs) {
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 10);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 4);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");
  EXPECT_EQ(ids[3], "d");

  req->done();
}

TEST_F(KnnQueryTest, multiValuedFillsAfterDeletingTopDoc) {
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  std::vector<std::string> del{"a"};
  h.deleteByIds(del, UpdateMessage::COMMIT);

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "b");
  EXPECT_EQ(ids[1], "c");
  EXPECT_EQ(ids[2], "d");

  req->done();
}

TEST_F(KnnQueryTest, multiValuedCandidateCapIsBestEffort) {
  MaxKnnCandidatesGuard guard(3);
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 4);
  {
    LogLevelGuard quiet;  // expected: shortfall at the test candidate cap
    req->execute();
  }

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");

  req->done();
}

// Multi-valued across multiple segments: each segment resolves its own value
// ranks via its own valueRank->docId map, combined with the per-segment FAISS-id
// prefix, and hits are grouped across segments.  A no-vector doc in each segment
// makes identity resolution land on the wrong doc, so correct ids prove the
// per-segment maps are applied (not valueRank-as-docId).
TEST_F(KnnQueryTest, multiValuedMultiSegment) {
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  // Segment 0 (docRanks): s0_a[0] 2 vecs, s0_g[1] none, s0_b[2] 1 vec.
  h.index(flatdoc("id", std::string("s0_a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0, 1, 0}}));
  h.index(flatdoc("id", std::string("s0_g")));
  h.index(flatdoc("id", std::string("s0_b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.8f, 0, 0}}));
  h.commit();

  // Segment 1 (docRanks): s1_g[0] none, s1_a[1] 1 vec, s1_b[2] 2 vecs.
  // s1_a's only vector is value rank 0 in seg1 -> must resolve to docId 1, not 0.
  h.index(flatdoc("id", std::string("s1_g")));
  h.index(flatdoc("id", std::string("s1_a"), "emb_vs",
                  std::vector<std::vector<float>>{{0.9f, 0, 0}}));
  h.index(flatdoc("id", std::string("s1_b"), "emb_vs",
                  std::vector<std::vector<float>>{{0, 0, 1}, {0, 0, 0.9f}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req->execute();
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  // closest chunks: s0_a{1,0,0}=0, s1_a{0.9,0,0}=.01, s0_b{0.8,0,0}=.04
  EXPECT_EQ(ids[0], "s0_a");
  EXPECT_EQ(ids[1], "s1_a");
  EXPECT_EQ(ids[2], "s0_b");
  std::set<std::string> got(ids.begin(), ids.end());
  EXPECT_FALSE(got.count("s0_g")) << "no-vector doc must not appear";
  EXPECT_FALSE(got.count("s1_g")) << "no-vector doc must not appear";
  req->done();
}

// kNN still works after a segment merge: mergeStrCol must have regenerated the
// valueRank->docId map against the merged (remapped) doc ids, and the rebuilt
// FAISS aux + query resolve hits to the correct merged docs.
TEST_F(KnnQueryTest, multiValuedSurvivesMerge) {
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  // Segment 0: a (2 vecs), b (1 vec).
  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0, 1, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.8f, 0, 0}}));
  h.commit();

  // Segment 1: c (1 vec), d (2 vecs).
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.9f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0, 0, 1}, {0, 0, 0.5f}}));
  h.commit();

  // Force-merge, then rebuild the FAISS aux over the merged segment.  Merged
  // docRanks: a=0, b=1, c=2, d=3; value ranks 0,1->a, 2->b, 3->c, 4,5->d.
  // (Identity would mis-map value rank 2->doc 2 (c) and 3->doc 3 (d).)
  h.getIndexWriter()->mergeSegments();
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req->execute();
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "a");  // {1,0,0} exact
  EXPECT_EQ(ids[1], "c");  // {0.9,0,0}
  EXPECT_EQ(ids[2], "b");  // {0.8,0,0}
  req->done();
}

// Deletes: deleted docs should not show up, even though FAISS still has
// their vectors.  liveDocs filtering happens inside FAISS via IDSelector.
//
// Mixes in a no-vector doc ("g") so the column is sparse.  Without the
// valueRank -> docRank lookup, IDSelector::is_member would test the wrong
// docRank against liveDocs and either filter the wrong doc or fail to filter
// the deleted one.
TEST_F(KnnQueryTest, filtersDeletedDocs) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // Layout (docRank -> id, vector):
  //   0 -> a [1, 0, 0]
  //   1 -> g (no vector)
  //   2 -> b [0.9, 0.1, 0]
  //   3 -> c [0, 1, 0]
  //   4 -> d [0, 0, 1]
  // Delete docRank 0 (=a).  If IDSelector mistakenly used valueRank as
  // docRank, deleting "a" would be checked against docRank 0 (correct here
  // by coincidence), but deleting "b" would check docRank 1 (=g, irrelevant)
  // instead of docRank 2.  Test for "b" deletion below would surface that.
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", std::string("g")));
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{0.9f, 0.1f, 0}));
  h.index(flatdoc("id", std::string("c"), "embedding_v", std::vector<float>{0, 1, 0}));
  h.index(flatdoc("id", std::string("d"), "embedding_v", std::vector<float>{0, 0, 1}));
  h.commit({"*"});

  // Delete "b" specifically - at docRank 2 (different from valueRank 1).
  std::vector<std::string> ids{"b"};
  h.deleteByIds(ids, UpdateMessage::COMMIT);

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1, 0, 0}, 2);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 2);
  auto resIds = resultIds(*req);
  ASSERT_EQ(resIds.size(), 2u);
  for (const auto& id : resIds) {
    EXPECT_NE(id, "b") << "deleted doc should not appear in results";
    EXPECT_NE(id, "g") << "no-vector doc should not appear in vector results";
  }
  // Top hit is "a" (exact match); second is "c" (next-closest live).
  EXPECT_EQ(resIds[0], "a");
  EXPECT_EQ(resIds[1], "c");

  req->done();
}

TEST_F(KnnQueryTest, topDocsFilterConstrainsKnnSearch) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("blue_a"), "color_s", "blue",
                  "embedding_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("blue_b"), "color_s", "blue",
                  "embedding_v", std::vector<float>{0.99f, 0.0f}));
  h.index(flatdoc("id", std::string("red_a"), "color_s", "red",
                  "embedding_v", std::vector<float>{0.8f, 0.0f}));
  h.index(flatdoc("id", std::string("red_b"), "color_s", "red",
                  "embedding_v", std::vector<float>{0.7f, 0.0f}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1.0f, 0.0f}, 2);
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  auto& nf = *topDocs.add_filter();
  nf.set_name("red");
  auto& m = *nf.mutable_query()->mutable_match();
  m.set_field("color_s");
  m.mutable_val()->set_s("red");

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 2);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "red_a");
  EXPECT_EQ(ids[1], "red_b");

  req->done();
}

TEST_F(KnnQueryTest, booleanOptionalKnnsCanFacet) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "group_s", "left",
                  "title_v", std::vector<float>{1.0f, 0.0f},
                  "body_v", std::vector<float>{0.2f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "group_s", "right",
                  "title_v", std::vector<float>{0.0f, 0.2f},
                  "body_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"), "group_s", "other",
                  "title_v", std::vector<float>{0.3f, 0.3f},
                  "body_v", std::vector<float>{0.3f, 0.3f}));
  h.commit({"*"});

  auto* req = LocalReq::create(soluxNode->getSearchEngine());
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_limit(10);
  topDocs.set_get_number(true);
  topDocs.mutable_fields()->Add("id");

  auto& boolean = *topDocs.mutable_query()->mutable_boolean();
  setKnnQuery(*boolean.add_optional(), "title_v", {1.0f, 0.0f}, 1);
  setKnnQuery(*boolean.add_optional(), "body_v", {0.0f, 1.0f}, 1);

  auto& facet = *(*topDocs.mutable_ops())["groups"].mutable_field_facet();
  facet.set_field("group_s");
  facet.set_limit(-1);

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 2);
  auto ids = resultIds(*req);
  std::set<std::string> got(ids.begin(), ids.end());
  EXPECT_TRUE(got.count("a"));
  EXPECT_TRUE(got.count("b"));
  EXPECT_FALSE(got.count("c"));

  const auto& docs = req->responses[0]->proto.ops().at("q").docs();
  ASSERT_TRUE(docs.ops().contains("groups"));
  const auto& facetResult = docs.ops().at("groups").facet();
  std::map<std::string, int64_t> counts;
  for (int i = 0; i < facetResult.counts_size(); i++) {
    counts[std::string(facetResult.bucket_ids().col_s().v(i))] = facetResult.counts(i);
  }
  ASSERT_EQ(counts.size(), 2u);
  EXPECT_EQ(counts["left"], 1);
  EXPECT_EQ(counts["right"], 1);

  req->done();
}

TEST_F(KnnQueryTest, booleanKnnFilterConstrainsRequiredKnn) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"),
                  "title_v", std::vector<float>{1.0f, 0.0f},
                  "body_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"),
                  "title_v", std::vector<float>{0.8f, 0.0f},
                  "body_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"),
                  "title_v", std::vector<float>{0.0f, 1.0f},
                  "body_v", std::vector<float>{0.5f, 0.5f}));
  h.commit({"*"});

  auto* req = LocalReq::create(soluxNode->getSearchEngine());
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_limit(10);
  topDocs.set_get_number(true);
  topDocs.mutable_fields()->Add("id");

  auto& boolean = *topDocs.mutable_query()->mutable_boolean();
  setKnnQuery(*boolean.add_required(), "title_v", {1.0f, 0.0f}, 1);
  setKnnQuery(*boolean.add_filter(), "body_v", {0.0f, 1.0f}, 1);

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "b");

  req->done();
}

TEST_F(KnnQueryTest, booleanTermRequiredUsesKnnFilter) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "body_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple",
                  "body_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange",
                  "body_v", std::vector<float>{0.0f, 0.9f}));
  h.commit({"*"});

  auto* req = LocalReq::create(soluxNode->getSearchEngine());
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_limit(10);
  topDocs.set_get_number(true);
  topDocs.mutable_fields()->Add("id");

  auto& boolean = *topDocs.mutable_query()->mutable_boolean();
  auto& required = *boolean.add_required();
  auto& match = *required.mutable_match();
  match.set_field("foo_w");
  match.mutable_val()->set_s("apple");
  setKnnQuery(*boolean.add_filter(), "body_v", {0.0f, 1.0f}, 1);

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "b");

  req->done();
}

TEST_F(KnnQueryTest, booleanDisjointRequiredAndKnnFilterReturnsEmpty) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "body_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "orange",
                  "body_v", std::vector<float>{0.0f, 1.0f}));
  h.commit({"*"});

  auto* req = LocalReq::create(soluxNode->getSearchEngine());
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_limit(10);
  topDocs.set_get_number(true);
  topDocs.mutable_fields()->Add("id");

  auto& boolean = *topDocs.mutable_query()->mutable_boolean();
  auto& required = *boolean.add_required();
  auto& match = *required.mutable_match();
  match.set_field("foo_w");
  match.mutable_val()->set_s("apple");
  setKnnQuery(*boolean.add_filter(), "body_v", {0.0f, 1.0f}, 1);

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_TRUE(resultIds(*req).empty());

  req->done();
}

TEST_F(KnnQueryTest, booleanFilterOnlyAppliesProhibited) {
  CollectionHelper h("main");
  h.clear();

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple", "state_s", "ok"));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple", "state_s", "blocked"));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange", "state_s", "ok"));
  h.commit();

  auto* req = LocalReq::create(soluxNode->getSearchEngine());
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_limit(10);
  topDocs.set_get_number(true);
  topDocs.set_get_scores(true);
  topDocs.mutable_fields()->Add("id");

  auto& boolean = *topDocs.mutable_query()->mutable_boolean();
  auto& filter = *boolean.add_filter();
  auto& filterMatch = *filter.mutable_match();
  filterMatch.set_field("foo_w");
  filterMatch.mutable_val()->set_s("apple");
  auto& prohibited = *boolean.add_prohibited();
  auto& prohibitedMatch = *prohibited.mutable_match();
  prohibitedMatch.set_field("state_s");
  prohibitedMatch.mutable_val()->set_s("blocked");

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 1u);
  ASSERT_EQ(scores.size(), 1u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(scores[0], 0.0f);

  req->done();
}

TEST_F(KnnQueryTest, booleanMinMatchWithRequiredIsRejected) {
  CollectionHelper h("main");
  h.clear();
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple"), UpdateMessage::COMMIT);

  auto* req = LocalReq::create(soluxNode->getSearchEngine());
  req->proto.mutable_collection()->add_name("main");
  auto& topDocs = *(*req->proto.mutable_ops())["q"].mutable_top_docs();
  auto& boolean = *topDocs.mutable_query()->mutable_boolean();
  boolean.set_min_match(1);
  auto& required = *boolean.add_required();
  auto& requiredMatch = *required.mutable_match();
  requiredMatch.set_field("foo_w");
  requiredMatch.mutable_val()->set_s("apple");
  auto& optional = *boolean.add_optional();
  auto& optionalMatch = *optional.mutable_match();
  optionalMatch.set_field("foo_w");
  optionalMatch.mutable_val()->set_s("banana");

  {
    LogLevelGuard quiet;
    req->execute();
  }

  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_NE(req->responses[0]->proto.error().find("min_match=1"), std::string::npos);

  req->done();
}

// Stress the IDSelector path: delete most of the docs and verify we still
// get k live docs back.
TEST_F(KnnQueryTest, manyDeletes) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // 6 docs at distances [0, .02, .08, .18, .32, .50] from query [1, 0].
  for (int i = 0; i < 6; i++) {
    h.index(flatdoc("id", "d" + std::to_string(i),
                    "embedding_v", std::vector<float>{1.0f - i * 0.1f, (float)i * 0.1f}));
  }
  h.commit({"*"});

  // Delete the 4 closest - d4 and d5 are the only live docs.  Without
  // IDSelector this would need overfetchFactor >= 4; with it, k=2 just works.
  std::vector<std::string> dels{"d0", "d1", "d2", "d3"};
  h.deleteByIds(dels, UpdateMessage::COMMIT);

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1.0f, 0.0f}, 2);
  req->execute();
  EXPECT_EQ(req->getMatchCount(), 2);
  auto ids = resultIds(*req);
  std::set<std::string> got(ids.begin(), ids.end());
  EXPECT_TRUE(got.count("d4"));
  EXPECT_TRUE(got.count("d5"));

  req->done();
}

// Missing aux index: indexing without ever calling build -> the field has no
// FAISS aux entry.  Query falls back to exact flat-over-column.
TEST_F(KnnQueryTest, missingAuxIndexFallsBackToColumnScan) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // Index but DO NOT build an aux index (commit without selectors).
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}),
          UpdateMessage::COMMIT);

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1, 0, 0}, 5);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "a");

  req->done();
}

// Dimension mismatch: query vector has different dims than the index.
// Surfaces as an exception out of search execution.  SearchEngine currently
// logs+swallows the exception (so execute() returns), but we verify by
// checking that the response carries no docs and the engine logged the error.
//
// Note: this used to crash the arena because TopDocsReq's ctor called
// createWeight, and Arena::Create registers ~T() before the body runs - a
// throwing ctor would leave a half-constructed object scheduled for cleanup.
// The fix moved createWeight to TopDocsReq::init(), which runs after the
// object is fully constructed and registered, so a throw here unwinds
// cleanly.
TEST_F(KnnQueryTest, dimMismatchReturnsErrorResponse) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0, 0}));
  h.commit({"*"});

  // Query is 3-d but index is 4-d.
  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1, 0, 0}, 1);
  {
    LogLevelGuard quiet;  // expected: dim-mismatch warns
    req->execute();
  }
  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_EQ(resultIds(*req).size(), 0u);

  // A single response is returned with the error string populated.
  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_NE(req->responses[0]->proto.error().find("dims 3 do not match segment dims 4"), std::string::npos);

  req->done();
}

TEST_F(KnnQueryTest, emptyQueryVectorReturnsErrorResponse) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.commit();

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {}, 1);
  {
    LogLevelGuard quiet;  // expected: empty query vector parse error
    req->execute();
  }
  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_TRUE(resultIds(*req).empty());
  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_NE(req->responses[0]->proto.error().find("non-empty query vector"), std::string::npos);

  req->done();
}

// Cosine: stored vectors get unit-normalized at build; the query is also
// normalized before search, so direction-only matches score IP=1.  Verifies
// the actual numeric scores returned, not just doc order - catches
// regressions in either FAISS-side normalization (build-time renorm of
// stored vectors, query-side renorm in KnnQuery::Weight) or scoreFromDist
// for COSINE (which currently passes IP through unchanged).
TEST_F(KnnQueryTest, cosineMetric) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::COSINE);

  // Three vectors in distinct directions, all non-unit length so the
  // normalize paths actually do work:
  //   - "east" and "east_far" both point +x (parallel) -> cosine=1 vs query.
  //   - "north" points +y (orthogonal) -> cosine=0.
  //   - "back" points -x (antiparallel) -> cosine=-1.
  h.index(flatdoc("id", std::string("east"), "embedding_v", std::vector<float>{2, 0, 0}));
  h.index(flatdoc("id", std::string("east_far"), "embedding_v", std::vector<float>{5, 0, 0}));
  h.index(flatdoc("id", std::string("north"), "embedding_v", std::vector<float>{0, 3, 0}));
  h.index(flatdoc("id", std::string("back"), "embedding_v", std::vector<float>{-4, 0, 0}));
  h.commit({"*"});

  // Non-unit query in +x direction - gets normalized inside KnnQuery::Weight.
  auto* req = makeKnnReq(*soluxNode, "embedding_v", {7, 0, 0}, 4);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 4);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 4u);
  ASSERT_EQ(scores.size(), 4u);

  // The two east* docs share score=1; order between them is unspecified.
  std::set<std::string> top2(ids.begin(), ids.begin() + 2);
  EXPECT_TRUE(top2.count("east"));
  EXPECT_TRUE(top2.count("east_far"));
  EXPECT_NEAR(scores[0], 1.0f, 1e-5);
  EXPECT_NEAR(scores[1], 1.0f, 1e-5);

  // Third is "north" (orthogonal, score 0); fourth is "back" (anti-parallel,
  // score -1).  This catches accidental abs() / clamp() in score conversion.
  EXPECT_EQ(ids[2], "north");
  EXPECT_NEAR(scores[2], 0.0f, 1e-5);
  EXPECT_EQ(ids[3], "back");
  EXPECT_NEAR(scores[3], -1.0f, 1e-5);

  req->done();
}

TEST_F(KnnQueryTest, zeroOnlyCosineSegmentDoesNotPoisonColumnScan) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::COSINE);

  {
    LogLevelGuard quiet;  // expected: zero cosine vector is skipped on write
    h.index(flatdoc("id", std::string("zero"), "embedding_v", std::vector<float>{0, 0, 0}));
    h.commit();
  }

  h.index(flatdoc("id", std::string("east"), "embedding_v", std::vector<float>{2, 0, 0}));
  h.index(flatdoc("id", std::string("north"), "embedding_v", std::vector<float>{0, 3, 0}));
  h.commit();

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {7, 0, 0}, 2);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 2);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "east");
  EXPECT_EQ(ids[1], "north");

  req->done();
}

// L2 score conversion: KnnQuery returns 1/(1+d^2), where d^2 is FAISS's
// squared L2.  Verify the formula end-to-end with concrete distances.
TEST_F(KnnQueryTest, l2Scores) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // Squared distances from query [0,0]:
  //   "exact"  [0,0]    -> 0   -> score 1.0
  //   "one"    [1,0]    -> 1   -> score 0.5
  //   "two"    [2,0]    -> 4   -> score 0.2
  h.index(flatdoc("id", std::string("exact"), "embedding_v", std::vector<float>{0, 0}));
  h.index(flatdoc("id", std::string("one"), "embedding_v", std::vector<float>{1, 0}));
  h.index(flatdoc("id", std::string("two"), "embedding_v", std::vector<float>{2, 0}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {0, 0}, 3);
  req->execute();

  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 3u);
  ASSERT_EQ(scores.size(), 3u);

  EXPECT_EQ(ids[0], "exact");
  EXPECT_NEAR(scores[0], 1.0f, 1e-5);
  EXPECT_EQ(ids[1], "one");
  EXPECT_NEAR(scores[1], 0.5f, 1e-5);
  EXPECT_EQ(ids[2], "two");
  EXPECT_NEAR(scores[2], 0.2f, 1e-5);

  req->done();
}

// Drives the host deepen loop through breadth rounds with an exact-score
// engine (the IVF-flat shape).  The candidate pool must stay sorted across a
// breadth change INCLUDING a later depth round at the same breadth: doc x's
// low vector (0.1, list 0) is absorbed in round 1, x's best vector (0.5,
// list 1) only arrives in the final depth round, after the pool already mixed
// breadths.  Collapsing an unsorted pool would score x at 0.1 and rank it
// last instead of third.
TEST_F(KnnQueryTest, breadthRoundsKeepCandidatePoolSorted) {
  FaissFlatAuxGuard auxGuard;
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t ntotal) {
    return std::make_unique<BreadthSplitEngine>(flat, ntotal, 7);
  });
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::IP);

  // valueRanks 0-5 (list 0): a's six vectors.  rank 6 (list 0): x's low
  // vector.  rank 7 (list 1): x's high vector.  ranks 8-11: h's four.
  // ranks 12-17: one filler vector per doc f1..f6.  18 vectors over 9 docs
  // keeps avgMult=2, so k=4 starts at kReq=8 and the loop runs:
  //   round 1 breadth 0: all 7 list-0 vectors (a's six + x@0.1), pool drained
  //   round 2 breadth 2: top-8 = a+h vectors only -> 3 docs, deepen kReq to 14
  //   round 3 breadth 2: appends x@0.5 + three fillers at the SAME breadth -
  //     the mixed pool must still be sorted before collapse.
  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{0.95f, 0}, {0.9f, 0}, {0.85f, 0},
                                                  {0.8f, 0}, {0.75f, 0}, {0.7f, 0}}));
  h.index(flatdoc("id", std::string("x"), "emb_vs",
                  std::vector<std::vector<float>>{{0.1f, 0}, {0.5f, 0}}));
  h.index(flatdoc("id", std::string("h"), "emb_vs",
                  std::vector<std::vector<float>>{{0.93f, 0}, {0.88f, 0},
                                                  {0.83f, 0}, {0.78f, 0}}));
  for (int i = 1; i <= 6; i++) {
    h.index(flatdoc("id", "f" + std::to_string(i), "emb_vs",
                    std::vector<std::vector<float>>{{0.46f - i * 0.01f, 0}}));
  }
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0}, 4);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 4);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "h");
  EXPECT_EQ(ids[2], "x");  // its 0.5 vector, not the 0.1 straggler
  EXPECT_EQ(ids[3], "f1");
  ASSERT_EQ(scores.size(), 4u);
  EXPECT_NEAR(scores[0], 0.95f, 1e-5);
  EXPECT_NEAR(scores[1], 0.93f, 1e-5);
  EXPECT_NEAR(scores[2], 0.5f, 1e-5);
  EXPECT_NEAR(scores[3], 0.45f, 1e-5);

  req->done();
}

// An approximate engine (the PQ shape: scores bucketed, order inverted within
// each bucket) must be corrected by the host: rescore appended candidates
// from the full-precision column, sort, then collapse.  Without the rescore
// the inverted order collapses to the wrong docs (a, d, c) at bucket scores.
TEST_F(KnnQueryTest, approximateScoresRescoredFromColumn) {
  FaissFlatAuxGuard auxGuard;
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t) {
    return std::make_unique<QuantizingEngine>(flat);
  });
  CollectionHelper h("main");
  h.clear();
  installMultiVecSchema(h.collection(), proto::VectorParams::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "emb_vs", {1, 0, 0}, 3);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");
  ASSERT_EQ(scores.size(), 3u);
  EXPECT_NEAR(scores[0], 1.0f, 1e-5);          // d^2 = 0
  EXPECT_NEAR(scores[1], 1.0f / 1.25f, 1e-5);  // d^2 = 0.25
  EXPECT_NEAR(scores[2], 1.0f / 1.36f, 1e-5);  // d^2 = 0.36

  req->done();
}

// COSINE with normalize_on_write=false stores RAW vectors in the column, so
// the rescore path must renormalize each candidate on the fly (the persisted
// cosineNormalizeColumnOnRescore policy).  The quantizing wrapper forces a
// rescore; correct cosine order AND scores prove the raw-column normalization
// ran (without it, rescore would return raw dot products like 5.0).
TEST_F(KnnQueryTest, cosineRawColumnRescoreNormalizes) {
  FaissFlatAuxGuard auxGuard;
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t) {
    return std::make_unique<QuantizingEngine>(flat);
  });
  CollectionHelper h("main");
  h.clear();
  installVecSchemaCosineRaw(h.collection());

  // Raw (non-unit) vectors with distinct cosines vs the +x query direction:
  //   east (5,0,0) -> 1.0,  c (4,3,0) -> 0.8,  b (3,4,0) -> 0.6
  // 0.8 and 0.6 land in the same quantization bucket (0.5) and arrive
  // inverted; only a normalize-on-rescore from the raw column restores c
  // ahead of b.
  h.index(flatdoc("id", std::string("east"), "embedding_v", std::vector<float>{5, 0, 0}));
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{3, 4, 0}));
  h.index(flatdoc("id", std::string("c"), "embedding_v", std::vector<float>{4, 3, 0}));
  h.commit({"*"});

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {7, 0, 0}, 3);
  req->execute();

  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "east");
  EXPECT_EQ(ids[1], "c");
  EXPECT_EQ(ids[2], "b");
  ASSERT_EQ(scores.size(), 3u);
  EXPECT_NEAR(scores[0], 1.0f, 1e-5);
  EXPECT_NEAR(scores[1], 0.8f, 1e-5);
  EXPECT_NEAR(scores[2], 0.6f, 1e-5);

  req->done();
}
