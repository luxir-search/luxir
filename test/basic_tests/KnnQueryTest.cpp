#include <gtest/gtest.h>

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

class KnnQueryTest : public SoluxTest {
protected:
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

    auto& knn = *topDocs.mutable_query()->mutable_knn();
    knn.set_field(field);
    knn.set_k(k);
    auto& f32 = *knn.mutable_query()->mutable_f32();
    for (float v : queryVec) f32.add_v(v);
    return lreq;
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

  // k=3 vectors requested; "a" owns the top two, so after grouping we get 2 docs.
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
// their vectors.  liveDocs filtering happens inside FAISS via IDSelector,
// so we always get exactly k live results back.
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

// Stress the IDSelector path: delete most of the docs and verify we still
// get exactly k live hits back without any over-fetch logic.
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
// FAISS aux entry.  Query should return no hits, not throw.
TEST_F(KnnQueryTest, missingAuxIndexReturnsEmpty) {
  CollectionHelper h("main");
  h.clear();
  installVecSchema(h.collection(), proto::VectorParams::L2);

  // Index but DO NOT build the aux index (commit without selectors).
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}),
          UpdateMessage::COMMIT);

  auto* req = makeKnnReq(*soluxNode, "embedding_v", {1, 0, 0}, 5);
  {
    LogLevelGuard quiet;  // expected: KnnQuery debug-logs "no aux index"
    req->execute();
  }

  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_EQ(resultIds(*req).size(), 0u);

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
TEST_F(KnnQueryTest, dimMismatchReturnsEmpty) {
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
  EXPECT_NE(req->responses[0]->proto.error().find("dims 3 do not match index dims 4"), std::string::npos);

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

