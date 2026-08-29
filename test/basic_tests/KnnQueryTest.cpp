#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/query/VectorEngine.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/LuxirNode.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;
namespace build = luxir::api::build;

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

// Mixed-exactness merge shape (the flat+IVF composition the composite engine
// produces): hits from exactSeg keep deliberately shifted scores and are
// claimed exact via exactSegOrds; all other segments' scores are quantized to
// coarse buckets and arrive approximate.  The shift is a tracer: a host that
// re-rescores exact-claimed hits erases it, while a host honoring
// exactSegOrds must surface it in the response scores.
class PartiallyExactEngine final : public VectorEngine {
  VectorEngine& inner;
  int32_t exactSeg;
  float shift;

public:
  PartiallyExactEngine(VectorEngine& inner, int32_t exactSeg, float shift) noexcept
    : inner(inner), exactSeg(exactSeg), shift(shift) {}

  VectorSearchResult search(const VectorSearchRequest& request) override {
    VectorSearchResult result = inner.search(request);
    for (auto& hit : result.hits) {
      if (hit.segOrd == exactSeg) {
        hit.score += shift;
      } else {
        hit.score = std::floor(hit.score * 2.0f) / 2.0f;
      }
    }
    std::stable_sort(result.hits.begin(), result.hits.end(),
                     [](const VectorEngineHit& a, const VectorEngineHit& b) {
                       return a.score > b.score;
                     });
    result.scoresAreExact = false;
    result.exactSegOrds.assign(1, exactSeg);
    return result;
  }
};

class KnnQueryTest : public LuxirTest {
protected:
  struct TopDocsFilterFoldGuard {
    bool saved;

    explicit TopDocsFilterFoldGuard(bool disabled)
      : saved(disableTopDocsFilterFold) {
      disableTopDocsFilterFold = disabled;
    }
    ~TopDocsFilterFoldGuard() {
      disableTopDocsFilterFold = saved;
    }
  };

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

  struct WholeMembershipPlanGuard {
    bool saved;

    explicit WholeMembershipPlanGuard(bool disabled)
      : saved(QueryPrep::disableWholeMembershipPlanForTests) {
      QueryPrep::disableWholeMembershipPlanForTests = disabled;
    }
    ~WholeMembershipPlanGuard() {
      QueryPrep::disableWholeMembershipPlanForTests = saved;
    }
  };

  // Shrinks the parallel-task grains so small test corpora still exercise
  // multi-chunk scans and per-segment rescore tasks.  Grain changes
  // scheduling only, never results (chunk boundaries are mode-independent).
  struct ScanGrainGuard {
    int64_t savedScanGrain;
    int64_t savedRescoreGrain;

    ScanGrainGuard(int64_t scanGrainVectors, int64_t rescoreGrainCandidates)
      : savedScanGrain(KnnQuery::scanTaskGrainVectors),
        savedRescoreGrain(KnnQuery::rescoreTaskGrainCandidates) {
      KnnQuery::scanTaskGrainVectors = scanGrainVectors;
      KnnQuery::rescoreTaskGrainCandidates = rescoreGrainCandidates;
    }

    ~ScanGrainGuard() {
      KnnQuery::scanTaskGrainVectors = savedScanGrain;
      KnnQuery::rescoreTaskGrainCandidates = savedRescoreGrain;
    }
  };

  struct IvfPqAuxGuard {
    bool savedIvfPq;
    int32_t savedNList;
    int32_t savedM;
    int32_t savedBits;
    int32_t savedNProbe;
    int64_t savedMinTraining;
    int64_t savedBuildThreshold;
    int32_t savedRefineCount;
    int32_t savedRefineRatio;

    // refineRatio configures the DEFAULT refine sizing as a pure multiple
    // (count=0, ratio=refineRatio) so tests that rely on the default see
    // multiplier behavior; most tests pass an explicit request
    // refine_candidates (an absolute pool size) anyway.
    IvfPqAuxGuard(int32_t nlist, int32_t m, int32_t bits,
                  int32_t nprobe, int64_t minTraining, int32_t refineRatio)
      : savedIvfPq(VectorIndexBuilder::buildFaissIvfPqAuxIndexes),
        savedNList(VectorIndexBuilder::ivfPqNList),
        savedM(VectorIndexBuilder::ivfPqM),
        savedBits(VectorIndexBuilder::ivfPqBits),
        savedNProbe(VectorIndexBuilder::ivfPqDefaultNProbe),
        savedMinTraining(VectorIndexBuilder::ivfPqMinTrainingVectors),
        savedBuildThreshold(VectorIndexBuilder::ivfPqBuildThresholdScanCost),
        savedRefineCount(KnnQuery::defaultAnnRefineCount),
        savedRefineRatio(KnnQuery::defaultAnnRefineRatio) {
      VectorIndexBuilder::buildFaissIvfPqAuxIndexes = true;
      VectorIndexBuilder::ivfPqNList = nlist;
      VectorIndexBuilder::ivfPqM = m;
      VectorIndexBuilder::ivfPqBits = bits;
      VectorIndexBuilder::ivfPqDefaultNProbe = nprobe;
      VectorIndexBuilder::ivfPqMinTrainingVectors = minTraining;
      VectorIndexBuilder::ivfPqBuildThresholdScanCost = 0;
      KnnQuery::defaultAnnRefineCount = 0;
      KnnQuery::defaultAnnRefineRatio = refineRatio;
    }

    ~IvfPqAuxGuard() {
      VectorIndexBuilder::buildFaissIvfPqAuxIndexes = savedIvfPq;
      VectorIndexBuilder::ivfPqNList = savedNList;
      VectorIndexBuilder::ivfPqM = savedM;
      VectorIndexBuilder::ivfPqBits = savedBits;
      VectorIndexBuilder::ivfPqDefaultNProbe = savedNProbe;
      VectorIndexBuilder::ivfPqMinTrainingVectors = savedMinTraining;
      VectorIndexBuilder::ivfPqBuildThresholdScanCost = savedBuildThreshold;
      KnnQuery::defaultAnnRefineCount = savedRefineCount;
      KnnQuery::defaultAnnRefineRatio = savedRefineRatio;
    }
  };

  void SetUp() override {
    auto col = luxirNode->getCollection("main");
    col->setSchema(Schema::createDefaultSchema());
  }

  // Install a schema where _v is a vector field with the given metric.
  static void installVecSchema(Collection& col, api::VectorMetric metric) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.column = true;
    f.metric = metric;
    b.set(col);
  }

  // Install a schema where _v is a COSINE vector field that stores RAW
  // (unnormalized) vectors in the column - the normalize-on-rescore mode.
  static void installVecSchemaCosineRaw(Collection& col) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.column = true;
    f.metric = api::VectorMetric::COSINE;
    f.normalize_on_write = false;
    b.set(col);
  }

  // Install a schema where _vs is a multi-valued vector field with the given metric.
  static void installMultiVecSchema(Collection& col, api::VectorMetric metric) {
    SchemaBuilder b;
    auto& f = b.templ("_vs");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.column = true;
    f.multi = true;
    f.metric = metric;
    b.set(col);
  }

  // Configure an existing TopDocs cursor with the standard KNN query plus
  // id/score/number, so tests can assert ordering.  Keeps the legacy param
  // order (field, vec, k, nprobe, refineCandidates, exact, minScanFraction);
  // qb::knn takes (exact, refineCandidates) swapped, handled here.
  static void configKnn(OpCursor& cur, std::string_view field,
                        const std::vector<float>& queryVec, int32_t k,
                        int32_t nprobe = 0, int32_t refineCandidates = 0,
                        bool exact = false, float minScanFraction = 0.0f) {
    cur.getScores().getNumber().fields({"id"});
    cur.rawQuery() = qb::knn(cur.mr(), field, queryVec, k, nprobe, exact,
                             refineCandidates, minScanFraction);
  }

  // Build a TopDocs request with a KNN query for the given field + query
  // vector + k.  Always pulls back "id" so tests can assert ordering.
  static LocalReq* makeKnnReq(LuxirNode& node, std::string_view field,
                              std::vector<float> queryVec, int32_t k,
                              int32_t nprobe = 0, int32_t refineCandidates = 0,
                              bool exact = false, float minScanFraction = 0.0f) {
    auto* lreq = LocalReq::create(node.getSearchEngine());
    auto& cur = lreq->collection("main").topDocs("q");
    configKnn(cur, field, queryVec, k, nprobe, refineCandidates, exact, minScanFraction);
    return lreq;
  }

  static LocalReq* makeKnnFilterReq(
      LuxirNode& node, std::string_view field, std::vector<float> queryVec,
      int32_t k, int32_t nprobe = 0, int32_t refineCandidates = 0,
      bool exact = false, float minScanFraction = 0.0f) {
    auto* lreq = LocalReq::create(node.getSearchEngine());
    auto& cur = lreq->collection("main").topDocs("q");
    cur.allQuery().fields({"id"}).getNumber().limit(-1);
    auto& top = std::get<api::TopDocs>(cur.rawOp().kind);
    auto* filter = build::allocArray(top.filter, 1, cur.mr());
    auto* stored = (api::Query*)cur.mr().allocate(
        sizeof(api::Query), alignof(api::Query));
    new (stored) api::Query(qb::knn(
        cur.mr(), field, queryVec, k, nprobe, exact, refineCandidates,
        minScanFraction));
    filter[0].query = stored;
    return lreq;
  }

  static void addMatchFilter(OpCursor& cur, std::string_view field,
                             std::string_view value) {
    auto& mr = cur.mr();
    auto& td = std::get<api::TopDocs>(cur.rawOp().kind);
    api::Filter* nf = build::allocArray(td.filter, 1, mr);
    auto* q = (api::Query*)mr.allocate(sizeof(api::Query), alignof(api::Query));
    new (q) api::Query(qb::match(mr, field, value));
    nf[0].query = q;
  }

  // Pull "id" out of a search response in result order.
  static std::vector<std::string> resultIds(LocalReq& req, std::string_view opName = "q") {
    std::vector<std::string> ids;
    const auto* dl = req.docList(opName);
    if (!dl) return ids;
    const auto* idCol = dl->columns.find("id");
    if (!idCol) return ids;
    const auto* col = std::get_if<api::ColStr>(&idCol->kind);
    if (!col) return ids;
    for (const auto& s : col->v) ids.emplace_back(s);
    return ids;
  }

  // Pull "_score_" out of a search response in result order (parallel to
  // resultIds).  Empty if the request didn't set get_scores.
  static std::vector<float> resultScores(LocalReq& req, std::string_view opName = "q") {
    std::vector<float> scores;
    const auto* dl = req.docList(opName);
    if (!dl) return scores;
    const auto* scoreCol = dl->columns.find("_score_");
    if (!scoreCol) return scores;
    const auto* col = std::get_if<api::ColFloat>(&scoreCol->kind);
    if (!col) return scores;
    for (float v : col->v) scores.push_back(v);
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
  installVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0, 0}, 3);
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

