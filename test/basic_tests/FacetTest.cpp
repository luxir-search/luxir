#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <cmath>
#include <functional>
#include <tbb/task_group.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "solux/util/random.h"
#include "solux/util/proto.h"
#include "solux/index/Inverter.h"
#include "solux/index/IndexWriter.h"
#include "solux/search/ops/StrFacetOp.h"

using namespace solux;
using namespace solux::test;

class FacetTest : public SoluxTest {
protected:
};

TEST_F(FacetTest, mergeableStrDataMergeVariants) {
  using Data = StrFacetOp::MergeableStrData;
  using CountVector = Data::CountVector;
  using OrdHash = Data::OrdHash;

  auto makeOrd = [](std::initializer_list<std::pair<const int64_t, int64_t>> vals, int64_t missing) {
    Data data;
    data.counts = OrdHash(vals);
    data.missing_num = missing;
    return data;
  };
  auto makeVec = [](std::initializer_list<int64_t> vals, int64_t missing) {
    Data data;
    data.counts = CountVector(vals);
    data.missing_num = missing;
    return data;
  };
  auto makeSkinny = [](std::initializer_list<std::pair<int64_t, int64_t>> vals, int64_t missing) {
    Data data;
    data.counts.emplace<SkinnyCounter8>(8);
    auto& skinny = std::get<SkinnyCounter8>(data.counts);
    for (auto [ord, count] : vals) {
      skinny.increment(ord, count);
    }
    data.missing_num = missing;
    return data;
  };
  auto asVec = [](const Data& data) {
    std::vector<int64_t> out(8);
    if (auto* ords = std::get_if<OrdHash>(&data.counts)) {
      for (auto [ord, count] : *ords) out[ord] = count;
    } else if (auto* vec = std::get_if<CountVector>(&data.counts)) {
      for (size_t i = 0; i < vec->size(); i++) out[i] = (*vec)[i];
    } else if (auto* skinny = std::get_if<SkinnyCounter8>(&data.counts)) {
      for (size_t i = 0; i < skinny->counts.size(); i++) out[i] = skinny->counts[i];
      for (auto [ord, count] : skinny->overflow) out[ord] += count;
    }
    return out;
  };
  auto expectMerge = [&](Data a, Data b, std::vector<int64_t> expected) {
    auto missing = a.missing_num + b.missing_num;
    auto* result = Data::merge(&a, &b);
    EXPECT_EQ(expected, asVec(*result));
    EXPECT_EQ(missing, result->missing_num);
  };

  {
    Data empty;
    empty.missing_num = 1;
    expectMerge(empty, makeOrd({{2, 3}}, 2), {0, 0, 3, 0, 0, 0, 0, 0});
  }
  {
    Data empty;
    empty.missing_num = 4;
    expectMerge(makeOrd({{1, 5}}, 3), empty, {0, 5, 0, 0, 0, 0, 0, 0});
  }
  expectMerge(makeOrd({{2, 1}}, 5), makeOrd({{2, 4}, {3, 6}}, 6), {0, 0, 5, 6, 0, 0, 0, 0});
  expectMerge(makeOrd({{1, 7}, {2, 1}}, 7), makeOrd({{1, 3}}, 8), {0, 10, 1, 0, 0, 0, 0, 0});
  expectMerge(makeSkinny({{2, 3}}, 9), makeOrd({{2, 4}, {4, 5}}, 10), {0, 0, 7, 0, 5, 0, 0, 0});
  expectMerge(makeVec({1, 0, 2, 0, 0, 0, 0, 0}, 11), makeOrd({{2, 5}, {5, 6}}, 12), {1, 0, 7, 0, 0, 6, 0, 0});
  expectMerge(makeVec({0, 2, 0, 0, 0, 0, 0, 0}, 13), makeSkinny({{1, 5}, {6, 300}}, 14), {0, 7, 0, 0, 0, 0, 300, 0});
  expectMerge(makeVec({1, 2, 0, 0, 0, 0, 0, 0}, 15), makeVec({3, 0, 4, 0, 0, 0, 0, 0}, 16), {4, 2, 4, 0, 0, 0, 0, 0});
}

TEST_F(FacetTest, emptyIndex) {
  CollectionHelper helper;
  helper.clear();
  
  // Test all field types that support faceting
  struct FieldTypeTest {
    std::string fieldName;
    std::string description;
    bool isRangeFacet;
  };
  
  std::vector<FieldTypeTest> fieldTypes = {
    {"price_i", "integer field", false},
    {"pricea_is", "multivalued integer field", false},
    {"category_s", "string field", false},
    {"categories_ss", "multivalued string field", false},
    {"description_w", "text field", false},
    {"price_i", "integer range facet", true},
    {"price_is", "multivalued integer range facet", true}
  };
  
  for (const auto& fieldType : fieldTypes) {
    // Create a search request with faceting on the field type
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("test_empty_index_" + fieldType.fieldName);
    
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.mutable_query()->set_all(true);
    
    // Add facet based on field type
    if (fieldType.isRangeFacet) {
      // Range facet for integer
      auto& facet = *ops["f_range"].mutable_range_facet();
      facet.set_field(fieldType.fieldName);
      facet.set_start(0);
      facet.set_end(100);
      facet.set_gap(10);
    } else {
      // Regular field facet
      auto& facet = *ops["f"].mutable_field_facet();
      facet.set_field(fieldType.fieldName);
      facet.set_limit(10);
      facet.set_missing(true); // Also test missing value handling
    }
    
    // Execute search on empty index
    lreq->engine.submit(*lreq, true);
    
    // Verify we get a response without crashing
    ASSERT_EQ(1, lreq->responses.size()) << "Failed for " << fieldType.description;
    
    // Check the appropriate facet result based on type
    if (fieldType.isRangeFacet) {
      ASSERT_TRUE(lreq->responses[0]->proto.ops().contains("f_range")) << "Failed for " << fieldType.description;
      const auto& facetResult = lreq->responses[0]->proto.ops().at("f_range").facet();
      
      // Range facets should have empty buckets
      ASSERT_EQ(0, facetResult.bucket_ids().multi_i().v_size()) << "Failed for " << fieldType.description;
      ASSERT_EQ(0, facetResult.counts_size()) << "Failed for " << fieldType.description;
    } else {
      ASSERT_TRUE(lreq->responses[0]->proto.ops().contains("f")) << "Failed for " << fieldType.description;
      const auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
      
      // Check appropriate bucket type based on field type
      if (fieldType.fieldName.contains("_i")) {
        // Integer field
        ASSERT_EQ(0, facetResult.bucket_ids().col_i().v_size()) << "Failed for " << fieldType.description;
      } else if (fieldType.fieldName.contains("_s") || fieldType.fieldName.contains("_w")) {
        // String or text field
        ASSERT_EQ(0, facetResult.bucket_ids().col_s().v_size()) << "Failed for " << fieldType.description;
      }
      ASSERT_EQ(0, facetResult.counts_size()) << "Failed for " << fieldType.description;
      
      // Since we requested missing=true, missing count should be 0 for empty index
      ASSERT_EQ(0, facetResult.missing()) << "Failed for " << fieldType.description;
    }
    
    lreq->done();
  }
}

TEST_F(FacetTest, emptyIndexNestedFacet) {
  CollectionHelper helper;
  helper.clear();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_empty_index_nested_facet");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->set_all(true);

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("category_s");
  facet.set_limit(10);
  facet.set_missing(true);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(0, docs.matches());
  ASSERT_TRUE(docs.ops().contains("f")) << lreq->toString();
  const auto& facetResult = docs.ops().at("f").facet();
  EXPECT_EQ(0, facetResult.bucket_ids().col_s().v_size());
  EXPECT_EQ(0, facetResult.counts_size());
  EXPECT_EQ(0, facetResult.missing());

  lreq->done();
}

