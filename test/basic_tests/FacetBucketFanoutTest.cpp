// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Parallel result-stage delivery of facet bucket children must produce the
// serial result exactly, at every task granularity.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class FacetBucketFanoutTest : public LuxirTest {
protected:
  static constexpr int SEGMENTS = 9;
  static constexpr int DOCS = 120;

  // Skewed segments with deletes in some of them.
  void indexData(CollectionHelper& helper) {
    helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
    int id = 0;
    for (int segment = 0; segment < SEGMENTS; segment++) {
      std::vector<Doc> docs;
      for (int i = 0; i < DOCS * (1 + segment % 3); i++, id++) {
        docs.push_back(flatdoc("id", std::to_string(id), "cat_s", "c" + std::to_string(id % 7 * id % 11),
            "n_i", id % 13, "t_i", id % 100, "u_s", "u" + std::to_string(id % 97),
            "x_i", id % 31, "x_d", (double)(id % 89) / 3, "tag_s", "t" + std::to_string(id % 23),
            "w_dt", "2020-01-0" + std::to_string(1 + id % 9)));
      }
      helper.indexAll(docs, UpdateMessage::COMMIT);
      if (segment % 4 == 1) {
        std::vector<std::string> ids;
        for (int i = id - 40; i < id; i += 3) ids.push_back(std::to_string(i));
        helper.deleteByIds(ids, UpdateMessage::COMMIT);
      }
    }
  }

  // Bytes the last request still holds in its tracker after it ran: results the
  // request cache keeps charged by the first run. A later run holds no more.
  size_t retained = 0;

  // The rendered response of a request run with the given parallelism.
  std::string run(CollectionHelper& helper, const std::string& json, int32_t maxParallel,
                  bool expectOk = true) {
    auto req = localReq(helper.getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("main");
    req->execute(maxParallel);
    EXPECT_EQ(expectOk, req->ok()) << req->errorMsg() << "\n" << json;
    retained = req->memoryTracker.bytes();
    if (!req->ok()) return req->errorCode();
    return renderSearchResponseBody(req->responses[0]->proto);
  }

  // Serial and parallel runs agree, and the parallel run fanned out blocks
  // of the expected granularity.
  void expectSame(CollectionHelper& helper, const std::string& json,
                  std::atomic<size_t> FacetBucketFanoutStats::* granularity) {
    SCOPED_TRACE(json);
    FacetBucketFanoutStats stats;
    SearchOverridesGuard guard(facetBucketFanoutStatsForTests);
    facetBucketFanoutStatsForTests = &stats;
    std::string serial = run(helper, json, 1);
    size_t serialRetained = retained;
    EXPECT_EQ(0u, stats.segments + stats.buckets + stats.domains);
    std::string parallel = run(helper, json, -1);
    EXPECT_LE(retained, serialRetained);
    EXPECT_EQ(serial, parallel);
    EXPECT_GT((stats.*granularity).load(), 0u) << stats.segments << " segment, " << stats.buckets
        << " bucket, " << stats.domains << " domain blocks";
    EXPECT_EQ(parallel, run(helper, json, 0));
  }
};

constexpr std::string_view METRICS =
    R"j("ops":{"c":"cardinality(u_s)","s":"sum(x_i)","p":"percentile(x_d,50)","v":"sum(count(u_s))"})j";

TEST_F(FacetBucketFanoutTest, termsChildrenRunPerSegmentAndBucket) {
  CollectionHelper helper;
  indexData(helper);
  expectSame(helper, R"j({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,)j"
      + std::string(METRICS) + "}}}}", &FacetBucketFanoutStats::buckets);
  expectSame(helper, R"j({"query":"tag_s:t1*","ops":{"f":{"field_facet":{"field":"cat_s","limit":5,)j"
      + std::string(METRICS) + "}}}}", &FacetBucketFanoutStats::buckets);
}

TEST_F(FacetBucketFanoutTest, jointProducersFanOutBuiltDomains) {
  CollectionHelper helper;
  indexData(helper);
  expectSame(helper, R"j({"ops":{"f":{"field_facet":{"field":"n_i","limit":-1,)j"
      + std::string(METRICS) + "}}}}", &FacetBucketFanoutStats::domains);
  expectSame(helper, R"j({"ops":{"f":{"range_facet":{"field":"t_i","start":0,"end":100,"gap":10,)j"
      + std::string(METRICS) + "}}}}", &FacetBucketFanoutStats::domains);
  // A tiny builder budget splits each segment pass into chunks.
  SearchOverridesGuard guard(forcedRangeFacetBucketDomainByteBudget);
  forcedRangeFacetBucketDomainByteBudget = 1;
  expectSame(helper, R"j({"ops":{"f":{"range_facet":{"field":"t_i","start":0,"end":100,"gap":7,)j"
      + std::string(METRICS) + "}}}}", &FacetBucketFanoutStats::domains);
}