TEST_F(KnnQueryTest, zeroBoostKeepsKnnMatchSetAndZeroesScores) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{0, 0}));
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{1, 0}));
  h.index(flatdoc("id", std::string("c"), "embedding_v", std::vector<float>{2, 0}));
  h.index(flatdoc("id", std::string("d"), "embedding_v", std::vector<float>{3, 0}));
  h.commit({"*"});

  auto* baseline = makeKnnReq(*luxirNode, "embedding_v", {0, 0}, 3);
  baseline->execute();
  ASSERT_TRUE(baseline->ok()) << baseline->errorMsg();
  auto expectedIds = resultIds(*baseline);

  auto* zero = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = zero->collection("main").topDocs("q");
  cur.getScores().getNumber().fields({"id"});
  auto knn = qb::knn(cur.mr(), "embedding_v", {0, 0}, 3);
  cur.rawQuery() = qb::boost(cur.mr(), knn, 0.0f);
  zero->execute();
  ASSERT_TRUE(zero->ok()) << zero->errorMsg();
  EXPECT_EQ(expectedIds, resultIds(*zero));
  auto scores = resultScores(*zero);
  ASSERT_EQ(expectedIds.size(), scores.size());
  for (float score : scores) EXPECT_FLOAT_EQ(0.0f, score);

  baseline->done();
  zero->done();
}

// Multi-segment: each segment contributes its share to the top-K, and the
// FAISS-id -> (segment, docRank) mapping is correct.  Interleaves no-vector
// docs in seg0 and seg1 so the per-segment columns are sparse - surfaces
// off-by-one if the valueRank -> docRank lookup is wrong on a per-segment
// basis (different segments have different sparse layouts).
TEST_F(KnnQueryTest, multiSegment) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0}, 3);
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

TEST_F(KnnQueryTest, routedFilterRepreparesTopKForSidewaysFacet) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", "a", "brand_s", "acme", "embedding_v",
                  std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", "d", "brand_s", "acme", "embedding_v",
                  std::vector<float>{0, 1, 0}));
  h.commit();
  h.index(flatdoc("id", "b", "brand_s", "beta", "embedding_v",
                  std::vector<float>{0.99f, 0.01f, 0}));
  h.index(flatdoc("id", "c", "brand_s", "beta", "embedding_v",
                  std::vector<float>{0.98f, 0.02f, 0}));
  h.commit();
  ASSERT_EQ(2u, h.getIndexWriter()->getIndexReader()->segments().size());

  auto request = localReq(luxirNode->getSearchEngine());
  auto& top = request->collection("main").topDocs("q")
      .getNumber().fields({"id"}).limit(2);
  top.rawQuery() = qb::knn(top.mr(), "embedding_v", {1, 0, 0}, 2);
  top.facet("brands", "brand_s").limit(-1);
  top.filter(qb::match(top.mr(), "brand_s", "acme"), {"brands"});
  int64_t prepares = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(2, request->getMatchCount());
  EXPECT_EQ((std::vector<std::string>{"a", "d"}), resultIds(*request));
  const auto& result = *request->docList("q")->ops.at("brands")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("acme", ids[0]);
  EXPECT_EQ("beta", ids[1]);
  EXPECT_EQ(1, result.counts[0]);
  EXPECT_EQ(1, result.counts[1]);
  // Carving the default {a,d} top-k would be {acme:2}; the sideways result
  // instead comes from the independently prepared all-brand top-k {a,b}.
  EXPECT_EQ(prepares + 2, KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed));
}

TEST_F(KnnQueryTest, selectedFacetComposesWithDomainSensitiveMain) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", "a", "brand_s", "acme", "embedding_v",
                  std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", "d", "brand_s", "acme", "embedding_v",
                  std::vector<float>{0, 1, 0}));
  h.index(flatdoc("id", "b", "brand_s", "beta", "embedding_v",
                  std::vector<float>{0.99f, 0.01f, 0}));
  h.commit({"*"});

  auto request = localReq(luxirNode->getSearchEngine());
  auto& top = request->collection("main").topDocs("q")
      .getNumber().limit(0);
  top.rawQuery() = qb::knn(top.mr(), "embedding_v", {1, 0, 0}, 2);
  auto& facet = top.facet("brands", "brand_s").limit(-1);
  auto& rawFacet = std::get<api::FieldFacet>(facet.rawOp().kind);
  auto* selected = build::allocMessage<api::Val>(facet.mr());
  selected->kind = build::arenaStr(facet.mr(), "acme");
  rawFacet.selected = selected;
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(2, request->getMatchCount());
  const auto& result = *request->docList("q")->ops.at("brands")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("acme", ids[0]);
  EXPECT_EQ("beta", ids[1]);
  EXPECT_EQ(1, result.counts[0]);
  EXPECT_EQ(1, result.counts[1]);
}

TEST_F(KnnQueryTest, resetDomainPreparesKnnInsideFullEligibility) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", "a", "scope_s", "drop", "brand_s", "beta",
                  "embedding_v", std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", "b", "scope_s", "keep", "brand_s", "acme",
                  "embedding_v", std::vector<float>{0.99f, 0.01f, 0}));
  h.index(flatdoc("id", "c", "scope_s", "keep", "brand_s", "beta",
                  "embedding_v", std::vector<float>{0.98f, 0.02f, 0}));
  h.commit({"*"});

  auto request = localReq(luxirNode->getSearchEngine());
  auto& top = request->collection("main").topDocs("q")
      .allQuery().getNumber().limit(0);
  top.filter(qb::match(top.mr(), "scope_s", "keep"));
  auto& facet = top.facet("brands", "brand_s").limit(-1);
  auto& domain = facet.rawOp().domain.emplace();
  auto* query = (api::Query*) facet.mr().allocate(
      sizeof(api::Query), alignof(api::Query));
  new (query) api::Query(
      qb::knn(facet.mr(), "embedding_v", {1, 0, 0}, 1));
  domain.query = query;
  domain.apply_parent_filters = true;
  auto* filters = build::allocArray(domain.filter, 1, facet.mr());
  filters[0] = qb::match(facet.mr(), "brand_s", "beta");
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(2, request->getMatchCount());
  const auto& result = *request->docList("q")->ops.at("brands")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ("beta", ids[0]);
  EXPECT_EQ(1, result.counts[0]);
}

TEST_F(KnnQueryTest, rootDomainPreparesKnnInsideLocalFilter) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", "a", "brand_s", "acme", "embedding_v",
                  std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", "b", "brand_s", "beta", "embedding_v",
                  std::vector<float>{0.9f, 0.1f, 0}));
  h.commit({"*"});

  auto request = localReq(luxirNode->getSearchEngine());
  request->collection("main");
  auto& facet = request->facet("brands", "brand_s").limit(-1);
  auto& domain = facet.rawOp().domain.emplace();
  auto* query = (api::Query*) facet.mr().allocate(
      sizeof(api::Query), alignof(api::Query));
  new (query) api::Query(
      qb::knn(facet.mr(), "embedding_v", {1, 0, 0}, 1));
  domain.query = query;
  auto* filters = build::allocArray(domain.filter, 1, facet.mr());
  filters[0] = qb::match(facet.mr(), "brand_s", "beta");
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  const auto& result =
      *request->responses[0]->proto.ops.at("brands")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ("beta", ids[0]);
  EXPECT_EQ(1, result.counts[0]);
}

TEST_F(KnnQueryTest, readerCanonicalDomainSourceRepreparesOutsideLiveFrame) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  FilterCacheConfig config;
  config.maxBytes = 4 * 1024 * 1024;
  config.minSegmentDocs = 0;
  config.admissionThreshold = 1;
  h.getIndexWriter()->filterCache = std::make_shared<FilterCache>(config);
  h.index(flatdoc("id", "a", "brand_s", "acme", "embedding_v",
                  std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", "b", "brand_s", "beta", "embedding_v",
                  std::vector<float>{0.9f, 0.1f, 0}));
  h.commit({"*"});
  auto cache = h.getIndexWriter()->getFilterCache();

  auto request = localReq(luxirNode->getSearchEngine());
  request->collection("main");
  auto addDomain = [&](std::string_view name, bool betaOnly) {
    auto& facet = request->facet(name, "brand_s").limit(-1);
    auto& domain = facet.rawOp().domain.emplace();
    auto* query = (api::Query*) facet.mr().allocate(
        sizeof(api::Query), alignof(api::Query));
    new (query) api::Query(
        qb::knn(facet.mr(), "embedding_v", {1, 0, 0}, 1));
    domain.query = query;
    if (betaOnly) {
      auto* filters = build::allocArray(domain.filter, 1, facet.mr());
      filters[0] = qb::match(facet.mr(), "brand_s", "beta");
    }
  };
  addDomain("all_brands", false);
  addDomain("beta_brands", true);
  int64_t prepares = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  auto before = cache->counters();
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  // One reader-stable kNN value plus the beta filter artifact.
  EXPECT_EQ(before.builds + 2, cache->counters().builds);
  EXPECT_EQ(prepares + 2, KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed));
  const auto& all =
      *request->responses[0]->proto.ops.at("all_brands")->facetResult();
  const auto& beta =
      *request->responses[0]->proto.ops.at("beta_brands")->facetResult();
  EXPECT_EQ("acme", std::get<api::ColStr>(all.bucket_ids->kind).v[0]);
  EXPECT_EQ("beta", std::get<api::ColStr>(beta.bucket_ids->kind).v[0]);
}

TEST_F(KnnQueryTest, identicalEffectiveVariantsShareKnnPreparation) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", "a", "brand_s", "acme", "embedding_v",
                  std::vector<float>{1, 0, 0}));
  h.index(flatdoc("id", "b", "brand_s", "beta", "embedding_v",
                  std::vector<float>{0, 1, 0}));
  h.commit({"*"});

  auto request = localReq(luxirNode->getSearchEngine());
  auto& top = request->collection("main").topDocs("q").limit(0);
  top.rawQuery() = qb::knn(top.mr(), "embedding_v", {1, 0, 0}, 2);
  top.facet("brands", "brand_s").limit(-1);
  top.filter(qb::all(), {"brands"});
  int64_t prepares = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(prepares + 1, KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed));
}

TEST_F(KnnQueryTest, forcePrepareCanonicalMainPreparesOnceAcrossVariants) {
  CollectionHelper h("main");
  h.index(flatdoc("id", "a", "text_w", "hit", "brand_s", "acme"));
  h.index(flatdoc("id", "b", "text_w", "hit", "brand_s", "beta"));
  h.commit({"*"});

  auto request = localReq(luxirNode->getSearchEngine());
  request->testForcePrepare = true;
  auto& top = request->collection("main").topDocs("q")
      .matchQuery("text_w", "hit").limit(0);
  top.facet("brands", "brand_s").limit(-1);
  top.filter(qb::match(top.mr(), "brand_s", "acme"), {"brands"});
  int64_t prepares = ForcePrepareQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  request->execute();
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(prepares + 1, ForcePrepareQuery::prepareCallsForTests.load(
      std::memory_order_relaxed));
}