TEST_F(FacetTest, singleSegment) {
  CollectionHelper helper;
  helper.clear();
  
  // Add some documents with integer and string fields using dynamic field naming
  std::vector<std::string> colors = {"red", "blue", "green", "red", "blue"};
  for (int i = 0; i < 5; i++) {
    helper.index(flatdoc("id", std::to_string(i), 
                        "price_i", i * 10,
                        "color_s", colors[i]), UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  
  // Test integer faceting
  {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("test_single_segment_int");
    
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.mutable_query()->set_all(true);
    
    auto& facet = *ops["f"].mutable_field_facet();
    facet.set_field("price_i");
    facet.set_limit(10);
    
    lreq->engine.submit(*lreq, true);
    
    ASSERT_EQ(1, lreq->responses.size());
    const auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
    
    // Should have 5 buckets (0, 10, 20, 30, 40)
    ASSERT_EQ(5, facetResult.bucket_ids().col_i().v_size());
    ASSERT_EQ(5, facetResult.counts_size());
    
    // Each bucket should have count of 1
    for (int i = 0; i < 5; i++) {
      EXPECT_EQ(i * 10, facetResult.bucket_ids().col_i().v(i));
      EXPECT_EQ(1, facetResult.counts(i));
    }
    
    lreq->done();
  }
  
  // Test string faceting
  {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("test_single_segment_string");
    
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.mutable_query()->set_all(true);
    
    auto& facet = *ops["f_str"].mutable_field_facet();
    facet.set_field("color_s");
    facet.set_limit(10);
    
    lreq->engine.submit(*lreq, true);
    
    ASSERT_EQ(1, lreq->responses.size());
    const auto& facetResult = lreq->responses[0]->proto.ops().at("f_str").facet();
    
    // Should have 3 unique colors
    ASSERT_EQ(3, facetResult.bucket_ids().col_s().v_size());
    ASSERT_EQ(3, facetResult.counts_size());
    
    // Check the counts for each color
    // Note: facets are typically sorted by count desc, then by value
    // We expect: red(2), blue(2), green(1)
    boost::unordered_flat_map<std::string, int> expectedCounts = {
      {"red", 2},
      {"blue", 2}, 
      {"green", 1}
    };
    
    for (int i = 0; i < facetResult.bucket_ids().col_s().v_size(); i++) {
      std::string color = std::string(facetResult.bucket_ids().col_s().v(i));
      EXPECT_TRUE(expectedCounts.count(color) > 0) << "Unexpected color: " << color;
      EXPECT_EQ(expectedCounts[color], facetResult.counts(i)) << "Wrong count for color: " << color;
    }
    
    lreq->done();
  }
}

TEST_F(FacetTest, multipleSegments) {
  CollectionHelper helper;
  helper.clear();
  
  // Add documents and commit multiple times to create multiple segments
  for (int seg = 0; seg < 3; seg++) {
    for (int i = 0; i < 3; i++) {
      helper.index(flatdoc("id", std::to_string(seg * 100 + i), "price_i", seg * 10 + i), UpdateMessage::NO_COMMIT);
    }
    helper.commit();
  }
  
  // Create search request with faceting
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_multiple_segments");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->set_all(true);
  
  auto& facet = *ops["f"].mutable_field_facet();
  facet.set_field("price_i");
  facet.set_limit(20);
  
  lreq->engine.submit(*lreq, true);
  
  ASSERT_EQ(1, lreq->responses.size());
  const auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
  
  // Should have 9 unique values: 0,1,2,10,11,12,20,21,22
  ASSERT_EQ(9, facetResult.bucket_ids().col_i().v_size());
  ASSERT_EQ(9, facetResult.counts_size());
  
  // Each value should have count of 1
  for (int i = 0; i < 9; i++) {
    EXPECT_EQ(1, facetResult.counts(i));
  }
  
  lreq->done();
}

TEST_F(FacetTest, fullTextFacetSegmentMissingField) {
  CollectionHelper helper;
  helper.clear();

  helper.index(flatdoc("id", "a", "body_w", "alpha beta"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "b", "body_w", "alpha"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "c", "other_s", "x"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d", "other_s", "y"), UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_full_text_facet_segment_missing_field");

  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->set_all(true);

  auto& facet = *ops["f"].mutable_field_facet();
  facet.set_field("body_w");
  facet.set_limit(-1);
  facet.set_missing(true);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  ASSERT_TRUE(lreq->responses[0]->proto.ops().contains("f")) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
  ASSERT_EQ(2, facetResult.bucket_ids().col_s().v_size());
  ASSERT_EQ(2, facetResult.counts_size());
  EXPECT_EQ("alpha", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(2, facetResult.counts(0));
  EXPECT_EQ("beta", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(1, facetResult.counts(1));
  EXPECT_EQ(2, facetResult.missing());

  lreq->done();
}

TEST_F(FacetTest, fullTextFacetNestedSparseArrayDomain) {
  CollectionHelper helper;
  helper.clear();

  for (int i = 0; i < 100; i++) {
    std::string id = std::to_string(i);
    if (i == 3) {
      helper.index(flatdoc("id", id, "pick_w", "yes", "body_w", "apple red"), UpdateMessage::NO_COMMIT);
    } else if (i == 17) {
      helper.index(flatdoc("id", id, "pick_w", "yes", "body_w", "red cherry"), UpdateMessage::NO_COMMIT);
    } else if (i == 88) {
      helper.index(flatdoc("id", id, "pick_w", "yes", "body_w", "blue apple"), UpdateMessage::NO_COMMIT);
    } else {
      helper.index(flatdoc("id", id, "body_w", "noise filler"), UpdateMessage::NO_COMMIT);
    }
  }
  helper.commit();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_full_text_facet_nested_sparse_array_domain");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("pick_w");
  match.mutable_val()->set_s("yes");

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("body_w");
  facet.set_limit(-1);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(3, docs.matches());
  ASSERT_TRUE(docs.ops().contains("f")) << lreq->toString();
  const auto& facetResult = docs.ops().at("f").facet();
  ASSERT_EQ(4, facetResult.bucket_ids().col_s().v_size());
  ASSERT_EQ(4, facetResult.counts_size());
  EXPECT_EQ("apple", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(2, facetResult.counts(0));
  EXPECT_EQ("red", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(2, facetResult.counts(1));
  EXPECT_EQ("blue", facetResult.bucket_ids().col_s().v(2));
  EXPECT_EQ(1, facetResult.counts(2));
  EXPECT_EQ("cherry", facetResult.bucket_ids().col_s().v(3));
  EXPECT_EQ(1, facetResult.counts(3));

  lreq->done();
}

TEST_F(FacetTest, vectorOptimization) {
  CollectionHelper helper;
  helper.clear();
  
  // Add documents with a small range of values to trigger vector optimization
  for (int i = 0; i < 100; i++) {
    helper.index(flatdoc("id", std::to_string(i), "score_i", i % 20), UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  
  // Create search request with faceting
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_vector_optimization");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->set_all(true);
  
  auto& facet = *ops["f"].mutable_field_facet();
  facet.set_field("score_i");
  facet.set_limit(30);
  
  lreq->engine.submit(*lreq, true);
  
  ASSERT_EQ(1, lreq->responses.size());
  const auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
  
  // Should have 20 unique values (0-19)
  ASSERT_EQ(20, facetResult.bucket_ids().col_i().v_size());
  ASSERT_EQ(20, facetResult.counts_size());
  
  // Each value should have count of 5 (100 docs / 20 values)
  for (int i = 0; i < 20; i++) {
    EXPECT_EQ(i, facetResult.bucket_ids().col_i().v(i));
    EXPECT_EQ(5, facetResult.counts(i));
  }
  
  lreq->done();
}

// Facet sorted by an inline sub-op (avg) with a finite limit.  This exercises
// FacetReq::init()'s "!sorts.empty()" branch, which moves the sort-field sub-op
// into inlineSubOps.  The existing SearchEngineTest coverage uses limit==-1,
// which inlines via a different branch and so masks regressions in this one.
TEST_F(FacetTest, sortBySubOp) {
  CollectionHelper helper;
  helper.clear();
  // 2 segments.  Per-category foo_i avg differs from per-category count so that
  // an avg-ascending sort produces a different bucket order than count-desc.
  //   a: count 3, avg 100
  //   b: count 1, avg 1
  //   c: count 2, avg 50
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 1), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 50), UpdateMessage::COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "a", "foo_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "c", "foo_i", 50), UpdateMessage::COMMIT);

  // Builds a facet on cat_s with an inline avg(foo_i) sub-op, sorted by that
  // sub-op, and returns the response facet result.
  auto runFacet = [&](int64_t limit, proto::SortSpec_SortDir dir) {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("test_sort_by_subop");

    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.mutable_query()->set_all(true);

    auto& facet = *ops["f"].mutable_field_facet();
    facet.set_field("cat_s");
    facet.set_limit(limit);
    auto& subAvg = *(*facet.mutable_ops())["avgsub"].mutable_gen_op();
    subAvg.set_name("avg");
    subAvg.mutable_args()->Add()->set_s("foo_i");
    auto& sort = *facet.mutable_sorts()->Add();
    sort.set_field("avgsub");
    sort.set_dir(dir);

    lreq->engine.submit(*lreq, true);
    return lreq;
  };

  // Ascending avg, all buckets.  Order is b(1), c(50), a(100) -- which differs
  // from count-desc (a, c, b), so this confirms we sorted by the sub-op.
  {
    auto* lreq = runFacet(10, proto::SortSpec_SortDir_ASC);
    const auto& facet = lreq->responses[0]->proto.ops().at("f").facet();
    ASSERT_EQ(3, facet.bucket_ids().col_s().v_size());
    EXPECT_EQ("b", facet.bucket_ids().col_s().v(0));
    EXPECT_EQ("c", facet.bucket_ids().col_s().v(1));
    EXPECT_EQ("a", facet.bucket_ids().col_s().v(2));
    EXPECT_EQ(1, facet.counts(0));
    EXPECT_EQ(2, facet.counts(1));
    EXPECT_EQ(3, facet.counts(2));
    const auto& avg = facet.ops().at("avgsub").arr_d();
    ASSERT_EQ(3, avg.v_size());
    EXPECT_EQ(1, avg.v(0));
    EXPECT_EQ(50, avg.v(1));
    EXPECT_EQ(100, avg.v(2));
    lreq->done();
  }

  // Descending avg with a finite limit smaller than the bucket count: keep the
  // top 2 by avg -> a(100), c(50).
  {
    auto* lreq = runFacet(2, proto::SortSpec_SortDir_DESC);
    const auto& facet = lreq->responses[0]->proto.ops().at("f").facet();
    ASSERT_EQ(2, facet.bucket_ids().col_s().v_size());
    EXPECT_EQ("a", facet.bucket_ids().col_s().v(0));
    EXPECT_EQ("c", facet.bucket_ids().col_s().v(1));
    const auto& avg = facet.ops().at("avgsub").arr_d();
    ASSERT_EQ(2, avg.v_size());
    EXPECT_EQ(100, avg.v(0));
    EXPECT_EQ(50, avg.v(1));
    lreq->done();
  }
}

TEST_F(FacetTest, limitMinusOneInlinesMultipleAvgSubOps) {
  CollectionHelper helper;
  helper.clear();

  const int totalDocs = 6000;
  auto categoryName = [](int i) {
    return "cat" + std::to_string(100000 + i);
  };

  std::vector<Doc> firstSegment;
  std::vector<Doc> secondSegment;
  firstSegment.reserve(totalDocs / 2);
  secondSegment.reserve(totalDocs / 2);
  for (int i = 0; i < totalDocs; i++) {
    auto doc = flatdoc("cat_s", categoryName(i),
                       "score_i", (int64_t)i,
                       "bonus_i", (int64_t)(totalDocs - i));
    if (i < totalDocs / 2) {
      firstSegment.push_back(std::move(doc));
    } else {
      secondSegment.push_back(std::move(doc));
    }
  }
  helper.indexAll(firstSegment, UpdateMessage::COMMIT);
  helper.indexAll(secondSegment, UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_limit_minus_one_inlines_multiple_avg_subops");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->set_all(true);

  auto& facet = *(*lreq->proto.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("cat_s");
  facet.set_limit(-1);
  auto& avgScore = *(*facet.mutable_ops())["avg_score"].mutable_gen_op();
  avgScore.set_name("avg");
  avgScore.mutable_args()->Add()->set_s("score_i");
  auto& avgBonus = *(*facet.mutable_ops())["avg_bonus"].mutable_gen_op();
  avgBonus.set_name("avg");
  avgBonus.mutable_args()->Add()->set_s("bonus_i");

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
  ASSERT_EQ(totalDocs, facetResult.bucket_ids().col_s().v_size());
  ASSERT_EQ(totalDocs, facetResult.counts_size());
  const auto& avgScoreResult = facetResult.ops().at("avg_score").arr_d();
  const auto& avgBonusResult = facetResult.ops().at("avg_bonus").arr_d();
  ASSERT_EQ(totalDocs, avgScoreResult.v_size());
  ASSERT_EQ(totalDocs, avgBonusResult.v_size());

  std::vector<int> checkIndexes = {0, 10, 11, 2999, 3000, totalDocs - 1};
  for (int idx : checkIndexes) {
    EXPECT_EQ(categoryName(idx), facetResult.bucket_ids().col_s().v(idx));
    EXPECT_EQ(1, facetResult.counts(idx));
    EXPECT_DOUBLE_EQ((double)idx, avgScoreResult.v(idx));
    EXPECT_DOUBLE_EQ((double)(totalDocs - idx), avgBonusResult.v(idx));
  }

  lreq->done();
}

TEST_F(FacetTest, unsupportedFacetOptionsRejected) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("cat_s", "a", "foo_i", 1, "body_w", "alpha"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("cat_s", "b", "foo_i", 2, "body_w", "beta"), UpdateMessage::COMMIT);

  struct Case {
    std::string name;
    std::function<void(proto::SearchRequest&)> configure;
    std::string expectSubstr; // a phrase the clear error message must contain
  };

  std::vector<Case> cases = {
    {"int_subop", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("foo_i");
      auto& avg = *(*facet.mutable_ops())["avg"].mutable_gen_op();
      avg.set_name("avg");
      avg.mutable_args()->Add()->set_s("foo_i");
    }, "not yet supported for int field facets"},
    {"int_sort", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("foo_i");
      auto& sort = *facet.mutable_sorts()->Add();
      sort.set_field("avg");
      sort.set_dir(proto::SortSpec_SortDir_ASC);
    }, "not yet supported for int field facets"},
    {"int_mincount_zero", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("foo_i");
      facet.set_mincount(0);
    }, "not supported for int field facets"},
    {"text_subop", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("body_w");
      auto& avg = *(*facet.mutable_ops())["avg"].mutable_gen_op();
      avg.set_name("avg");
      avg.mutable_args()->Add()->set_s("foo_i");
    }, "not yet supported for text field facets"},
    {"range_sort", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_range_facet();
      facet.set_field("foo_i");
      facet.set_start(0);
      facet.set_end(10);
      facet.set_gap(1);
      auto& sort = *facet.mutable_sorts()->Add();
      sort.set_field("avg");
      sort.set_dir(proto::SortSpec_SortDir_ASC);
    }, "not yet supported for range facets"},
    {"range_mincount_zero", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_range_facet();
      facet.set_field("foo_i");
      facet.set_start(0);
      facet.set_end(10);
      facet.set_gap(1);
      facet.set_mincount(0);
    }, "not supported for range facets"},
    {"string_unknown_sort", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("cat_s");
      auto& sort = *facet.mutable_sorts()->Add();
      sort.set_field("not_a_subop");
      sort.set_dir(proto::SortSpec_SortDir_ASC);
    }, "unknown sort field"},
    {"string_two_sorts", [](proto::SearchRequest& req) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("cat_s");
      auto& sort1 = *facet.mutable_sorts()->Add();
      sort1.set_field("first");
      sort1.set_dir(proto::SortSpec_SortDir_ASC);
      auto& sort2 = *facet.mutable_sorts()->Add();
      sort2.set_field("second");
      sort2.set_dir(proto::SortSpec_SortDir_DESC);
    }, "multiple sort fields"}
  };

  for (const auto& testCase : cases) {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("test_unsupported_facet_options_" + testCase.name);
    testCase.configure(lreq->proto);

    lreq->engine.submit(*lreq, true);

    ASSERT_EQ(1, lreq->responses.size()) << testCase.name;
    const auto& error = lreq->responses[0]->proto.error();
    EXPECT_TRUE(lreq->responses[0]->proto.has_error()) << testCase.name << "\n" << lreq->toString();
    // Lock in the clear-message contract: the facet name and the specific
    // unsupported-option phrase must both appear.
    EXPECT_NE(error.find("'f'"), std::string::npos) << testCase.name << ": '" << error << "'";
    EXPECT_NE(error.find(testCase.expectSubstr), std::string::npos) << testCase.name << ": '" << error << "'";
    lreq->done();
  }
}

