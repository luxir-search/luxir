#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "luxir/reader/SkipStats.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

// TopDocs and Fusion as facet bucket children: one independent ranked list per
// bucket, delivered in the bucket's slot of the parent FacetResult's ops entry.
class FacetTopDocsTest : public LuxirTest {
protected:
  static const api::FacetResult& rootFacet(const LocalReq& req,
                                           std::string_view name) {
    return *req.responses[0]->proto.ops.at(name)->facetResult();
  }

  static const api::FacetResult& topFacet(const LocalReq& req,
                                          std::string_view facet) {
    return *req.docList("q")->ops.at(facet)->facetResult();
  }

  static std::vector<std::string> bucketIds(const api::FacetResult& result) {
    std::vector<std::string> ids;
    for (auto id : std::get<api::ColStr>(result.bucket_ids->kind).v) {
      ids.emplace_back(id);
    }
    return ids;
  }

  // The bucket child's DocList: ops[name] is an ArrVal with one slot per
  // emitted bucket.
  static const api::DocList* bucketDocs(const api::FacetResult& result,
                                        std::string_view op, size_t bucket) {
    const auto* val = result.ops.find(op);
    if (val == nullptr) return nullptr;
    const auto* arr = std::get_if<api::ArrVal>(&(**val).kind);
    if (arr == nullptr || bucket >= arr->v.size()) return nullptr;
    return arr->v[bucket].docList();
  }

  static std::vector<std::string> ids(const api::DocList& docs) {
    std::vector<std::string> out;
    const auto* col = docs.columns.find("id");
    if (col == nullptr) return out;
    for (auto id : std::get<api::ColStr>(col->kind).v) out.emplace_back(id);
    return out;
  }

  static void sortBy(OpCursor& cursor, std::string_view expr,
                     api::SortSpec::SortDir dir) {
    auto& topDocs = std::get<api::TopDocs>(cursor.rawOp().kind);
    auto* sorts = api::build::allocArray(topDocs.sorts, 1, cursor.mr());
    sorts[0].expr = api::build::arenaStr(cursor.mr(), expr);
    sorts[0].dir = dir;
  }

  // Bucket "big" holds 300 docs (n_i = 0..299), 256 of them in the first
  // segment so one segment run is exactly 256 rows; bucket "small" holds 3.
  static void indexBigBucket(CollectionHelper& helper) {
    std::vector<Doc> first;
    for (int i = 0; i < 256; i++) {
      first.push_back(flatdoc("id", "d" + std::to_string(i), "cat_s", "big",
                              "n_i", (int64_t)i));
    }
    ASSERT_TRUE(helper.indexAll(first, UpdateMessage::COMMIT).success);
    std::vector<Doc> second;
    for (int i = 256; i < 300; i++) {
      second.push_back(flatdoc("id", "d" + std::to_string(i), "cat_s", "big",
                               "n_i", (int64_t)i));
    }
    for (int i = 0; i < 3; i++) {
      second.push_back(flatdoc("id", "s" + std::to_string(i), "cat_s", "small",
                               "n_i", (int64_t)(1000 + i)));
    }
    ASSERT_TRUE(helper.indexAll(second, UpdateMessage::COMMIT).success);
  }
};

// Regression seed: a prepared per-bucket TopDocs must see every segment's
// bucket domain, and the result lands in the bucket slot.
TEST_F(FacetTopDocsTest, bucketPreparedTopDocsRetainsSegmentDomains) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a", "cat_s", "x"),
    flatdoc("id", "b"),
    flatdoc("id", "c"),
  }, UpdateMessage::COMMIT);
  helper.indexAll(std::array{
    flatdoc("id", "d"),
    flatdoc("id", "e"),
    flatdoc("id", "f", "cat_s", "x"),
  }, UpdateMessage::COMMIT);
  ASSERT_EQ(2u, helper.getIndexWriter()->getIndexReader()->segments().size());

  auto req = localReq(luxirNode->getSearchEngine());
  req->testForcePrepare = true;
  auto& topDocs = req->collection("main").topDocs("q").allQuery().limit(0);
  auto& facet = topDocs.facet("f", "cat_s").limit(10);
  facet.topDocs("bucket_docs").allQuery().fields({"id"}).getNumber().limit(-1);
  req->execute();
  ASSERT_OK(req);

  const auto& result = topFacet(*req, "f");
  ASSERT_EQ(1u, result.counts.size());
  EXPECT_EQ(2, result.counts[0]);
  const auto* docs = bucketDocs(result, "bucket_docs", 0);
  ASSERT_NE(nullptr, docs);
  EXPECT_EQ(2, docs->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"a", "f"}), ids(*docs));
}