TEST_F(KnnQueryTest, routedPreparedFilterUsesParentBaseDomain) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.getIndexWriter()->mergePolicy->setMergeFactor(100);
  for (int32_t i = 0; i < 8; i++) {
    h.index(flatdoc(
        "id", std::to_string(i),
        "color_s", (i & 1) == 0 ? "red" : "blue",
        "embedding_v", std::vector<float>{(float) (8 - i), (float) i, 0}));
    h.commit();
  }
  h.commit({"*"});
  ASSERT_EQ(8u, h.getIndexWriter()->getIndexReader()->segments().size());

  auto request = localReq(luxirNode->getSearchEngine());
  auto& top = request->collection("main").topDocs("q")
      .allQuery().getNumber().limit(0);
  top.facet("colors", "color_s").limit(-1);
  top.filter(
      qb::knn(top.mr(), "embedding_v", {1, 0, 0}, 3), {"colors"});
  request->execute(false);

  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(3, request->getMatchCount());
  const auto& docs = *request->docList("q");
  const auto* value = docs.ops.find("colors");
  ASSERT_NE(value, nullptr);
  const auto& facet = std::get<api::FacetResult>((*value)->kind);
  const auto& ids = std::get<api::ColStr>(facet.bucket_ids->kind);
  ASSERT_EQ(2u, ids.v.size());
  std::map<std::string, int64_t> counts;
  for (size_t i = 0; i < ids.v.size(); i++) {
    counts[std::string(ids.v[i])] = facet.counts[i];
  }
  EXPECT_EQ((std::map<std::string, int64_t>{{"blue", 4}, {"red", 4}}),
            counts);
}

// Multi-valued: a doc owns several vectors; FAISS hits must be grouped back to
// the owning doc (one hit per doc, best score) via the valueRank->docId column.
// An interleaved no-vector doc ("g") makes identity (valueRank==docId) resolve a
// vector to the wrong doc, so its absence from the results proves the reverse map
// is used.  A delete phase then exercises the multi-valued is_member resolve path.
TEST_F(KnnQueryTest, multiValuedGrouping) {
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

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
  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
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

  auto* req2 = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
  req2->execute();
  auto ids2 = resultIds(*req2);
  ASSERT_EQ(ids2.size(), 1u);
  EXPECT_EQ(ids2[0], "b");
  req2->done();
}

TEST_F(KnnQueryTest, multiValuedGuaranteesKDocs) {
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
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
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 10);
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
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
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
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 4);
  {
    ExpectLog quiet("at candidate cap");
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
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
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
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
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
  installVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0}, 2);
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
  installVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("blue_a"), "color_s", "blue",
                  "embedding_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("blue_b"), "color_s", "blue",
                  "embedding_v", std::vector<float>{0.99f, 0.0f}));
  h.index(flatdoc("id", std::string("red_a"), "color_s", "red",
                  "embedding_v", std::vector<float>{0.8f, 0.0f}));
  h.index(flatdoc("id", std::string("red_b"), "color_s", "red",
                  "embedding_v", std::vector<float>{0.7f, 0.0f}));
  h.commit({"*"});

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  configKnn(cur, "embedding_v", {1.0f, 0.0f}, 2);
  addMatchFilter(cur, "color_s", "red");

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
  installVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.limit(10).getNumber().fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(), /*required=*/{},
      /*optional=*/{qb::knn(cur.mr(), "title_v", {1.0f, 0.0f}, 1),
                    qb::knn(cur.mr(), "body_v", {0.0f, 1.0f}, 1)});

  cur.facet("groups", "group_s").limit(-1);

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 2);
  auto ids = resultIds(*req);
  std::set<std::string> got(ids.begin(), ids.end());
  EXPECT_TRUE(got.count("a"));
  EXPECT_TRUE(got.count("b"));
  EXPECT_FALSE(got.count("c"));

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_TRUE(docs->ops.contains("groups"));
  const auto* facetResult = (*docs->ops.at("groups")).facetResult();
  ASSERT_NE(facetResult, nullptr);
  const auto& facetBids = std::get<api::ColStr>(facetResult->bucket_ids->kind);
  std::map<std::string, int64_t> counts;
  for (size_t i = 0; i < facetResult->counts.size(); i++) {
    counts[std::string(facetBids.v[i])] = facetResult->counts[i];
  }
  ASSERT_EQ(counts.size(), 2u);
  EXPECT_EQ(counts["left"], 1);
  EXPECT_EQ(counts["right"], 1);

  req->done();
}

TEST_F(KnnQueryTest, booleanKnnFilterConstrainsRequiredKnn) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.limit(10).getNumber().fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::knn(cur.mr(), "title_v", {1.0f, 0.0f}, 1)},
      /*optional=*/{}, /*prohibited=*/{},
      /*filter=*/{qb::knn(cur.mr(), "body_v", {0.0f, 1.0f}, 1)});

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "b");

  req->done();
}

TEST_F(KnnQueryTest, booleanTermRequiredUsesKnnFilter) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "body_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple",
                  "body_v", std::vector<float>{0.0f, 1.0f}));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange",
                  "body_v", std::vector<float>{0.0f, 0.9f}));
  h.commit({"*"});

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.limit(10).getNumber().fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "foo_w", "apple")},
      /*optional=*/{}, /*prohibited=*/{},
      /*filter=*/{qb::knn(cur.mr(), "body_v", {0.0f, 1.0f}, 1)});

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "b");

  req->done();
}

TEST_F(KnnQueryTest, booleanDisjointRequiredAndKnnFilterReturnsEmpty) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple",
                  "body_v", std::vector<float>{1.0f, 0.0f}));
  h.index(flatdoc("id", std::string("b"), "foo_w", "orange",
                  "body_v", std::vector<float>{0.0f, 1.0f}));
  h.commit({"*"});

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.limit(10).getNumber().fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "foo_w", "apple")},
      /*optional=*/{}, /*prohibited=*/{},
      /*filter=*/{qb::knn(cur.mr(), "body_v", {0.0f, 1.0f}, 1)});

  req->execute();

  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_TRUE(resultIds(*req).empty());

  req->done();
}

TEST_F(KnnQueryTest, booleanFilterOnlyAppliesProhibited) {
  CollectionHelper h("main");

  h.index(flatdoc("id", std::string("a"), "foo_w", "apple", "state_s", "ok"));
  h.index(flatdoc("id", std::string("b"), "foo_w", "apple", "state_s", "blocked"));
  h.index(flatdoc("id", std::string("c"), "foo_w", "orange", "state_s", "ok"));
  h.commit();

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.limit(10).withStats().fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{}, /*optional=*/{},
      /*prohibited=*/{qb::match(cur.mr(), "state_s", "blocked")},
      /*filter=*/{qb::match(cur.mr(), "foo_w", "apple")});

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

TEST_F(KnnQueryTest, booleanMinMatchWithRequiredConstrains) {
  CollectionHelper h("main");
  h.index(flatdoc("id", std::string("a"), "foo_w", "apple"), UpdateMessage::COMMIT);

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getNumber();
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "foo_w", "apple")},
      /*optional=*/{qb::match(cur.mr(), "foo_w", "banana")},
      /*prohibited=*/{}, /*filter=*/{}, /*minMatch=*/1);
  req->execute();

  // min_match composes with required clauses: the optional group is a real
  // constraint, and "banana" matches nothing here.
  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_TRUE(req->responses[0]->proto.error.empty()) << req->responses[0]->proto.error;
  EXPECT_EQ(0, req->getMatchCount());

  req->done();
}

// Stress the IDSelector path: delete most of the docs and verify we still
// get k live docs back.
TEST_F(KnnQueryTest, manyDeletes) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

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

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1.0f, 0.0f}, 2);
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
  installVecSchema(h.collection(), api::VectorMetric::L2);

  // Index but DO NOT build an aux index (commit without selectors).
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}),
          UpdateMessage::COMMIT);

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0}, 5);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 1);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 1u);
  EXPECT_EQ(ids[0], "a");

  req->done();
}

TEST_F(KnnQueryTest, ivfPqAuxUsesColumnRescore) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/64);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 160; i++) {
    float x = (float)i;
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{x, x * 0.01f, x * 0.02f, x * 0.03f}));
  }
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {0, 0, 0, 0}, 5,
                         /*nprobe=*/4, /*refineCandidates=*/320);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 5);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 5u);
  ASSERT_EQ(scores.size(), 5u);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(ids[(size_t)i], "doc" + std::to_string(i));
  }
  EXPECT_NEAR(scores[0], 1.0f, 1e-5);
  for (size_t i = 1; i < scores.size(); i++) {
    EXPECT_GT(scores[i - 1], scores[i]);
  }

  req->done();
}

TEST_F(KnnQueryTest, ivfPqApproximateRecallAtOneProbe) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int cluster = 0; cluster < 4; cluster++) {
    for (int i = 0; i < 80; i++) {
      float base = (float)(cluster * 1000);
      float off = (float)i * 0.01f;
      h.index(flatdoc("id", "c" + std::to_string(cluster) + "_" + std::to_string(i),
                      "embedding_v", std::vector<float>{base + off, off, 0.0f, 0.0f}));
    }
  }
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {2000, 0, 0, 0}, 5,
                         /*nprobe=*/1, /*refineCandidates=*/40);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 5);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 5u);
  std::set<std::string> exactTop{"c2_0", "c2_1", "c2_2", "c2_3", "c2_4"};
  int hits = 0;
  for (const auto& id : ids) {
    if (exactTop.count(id)) hits++;
  }
  EXPECT_GE(hits, 3) << "IVF+PQ one-probe recall should recover most exact top hits";

  req->done();
}