// Finding 1: a prepare-requiring query (force_prepare) on an empty index must
// still emit its nested ops. Pre-fix, doPrepareDomain's segnum<0 branch only
// called doneCollecting() and dropped the nested facet/avg.
TEST_F(FacetTest, emptyIndexForcePrepareNestedOps) {
  CollectionHelper helper;
  helper.clear();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_empty_index_force_prepare_nested_ops");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->mutable_force_prepare()->mutable_query()->set_all(true);

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("category_s");
  facet.set_limit(10);

  auto& avg = *(*topDocs.mutable_ops())["a"].mutable_gen_op();
  avg.set_name("avg");
  avg.mutable_args()->Add()->set_s("price_i");

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(0, docs.matches());
  // Both nested calculator families must still emit on an empty prepared index.
  ASSERT_TRUE(docs.ops().contains("f")) << lreq->toString();
  EXPECT_EQ(0, docs.ops().at("f").facet().bucket_ids().col_s().v_size());
  ASSERT_TRUE(docs.ops().contains("a")) << lreq->toString();
  EXPECT_TRUE(std::isnan(docs.ops().at("a").d()));

  lreq->done();
}

TEST_F(FacetTest, stringFacetMincountZeroShowsAllValues) {
  CollectionHelper helper;
  helper.clear();
  const int aDocs = 700;
  std::vector<Doc> docs;
  for (int i = 0; i < aDocs; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "a", "sel_s", "yes"));
  }
  for (int i = 0; i < 5; i++) {
    docs.push_back(flatdoc("id", std::to_string(1000 + i), "cat_s", "b", "sel_s", "no"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_string_facet_mincount_zero");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("sel_s");
  match.mutable_val()->set_s("yes");

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("cat_s");
  facet.set_limit(-1);
  facet.set_mincount(0);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("q").docs().ops().at("f").facet();
  ASSERT_EQ(2, facetResult.bucket_ids().col_s().v_size()) << lreq->toString();
  ASSERT_EQ(2, facetResult.counts_size());
  EXPECT_EQ("a", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(aDocs, facetResult.counts(0));
  EXPECT_EQ("b", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(0, facetResult.counts(1));

  lreq->done();
}

TEST_F(FacetTest, stringFacetMincountZeroPadsZerosByValue) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("id", "1", "cat_s", "x", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "x", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "y", "sel_s", "no"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "cat_s", "z", "sel_s", "no"), UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_string_facet_mincount_zero_small_cardinality");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("sel_s");
  match.mutable_val()->set_s("yes");

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("cat_s");
  facet.set_limit(-1);
  facet.set_mincount(0);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("q").docs().ops().at("f").facet();
  ASSERT_EQ(3, facetResult.bucket_ids().col_s().v_size()) << lreq->toString();
  ASSERT_EQ(3, facetResult.counts_size());
  EXPECT_EQ("x", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(2, facetResult.counts(0));
  EXPECT_EQ("y", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(0, facetResult.counts(1));
  EXPECT_EQ("z", facetResult.bucket_ids().col_s().v(2));
  EXPECT_EQ(0, facetResult.counts(2));

  lreq->done();
}

TEST_F(FacetTest, stringFacetMincountZeroPadsToFiniteLimit) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("id", "1", "cat_s", "a", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "cat_s", "a", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "cat_s", "b", "sel_s", "yes"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "cat_s", "c", "sel_s", "no"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "5", "cat_s", "d", "sel_s", "no"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "6", "cat_s", "e", "sel_s", "no"), UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_string_facet_mincount_zero_finite_limit");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("sel_s");
  match.mutable_val()->set_s("yes");

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("cat_s");
  facet.set_limit(4);
  facet.set_mincount(0);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("q").docs().ops().at("f").facet();
  ASSERT_EQ(4, facetResult.bucket_ids().col_s().v_size()) << lreq->toString();
  ASSERT_EQ(4, facetResult.counts_size());
  EXPECT_EQ("a", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(2, facetResult.counts(0));
  EXPECT_EQ("b", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(1, facetResult.counts(1));
  EXPECT_EQ("c", facetResult.bucket_ids().col_s().v(2));
  EXPECT_EQ(0, facetResult.counts(2));
  EXPECT_EQ("d", facetResult.bucket_ids().col_s().v(3));
  EXPECT_EQ(0, facetResult.counts(3));

  lreq->done();
}

TEST_F(FacetTest, fullTextFacetMincountZeroShowsOutOfDomainTerms) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("id", "1", "sel_s", "yes", "body_w", "alpha beta"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "sel_s", "yes", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "sel_s", "no", "body_w", "gamma"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "sel_s", "no", "body_w", "delta"), UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_full_text_facet_mincount_zero");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("sel_s");
  match.mutable_val()->set_s("yes");

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("body_w");
  facet.set_limit(-1);
  facet.set_mincount(0);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("q").docs().ops().at("f").facet();
  ASSERT_EQ(4, facetResult.bucket_ids().col_s().v_size()) << lreq->toString();
  ASSERT_EQ(4, facetResult.counts_size());
  EXPECT_EQ("alpha", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(2, facetResult.counts(0));
  EXPECT_EQ("beta", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(1, facetResult.counts(1));
  EXPECT_EQ("delta", facetResult.bucket_ids().col_s().v(2));
  EXPECT_EQ(0, facetResult.counts(2));
  EXPECT_EQ("gamma", facetResult.bucket_ids().col_s().v(3));
  EXPECT_EQ(0, facetResult.counts(3));

  lreq->done();
}

// Finding 3: avg op nested under a selective query (sparse ArrDocSet domain).
// Locks in correct sparse-domain handling after dropping the BitDocSet C-cast.
TEST_F(FacetTest, avgNestedSparseArrayDomain) {
  CollectionHelper helper;
  helper.clear();
  for (int i = 0; i < 100; i++) {
    std::string id = std::to_string(i);
    if (i == 5) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "val_i", 10), UpdateMessage::NO_COMMIT);
    } else if (i == 50) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "val_i", 20), UpdateMessage::NO_COMMIT);
    } else if (i == 95) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "val_i", 30), UpdateMessage::NO_COMMIT);
    } else {
      helper.index(flatdoc("id", id, "val_i", 999), UpdateMessage::NO_COMMIT);
    }
  }
  helper.commit();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_avg_nested_sparse_array_domain");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("pick_s");
  match.mutable_val()->set_s("yes");

  auto& avg = *(*topDocs.mutable_ops())["a"].mutable_gen_op();
  avg.set_name("avg");
  avg.mutable_args()->Add()->set_s("val_i");

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(3, docs.matches());
  // avg over the 3 in-domain docs: (10+20+30)/3 = 20
  EXPECT_DOUBLE_EQ(20.0, docs.ops().at("a").d());

  lreq->done();
}