// Each bucket ranks independently over its own domain: by the child's own
// query score, or by its own sort.
TEST_F(FacetTopDocsTest, stringFacetBucketsRankIndependently) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a", "cat_s", "x", "body_w", "apple apple", "price_i", (int64_t)5),
    flatdoc("id", "b", "cat_s", "x", "body_w", "apple", "price_i", (int64_t)7),
    flatdoc("id", "c", "cat_s", "x", "body_w", "pear", "price_i", (int64_t)9),
  }, UpdateMessage::COMMIT);
  helper.indexAll(std::array{
    flatdoc("id", "d", "cat_s", "y", "body_w", "apple", "price_i", (int64_t)1),
    flatdoc("id", "e", "cat_s", "y", "body_w", "pear", "price_i", (int64_t)2),
  }, UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  auto& top = req->collection("main").topDocs("q").allQuery().limit(0);
  auto& facet = top.facet("f", "cat_s").limit(-1);
  facet.topDocs("hits").matchQuery("body_w", "apple").limit(10).getNumber()
      .fields({"id"});
  auto& cheapest = facet.topDocs("cheapest").allQuery().limit(1).fields({"id"});
  sortBy(cheapest, "price_i", api::SortSpec::SortDir::ASC);
  req->execute();
  ASSERT_OK(req);

  const auto& result = topFacet(*req, "f");
  EXPECT_EQ((std::vector<std::string>{"x", "y"}), bucketIds(result));
  const auto* hitsX = bucketDocs(result, "hits", 0);
  const auto* hitsY = bucketDocs(result, "hits", 1);
  ASSERT_NE(nullptr, hitsX);
  ASSERT_NE(nullptr, hitsY);
  EXPECT_EQ(2, hitsX->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), ids(*hitsX));
  EXPECT_EQ(1, hitsY->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"d"}), ids(*hitsY));
  const auto* cheapX = bucketDocs(result, "cheapest", 0);
  const auto* cheapY = bucketDocs(result, "cheapest", 1);
  ASSERT_NE(nullptr, cheapX);
  ASSERT_NE(nullptr, cheapY);
  EXPECT_EQ((std::vector<std::string>{"a"}), ids(*cheapX));
  EXPECT_EQ((std::vector<std::string>{"d"}), ids(*cheapY));
}

// A bucket child never streams: every row assembles into the one final
// response, past the 256-row transport batch, with a 256-row segment run.
TEST_F(FacetTopDocsTest, bucketDocsAssembleIntoOneResponse) {
  CollectionHelper helper;
  indexBigBucket(helper);

  auto req = localReq(luxirNode->getSearchEngine());
  auto& top = req->collection("main").topDocs("q").allQuery().limit(0);
  auto& hits = top.facet("f", "cat_s").limit(-1)
      .topDocs("hits").allQuery().limit(-1).batchSize(1).getNumber()
      .fields({"id"});
  sortBy(hits, "n_i", api::SortSpec::SortDir::ASC);
  req->execute();
  ASSERT_OK(req);

  ASSERT_EQ(1u, req->responses.size());
  EXPECT_FALSE(req->responses[0]->proto.more);
  const auto& result = topFacet(*req, "f");
  EXPECT_EQ((std::vector<std::string>{"big", "small"}), bucketIds(result));
  const auto* big = bucketDocs(result, "hits", 0);
  ASSERT_NE(nullptr, big);
  EXPECT_EQ(300, big->found.value_or(-1));
  EXPECT_FALSE(big->more);
  auto bigIds = ids(*big);
  ASSERT_EQ(300u, bigIds.size());
  for (int i = 0; i < 300; i++) {
    EXPECT_EQ("d" + std::to_string(i), bigIds[(size_t)i]) << i;
  }
  const auto* small = bucketDocs(result, "hits", 1);
  ASSERT_NE(nullptr, small);
  EXPECT_EQ((std::vector<std::string>{"s0", "s1", "s2"}), ids(*small));
}