// exact=true is a result contract: true top-k regardless of which ANN index
// exists or what nprobe/refine say.  The data makes the ANN path miss by
// construction: two tight blobs (nlist=2) plus one "outlier" vector at 600,
// which k-means must assign to blob B's list (600 is closer to ~1000 than to
// ~0).  The query at 400 is closer to blob A's centroid, so nprobe=1 probes
// only A - the outlier, the true nearest doc by a 2x margin, is structurally
// invisible to the approximate path.  exact must recover it.
TEST_F(KnnQueryTest, exactBypassesApproximateIndex) {
  IvfPqAuxGuard guard(/*nlist=*/2, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 80; i++) {
    float off = (float)i * 0.01f;
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{off, 0.0f, 0.0f, 0.0f}));
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{1000.0f + off, 0.0f, 0.0f, 0.0f}));
  }
  h.index(flatdoc("id", std::string("outlier"),
                  "embedding_v", std::vector<float>{600.0f, 0.0f, 0.0f, 0.0f}));
  h.commit({"*"});

  // Approximate at nprobe=1: probes blob A's list only; the outlier (true #1,
  // distance 200 vs blob A's best ~399) cannot appear.
  auto* approx = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                            /*nprobe=*/1, /*refineCandidates=*/40);
  approx->execute();
  auto approxIds = resultIds(*approx);
  ASSERT_EQ(approxIds.size(), 5u);
  for (const auto& id : approxIds) {
    EXPECT_NE(id, "outlier") << "nprobe=1 should be unable to reach the outlier's list";
  }
  approx->done();

  // exact: identical request plus the contract flag; hostile nprobe/refine
  // are ignored and the true top-5 comes back in order.
  auto* req = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                         /*nprobe=*/1, /*refineCandidates=*/40, /*exact=*/true);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 5);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 5u);
  EXPECT_EQ(ids[0], "outlier");
  for (int i = 1; i < 5; i++) {
    EXPECT_EQ(ids[(size_t)i], "a" + std::to_string(80 - i));  // a79, a78, a77, a76
  }
  ASSERT_EQ(scores.size(), 5u);
  for (size_t i = 1; i < scores.size(); i++) {
    EXPECT_GT(scores[i - 1], scores[i]);
  }

  req->done();
}

TEST_F(KnnQueryTest, minScanFractionFloorsExplicitNProbe) {
  IvfPqAuxGuard guard(/*nlist=*/2, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 80; i++) {
    float off = (float)i * 0.01f;
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{off, 0.0f, 0.0f, 0.0f}));
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{1000.0f + off, 0.0f, 0.0f, 0.0f}));
  }
  h.index(flatdoc("id", std::string("outlier"),
                  "embedding_v", std::vector<float>{600.0f, 0.0f, 0.0f, 0.0f}));
  h.commit({"*"});

  auto* narrow = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                            /*nprobe=*/1, /*refineCandidates=*/40);
  narrow->execute();
  auto narrowIds = resultIds(*narrow);
  narrow->done();
  ASSERT_EQ(narrowIds.size(), 5u);
  for (const auto& id : narrowIds) {
    EXPECT_NE(id, "outlier");
  }

  auto* floor = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                           /*nprobe=*/1, /*refineCandidates=*/200,
                           /*exact=*/false, /*minScanFraction=*/1.0f);
  floor->execute();
  auto floorIds = resultIds(*floor);
  floor->done();

  auto* exact = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                           /*nprobe=*/1, /*refineCandidates=*/200, /*exact=*/true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();

  ASSERT_EQ(floorIds.size(), 5u);
  EXPECT_EQ(floorIds[0], "outlier");
  EXPECT_EQ(floorIds, exactIds);
}

// Regression: nprobe=0 (the default) must NOT silently probe every IVF list.
// The default scan fraction is anchored on the reference index's own default
// (1/sqrt(referenceBreadth) = N^-0.25), a lean start, not a full scan.  Same
// hostile geometry as exactBypassesApproximateIndex: two tight blobs (nlist=2)
// plus an outlier at 600 that k-means assigns to blob B's list; the query at
// 400 sits in blob A's list.  refine_candidates is held below blob A's doc
// count so the host fills its candidate target from blob A alone and does not
// auto-deepen into blob B - a default that full-scanned would surface the
// outlier (its true #1), so its absence proves the default stayed lean.
// min_scan_fraction=1 over the same request recovers it, confirming the corpus
// would reveal a full scan if one happened.
TEST_F(KnnQueryTest, defaultNProbeStaysLeanNotFullScan) {
  IvfPqAuxGuard guard(/*nlist=*/2, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 80; i++) {
    float off = (float)i * 0.01f;
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{off, 0.0f, 0.0f, 0.0f}));
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{1000.0f + off, 0.0f, 0.0f, 0.0f}));
  }
  h.index(flatdoc("id", std::string("outlier"),
                  "embedding_v", std::vector<float>{600.0f, 0.0f, 0.0f, 0.0f}));
  h.commit({"*"});

  auto* dflt = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                          /*nprobe=*/0, /*refineCandidates=*/40);
  dflt->execute();
  auto dfltIds = resultIds(*dflt);
  dflt->done();
  ASSERT_EQ(dfltIds.size(), 5u);
  for (const auto& id : dfltIds) {
    EXPECT_NE(id, "outlier")
        << "nprobe=0 must stay at the lean index default, not probe every list";
  }

  auto* full = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                          /*nprobe=*/0, /*refineCandidates=*/200,
                          /*exact=*/false, /*minScanFraction=*/1.0f);
  full->execute();
  auto fullIds = resultIds(*full);
  full->done();
  ASSERT_EQ(fullIds.size(), 5u);
  EXPECT_EQ(fullIds[0], "outlier")
      << "min_scan_fraction=1 forces the exhaustive scan the default must avoid";
}

TEST_F(KnnQueryTest, mixedIndexedAndBelowThresholdCompositionMatchesExact) {
  IvfPqAuxGuard guard(/*nlist=*/2, /*m=*/1, /*bits=*/1,
                      /*nprobe=*/2, /*minTraining=*/2, /*refineRatio=*/64);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 80; i++) {
    h.index(flatdoc("id", "big" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  VectorIndexBuilder::ivfPqBuildThresholdScanCost = 1000;
  h.index(flatdoc("id", std::string("tiny0"),
                  "embedding_v", std::vector<float>{100.0f, 0.0f, 0.0f, 0.0f}));
  h.index(flatdoc("id", std::string("tiny1"),
                  "embedding_v", std::vector<float>{101.0f, 0.0f, 0.0f, 0.0f}));
  h.commit();

  auto* approx = makeKnnReq(*luxirNode, "embedding_v", {101, 0, 0, 0}, 5,
                            /*nprobe=*/2, /*refineCandidates=*/200);
  approx->execute();
  auto approxIds = resultIds(*approx);
  approx->done();

  auto* exact = makeKnnReq(*luxirNode, "embedding_v", {101, 0, 0, 0}, 5,
                           /*nprobe=*/2, /*refineCandidates=*/200, /*exact=*/true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();

  ASSERT_EQ(exactIds.size(), 5u);
  EXPECT_EQ(exactIds[0], "tiny1");
  EXPECT_EQ(exactIds[1], "tiny0");
  EXPECT_EQ(approxIds, exactIds);
}

TEST_F(KnnQueryTest, perSegmentExactBypassesApproximateMiss) {
  IvfPqAuxGuard guard(/*nlist=*/2, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 80; i++) {
    float off = (float)i * 0.01f;
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{off, 0.0f, 0.0f, 0.0f}));
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{1000.0f + off, 0.0f, 0.0f, 0.0f}));
  }
  h.index(flatdoc("id", std::string("outlier"),
                  "embedding_v", std::vector<float>{600.0f, 0.0f, 0.0f, 0.0f}));
  h.commit({"*"});

  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "far" + std::to_string(i),
                    "embedding_v", std::vector<float>{2000.0f + (float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit();

  auto* approx = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                            /*nprobe=*/1, /*refineCandidates=*/40);
  approx->execute();
  auto approxIds = resultIds(*approx);
  approx->done();
  ASSERT_EQ(approxIds.size(), 5u);
  for (const auto& id : approxIds) {
    EXPECT_NE(id, "outlier") << "segment-local nprobe=1 should miss the outlier list";
  }

  auto* exact = makeKnnReq(*luxirNode, "embedding_v", {400, 0, 0, 0}, 5,
                           /*nprobe=*/1, /*refineCandidates=*/40, /*exact=*/true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();
  ASSERT_EQ(exactIds.size(), 5u);
  EXPECT_EQ(exactIds[0], "outlier");
}

// Milestone-3 scan parallelism: with the scan grain forced to 1, every
// selected IVF list becomes its own (segment, list-range) task and every
// segment's rescore bucket its own task.  Chunk boundaries and the
// top-candidates cut are total-order deterministic, so a parallel run must
// be BIT-IDENTICAL to a serial run of the same query - ids and scores - and
// with min_scan_fraction=1 (a genuine full-breadth pin: every list of every
// segment is selected, independent of k-means placement) plus a rescore pool
// covering the corpus, identical to the exact contract too.  Mixed
// composition (TWO IVF segments, both committed with a build selector, plus
// one aux-less flat segment), sparse no-vector docs, and post-build deletes
// all ride along.
TEST_F(KnnQueryTest, parallelChunkedScanMatchesSerialAndExact) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  ScanGrainGuard grains(/*scanGrainVectors=*/1, /*rescoreGrainCandidates=*/1);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  // 160 vectors per segment: above the PQ training floor (39 * 2^bits =
  // 156 at bits=2) so these segments genuinely build IVF - below it the
  // builder silently falls back to flat and this test would pass without
  // exercising IVF chunking at all (asserted below).
  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
    if (i % 7 == 0) h.index(flatdoc("id", "ga" + std::to_string(i)));
  }
  h.commit({"*"});

  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i + 0.5f, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_EQ(reader->segments().size(), 2u);
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.embedding_v"), nullptr)
        << "segment 0 fell back to flat (below IVF training floor)";
    ASSERT_NE(reader->segments()[1].getAuxReader("vec.embedding_v"), nullptr)
        << "segment 1 fell back to flat (below IVF training floor)";
  }

  // Plain commit: no aux build for this segment, so it scans flat.
  h.index(flatdoc("id", std::string("tiny0"),
                  "embedding_v", std::vector<float>{40.25f, 0.0f, 0.0f, 0.0f}));
  h.commit();

  std::vector<std::string> dels{"a40", "b40"};
  h.deleteByIds(dels, UpdateMessage::COMMIT);

  std::vector<float> queryVec{40.3f, 0.0f, 0.0f, 0.0f};
  // refine_candidates covers every live vector, so the terminal rescore is
  // exhaustive and exact parity is guaranteed, not k-means-placement luck.
  auto* par = makeKnnReq(*luxirNode, "embedding_v", queryVec, 10,
                         /*nprobe=*/0, /*refineCandidates=*/400,
                         /*exact=*/false, /*minScanFraction=*/1.0f);
  par->execute(/*parallel=*/true);
  auto parIds = resultIds(*par);
  auto parScores = resultScores(*par);
  par->done();

  auto* ser = makeKnnReq(*luxirNode, "embedding_v", queryVec, 10,
                         /*nprobe=*/0, /*refineCandidates=*/400,
                         /*exact=*/false, /*minScanFraction=*/1.0f);
  ser->execute(/*parallel=*/false);
  auto serIds = resultIds(*ser);
  auto serScores = resultScores(*ser);
  ser->done();

  auto* exact = makeKnnReq(*luxirNode, "embedding_v", queryVec, 10,
                           /*nprobe=*/0, /*refineCandidates=*/400, /*exact=*/true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();

  ASSERT_EQ(parIds.size(), 10u);
  EXPECT_EQ(parIds, serIds);
  EXPECT_EQ(parScores, serScores);
  EXPECT_EQ(parIds, exactIds);
  EXPECT_EQ(parIds[0], "tiny0");  // 0.05 away, served by the flat segment
  for (const auto& id : parIds) {
    EXPECT_NE(id, "a40") << "deleted doc must not appear";
    EXPECT_NE(id, "b40") << "deleted doc must not appear";
  }
}