// Finding 4: full-text facet per-doc missing. A segment that has the field but
// where some in-domain docs lack any token must count those as missing.
// Pre-fix, missing was only counted when the whole segment lacked the field.
TEST_F(FacetTest, fullTextFacetMixedPresenceMissing) {
  CollectionHelper helper;
  helper.clear();
  // single segment, mixed presence: 2 docs with body_w, 2 without.
  helper.index(flatdoc("id", "1", "body_w", "alpha beta"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "2", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "3", "other_s", "x"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "4", "other_s", "y"), UpdateMessage::COMMIT);

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_full_text_facet_mixed_presence_missing");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.mutable_query()->set_all(true);

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("body_w");
  facet.set_limit(-1);
  facet.set_missing(true);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& facetResult = lreq->responses[0]->proto.ops().at("q").docs().ops().at("f").facet();
  ASSERT_EQ(2, facetResult.bucket_ids().col_s().v_size());
  EXPECT_EQ("alpha", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ(2, facetResult.counts(0));
  EXPECT_EQ("beta", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(1, facetResult.counts(1));
  // docs 3 and 4 have no body_w token -> missing = 2 (pre-fix counted 0).
  EXPECT_EQ(2, facetResult.missing());

  lreq->done();
}

// Full-text facet missing under a selective query: the domain is a sparse
// ArrDocSet, so missing is computed by intersecting the indexed docs-with-value
// set with the domain. Verifies out-of-domain docs that have the field are
// excluded from both the buckets and the have-field count.
TEST_F(FacetTest, fullTextFacetSparseDomainMissing) {
  CollectionHelper helper;
  helper.clear();
  for (int i = 0; i < 100; i++) {
    std::string id = std::to_string(i);
    if (i == 5) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "body_w", "alpha"), UpdateMessage::NO_COMMIT);
    } else if (i == 50) {
      helper.index(flatdoc("id", id, "pick_s", "yes", "body_w", "beta"), UpdateMessage::NO_COMMIT);
    } else if (i == 95) {
      helper.index(flatdoc("id", id, "pick_s", "yes"), UpdateMessage::NO_COMMIT); // in domain, no body_w
    } else if (i == 10 || i == 20 || i == 30) {
      helper.index(flatdoc("id", id, "body_w", "gamma"), UpdateMessage::NO_COMMIT); // has body_w, not in domain
    } else {
      helper.index(flatdoc("id", id, "other_s", "z"), UpdateMessage::NO_COMMIT);
    }
  }
  helper.commit();

  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_full_text_facet_sparse_domain_missing");

  auto& topDocs = *(*lreq->proto.mutable_ops())["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  auto& match = *topDocs.mutable_query()->mutable_match();
  match.set_field("pick_s");
  match.mutable_val()->set_s("yes");

  auto& facet = *(*topDocs.mutable_ops())["f"].mutable_field_facet();
  facet.set_field("body_w");
  facet.set_limit(-1);
  facet.set_missing(true);

  lreq->engine.submit(*lreq, true);

  ASSERT_EQ(1, lreq->responses.size()) << lreq->toString();
  ASSERT_FALSE(lreq->responses[0]->proto.has_error()) << lreq->toString();
  const auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(3, docs.matches());
  const auto& facetResult = docs.ops().at("f").facet();
  // Only in-domain docs contribute: alpha (doc 5) and beta (doc 50). The gamma
  // docs have body_w but are out of domain, so they appear in neither the
  // buckets nor the have-field count.
  ASSERT_EQ(2, facetResult.bucket_ids().col_s().v_size());
  EXPECT_EQ("alpha", facetResult.bucket_ids().col_s().v(0));
  EXPECT_EQ("beta", facetResult.bucket_ids().col_s().v(1));
  EXPECT_EQ(1, facetResult.counts(0));
  EXPECT_EQ(1, facetResult.counts(1));
  // domain = {5, 50, 95}; doc 95 has no body_w -> missing = 1.
  EXPECT_EQ(1, facetResult.missing());

  lreq->done();
}