// The ancestry rule: a TopDocs nested under a bucket TopDocs has no slot of
// its own but still assembles into the final response.
TEST_F(FacetTopDocsTest, topDocsUnderBucketTopDocsAssemblesIntoOneResponse) {
  CollectionHelper helper;
  indexBigBucket(helper);

  auto req = localReq(luxirNode->getSearchEngine());
  auto& top = req->collection("main").topDocs("q").allQuery().limit(0);
  auto& outer = top.facet("f", "cat_s").limit(-1)
      .topDocs("outer").allQuery().limit(0).getNumber();
  auto& inner = outer.topDocs("inner").allQuery().limit(-1).batchSize(1)
      .getNumber().fields({"id"});
  sortBy(inner, "n_i", api::SortSpec::SortDir::ASC);
  req->execute();
  ASSERT_OK(req);

  ASSERT_EQ(1u, req->responses.size());
  const auto& result = topFacet(*req, "f");
  const auto* outerBig = bucketDocs(result, "outer", 0);
  ASSERT_NE(nullptr, outerBig);
  EXPECT_EQ(300, outerBig->found.value_or(-1));
  EXPECT_EQ(0, outerBig->row_count);
  const auto* innerBig = outerBig->ops.at("inner")->docList();
  ASSERT_NE(nullptr, innerBig);
  EXPECT_EQ(300, innerBig->found.value_or(-1));
  EXPECT_EQ(300, innerBig->row_count);
  auto innerIds = ids(*innerBig);
  ASSERT_EQ(300u, innerIds.size());
  EXPECT_EQ("d0", innerIds.front());
  EXPECT_EQ("d299", innerIds.back());
  const auto* outerSmall = bucketDocs(result, "outer", 1);
  ASSERT_NE(nullptr, outerSmall);
  EXPECT_EQ(3, outerSmall->ops.at("inner")->docList()->row_count);
}

// Range facets feed result children segment-major across a block of
// bindings. Several TopDocs bindings interleaved with a metric must agree
// with the one-binding-per-block arrangement.
TEST_F(FacetTopDocsTest, rangeFacetBucketTopDocsAgreeAcrossBindingBlocks) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a", "val_i", (int64_t)1, "price_i", (int64_t)10, "body_w", "apple"),
    flatdoc("id", "b", "val_i", (int64_t)12, "price_i", (int64_t)20, "body_w", "apple pear"),
    flatdoc("id", "c", "val_i", (int64_t)25, "price_i", (int64_t)30, "body_w", "pear"),
  }, UpdateMessage::COMMIT);
  helper.indexAll(std::array{
    flatdoc("id", "d", "val_i", (int64_t)5, "price_i", (int64_t)40, "body_w", "apple"),
    flatdoc("id", "e", "val_i", (int64_t)15, "price_i", (int64_t)50, "body_w", "plum"),
    flatdoc("id", "f", "val_i", (int64_t)27, "price_i", (int64_t)60, "body_w", "apple"),
  }, UpdateMessage::COMMIT);

  SearchOverridesGuard guard(forcedRangeFacetBindingStateChunkBytes,
                             rangeFacetBindingBlockCounter);
  std::size_t blocks = 0;
  rangeFacetBindingBlockCounter = &blocks;
  auto run = [&](std::size_t chunkBytes) {
    forcedRangeFacetBindingStateChunkBytes = chunkBytes;
    blocks = 0;
    auto req = localReq(helper.getSearchEngine());
    auto& facet = req->collection("main").rangeFacet("f", "val_i")
        .range(0, 40, 10);  // [30,40) is empty
    facet.topDocs("hits").matchQuery("body_w", "apple").limit(2).getNumber()
        .fields({"id"});
    auto& byPrice = facet.topDocs("by_price").allQuery().limit(2).fields({"id"});
    sortBy(byPrice, "price_i", api::SortSpec::SortDir::DESC);
    facet.sum("total", "price_i");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    const auto& result = rootFacet(*req, "f");
    EXPECT_EQ((std::vector<int64_t>{2, 2, 2, 0}),
              std::vector<int64_t>(result.counts.begin(), result.counts.end()));
    const auto* hits0 = bucketDocs(result, "hits", 0);
    const auto* hits1 = bucketDocs(result, "hits", 1);
    const auto* hits2 = bucketDocs(result, "hits", 2);
    const auto* hits3 = bucketDocs(result, "hits", 3);
    EXPECT_EQ(2, hits0 ? hits0->found.value_or(-1) : -2);
    EXPECT_EQ(1, hits1 ? hits1->found.value_or(-1) : -2);
    EXPECT_EQ(1, hits2 ? hits2->found.value_or(-1) : -2);
    EXPECT_EQ(0, hits3 ? hits3->found.value_or(-1) : -2);
    EXPECT_EQ(0, hits3 ? hits3->row_count : -2);
    const auto* price1 = bucketDocs(result, "by_price", 1);
    EXPECT_EQ((std::vector<std::string>{"e", "b"}),
              price1 ? ids(*price1) : std::vector<std::string>{});
    std::vector<std::byte> encoded;
    EXPECT_TRUE(api::encode(result, encoded));
    return encoded;
  };

  auto expected = run(0);
  EXPECT_EQ(1u, blocks);
  auto split = run(1);
  EXPECT_EQ(4u, blocks);
  EXPECT_EQ(expected, split);
}