// Same parallel == serial identity for multi-valued fields, where the
// candidate cut, cross-segment merge, doc collapse, and per-segment rescore
// buckets all interact.  A small refine_candidates pin makes the bounded
// approximate cut actually bite (kReq well below the pooled vector count).
TEST_F(KnnQueryTest, parallelMultiValuedIvfMatchesSerial) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  ScanGrainGuard grains(/*scanGrainVectors=*/1, /*rescoreGrainCandidates=*/1);
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  // Vector counts (180 and 160) sit above the PQ training floor of 156.
  for (int i = 0; i < 60; i++) {
    float base = (float)i;
    h.index(flatdoc("id", "a" + std::to_string(i), "emb_vs",
                    std::vector<std::vector<float>>{
                      {base, 0.0f, 0.0f, 0.0f},
                      {base, 1.0f, 0.0f, 0.0f},
                      {base, 2.0f, 0.0f, 0.0f}}));
  }
  h.commit({"*"});
  for (int i = 0; i < 80; i++) {
    float base = (float)i + 0.5f;
    h.index(flatdoc("id", "b" + std::to_string(i), "emb_vs",
                    std::vector<std::vector<float>>{
                      {base, 0.0f, 0.0f, 0.0f},
                      {base, 1.0f, 0.0f, 0.0f}}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_EQ(reader->segments().size(), 2u);
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.emb_vs"), nullptr)
        << "segment 0 fell back to flat (below IVF training floor)";
    ASSERT_NE(reader->segments()[1].getAuxReader("vec.emb_vs"), nullptr)
        << "segment 1 fell back to flat (below IVF training floor)";
  }

  std::vector<float> queryVec{20.2f, 0.0f, 0.0f, 0.0f};
  auto* par = makeKnnReq(*luxirNode, "emb_vs", queryVec, 10,
                         /*nprobe=*/4, /*refineCandidates=*/20);
  par->execute(/*parallel=*/true);
  auto parIds = resultIds(*par);
  auto parScores = resultScores(*par);
  par->done();

  auto* ser = makeKnnReq(*luxirNode, "emb_vs", queryVec, 10,
                         /*nprobe=*/4, /*refineCandidates=*/20);
  ser->execute(/*parallel=*/false);
  auto serIds = resultIds(*ser);
  auto serScores = resultScores(*ser);
  ser->done();

  ASSERT_EQ(parIds.size(), 10u) << "k docs guaranteed despite the candidate cut";
  EXPECT_EQ(parIds, serIds);
  EXPECT_EQ(parScores, serScores);
}

// Parallel mode must reach a kNN nested inside a boolean clause:
// BooleanQuery::prepare rebuilds the child PrepareContext, and dropping
// ctx.parallel there would leave hybrid boolean+kNN queries silently
// single-threaded (results are identical either way, so the spawn counter is
// the only observable).
TEST_F(KnnQueryTest, booleanNestedKnnPropagatesParallelism) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  ScanGrainGuard grains(/*scanGrainVectors=*/1, /*rescoreGrainCandidates=*/1);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "a" + std::to_string(i), "foo_w", "apple",
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});
  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "b" + std::to_string(i), "foo_w", "apple",
                    "embedding_v", std::vector<float>{(float)i + 0.5f, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.embedding_v"), nullptr)
        << "segment 0 fell back to flat (below IVF training floor)";
  }

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.limit(10).getNumber().fields({"id"});
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "foo_w", "apple"),
                    qb::knn(cur.mr(), "embedding_v", {40.3f, 0.0f, 0.0f, 0.0f}, 5,
                            /*nprobe=*/0, /*exact=*/false, /*refineCandidates=*/0,
                            /*minScanFraction=*/1.0f)});

  int64_t before = KnnQuery::parallelScanRoundsForTests.load(std::memory_order_relaxed);
  req->execute(/*parallel=*/true);
  EXPECT_GT(KnnQuery::parallelScanRoundsForTests.load(std::memory_order_relaxed), before)
      << "nested kNN never spawned parallel scan tasks";
  EXPECT_EQ(req->getMatchCount(), 5);
  req->done();
}

// Unfiltered queries on a segment with deletes use the rank-space liveness
// bitmap as the whole FAISS selector, built ONCE per liveDocs generation and
// cached on the aux reader: a second query must not rebuild it, and a commit
// with new deletes must.  Results stay correct against the exact contract in
// both generations (the bitmap IS the eligibility filtering here - a stale
// or mis-keyed cache would surface deleted docs).
TEST_F(KnnQueryTest, ivfDeletesUseCachedRankLiveBitmap) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "d" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_EQ(reader->segments().size(), 1u);
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.embedding_v"), nullptr)
        << "segment fell back to flat (below IVF training floor)";
  }
  std::vector<std::string> dels{"d40"};
  h.deleteByIds(dels, UpdateMessage::COMMIT);

  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_EQ(reader->segments().size(), 1u);
    ASSERT_NE(reader->segments()[0].liveDocs(), nullptr) << "delete did not produce liveDocs";
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.embedding_v"), nullptr)
        << "IVF overlay missing after delete commit";
  }

  auto runAndCheck = [&](const std::string& deletedId) {
    auto* req = makeKnnReq(*luxirNode, "embedding_v", {40.3f, 0.0f, 0.0f, 0.0f}, 5,
                           /*nprobe=*/0, /*refineCandidates=*/200,
                           /*exact=*/false, /*minScanFraction=*/1.0f);
    req->execute();
    auto ids = resultIds(*req);
    req->done();
    auto* exact = makeKnnReq(*luxirNode, "embedding_v", {40.3f, 0.0f, 0.0f, 0.0f}, 5,
                             /*nprobe=*/0, /*refineCandidates=*/200, /*exact=*/true);
    exact->execute();
    auto exactIds = resultIds(*exact);
    exact->done();
    EXPECT_EQ(ids, exactIds);
    for (const auto& id : ids) EXPECT_NE(id, deletedId);
  };

  int64_t builds0 = KnnQuery::rankLiveBitmapBuildsForTests.load(std::memory_order_relaxed);
  runAndCheck("d40");
  int64_t builds1 = KnnQuery::rankLiveBitmapBuildsForTests.load(std::memory_order_relaxed);
  EXPECT_EQ(builds1, builds0 + 1) << "first query builds the bitmap";
  runAndCheck("d40");
  int64_t builds2 = KnnQuery::rankLiveBitmapBuildsForTests.load(std::memory_order_relaxed);
  EXPECT_EQ(builds2, builds1) << "second query must reuse the cached bitmap";

  std::vector<std::string> dels2{"d41"};
  h.deleteByIds(dels2, UpdateMessage::COMMIT);
  runAndCheck("d41");
  int64_t builds3 = KnnQuery::rankLiveBitmapBuildsForTests.load(std::memory_order_relaxed);
  EXPECT_EQ(builds3, builds2 + 1) << "new liveGen must rebuild the bitmap";
}

