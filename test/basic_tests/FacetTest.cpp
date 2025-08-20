#include <gtest/gtest.h>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

class FacetTest : public SoluxTest {
protected:
};

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

TEST_F(FacetTest, singleSegment) {
  CollectionHelper helper;
  helper.clear();
  
  // Add some documents with integer field using dynamic field naming
  for (int i = 0; i < 5; i++) {
    helper.index(flatdoc("id", std::to_string(i), "price_i", i * 10), UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  
  // Create a search request with faceting
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  lreq->proto.set_request_id("test_single_segment");
  
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