// Query facets: the count pass takes the direct count even with result
// children present, and each bucket gets its own ranked list.
TEST_F(FacetTopDocsTest, queryFacetBucketTopDocs) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a", "price_i", (int64_t)3),
    flatdoc("id", "b", "price_i", (int64_t)1),
    flatdoc("id", "c", "price_i", (int64_t)8),
    flatdoc("id", "d", "price_i", (int64_t)5),
    flatdoc("id", "e", "price_i", (int64_t)9),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"limit":0,"get_number":true,"ops":{"tiers":{
    "query_facet":{
      "buckets":{"cheap":"price_i:[* TO 5]","dear":"price_i:[6 TO *]"},
      "ops":{"hits":{"top_docs":{"limit":2,"get_number":true,"fields":["id"],
        "sorts":[{"field":"price_i","dir":"asc"}]}}}}}}})json",
      req->rawRequest(), req->mr);
  req->collection("main");
  req->execute();
  ASSERT_OK(req);

  const auto& result = topFacet(*req, "tiers");
  EXPECT_EQ((std::vector<std::string>{"cheap", "dear"}), bucketIds(result));
  EXPECT_EQ((std::vector<int64_t>{3, 2}),
            std::vector<int64_t>(result.counts.begin(), result.counts.end()));
  const auto* cheap = bucketDocs(result, "hits", 0);
  const auto* dear = bucketDocs(result, "hits", 1);
  ASSERT_NE(nullptr, cheap);
  ASSERT_NE(nullptr, dear);
  EXPECT_EQ(3, cheap->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"b", "a"}), ids(*cheap));
  EXPECT_EQ(2, dear->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"c", "e"}), ids(*dear));
}

// A fusion under a bucket runs per bucket, and its own sub-ops see that
// bucket's fused candidate set through the bucket slot.
TEST_F(FacetTopDocsTest, fusionUnderStringFacet) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a1", "cat_s", "a", "body_w", "x y", "n_i", (int64_t)1),
    flatdoc("id", "a2", "cat_s", "a", "body_w", "x", "n_i", (int64_t)2),
    flatdoc("id", "a3", "cat_s", "a", "body_w", "y", "n_i", (int64_t)4),
    flatdoc("id", "a4", "cat_s", "a", "body_w", "z", "n_i", (int64_t)8),
    flatdoc("id", "b1", "cat_s", "b", "body_w", "y", "n_i", (int64_t)16),
    flatdoc("id", "b2", "cat_s", "b", "body_w", "z", "n_i", (int64_t)32),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"limit":0,"ops":{"cats":{"field_facet":{
    "field":"cat_s","limit":-1,
    "ops":{"fused":{"fusion":{
      "sources":{"x":{"query":"body_w:x","limit":10},
                 "y":{"query":"body_w:y","limit":10}},
      "rrf":{},"limit":10,"get_number":true,"fields":["id"],
      "ops":{"total":{"expr_op":"sum(n_i)"}}}}}}}}})json",
      req->rawRequest(), req->mr);
  req->collection("main");
  req->execute();
  ASSERT_OK(req);

  const auto& result = topFacet(*req, "cats");
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), bucketIds(result));
  const auto* fusedA = bucketDocs(result, "fused", 0);
  const auto* fusedB = bucketDocs(result, "fused", 1);
  ASSERT_NE(nullptr, fusedA);
  ASSERT_NE(nullptr, fusedB);
  EXPECT_EQ(3, fusedA->found.value_or(-1));
  auto idsA = ids(*fusedA);
  ASSERT_EQ(3u, idsA.size());
  EXPECT_EQ("a1", idsA[0]);  // the only doc in both source lists
  EXPECT_EQ(1, fusedB->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"b1"}), ids(*fusedB));
  EXPECT_EQ(7, std::get<int64_t>(fusedA->ops.at("total")->kind));
  EXPECT_EQ(16, std::get<int64_t>(fusedB->ops.at("total")->kind));
}