// Filtered query on an IVF segment with deletes: the domain is filter
// intersect liveDocs (NOT the liveDocs docset itself), so eligibility goes
// through the resolve-based selector while the allocator's live accounting
// reads the cached bitmap.  Per the live-filtered domain contract the
// engines never re-check liveDocs - a violation would surface the deleted
// doc here.
TEST_F(KnnQueryTest, ivfFilteredQueryWithDeletesMatchesExact) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 160; i++) {
    h.index(flatdoc("id", "d" + std::to_string(i),
                    "color_s", (i % 2 == 0) ? "red" : "blue",
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.embedding_v"), nullptr)
        << "segment fell back to flat (below IVF training floor)";
  }
  // d40 is red and nearest the query; deleting it must remove it even
  // though it passes the filter.
  std::vector<std::string> dels{"d40"};
  h.deleteByIds(dels, UpdateMessage::COMMIT);

  auto makeFiltered = [&](bool exact) {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    configKnn(cur, "embedding_v", {40.3f, 0.0f, 0.0f, 0.0f}, 5,
              /*nprobe=*/0, /*refineCandidates=*/200,
              exact, exact ? 0.0f : 1.0f);
    addMatchFilter(cur, "color_s", "red");
    return req;
  };

  auto* req = makeFiltered(false);
  req->execute();
  auto ids = resultIds(*req);
  req->done();

  auto* exact = makeFiltered(true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();

  ASSERT_EQ(ids.size(), 5u);
  EXPECT_EQ(ids, exactIds);
  EXPECT_EQ(ids[0], "d42");  // nearest red after d40's deletion
  for (const auto& id : ids) {
    EXPECT_NE(id, "d40") << "deleted doc must not appear despite passing the filter";
    EXPECT_NE(id, "d41") << "blue doc must not pass the red filter";
  }
}

TEST_F(KnnQueryTest, filterCacheMatchesUncachedExactAndIvfWithDeletes) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  FilterCacheConfig cacheConfig;
  cacheConfig.maxBytes = 4 * 1024 * 1024;
  cacheConfig.minSegmentDocs = 0;
  h.getIndexWriter()->filterCache = std::make_shared<FilterCache>(cacheConfig);
  for (int i = 0; i < 160; i++) {
    h.index(flatdoc(
        "id", "d" + std::to_string(i),
        "embedding_v",
        std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});
  std::vector<std::string> deletes{"d40"};
  h.deleteByIds(deletes, UpdateMessage::COMMIT);
  auto reader = h.getIndexWriter()->getIndexReader();
  ASSERT_NE(reader->segments()[0].liveDocs(), nullptr);
  ASSERT_NE(reader->segments()[0].getAuxReader("vec.embedding_v"), nullptr);
  auto cache = h.getIndexWriter()->getFilterCache();

  auto run = [&](bool exact) {
    auto* req = makeKnnFilterReq(
        *luxirNode, "embedding_v", {40.3f, 0.0f, 0.0f, 0.0f}, 5,
        /*nprobe=*/0, /*refineCandidates=*/200, exact,
        exact ? 0.0f : 1.0f);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    auto ids = resultIds(*req);
    std::sort(ids.begin(), ids.end());
    req->done();
    return ids;
  };

  for (bool exact : {true, false}) {
    auto uncached = run(exact);
    auto beforeBuild = cache->counters();
    EXPECT_EQ(uncached, run(exact));
    auto built = cache->counters();
    EXPECT_GT(built.builds, beforeBuild.builds);
    EXPECT_EQ(uncached, run(exact));
    EXPECT_GT(cache->counters().readerStableHits,
              built.readerStableHits);
    EXPECT_EQ(uncached.end(),
              std::find(uncached.begin(), uncached.end(), "d40"));
  }
}

TEST_F(KnnQueryTest, wholeReaderCountAndFieldSortSkipAnnPrepare) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  FilterCacheConfig cacheConfig;
  cacheConfig.maxBytes = 4 * 1024 * 1024;
  cacheConfig.minSegmentDocs = 0;
  cacheConfig.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  h.getIndexWriter()->filterCache = cache;
  for (int32_t i = 0; i < 64; i++) {
    h.index(flatdoc(
        "id", "d" + std::to_string(i), "rank_i", (int64_t)(64 - i),
        "group_s", (i & 1) == 0 ? "keep" : "drop",
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  h.commit({"*"});

  auto runCount = [&](std::span<const float> vector, int32_t k,
                      bool foldedFilter) {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q").getNumber().limit(0);
    cur.rawQuery() = qb::knn(cur.mr(), "embedding_v", vector, k, 0,
                             /*exact=*/true);
    if (foldedFilter) addMatchFilter(cur, "group_s", "keep");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    int64_t count = req->getMatchCount("q");
    req->done();
    return count;
  };
  auto runFieldSort = [&](std::span<const float> vector, int32_t k) {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main")
                    .topDocs("q").fields({"id"}).getNumber().limit(4);
    cur.rawQuery() = qb::knn(cur.mr(), "embedding_v", vector, k, 0,
                             /*exact=*/true);
    qb::sort(cur, "rank_i", qb::ASC);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    auto ids = resultIds(*req);
    int64_t count = req->getMatchCount("q");
    req->done();
    return std::pair(count, std::move(ids));
  };

  std::array<float, 2> countVector{0.0f, 0.0f};
  std::array<float, 2> sortVector{0.25f, 0.0f};
  int64_t prepareStart = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  int64_t uncachedCount;
  std::pair<int64_t, std::vector<std::string>> uncachedSort;
  {
    WholeMembershipPlanGuard guard(true);
    uncachedCount = runCount(countVector, 7, false);
    uncachedSort = runFieldSort(sortVector, 8);
  }
  ASSERT_EQ(7, uncachedCount);
  ASSERT_EQ(8, uncachedSort.first);
  ASSERT_EQ((std::vector<std::string>{"d7", "d6", "d5", "d4"}),
            uncachedSort.second);
  ASSERT_EQ(prepareStart + 2,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));

  auto beforeCountBuild = cache->counters();
  EXPECT_EQ(uncachedCount, runCount(countVector, 7, false));
  auto countBuilt = cache->counters();
  EXPECT_EQ(beforeCountBuild.builds + 1, countBuilt.builds);
  int64_t afterCountBuild = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  EXPECT_EQ(uncachedCount, runCount(countVector, 7, false));
  EXPECT_EQ(afterCountBuild,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_EQ(countBuilt.readerStableHits + 1,
            cache->counters().readerStableHits);

  auto beforeSortBuild = cache->counters();
  EXPECT_EQ(uncachedSort, runFieldSort(sortVector, 8));
  auto sortBuilt = cache->counters();
  EXPECT_EQ(beforeSortBuild.builds + 1, sortBuilt.builds);
  int64_t afterSortBuild = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  EXPECT_EQ(uncachedSort, runFieldSort(sortVector, 8));
  EXPECT_EQ(afterSortBuild,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_EQ(sortBuilt.readerStableHits + 1,
            cache->counters().readerStableHits);

  std::array<float, 2> foldedVector{1.0f, 0.0f};
  EXPECT_EQ(5, runCount(foldedVector, 5, true));
  auto foldedBuilt = cache->counters();
  int64_t afterFoldedBuild = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  EXPECT_EQ(5, runCount(foldedVector, 5, true));
  EXPECT_EQ(afterFoldedBuild,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_EQ(foldedBuilt.readerStableHits + 1,
            cache->counters().readerStableHits);

  auto beforeScored = cache->counters();
  int64_t scoredPrepareStart = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  for (int repeat = 0; repeat < 3; repeat++) {
    auto* req = makeKnnReq(
        *luxirNode, "embedding_v", {31.0f, 0.0f}, 6,
        /*nprobe=*/0, /*refineCandidates=*/0, /*exact=*/true);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    EXPECT_EQ(6u, resultIds(*req).size());
    req->done();
  }
  auto afterScored = cache->counters();
  EXPECT_EQ(scoredPrepareStart + 3,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_EQ(beforeScored.admissions, afterScored.admissions);
  EXPECT_EQ(beforeScored.builds, afterScored.builds);
  EXPECT_EQ(beforeScored.readerStableHits, afterScored.readerStableHits);
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST_F(KnnQueryTest, wholeReaderRefreshAndDeleteOnlyRefreshRebuild) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  FilterCacheConfig cacheConfig;
  cacheConfig.maxBytes = 4 * 1024 * 1024;
  cacheConfig.minSegmentDocs = 0;
  cacheConfig.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  h.getIndexWriter()->filterCache = cache;
  for (int32_t i = 0; i < 32; i++) {
    h.index(flatdoc(
        "id", "d" + std::to_string(i), "rank_i", (int64_t)i,
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  h.commit({"*"});
  std::array<float, 2> queryVector{0.0f, 0.0f};

  auto run = [&]() {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main")
                    .topDocs("q").fields({"id"}).getNumber().limit(6);
    cur.rawQuery() = qb::knn(cur.mr(), "embedding_v", queryVector, 6, 0,
                             /*exact=*/true);
    qb::sort(cur, "rank_i", qb::ASC);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    auto result = resultIds(*req);
    req->done();
    return result;
  };

  EXPECT_EQ((std::vector<std::string>{"d0", "d1", "d2", "d3", "d4", "d5"}),
            run());
  int64_t initialPrepare = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  EXPECT_EQ((std::vector<std::string>{"d0", "d1", "d2", "d3", "d4", "d5"}),
            run());
  EXPECT_EQ(initialPrepare,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  auto firstReader = h.getIndexWriter()->getIndexReader();

  h.index(flatdoc("id", "far", "rank_i", (int64_t)100,
                  "embedding_v", std::vector<float>{100.0f, 0.0f}));
  h.commit({"*"});
  auto secondReader = h.getIndexWriter()->getIndexReader();
  ASSERT_GT(secondReader->commitTime(), firstReader->commitTime());
  auto beforeRefreshBuild = cache->counters();
  EXPECT_EQ((std::vector<std::string>{"d0", "d1", "d2", "d3", "d4", "d5"}),
            run());
  EXPECT_EQ(beforeRefreshBuild.readerStableRefreshes + 1,
            cache->counters().readerStableRefreshes);

  std::vector<std::string> deletes{"d0"};
  h.deleteByIds(deletes, UpdateMessage::COMMIT);
  auto thirdReader = h.getIndexWriter()->getIndexReader();
  ASSERT_GT(thirdReader->commitTime(), secondReader->commitTime());
  auto beforeDeleteBuild = cache->counters();
  EXPECT_EQ((std::vector<std::string>{"d1", "d2", "d3", "d4", "d5", "d6"}),
            run());
  EXPECT_EQ(beforeDeleteBuild.readerStableRefreshes + 1,
            cache->counters().readerStableRefreshes);
  int64_t deleteBuildPrepare = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  EXPECT_EQ((std::vector<std::string>{"d1", "d2", "d3", "d4", "d5", "d6"}),
            run());
  EXPECT_EQ(deleteBuildPrepare,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_GE(cache->counters().readerStableRetires, 2u);
  ASSERT_NO_THROW(cache->validateForTest());
}

TEST_F(KnnQueryTest, fusionFieldSortDoesNotSightWholeReaderCache) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  FilterCacheConfig cacheConfig;
  cacheConfig.maxBytes = 4 * 1024 * 1024;
  cacheConfig.minSegmentDocs = 0;
  cacheConfig.admissionThreshold = 1;
  auto cache = std::make_shared<FilterCache>(cacheConfig);
  h.getIndexWriter()->filterCache = cache;
  for (int32_t i = 0; i < 16; i++) {
    h.index(flatdoc(
        "id", "d" + std::to_string(i), "rank_i", (int64_t)(16 - i),
        "embedding_v", std::vector<float>{(float)i, 0.0f}));
  }
  h.commit({"*"});

  auto run = [&]() {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    req->collection("main");
    auto& fusion = req->topDocs("f").rawOp().kind.emplace<api::Fusion>();
    fusion.limit = 4;
    fusion.get_number = true;
    fusion.rrf.emplace().k = 60;
    auto* fields = build::allocArray(fusion.fields, 1, req->mr);
    fields[0] = build::arenaStr(req->mr, "id");

    using Pair = std::pair<std::string_view, api::TopDocs>;
    auto* sources = (Pair*) req->mr.allocate(sizeof(Pair), alignof(Pair));
    std::uninitialized_value_construct_n(sources, 1);
    sources[0].first = build::arenaStr(req->mr, "vec");
    fusion.sources = api::map_view<std::string_view, api::TopDocs>(
        std::span<const Pair>(sources, 1));
    auto& source = sources[0].second;
    source.limit = 4;
    source.get_number = true;
    auto* query = (api::Query*) req->mr.allocate(
        sizeof(api::Query), alignof(api::Query));
    new (query) api::Query(qb::knn(
        req->mr, "embedding_v", {0.0f, 0.0f}, 6, 0,
        /*exact=*/true));
    source.query = query;
    auto* sorts = build::allocArray(source.sorts, 1, req->mr);
    sorts[0].expr = build::arenaStr(req->mr, "rank_i");
    sorts[0].dir = api::SortSpec_::SortDir::ASC;

    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    auto ids = resultIds(*req, "f");
    req->done();
    return ids;
  };

  int64_t prepares = KnnQuery::prepareCallsForTests.load(
      std::memory_order_relaxed);
  for (int repeat = 0; repeat < 3; repeat++) {
    EXPECT_EQ((std::vector<std::string>{"d5", "d4", "d3", "d2"}),
              run());
  }
  EXPECT_EQ(prepares + 3,
            KnnQuery::prepareCallsForTests.load(std::memory_order_relaxed));
  EXPECT_EQ(0u, cache->counters().admissions);
  EXPECT_EQ(0u, cache->counters().buildAttempts);
  EXPECT_EQ(0u, cache->entryCountForTest());
}

TEST_F(KnnQueryTest, uncachedApproximateMembershipIsDeterministic) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  for (int i = 0; i < 160; i++) {
    h.index(flatdoc(
        "id", "d" + std::to_string(i),
        "embedding_v",
        std::vector<float>{(float)i, (float)(i % 7), 0.0f, 0.0f}));
  }
  h.commit({"*"});
  std::vector<std::string> deletes{"d40"};
  h.deleteByIds(deletes, UpdateMessage::COMMIT);
  auto pinned = h.getIndexWriter()->getIndexReader();

  std::vector<std::string> expected;
  for (int repeat = 0; repeat < 5; repeat++) {
    auto* req = makeKnnFilterReq(
        *luxirNode, "embedding_v", {40.3f, 0.0f, 0.0f, 0.0f}, 8,
        /*nprobe=*/1, /*refineCandidates=*/40, /*exact=*/false);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    auto ids = resultIds(*req);
    std::sort(ids.begin(), ids.end());
    req->done();
    if (repeat == 0) expected = ids;
    EXPECT_EQ(expected, ids);
    EXPECT_EQ(pinned->commitTime(),
              h.getIndexWriter()->getIndexReader()->commitTime());
  }
}

TEST_F(KnnQueryTest, selectiveFilterPreparedSkipMatchesCollectorRecheck) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 160; i++) {
    bool keep = (i % 5) == 0;
    h.index(flatdoc("id", "d" + std::to_string(i),
                    "keep_s", keep ? "yes" : "no",
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto makeReq = [&](bool wrapInBoolean) {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.getScores().getNumber().fields({"id"});
    auto knn = [&] {
      return qb::knn(cur.mr(), "embedding_v", {42.2f, 0.0f, 0.0f, 0.0f}, /*k=*/6,
                     /*nprobe=*/4, /*exact=*/false, /*refineCandidates=*/160);
    };
    cur.rawQuery() = wrapInBoolean ? qb::boolean(cur.mr(), /*required=*/{knn()}) : knn();
    addMatchFilter(cur, "keep_s", "yes");
    return req;
  };

  auto* skip = makeReq(false);
  skip->execute();
  ASSERT_OK(skip);
  auto skipIds = resultIds(*skip);
  auto skipScores = resultScores(*skip);

  // Folding gives the Boolean root an internal materialized filter domain,
  // so it advertises that its prepared output is already a subset. Keep the
  // comparison arm genuinely different: the passive filter builds the outer
  // domain, while the filter-free Boolean wrapper does not advertise subset
  // membership and the collector rechecks that domain.
  auto* recheck = makeReq(true);
  {
    TopDocsFilterFoldGuard passive(/*disabled=*/true);
    recheck->execute();
  }
  ASSERT_OK(recheck);
  auto recheckIds = resultIds(*recheck);
  auto recheckScores = resultScores(*recheck);

  ASSERT_EQ(skipIds.size(), 6u);
  EXPECT_EQ(skipIds, recheckIds);
  ASSERT_EQ(skipScores.size(), recheckScores.size());
  for (size_t i = 0; i < skipScores.size(); i++) {
    EXPECT_EQ(std::bit_cast<uint32_t>(skipScores[i]),
              std::bit_cast<uint32_t>(recheckScores[i])) << "i=" << i;
  }
  for (const auto& id : skipIds) {
    ASSERT_GE(id.size(), 2u);
    int doc = std::stoi(id.substr(1));
    EXPECT_EQ(doc % 5, 0) << id;
  }

  skip->done();
  recheck->done();
}

// A NaN/Inf query vector is rejected loudly: NaN scores would break the
// strict-total-order contract the bounded candidate cuts and result sorts
// rely on.
TEST_F(KnnQueryTest, nanQueryVectorReturnsErrorResponse) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v",
                         {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 1);
  {
    ExpectLog quiet("Search request failed:");
    req->execute();
  }
  EXPECT_EQ(req->getMatchCount(), 0);
  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_NE(req->responses[0]->proto.error.find("finite"), std::string::npos);
  req->done();
}

// A NaN stored vector must not poison the comparators (a non-total order
// makes the sorts UB and the bounded cuts fold-order dependent); finiteScore
// maps its score to the worst finite float so it deterministically ranks
// last (still collected - unlike -infinity, which the score collectors'
// sentinel would silently drop).
TEST_F(KnnQueryTest, nanStoredVectorRanksLast) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::IP);

  h.index(flatdoc("id", std::string("good1"), "embedding_v", std::vector<float>{3, 0, 0}));
  h.index(flatdoc("id", std::string("bad"), "embedding_v",
                  std::vector<float>{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}));
  h.index(flatdoc("id", std::string("good2"), "embedding_v", std::vector<float>{2, 0, 0}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0}, 3);
  req->execute();
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "good1");
  EXPECT_EQ(ids[1], "good2");
  EXPECT_EQ(ids[2], "bad");
  req->done();
}

// The maxKnnCandidates host cap is a heuristic; exact is a contract and must
// not be silently truncated by it.  Without exact, the same capped request
// returns only cap docs.
TEST_F(KnnQueryTest, exactIgnoresMaxKnnCandidatesCap) {
  MaxKnnCandidatesGuard guard(2);

  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);
  for (int i = 0; i < 10; i++) {
    h.index(flatdoc("id", "doc" + std::to_string(i),
                    "embedding_v", std::vector<float>{(float)i, 0.0f, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto* capped = makeKnnReq(*luxirNode, "embedding_v", {0, 0, 0, 0}, 5);
  {
    ExpectLog quiet("at candidate cap");  // non-exact falls short of k at the cap
    capped->execute();
  }
  EXPECT_EQ(capped->getMatchCount(), 2) << "non-exact respects the host cap";
  capped->done();

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {0, 0, 0, 0}, 5,
                         /*nprobe=*/0, /*refineCandidates=*/0, /*exact=*/true);
  req->execute();
  EXPECT_EQ(req->getMatchCount(), 5) << "exact fulfills k despite the host cap";
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 5u);
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ(ids[(size_t)i], "doc" + std::to_string(i));
  }
  req->done();
}

TEST_F(KnnQueryTest, ivfPqMultiValuedUsesReverseMapAndCollapse) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/64);
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{0, 0, 0, 0}, {0.01f, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("g")));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{2, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{3, 0, 0, 0}}));
  for (int i = 0; i < 155; i++) {
    float x = 50.0f + (float)i;
    h.index(flatdoc("id", "f" + std::to_string(i), "emb_vs",
                    std::vector<std::vector<float>>{{
                        x, (float)(i % 7) * 0.1f, (float)(i % 13) * 0.05f, 0.25f}}));
  }
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {0, 0, 0, 0}, 3,
                         /*nprobe=*/4, /*refineCandidates=*/192);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 3);
  auto ids = resultIds(*req);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "a");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");
  for (const auto& id : ids) {
    EXPECT_NE(id, "g");
  }

  req->done();
}