//
// Comprehensive random faceting test class
//
class RandomFacetTest : public SoluxTest {
protected:
  static constexpr int MERGE_FACTOR = 10;
  static constexpr int NUM_FIELDS = 8;  // we do a linear search on field names in the document, so keep this small.
  static constexpr int PERCENT_PARA = 80;  // percent of the time we do parallel faceting on a single request

  // Field definitions with various characteristics
  struct FieldDef {
    std::string name;
    bool isInt;
    bool multiValued;
    int numUniqueValues;
    int maxValuesPerDoc;
    int sparsityPercent;  // 0 = always missing, 100 = always present
  };
  
  // No need for FacetRequest struct anymore - we'll just use the protobuf directly
  
  // Model to track documents and calculate facets dynamically
  class Model {
  public:
    std::vector<Doc> docs;
    
    void addDoc(const Doc& doc) {
      docs.push_back(doc);
    }
    
    std::vector<size_t> allDocIndexes() const {
      std::vector<size_t> out;
      out.reserve(docs.size());
      for (size_t i = 0; i < docs.size(); i++) {
        out.push_back(i);
      }
      return out;
    }

    std::vector<size_t> matchingDocIndexes(const proto::Query& query) const {
      std::vector<size_t> out;
      out.reserve(docs.size());
      for (size_t i = 0; i < docs.size(); i++) {
        if (matchesQuery(docs[i], query)) {
          out.push_back(i);
        }
      }
      return out;
    }

    static void findAll(const Doc& doc, std::string_view name, std::vector<const FieldVal*>& out) {
      out.clear();
      for (const auto& nv : doc) {
        if (nv.name == name) {
          out.push_back(&nv.val);
        }
      }
    }

    // Dump model state for debugging
    void dumpModel(const std::string& field, const proto::Query& query) const {
      LOG_ERROR("=== Model Dump for field '{}' ===", field);
      LOG_ERROR("Total docs: {}", docs.size());
      
      // Count matching docs and field values
      boost::unordered_flat_map<std::string, int> valueCounts;
      boost::unordered_flat_map<int64_t, int> intValueCounts;
      int matchingDocs = 0;
      int docsWithField = 0;
      int missingField = 0;
      std::vector<const FieldVal*> vals;
      for (const auto& doc : docs) {
        bool matches = matchesQuery(doc, query);
        if (!matches) continue;
      
        matchingDocs++;
        findAll(doc, field, vals);
        if (!vals.empty()) {
          docsWithField++;
          for (auto* val : vals) {
            if (auto* strVal = std::get_if<std::string>(val)) {
              valueCounts[*strVal]++;
            } else if (auto* intVal = std::get_if<int64_t>(val)) {
              intValueCounts[*intVal]++;
            }
          }
        } else {
          missingField++;
        }
      }
      
      std::string queryStr;
      if (query.has_match()) {
        queryStr = "match(" + std::string(query.match().field()) + "=" + 
          (query.match().val().has_s() ? std::string(query.match().val().s()) : 
           std::to_string(query.match().val().i())) + ")";
      } else {
        queryStr = "all";
      }
      LOG_ERROR("Query: {}", queryStr);
      LOG_ERROR("Matching docs: {}", matchingDocs);
      LOG_ERROR("Docs with field '{}': {}", field, docsWithField);
      LOG_ERROR("Missing field '{}': {}", field, missingField);
      
      if (!valueCounts.empty()) {
        LOG_ERROR("String value distribution:");
        std::vector<std::pair<std::string, int>> sortedValues(valueCounts.begin(), valueCounts.end());
        std::sort(sortedValues.begin(), sortedValues.end());
        for (const auto& [val, count] : sortedValues) {
          LOG_ERROR("  '{}': {}", val, count);
        }
      }
      
      if (!intValueCounts.empty()) {
        LOG_ERROR("Int value distribution:");
        std::vector<std::pair<int64_t, int>> sortedIntValues(intValueCounts.begin(), intValueCounts.end());
        std::sort(sortedIntValues.begin(), sortedIntValues.end());
        for (const auto& [val, count] : sortedIntValues) {
          LOG_ERROR("  {}: {}", val, count);
        }
      }
      
      LOG_ERROR("=== End Model Dump ===");
    }
    