// A facet under a bucket TopDocs sees the whole bucket domain, not only the
// returned hits, and routes through the bucket slot.
TEST_F(FacetTopDocsTest, facetUnderBucketTopDocsSeesBucketDomain) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "a1", "cat_s", "a", "tag_s", "t1"),
    flatdoc("id", "a2", "cat_s", "a", "tag_s", "t1"),
    flatdoc("id", "a3", "cat_s", "a", "tag_s", "t2"),
    flatdoc("id", "b1", "cat_s", "b", "tag_s", "t2"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("main").facet("f", "cat_s").limit(-1);
  auto& hits = facet.topDocs("hits").allQuery().limit(1).getNumber()
      .fields({"id"});
  hits.facet("tags", "tag_s").limit(-1);
  // A descendant sees the bucket documents matching its TopDocs, not the
  // whole bucket.
  auto& t1 = facet.topDocs("hits_t1").matchQuery("tag_s", "t1").limit(1)
      .getNumber().fields({"id"});
  t1.facet("tags", "tag_s").limit(-1);
  req->execute();
  ASSERT_OK(req);

  const auto& result = rootFacet(*req, "f");
  const auto* t1A = bucketDocs(result, "hits_t1", 0);
  ASSERT_NE(nullptr, t1A);
  EXPECT_EQ(2, t1A->found.value_or(-1));
  EXPECT_EQ((std::vector<std::string>{"t1"}),
            bucketIds(*t1A->ops.at("tags")->facetResult()));
  const auto* t1B = bucketDocs(result, "hits_t1", 1);
  ASSERT_NE(nullptr, t1B);
  EXPECT_EQ(0, t1B->found.value_or(-1));
  EXPECT_EQ(0u, t1B->ops.at("tags")->facetResult()->counts.size());
  const auto* hitsA = bucketDocs(result, "hits", 0);
  ASSERT_NE(nullptr, hitsA);
  EXPECT_EQ(3, hitsA->found.value_or(-1));
  EXPECT_EQ(1, hitsA->row_count);
  const auto* tagsA = hitsA->ops.at("tags")->facetResult();
  ASSERT_NE(nullptr, tagsA);
  EXPECT_EQ((std::vector<std::string>{"t1", "t2"}), bucketIds(*tagsA));
  EXPECT_EQ((std::vector<int64_t>{2, 1}),
            std::vector<int64_t>(tagsA->counts.begin(), tagsA->counts.end()));
  const auto* hitsB = bucketDocs(result, "hits", 1);
  ASSERT_NE(nullptr, hitsB);
  const auto* tagsB = hitsB->ops.at("tags")->facetResult();
  ASSERT_NE(nullptr, tagsB);
  EXPECT_EQ((std::vector<std::string>{"t2"}), bucketIds(*tagsB));
}

TEST_F(FacetTopDocsTest, emptyIndex) {
  CollectionHelper helper;
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").facet("f", "cat_s").limit(5)
      .topDocs("hits").allQuery().limit(5).fields({"id"});
  req->execute();
  ASSERT_OK(req);
  const auto& result = rootFacet(*req, "f");
  EXPECT_EQ(0u, result.counts.size());
  EXPECT_EQ(nullptr, bucketDocs(result, "hits", 0));
}