// Multi-valued IVF segment under deletes: the cached rank-live bitmap is
// built by walking DELETED docs and must clear every rank a deleted doc
// owns (via the forward start/end-rank map).  Deleting the top doc "a"
// (two vectors - both ranks must drop) and the no-vector doc "g" (the
// deleted-doc-without-field skip) must leave the survivors matching exact.
TEST_F(KnnQueryTest, ivfPqMultiValuedDeletesClearAllRanks) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/64);
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{0, 0, 0, 0}, {0.01f, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("g")));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{2, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{3, 0, 0, 0}}));
  for (int i = 0; i < 155; i++) {
    float x = 50.0f + (float)i;
    h.index(flatdoc("id", "f" + std::to_string(i), "emb_vs",
                    std::vector<std::vector<float>>{{
                        x, (float)(i % 7) * 0.1f, (float)(i % 13) * 0.05f, 0.25f}}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_EQ(reader->segments().size(), 1u);
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.emb_vs"), nullptr)
        << "segment fell back to flat (below IVF training floor)";
  }

  std::vector<std::string> dels{"a", "g"};
  h.deleteByIds(dels, UpdateMessage::COMMIT);

  int64_t builds0 = KnnQuery::rankLiveBitmapBuildsForTests.load(std::memory_order_relaxed);
  auto* req = makeKnnReq(*luxirNode, "emb_vs", {0, 0, 0, 0}, 3,
                         /*nprobe=*/0, /*refineCandidates=*/200,
                         /*exact=*/false, /*minScanFraction=*/1.0f);
  req->execute();
  auto ids = resultIds(*req);
  req->done();
  auto* exact = makeKnnReq(*luxirNode, "emb_vs", {0, 0, 0, 0}, 3,
                           /*nprobe=*/0, /*refineCandidates=*/200, /*exact=*/true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();
  int64_t builds1 = KnnQuery::rankLiveBitmapBuildsForTests.load(std::memory_order_relaxed);
  EXPECT_EQ(builds1, builds0 + 1) << "one bitmap build for the IVF query";

  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids, exactIds);
  EXPECT_EQ(ids[0], "b");
  EXPECT_EQ(ids[1], "c");
  EXPECT_EQ(ids[2], "d");
}

// Forces the widen loop past round 1 on an IVF segment so the seen-fold
// fires: the pooled-hit set becomes per-segment eligible bitmaps (seeded
// from the rank-live bitmap, so the deleted near-doc must STAY excluded on
// deepen rounds) handed to FAISS as the NOT-pooled selector - depth rounds
// then return only fresh hits.  A 40-vector doc hogs the small first-round
// pool (refine_candidates=8 -> targetDocReq=8 docs, round 1 yields ~1);
// the loop must still surface the runner-up docs and match exact.
TEST_F(KnnQueryTest, ivfDeepenExcludesPooledAndDeleted) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/8);
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  std::vector<std::vector<float>> zvecs;
  for (int i = 0; i < 40; i++) {
    zvecs.push_back({(float)i * 0.001f, 0, 0, 0});
  }
  h.index(flatdoc("id", std::string("z"), "emb_vs", zvecs));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{2, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{3, 0, 0, 0}}));
  h.index(flatdoc("id", std::string("del"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0, 0}, {0.6f, 0, 0, 0}}));
  for (int i = 0; i < 155; i++) {
    float x = 50.0f + (float)i;
    h.index(flatdoc("id", "f" + std::to_string(i), "emb_vs",
                    std::vector<std::vector<float>>{{
                        x, (float)(i % 7) * 0.1f, (float)(i % 13) * 0.05f, 0.25f}}));
  }
  h.commit({"*"});
  {
    auto reader = h.getIndexWriter()->getIndexReader();
    ASSERT_EQ(reader->segments().size(), 1u);
    ASSERT_NE(reader->segments()[0].getAuxReader("vec.emb_vs"), nullptr)
        << "segment fell back to flat (below IVF training floor)";
  }
  std::vector<std::string> dels{"del"};
  h.deleteByIds(dels, UpdateMessage::COMMIT);

  int64_t folds0 = KnnQuery::seenFoldsForTests.load(std::memory_order_relaxed);
  int64_t stale0 = KnnQuery::staleHitsForTests.load(std::memory_order_relaxed);
  auto* req = makeKnnReq(*luxirNode, "emb_vs", {0, 0, 0, 0}, 4,
                         /*nprobe=*/0, /*refineCandidates=*/8,
                         /*exact=*/false, /*minScanFraction=*/1.0f);
  req->execute();
  auto ids = resultIds(*req);
  req->done();
  int64_t folds1 = KnnQuery::seenFoldsForTests.load(std::memory_order_relaxed);
  EXPECT_GT(folds1, folds0) << "query never deepened; fold path unexercised";
  // The eligible contract is binding: post-fold rounds must return only
  // fresh hits.  A stale hit means the selector wiring quietly disconnected
  // (deepen rounds would re-return the pool and the results would STILL
  // look right, because markSeen dedups - this counter is the tripwire).
  int64_t stale1 = KnnQuery::staleHitsForTests.load(std::memory_order_relaxed);
  EXPECT_EQ(stale1, stale0) << "an engine returned an already-pooled rank post-fold";

  auto* exact = makeKnnReq(*luxirNode, "emb_vs", {0, 0, 0, 0}, 4,
                           /*nprobe=*/0, /*refineCandidates=*/8, /*exact=*/true);
  exact->execute();
  auto exactIds = resultIds(*exact);
  exact->done();

  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids, exactIds);
  EXPECT_EQ(ids[0], "z");
  EXPECT_EQ(ids[1], "b");
  EXPECT_EQ(ids[2], "c");
  EXPECT_EQ(ids[3], "d");
  for (const auto& id : ids) EXPECT_NE(id, "del");
}