    // Check if a document matches a query
    bool matchesQuery(const Doc& doc, const proto::Query& query) const {
      if (query.has_all() && query.all()) {
        return true;
      }
      
      if (query.has_match()) {
        const auto& match = query.match();
        std::vector<const FieldVal*> vals;
        findAll(doc, match.field(), vals);
        if (vals.empty()) return false;
        if (match.val().has_s()) {
          for (auto* val : vals) {
            if (auto* strVal = std::get_if<std::string>(val)) {
              if (*strVal == match.val().s()) return true;
            }
          }
        } else if (match.val().has_i()) {
          for (auto* val : vals) {
            if (auto* intVal = std::get_if<int64_t>(val)) {
              if (*intVal == match.val().i()) return true;
            }
          }
        }
        return false;
      }
      
      // TODO: Add support for range, boolean queries
      return true;  // Default to matching for unsupported query types
    }
    // Apply sorting and limits to facet results
    template<typename T>
    std::vector<std::pair<T, int64_t>> sortAndLimitFacets(
        const boost::unordered_flat_map<T, int64_t>& counts,
        int limit,
        int64_t minCount) const {
      
      std::vector<std::pair<T, int64_t>> result;
      for (const auto& [val, count] : counts) {
        if (count >= minCount) {
          result.push_back({val, count});
        }
      }
      
      // Sort by count desc, then value asc
      std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });
      
      if (limit >= 0 && result.size() > static_cast<size_t>(limit)) {
        result.resize(limit);
      }
      
      return result;
    }
    
    // Process all operations in an ops map recursively
    void processOps(const google::protobuf::Map<std::string, proto::SearchOp>& requestOps,
                    google::protobuf::Map<std::string, proto::Val>* responseOps) const {
      
      auto allDocs = allDocIndexes();
      for (const auto& [opName, searchOp] : requestOps) {
        if (searchOp.has_field_facet()) {
          // Process field facet - facets at root level operate on all documents
          (*responseOps)[opName] = calculateFieldFacet(searchOp.field_facet(), allDocs);
        } else if (searchOp.has_top_docs()) {
          // Process top docs query (which may have nested ops)
          const auto& topDocs = searchOp.top_docs();
          proto::Val result;
          auto* docList = result.mutable_docs();
          
          // The query in top_docs defines the domain for nested ops
          proto::Query effectiveQuery = topDocs.has_query() ? topDocs.query() : proto::Query();
          if (!effectiveQuery.kind_case()) {
            effectiveQuery.set_all(true);  // Default to all if no query specified
          }
          
          auto matchingDocs = matchingDocIndexes(effectiveQuery);
          docList->set_matches(matchingDocs.size());
          
          // Process any nested operations under this query with the query as their domain
          if (topDocs.ops_size() > 0) {
            processOpsWithDomain(topDocs.ops(), docList->mutable_ops(), matchingDocs);
          }
          
          (*responseOps)[opName] = result;
        }
        // Add other operation types as needed
      }
    }
    
    // Process operations with a specific domain query (for nested ops)
    void processOpsWithDomain(const google::protobuf::Map<std::string, proto::SearchOp>& requestOps,
                               google::protobuf::Map<std::string, proto::Val>* responseOps,
                               const std::vector<size_t>& domainDocs) const {
      
      for (const auto& [opName, searchOp] : requestOps) {
        if (searchOp.has_field_facet()) {
          // Nested facet uses the domain query from its parent
          (*responseOps)[opName] = calculateFieldFacet(searchOp.field_facet(), domainDocs);
        } else if (searchOp.has_top_docs()) {
          // Nested top_docs would combine its query with the domain query
          // This is more complex and would need proper query combination logic
          // For now, just using the nested query
          const auto& topDocs = searchOp.top_docs();
          proto::Val result;
          auto* docList = result.mutable_docs();
          
          if (topDocs.has_query()) {
            auto matchingDocs = matchingDocIndexes(topDocs.query());
            docList->set_matches(matchingDocs.size());
            if (topDocs.ops_size() > 0) {
              processOpsWithDomain(topDocs.ops(), docList->mutable_ops(), matchingDocs);
            }
          } else {
            docList->set_matches(domainDocs.size());
            if (topDocs.ops_size() > 0) {
              processOpsWithDomain(topDocs.ops(), docList->mutable_ops(), domainDocs);
            }
          }
          
          (*responseOps)[opName] = result;
        }
      }
    }
          
    // Calculate expected facet results for a single field facet
    proto::Val calculateFieldFacet(const proto::FieldFacet& facetOp,
                                   const std::vector<size_t>& domainDocs) const {
      proto::Val result;
      auto* facetResult = result.mutable_facet();
          
      std::string fieldName(facetOp.field());
      int64_t limit = facetOp.limit();
      bool hasMin = facetOp.has_mincount();
      int64_t mincount = hasMin ? std::max<int64_t>(facetOp.mincount(), 1) : 1;
      bool includeMissing = facetOp.missing();
    
      // Determine field type from field name convention
      bool isIntField = fieldName.ends_with("_i") || fieldName.ends_with("_is");
      bool showZeros = !isIntField && hasMin && facetOp.mincount() == 0;
      
      // Count values for documents matching the domain query
      boost::unordered_flat_map<int64_t, int64_t> intCounts;
      boost::unordered_flat_map<std::string, int64_t> strCounts;
      int64_t missingCount = 0;
      
      std::vector<const FieldVal*> vals;
      if (showZeros) {
        for (const auto& doc : docs) {
          findAll(doc, fieldName, vals);
          for (auto* val : vals) {
            if (auto* strVal = std::get_if<std::string>(val)) {
              strCounts.try_emplace(*strVal, 0);
            }
          }
        }
      }
      
      for (auto docIdx : domainDocs) {
        const auto& doc = docs[docIdx];
        findAll(doc, fieldName, vals);
        bool hasField = false;
        for (auto* val : vals) {
          if (isIntField) {
            if (auto* intVal = std::get_if<int64_t>(val)) {
              intCounts[*intVal]++;
              hasField = true;
            }
          } else {
            if (auto* strVal = std::get_if<std::string>(val)) {
              strCounts[*strVal]++;
              hasField = true;
            }
          }
        }
        if (!hasField) {
          missingCount++;
        }
      }
      
      // Apply mincount, sort, and limit, then populate protobuf result
      if (isIntField) {
        auto sorted = sortAndLimitFacets(intCounts, limit, mincount);
        auto* bucketIds = facetResult->mutable_bucket_ids()->mutable_col_i();
        for (const auto& [val, count] : sorted) {
          bucketIds->add_v(val);
          facetResult->add_counts(count);
        }
      } else {
        auto sorted = sortAndLimitFacets(strCounts, limit, showZeros ? 0 : mincount);
        auto* bucketIds = facetResult->mutable_bucket_ids()->mutable_col_s();
        for (const auto& [val, count] : sorted) {
          bucketIds->add_v(val);
          facetResult->add_counts(count);
        }
      }
      
      // Set missing count if requested
      if (includeMissing) {
        facetResult->set_missing(missingCount);
      }
      
      // Process any nested operations (sub-facets) if present
      if (facetOp.ops_size() > 0) {
        // For each bucket, calculate sub-operations with documents filtered to that bucket
        // This would require creating a filtered domain for each bucket
        // For now, leaving as TODO since it requires more complex domain filtering
        // processOps(facetOp.ops(), facetResult->mutable_ops(), bucketSpecificQuery);
      }
      
      return result;
    }
  };
  
  // Build index with random data using parallel segment construction
  void buildRandomIndex(CollectionHelper& helper, Model& model, Rng& rng,
                       const std::vector<FieldDef>& fields,
                       int maxSegments = MERGE_FACTOR-1, int maxDocsPerSegment = 100) {
    helper.clear();
    
    // Random number of segments
    int numSegments = rng.rint(1, std::min(maxSegments, MERGE_FACTOR));
    

    
    // Get index writer for parallel segment building
    auto iw = helper.getIndexWriter();
    
    // Pre-obtain inverters for parallel processing
    std::vector<Inverter*> inverters;
    inverters.reserve(numSegments);
    for (int i = 0; i < numSegments; i++) {
      inverters.push_back(&iw->obtainInverter());
    }
    
    // Track documents for the model
    std::vector<std::vector<Doc>> segmentDocs(numSegments);
    
    // Get a single seed for all segments (don't call rng() inside parallel tasks)
    uint64_t baseSeed = rng();
    
    // Build segments in parallel using TBB
    tbb::task_group tg;
    for (int segNum = 0; segNum < numSegments; segNum++) {
      tg.run([&, segNum, baseSeed]() {
        Rng segRng(baseSeed + segNum);  // Deterministic seed per segment

        // Pre-calculate which fields should exist in all documents in this segment (5% chance per field)
        // and which fields should not exist at all in this segment (5% chance per field)
        boost::container::small_vector<uint8_t,8> fieldExists(fields.size());
        for (size_t i = 0; i < fields.size(); i++) {
          fieldExists[i] = segRng.rint(100);
        }

        Inverter& inverter = *inverters[segNum];

        // Random number of documents per segment
        int segDocCount = segRng.rint(1, maxDocsPerSegment);
        
        // get IndexHandlers for the fields that exist in this segment
        auto* idHandler = &inverter.getIndexHandler("id");
        boost::container::small_vector<Inverter::IndexHandler*,8> handlers(fields.size());
        for (size_t i = 0; i < fields.size(); i++) {
          if (fieldExists[i] < 5) {
            continue; // Field does not exist in this segment
          }
          handlers[i] = &inverter.getIndexHandler(fields[i].name);
        }
        
        for (int docIdx = 0; docIdx < segDocCount; docIdx++) {
          Doc doc;
          int64_t docId = segNum * 10000 + docIdx;
          doc.push_back({"id", std::to_string(docId)});
          
          inverter.startDoc();
          idHandler->index(inverter, std::to_string(docId));
          
          // Add random field values
          for (size_t fieldIdx = 0; fieldIdx < fields.size(); fieldIdx++) {
            const auto& field = fields[fieldIdx];

            if (fieldExists[fieldIdx] < 5) {
              continue;
            }

            // most of the time do a normal sparsity check
            if (fieldExists[fieldIdx] < 95 && !(segRng.rint(100) < field.sparsityPercent)) {
              continue;
            }

            if (field.isInt) {
              if (field.multiValued) {
                int count = segRng.rint(1, field.maxValuesPerDoc + 1);
                auto vals = sampleDistinctInts(segRng, field.numUniqueValues, count);
                handlers[fieldIdx]->index(inverter, std::span<const int64_t>(vals.data(), vals.size()));
                for (auto val : vals) {
                  doc.push_back({field.name, val});
                }
              } else {
                int64_t val = segRng.rint(field.numUniqueValues);
                handlers[fieldIdx]->index(inverter, val);
                doc.push_back({field.name, val});
              }
            }
            else {
              if (field.multiValued) {
                int count = segRng.rint(1, field.maxValuesPerDoc + 1);
                auto ords = sampleDistinctInts(segRng, field.numUniqueValues, count);
                std::vector<std::string> vals;
                std::vector<std::string_view> views;
                vals.reserve(ords.size());
                views.reserve(ords.size());
                for (auto ord : ords) {
                  vals.push_back("v" + std::to_string(ord));
                }
                for (auto& val : vals) {
                  views.push_back(val);
                }
                handlers[fieldIdx]->index(inverter, std::span<std::string_view>(views.data(), views.size()));
                for (auto& val : vals) {
                  doc.push_back({field.name, val});
                }
              } else {
                std::string val = "v" + std::to_string(segRng.rint(field.numUniqueValues));
                handlers[fieldIdx]->index(inverter, val);
                doc.push_back({field.name, val});
              }
            }

          }
          
          inverter.finishDoc();
          segmentDocs[segNum].push_back(doc);
        }
        
        iw->releaseInverter(inverter, true);  // Request immediate flush
      });
    }
    tg.wait();
    
    // Add all documents to the model
    for (const auto& segDocs : segmentDocs) {
      for (const auto& doc : segDocs) {
        model.addDoc(doc);
      }
    }
    
    helper.commit();
  }
  
  static std::vector<int64_t> sampleDistinctInts(Rng& rng, int max, int count) {
    std::vector<int64_t> vals;
    vals.reserve(count);
    while ((int)vals.size() < count) {
      int64_t val = rng.rint(max);
      bool exists = false;
      for (auto existing : vals) {
        if (existing == val) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        vals.push_back(val);
      }
    }
    return vals;
  }

  // Generate a random facet configuration directly on the protobuf
  static void generateRandomFacet(Rng& rng, proto::FieldFacet* facet, const FieldDef& field) {
    facet->set_field(field.name);
    
    // Random limit - avoid problematic edge cases for now
    int limitChoice = rng.rint(5);
    if (limitChoice == 0) {
      facet->set_limit(-1);  // No limit
    } else if (limitChoice == 1) {
      facet->set_limit(1);  // Exactly 1 result
    } else if (limitChoice == 2) {
      facet->set_limit(5);  // Small limit
    } else if (limitChoice == 3) {
      facet->set_limit(20);  // Medium limit
    } else {
      facet->set_limit(rng.rint(1, 50));  // Random limit
    }
    // Random mincount. Leaving it unset is distinct from explicit 0.
    int mincountChoice = rng.rint(field.isInt ? 4 : 5);
    if (mincountChoice == 0) {
      // unset: parser maps this to minCount=-1, effective min 1
    } else if (!field.isInt && mincountChoice == 1) {
      facet->set_mincount(0);  // string/id facets support zero-count buckets
    } else if (mincountChoice == 2) {
      facet->set_mincount(2);
    } else if (mincountChoice == 3) {
      facet->set_mincount(5);
    } else {
      facet->set_mincount(1 + rng.rint(3));
    }
    
    // Random missing
    facet->set_missing(rng.rbool());
    
    // TODO: Add sub-facets (ops) generation
  }