// Cached filter membership composed with many short-lived bucket domains:
// every bucket must see its own intersection, through both the pure-count
// whole-membership route and the explicit filter route.
TEST_F(FacetTopDocsTest, cachedMembershipComposesPerBucket) {
  CollectionHelper helper;
  // Admit on first sighting so the first bucket builds the shared value and
  // every later bucket hits it within the same request.
  auto cache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0, .admissionThreshold = 1});
  helper.getIndexWriter()->filterCache = cache;
  // Bucket sizes and kept counts vary so a stale composition served to a
  // neighbouring bucket cannot pass on equal cardinality.
  constexpr int BUCKETS = 40;
  auto bucketSize = [](int b) { return 4 + b % 3; };
  auto keptCount = [](int b) { return 1 + b % 2; };
  std::vector<Doc> docs;
  for (int b = 0; b < BUCKETS; b++) {
    for (int n = 0; n < bucketSize(b); n++) {
      docs.push_back(flatdoc(
          "id", "c" + std::to_string(b) + "_" + std::to_string(n),
          "cat_s", "c" + std::to_string(b),
          "keep_s", n < keptCount(b) ? "yes" : "no", "n_i", (int64_t)n));
    }
  }
  std::span<const Doc> all(docs);
  ASSERT_TRUE(helper.indexAll(all.first(all.size() / 2),
                              UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(all.subspan(all.size() / 2),
                              UpdateMessage::COMMIT).success);

  auto check = [&](LocalReq& req, auto&& expectedFound, bool expectRows) {
    const auto& result = topFacet(req, "f");
    auto cats = bucketIds(result);
    ASSERT_EQ((size_t)BUCKETS, cats.size());
    for (size_t i = 0; i < cats.size(); i++) {
      int b = std::stoi(cats[i].substr(1));
      const auto* hits = bucketDocs(result, "hits", i);
      ASSERT_NE(nullptr, hits) << cats[i];
      int64_t found = expectedFound(b);
      EXPECT_EQ(found, hits->found.value_or(-1)) << cats[i];
      if (expectRows) {
        std::vector<std::string> expectedIds;
        for (int64_t n = 0; n < found; n++) {
          expectedIds.push_back(cats[i] + "_" + std::to_string(n));
        }
        EXPECT_EQ(expectedIds, ids(*hits)) << cats[i];
      }
    }
  };

  {
    // limit 0 + get_number: whole-membership count of the keep query,
    // composed with each bucket domain. Two live terms keep the count from
    // being constant per segment, which would bypass membership entirely. The
    // first bucket builds the value; every later bucket hits the request slot.
    SkipStatsScope stats;
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q").allQuery().limit(0);
    top.facet("f", "cat_s").limit(-1)
        .topDocs("hits").exprQuery("keep_s:yes OR keep_s:no").limit(0)
        .getNumber();
    req->execute();
    ASSERT_OK(req);
    check(*req, bucketSize, false);
    EXPECT_GT(SkipStats::wholeCountHits, 0);
  }
  {
    // Ranked hits with a separate (unfolded) filter under forced preparation:
    // the prepared filter's cached raw membership is composed with each
    // bucket domain. A root request publishes the value first so the bucket
    // request's first probe per segment is a shared hit and the rest hit the
    // request slot.
    SearchOverridesGuard guard(disableTopDocsFilterFold);
    disableTopDocsFilterFold = true;
    int64_t totalKept = 0;
    for (int b = 0; b < BUCKETS; b++) totalKept += keptCount(b);
    {
      auto warm = localReq(helper.getSearchEngine());
      warm->collection("main").topDocs("q").allQuery()
          .matchFilter("keep_s", "yes").limit(0).getNumber();
      warm->execute();
      ASSERT_OK(warm);
      EXPECT_EQ(totalKept, warm->getMatchCount());
    }
    auto before = cache->counters();
    auto req = localReq(helper.getSearchEngine());
    req->testForcePrepare = true;
    auto& top = req->collection("main").topDocs("q").allQuery().limit(0);
    auto& hits = top.facet("f", "cat_s").limit(-1)
        .topDocs("hits").allQuery().matchFilter("keep_s", "yes").limit(-1)
        .getNumber().fields({"id"});
    sortBy(hits, "n_i", api::SortSpec::SortDir::ASC);
    req->execute();
    ASSERT_OK(req);
    check(*req, keptCount, true);
    EXPECT_GE(cache->counters().hits, before.hits + 2);
  }
}