TEST_F(KnnQueryTest, ivfPqCosineRawColumnRescoreNormalizes) {
  IvfPqAuxGuard guard(/*nlist=*/4, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/4, /*minTraining=*/16, /*refineRatio=*/64);
  CollectionHelper h("main");
  installVecSchemaCosineRaw(h.collection());

  h.index(flatdoc("id", std::string("east"), "embedding_v", std::vector<float>{5, 0, 0, 0}));
  h.index(flatdoc("id", std::string("b"), "embedding_v", std::vector<float>{3, 4, 0, 0}));
  h.index(flatdoc("id", std::string("c"), "embedding_v", std::vector<float>{4, 3, 0, 0}));
  for (int i = 0; i < 157; i++) {
    h.index(flatdoc("id", "f" + std::to_string(i), "embedding_v",
                    std::vector<float>{-1.0f - (float)i * 0.01f,
                                       10.0f + (float)(i % 11),
                                       (float)(i % 5) * 0.1f,
                                       0.25f}));
  }
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {7, 0, 0, 0}, 3,
                         /*nprobe=*/4, /*refineCandidates=*/192);
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

TEST_F(KnnQueryTest, requestedNProbeCapsBreadthDeepening) {
  IvfPqAuxGuard guard(/*nlist=*/2, /*m=*/2, /*bits=*/2,
                      /*nprobe=*/1, /*minTraining=*/16, /*refineRatio=*/32);
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  for (int i = 0; i < 160; i++) {
    float off = (float)i * 0.001f;
    h.index(flatdoc("id", "red" + std::to_string(i), "color_s", "red",
                    "embedding_v", std::vector<float>{off, off, 0.0f, 0.0f}));
  }
  for (int i = 0; i < 160; i++) {
    float off = (float)i * 0.001f;
    h.index(flatdoc("id", "blue" + std::to_string(i), "color_s", "blue",
                    "embedding_v", std::vector<float>{1000.0f + off, off, 0.0f, 0.0f}));
  }
  h.commit({"*"});

  auto buildBlue = [&](int32_t nprobe) {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    configKnn(cur, "embedding_v", {0, 0, 0, 0}, 3, nprobe, /*refineCandidates=*/96);
    addMatchFilter(cur, "color_s", "blue");
    return req;
  };

  auto* capped = buildBlue(/*nprobe=*/1);
  capped->execute();
  EXPECT_EQ(capped->getMatchCount(), 0)
      << "request nprobe=1 should cap breadth and not broaden into the blue list";
  capped->done();

  auto* broaden = buildBlue(/*nprobe=*/0);
  broaden->execute();
  EXPECT_EQ(broaden->getMatchCount(), 3)
      << "without request nprobe, the host may broaden from the index default";
  auto ids = resultIds(*broaden);
  ASSERT_EQ(ids.size(), 3u);
  for (const auto& id : ids) {
    EXPECT_TRUE(id.starts_with("blue"));
  }
  broaden->done();
}

// Dimension mismatch: query vector has different dims than the index.
// Surfaces as an exception out of search execution.  SearchEngine currently
// logs+swallows the exception (so execute() returns), but we verify by
// checking that the response carries no docs and the engine logged the error.
//
// Note: this used to crash the arena.  TopDocsReq's ctor called createWeight,
// and protobuf's Arena::Create registered ~T() before the ctor body ran, so a
// throwing ctor left a half-constructed object scheduled for cleanup.  The fix
// moved createWeight to TopDocsReq::init() (runs after construction).  Luxir now
// also allocates ops via luxir::arenaCreate, which registers the destructor only
// after the ctor succeeds, so a throwing arena ctor is safe regardless.
TEST_F(KnnQueryTest, dimMismatchReturnsErrorResponse) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0, 0}));
  h.commit({"*"});

  // Query is 3-d but index is 4-d.
  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0}, 1);
  {
    ExpectLog quiet("Search request failed:");
    req->execute();
  }
  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_EQ(resultIds(*req).size(), 0u);

  // A single response is returned with the error string populated.
  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_NE(req->responses[0]->proto.error.find("dims 3 do not match segment dims 4"), std::string::npos);

  req->done();
}

TEST_F(KnnQueryTest, emptyQueryVectorReturnsErrorResponse) {
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "embedding_v", std::vector<float>{1, 0, 0}));
  h.commit();

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {}, 1);
  {
    ExpectLog quiet("Search request failed:");
    req->execute();
  }
  EXPECT_EQ(req->getMatchCount(), 0);
  EXPECT_TRUE(resultIds(*req).empty());
  ASSERT_EQ(req->responses.size(), 1u);
  EXPECT_NE(req->responses[0]->proto.error.find("non-empty query vector"), std::string::npos);

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
  installVecSchema(h.collection(), api::VectorMetric::COSINE);

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
  auto* req = makeKnnReq(*luxirNode, "embedding_v", {7, 0, 0}, 4);
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
  installVecSchema(h.collection(), api::VectorMetric::COSINE);

  {
    ExpectLog quiet("skipping zero / near-zero vector");
    h.index(flatdoc("id", std::string("zero"), "embedding_v", std::vector<float>{0, 0, 0}));
    h.commit();
  }

  h.index(flatdoc("id", std::string("east"), "embedding_v", std::vector<float>{2, 0, 0}));
  h.index(flatdoc("id", std::string("north"), "embedding_v", std::vector<float>{0, 3, 0}));
  h.commit();

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {7, 0, 0}, 2);
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
  installVecSchema(h.collection(), api::VectorMetric::L2);

  // Squared distances from query [0,0]:
  //   "exact"  [0,0]    -> 0   -> score 1.0
  //   "one"    [1,0]    -> 1   -> score 0.5
  //   "two"    [2,0]    -> 4   -> score 0.2
  h.index(flatdoc("id", std::string("exact"), "embedding_v", std::vector<float>{0, 0}));
  h.index(flatdoc("id", std::string("one"), "embedding_v", std::vector<float>{1, 0}));
  h.index(flatdoc("id", std::string("two"), "embedding_v", std::vector<float>{2, 0}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {0, 0}, 3);
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
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t ntotal) {
    return std::make_unique<BreadthSplitEngine>(flat, ntotal, 7);
  });
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::IP);

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

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0}, 4);
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
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t) {
    return std::make_unique<QuantizingEngine>(flat);
  });
  CollectionHelper h("main");
  installMultiVecSchema(h.collection(), api::VectorMetric::L2);

  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 0}, {0.99f, 0, 0}, {0.98f, 0, 0}}));
  h.index(flatdoc("id", std::string("b"), "emb_vs",
                  std::vector<std::vector<float>>{{0.5f, 0, 0}}));
  h.index(flatdoc("id", std::string("c"), "emb_vs",
                  std::vector<std::vector<float>>{{0.4f, 0, 0}}));
  h.index(flatdoc("id", std::string("d"), "emb_vs",
                  std::vector<std::vector<float>>{{0.3f, 0, 0}}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "emb_vs", {1, 0, 0}, 3);
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

// A mixed merge (some segments exact, some approximate - the flat+IVF
// composition shape) must rescore ONLY the approximate segments' hits.  The
// wrapper shifts the exact-claimed segment's scores by +0.25: those scores
// surviving to the response proves their column re-read was skipped, while
// the quantized segment's hits coming back at full precision proves the
// rescore still ran where needed.
TEST_F(KnnQueryTest, mixedExactSegmentsSkipColumnRescore) {
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t) {
    return std::make_unique<PartiallyExactEngine>(flat, 0, 0.25f);
  });
  CollectionHelper h("main");
  installVecSchema(h.collection(), api::VectorMetric::L2);

  // Segment 0: the exact-claimed segment.
  h.index(flatdoc("id", std::string("a1"), "embedding_v", std::vector<float>{1.0f, 0, 0}));
  h.index(flatdoc("id", std::string("a2"), "embedding_v", std::vector<float>{0.9f, 0, 0}));
  h.commit();
  // Segment 1: the approximate segment.  Both scores quantize into the same
  // 0.5 bucket, so only a column rescore restores their order and values.
  h.index(flatdoc("id", std::string("b1"), "embedding_v", std::vector<float>{0.95f, 0, 0}));
  h.index(flatdoc("id", std::string("b2"), "embedding_v", std::vector<float>{0.5f, 0, 0}));
  h.commit({"*"});

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {1, 0, 0}, 4);
  req->execute();

  EXPECT_EQ(req->getMatchCount(), 4);
  auto ids = resultIds(*req);
  auto scores = resultScores(*req);
  ASSERT_EQ(ids.size(), 4u);
  EXPECT_EQ(ids[0], "a1");
  EXPECT_EQ(ids[1], "a2");
  EXPECT_EQ(ids[2], "b1");
  EXPECT_EQ(ids[3], "b2");
  ASSERT_EQ(scores.size(), 4u);
  EXPECT_NEAR(scores[0], 1.25f, 1e-5);                 // 1.0 + shift: not re-read
  EXPECT_NEAR(scores[1], 1.0f / 1.01f + 0.25f, 1e-5);  // shifted: not re-read
  EXPECT_NEAR(scores[2], 1.0f / 1.0025f, 1e-5);        // rescored from column
  EXPECT_NEAR(scores[3], 1.0f / 1.25f, 1e-5);          // rescored from column

  req->done();
}

// COSINE with normalize_on_write=false stores RAW vectors in the column, so
// the rescore path must renormalize each candidate on the fly (the persisted
// cosineNormalizeColumnOnRescore policy).  The quantizing wrapper forces a
// rescore; correct cosine order AND scores prove the raw-column normalization
// ran (without it, rescore would return raw dot products like 5.0).
TEST_F(KnnQueryTest, cosineRawColumnRescoreNormalizes) {
  EngineWrapperGuard guard([](VectorEngine& flat, int64_t) {
    return std::make_unique<QuantizingEngine>(flat);
  });
  CollectionHelper h("main");
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

  auto* req = makeKnnReq(*luxirNode, "embedding_v", {7, 0, 0}, 3);
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