public:
  void runRandomTest(int numIndexes = 20, int requestsPerIndex = 20, 
                     int maxSegments = MERGE_FACTOR-1, int maxDocsPerSegment = 250) {
    
    for (int iteration = 0; iteration < numIndexes; iteration++) {
      CollectionHelper helper;
      Model model;
      
      // Generate random field definitions for this iteration
      std::vector<FieldDef> fields;
      for (int i = 0; i < NUM_FIELDS; i++) {
        FieldDef field;
        int kind = i % 4;
        if (kind == 0) {
          field.name = "field" + std::to_string(i) + "_i";
          field.isInt = true;
          field.multiValued = false;
        } else if (kind == 1) {
          field.name = "field" + std::to_string(i) + "_s";
          field.isInt = false;
          field.multiValued = false;
        } else if (kind == 2) {
          field.name = "field" + std::to_string(i) + "_is";
          field.isInt = true;
          field.multiValued = true;
        } else {
          field.name = "field" + std::to_string(i) + "_ss";
          field.isInt = false;
          field.multiValued = true;
        }
        field.maxValuesPerDoc = field.multiValued ? 2 + rng.rint(3) : 1;
        int cardClass = i % 3;
        if (cardClass == 0) {
          field.numUniqueValues = 3 + rng.rint(8);
          field.sparsityPercent = 70 + rng.rint(30);
        } else if (cardClass == 1) {
          field.numUniqueValues = 20 + rng.rint(80);
          field.sparsityPercent = 35 + rng.rint(60);
        } else {
          field.numUniqueValues = 300 + rng.rint(1200);
          field.sparsityPercent = 20 + rng.rint(50);
        }
        field.numUniqueValues = std::max(field.numUniqueValues, field.maxValuesPerDoc);
        fields.push_back(field);
      }
      
      // Build index with parallel segment construction
      buildRandomIndex(helper, model, rng, fields, maxSegments, maxDocsPerSegment);
      
      // Run multiple random facet tests on this index in parallel
      int numParallelTests = requestsPerIndex;
      
      // Run tests in parallel using TBB
      tbb::parallel_for(tbb::blocked_range<int>(0, numParallelTests),
        [&](const tbb::blocked_range<int>& range) {
          // Create a local RNG for this thread with deterministic seed
          Rng localRng(iteration * 1000000 + range.begin());
          
          for (int testNum = range.begin(); testNum != range.end(); ++testNum) {
            // Advance RNG to ensure different seed for each test
            localRng(); 
            
            auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
            lreq->proto.mutable_collection()->add_name("main");
            lreq->proto.set_request_id("random_test_" + std::to_string(iteration) + "_" + std::to_string(testNum));
        
            auto& ops = *lreq->proto.mutable_ops();
            
            // Create a query operation
            auto& topDocs = *ops["q"].mutable_top_docs();
            topDocs.set_get_number(true);
            
            // Generate query directly: 80% match query, 20% all query
            proto::Query* query = topDocs.mutable_query();
            if (localRng.rint(100) < 80 && !fields.empty()) {
              // Only use string fields for match queries
              std::vector<int> candidateFields;
              for (int idx = 0; idx < (int)fields.size(); idx++) {
                if (!fields[idx].isInt && fields[idx].sparsityPercent >= 50) {
                  candidateFields.push_back(idx);
                }
              }
              
              if (!candidateFields.empty()) {
                int queryFieldIdx = candidateFields[localRng.rint((int)candidateFields.size())];
                const auto& queryField = fields[queryFieldIdx];
                auto& matchQuery = *query->mutable_match();
                matchQuery.set_field(queryField.name);
                int valueIdx = localRng.rint(queryField.numUniqueValues);
                matchQuery.mutable_val()->set_s("v" + std::to_string(valueIdx));
              } else {
                query->set_all(true);
              }
            } else {
              query->set_all(true);
            }

            // Decide on root vs nested facets
            bool useRootFacets = localRng.rint(100) < 10;
            
            // Get reference to the correct ops map (root level or under query)
            auto* opsMap = useRootFacets ? &ops : topDocs.mutable_ops();
            
            LOG_TRACE("Creating {} facets, root={}", fields.size(), useRootFacets);
            for (size_t f = 0; f < fields.size(); f++) {
              const auto& field = fields[f];
              std::string facetName = "f" + std::to_string(f);
            
              // Generate facet directly in the ops map
              auto& facet = *(*opsMap)[facetName].mutable_field_facet();
              generateRandomFacet(localRng, &facet, field);
            }
        
            // Execute the request
            bool para = (localRng.rint(100) < PERCENT_PARA);
            lreq->engine.submit(*lreq, para);
            
            ASSERT_EQ(1, lreq->responses.size());

            // Verify facet results
            const auto& response = lreq->responses[0]->proto;
            
            // Calculate expected results for all operations using an arena
            auto* arena = createArena(4096);  // 4KB initial arena size
            auto* expectedResponse = google::protobuf::Arena::Create<proto::SearchResponse>(arena);
            
            // Process all operations to calculate expected results
            model.processOps(lreq->proto.ops(), expectedResponse->mutable_ops());
            
            // Now verify each facet result against expected values
            // Iterate over the ops map we populated with facets
            for (const auto& [facetName, searchOp] : *opsMap) {
              // Skip non-facet operations (like the query itself if at root level)
              if (!searchOp.has_field_facet()) {
                continue;
              }
              
              // Get expected result from our calculated response
              const proto::Val* expectedVal = nullptr;
              if (useRootFacets) {
                // Root-level facet
                if (expectedResponse->ops().contains(facetName)) {
                  expectedVal = &expectedResponse->ops().at(facetName);
                }
              } else {
                // Nested facet under query
                if (expectedResponse->ops().contains("q") && 
                    expectedResponse->ops().at("q").has_docs() &&
                    expectedResponse->ops().at("q").docs().ops().contains(facetName)) {
                  expectedVal = &expectedResponse->ops().at("q").docs().ops().at(facetName);
                }
              }
              
              ASSERT_TRUE(expectedVal != nullptr && expectedVal->has_facet())
                << "Expected facet result not found for " << facetName;
              const auto& expectedFacet = expectedVal->facet();
              
              // Extract actual facet result
              const proto::FacetResult* actualFacet = nullptr;
              
              if (useRootFacets) {
                // Check root-level operations
                if (response.ops().contains(facetName)) {
                  actualFacet = &response.ops().at(facetName).facet();
                }
              } else {
                // Check nested operations under query - access through docs().ops()
                if (response.ops().contains("q") && 
                    response.ops().at("q").has_docs() &&
                    response.ops().at("q").docs().ops().contains(facetName)) {
                  actualFacet = &response.ops().at("q").docs().ops().at(facetName).facet();
                }
              }
              
              ASSERT_TRUE(actualFacet != nullptr) 
                << "Iteration " << iteration << ", test " << testNum 
                << ", facet " << facetName << " - Facet not found in response"
                << " (useRootFacets=" << useRootFacets << ")";
              
              // Verify bucket IDs and counts
              if (expectedFacet.bucket_ids().has_col_i()) {
                ASSERT_TRUE(actualFacet->bucket_ids().has_col_i())
                  << "Expected integer buckets for field " << std::string(searchOp.field_facet().field());
                
                const auto& actualBuckets = actualFacet->bucket_ids().col_i();
                const auto& expectedBuckets = expectedFacet.bucket_ids().col_i();
                ASSERT_EQ(actualBuckets.v_size(), expectedBuckets.v_size())
                  << "Mismatch in number of buckets for facet " << facetName;
                
                for (int i = 0; i < actualBuckets.v_size(); i++) {
                  if (actualBuckets.v(i) != expectedBuckets.v(i)) {
                    EXPECT_EQ(actualBuckets.v(i), expectedBuckets.v(i))  // place breakpoint here
                    << "Bucket " << i << " value mismatch for facet " << facetName;
                  }
                }
              } else {
                ASSERT_TRUE(actualFacet->bucket_ids().has_col_s())
                  << "Expected string buckets for field " << std::string(searchOp.field_facet().field());
                
                const auto& actualBuckets = actualFacet->bucket_ids().col_s();
                const auto& expectedBuckets = expectedFacet.bucket_ids().col_s();
                ASSERT_EQ(actualBuckets.v_size(), expectedBuckets.v_size())
                  << "Mismatch in number of buckets for facet " << facetName;
                
                for (int i = 0; i < actualBuckets.v_size(); i++) {
                  EXPECT_EQ(actualBuckets.v(i), expectedBuckets.v(i))
                    << "Bucket " << i << " value mismatch for facet " << facetName;
                }
              }
              
              // Verify counts
              ASSERT_EQ(actualFacet->counts_size(), expectedFacet.counts_size())
                << "Mismatch in number of counts for facet " << facetName;
              
              for (int i = 0; i < expectedFacet.counts_size(); i++) {
                EXPECT_EQ(actualFacet->counts(i), expectedFacet.counts(i))
                  << "Count mismatch for bucket " << i << " in facet " << facetName;
              }
              
              // Verify missing count if requested
              if (searchOp.field_facet().missing()) {
                EXPECT_EQ(actualFacet->missing(), expectedFacet.missing())
                  << "Missing count mismatch for facet " << facetName;
              }
            }
            
            // Clean up the arena
            releaseArena(arena);
            
            lreq->done();
          }  // end of for loop in lambda
        });  // end of parallel_for

    }  // end of iteration loop
  }  // end of runRandomTest
};


TEST_F(RandomFacetTest, randomFaceting) {
  runRandomTest(12, 120);  // 12 indexes, 120 requests per index = 1440 requests, one facet per field
}