// Multiterm bucket queries share per-segment expansions: a segment task
// produces the block and fans its domains out. Term buckets run per bucket.
TEST_F(FacetBucketFanoutTest, queryBuckets) {
  CollectionHelper helper;
  indexData(helper);
  expectSame(helper, R"j({"ops":{"q":{"query_facet":{"buckets":{"a":"n_i:3","b":"tag_s:t1",)j"
      R"j("c":"cat_s:c0 OR tag_s:t2","d":"*:*"},)j" + std::string(METRICS) + "}}}}",
      &FacetBucketFanoutStats::buckets);
  expectSame(helper, R"j({"ops":{"q":{"query_facet":{"buckets":{"a":"n_i:3","b":"tag_s:t1*",)j"
      R"j("c":"cat_s:c0 OR tag_s:t2*","d":"*:*"},)j" + std::string(METRICS) + "}}}}",
      &FacetBucketFanoutStats::domains);
}

// Leaf facet children, including query facets with multiterm weights, share
// per-segment query state across buckets: one task feeds a segment.
TEST_F(FacetBucketFanoutTest, leafFacetChildrenRunPerSegment) {
  CollectionHelper helper;
  indexData(helper);
  expectSame(helper, R"j({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,"ops":{)j"
      R"j("n":{"field_facet":{"field":"n_i","limit":3}},"c":"cardinality(u_s)"}}}}})j",
      &FacetBucketFanoutStats::segments);
  expectSame(helper, R"j({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,"ops":{)j"
      R"j("q":{"query_facet":{"buckets":{"a":"tag_s:t1*","b":"tag_s:t2*","c":"u_s:u1*"}}}}}}}})j",
      &FacetBucketFanoutStats::segments);
  expectSame(helper, R"j({"ops":{"f":{"range_facet":{"field":"t_i","start":0,"end":100,"gap":25,)j"
      R"j("ops":{"s":{"field_facet":{"field":"tag_s","limit":2}},"c":"cardinality(u_s)"}}}}})j",
      &FacetBucketFanoutStats::segments);
}

// A child with result children of its own keeps the outer block serial; the
// nested executor still fans out.
TEST_F(FacetBucketFanoutTest, nestedResultChildren) {
  CollectionHelper helper;
  indexData(helper);
  expectSame(helper, R"j({"ops":{"f":{"field_facet":{"field":"n_i","limit":4,"ops":{)j"
      R"j("g":{"field_facet":{"field":"cat_s","limit":3,"ops":{"c":"cardinality(u_s)","s":"sum(x_i)"}}}}}}}})j",
      &FacetBucketFanoutStats::buckets);
}

TEST_F(FacetBucketFanoutTest, killSwitchKeepsSerialLoop) {
  CollectionHelper helper;
  indexData(helper);
  FacetBucketFanoutStats stats;
  SearchOverridesGuard guard(facetBucketFanoutStatsForTests, forcedFacetBucketFanout);
  facetBucketFanoutStatsForTests = &stats;
  forcedFacetBucketFanout = FacetBucketFanout::SERIAL;
  std::string json = R"j({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,)j"
      + std::string(METRICS) + "}}}}";
  std::string parallel = run(helper, json, -1);
  EXPECT_EQ(0u, stats.segments + stats.buckets + stats.domains);
  EXPECT_EQ(run(helper, json, 1), parallel);
}

// A child that fails inside a task fails the request as serial execution
// does: the group cancels the other tasks, the join rethrows, and every
// charge returns.
TEST_F(FacetBucketFanoutTest, failingChildFailsRequest) {
  CollectionHelper helper;
  indexData(helper);
  // A non-finite date_iso result throws as its binding completes.
  std::string bad = R"j("ops":{"c":"cardinality(u_s)","bad":"percentile(w_dt,50)+1e308+1e308"})j";
  for (std::string json : {
           R"j({"flags":["date_iso"],"ops":{"f":{"field_facet":{"field":"cat_s","limit":5,)j" + bad + "}}}}",
           R"j({"flags":["date_iso"],"ops":{"f":{"range_facet":{"field":"t_i","start":0,"end":100,"gap":1,)j"
               + bad + "}}}}"}) {
    SCOPED_TRACE(json);
    FacetBucketFanoutStats stats;
    SearchOverridesGuard guard(facetBucketFanoutStatsForTests);
    facetBucketFanoutStatsForTests = &stats;
    std::string serial = run(helper, json, 1, false);
    EXPECT_EQ(serial, run(helper, json, -1, false));
    EXPECT_EQ(serial, run(helper, json, 0, false));
    EXPECT_GT(stats.buckets + stats.domains, 0u);
  }
}

}  // namespace
