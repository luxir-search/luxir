#include <gtest/gtest.h>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/FieldSortCollector2.h"
#include "solux/search/SortField.h"
#include "solux/util/random.h"
#include <charconv>
#include <algorithm>

using namespace solux;
using namespace solux::test;

class SortCollectorTest : public SoluxTest {
};

TEST_F(SortCollectorTest, testPQ) {
  class SortDoc {
  public:
    int doc;
    int64_t sortValue;
  };

  // for an ascending compare we want the least competitive (highest sortValue) at the top of the heap
  // This is the normal case for a max-heap, so the sort order is just the natural order
  auto ascendingCompare = [](const SortDoc& a, const SortDoc& b) {
    if (a.sortValue != b.sortValue) {
      return a.sortValue < b.sortValue;
    }
    return a.doc < b.doc; // tie-breaker, low docid first
  };

  std::vector<SortDoc> sortDocs(3);
  DirectPQ<SortDoc, decltype(ascendingCompare)> pq(sortDocs);

  pq.insertWithOverflow({1, 500});
  pq.insertWithOverflow({2, 800});
  pq.insertWithOverflow({3, 200});
  pq.insertWithOverflow({4, 400});
  pq.insertWithOverflow({5, 100});
  pq.insertWithOverflow({6, 400});  // repeated value, tie-break with docid ascending
  pq.insertWithOverflow({7, 700});

  // 100 200 400 500 700 800 - should have 100,200,400 in the heap with the least competative at top()
  ASSERT_EQ(pq.top().sortValue, 400);
  ASSERT_EQ(pq.top().doc, 4);

  // now using the standard sort_heap with the comparator for an ascending sort should result in sorted order
  std::sort_heap(sortDocs.begin(), sortDocs.end(), ascendingCompare);
  ASSERT_EQ(sortDocs[0].sortValue, 100);
}

TEST_F(SortCollectorTest, SortByPriceAscending) {
  CollectionHelper helper;
  helper.clear();

  // Add documents with different prices
  helper.index(flatdoc("id_s", "doc1", "price_i", 100, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50, "rating_i", 4), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150, "rating_i", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc4", "price_i", 75, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "price_i", 100, "rating_i", 4), UpdateMessage::COMMIT);

  // Create a search request that sorts by price ascending
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");

  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(10);

  // Match all documents
  topDocs.mutable_query()->set_all(true);

  // Sort by price ascending
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);

  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");

  lreq->engine.submit(*lreq, true);

  ASSERT_GT(lreq->responses.size(), 0) << "No responses received";
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();

  ASSERT_EQ(5, docs.matches());

  // Check if we have columns
  ASSERT_GT(docs.columns_size(), 0) << "No columns returned";

  // Check if values were actually loaded
  auto& idCol = docs.columns().at("id_s").col_s();
  auto& priceCol = docs.columns().at("price_i").col_i();

  ASSERT_GT(idCol.v_size(), 0) << "No id values loaded";
  ASSERT_GT(priceCol.v_size(), 0) << "No price values loaded";

  // Debug: print what we got
  for (int i = 0; i < idCol.v_size(); i++) {
    std::cout << "Position " << i << ": " << idCol.v(i) << " price=" << priceCol.v(i) << std::endl;
  }
  
  // Verify sort order by price: doc2(50), doc4(75), doc1(100), doc5(100), doc3(150)
  ASSERT_EQ("doc2", idCol.v(0));
  ASSERT_EQ(50, priceCol.v(0));

  ASSERT_EQ("doc4", docs.columns().at("id_s").col_s().v(1));
  ASSERT_EQ(75, docs.columns().at("price_i").col_i().v(1));

  // doc1 and doc5 have same price, so they should be ordered by docid
  ASSERT_EQ(100, docs.columns().at("price_i").col_i().v(2));
  ASSERT_EQ(100, docs.columns().at("price_i").col_i().v(3));

  ASSERT_EQ("doc3", docs.columns().at("id_s").col_s().v(4));
  ASSERT_EQ(150, docs.columns().at("price_i").col_i().v(4));

  lreq->done();
}

TEST_F(SortCollectorTest, DISABLED_SortByMultipleFields) {
  CollectionHelper helper;
  helper.clear();

  // Add documents with different ratings and prices
  helper.index(flatdoc("id_s", "doc1", "price_i", 100, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50, "rating_i", 4), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150, "rating_i", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc4", "price_i", 75, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "price_i", 100, "rating_i", 4), UpdateMessage::COMMIT);

  // Create a search request that sorts by rating desc, then price asc
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");

  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(10);

  // Match all documents
  topDocs.mutable_query()->set_all(true);

  // Sort by rating descending, then price ascending
  auto* sortSpec1 = topDocs.add_sorts();
  sortSpec1->set_field("rating_i");
  sortSpec1->set_dir(proto::SortSpec::DESC);

  auto* sortSpec2 = topDocs.add_sorts();
  sortSpec2->set_field("price_i");
  sortSpec2->set_dir(proto::SortSpec::ASC);

  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");
  topDocs.mutable_fields()->Add("rating_i");

  lreq->engine.submit(*lreq, true);

  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(5, docs.matches());

  // Expected order:
  // rating 5: doc4(75), doc1(100)
  // rating 4: doc2(50), doc5(100)
  // rating 3: doc3(150)

  ASSERT_EQ("doc4", docs.columns().at("id_s").col_s().v(0));
  ASSERT_EQ(5, docs.columns().at("rating_i").col_i().v(0));
  ASSERT_EQ(75, docs.columns().at("price_i").col_i().v(0));

  ASSERT_EQ("doc1", docs.columns().at("id_s").col_s().v(1));
  ASSERT_EQ(5, docs.columns().at("rating_i").col_i().v(1));
  ASSERT_EQ(100, docs.columns().at("price_i").col_i().v(1));

  ASSERT_EQ("doc2", docs.columns().at("id_s").col_s().v(2));
  ASSERT_EQ(4, docs.columns().at("rating_i").col_i().v(2));
  ASSERT_EQ(50, docs.columns().at("price_i").col_i().v(2));

  lreq->done();
}

TEST_F(SortCollectorTest, SortByPriceDescending) {
  CollectionHelper helper;
  helper.clear();

  // Add documents
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150), UpdateMessage::COMMIT);

  // Create a search request that sorts by price descending
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");

  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_limit(10);

  // Match all documents
  topDocs.mutable_query()->set_all(true);

  // Sort by price descending
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::DESC);

  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");

  lreq->engine.submit(*lreq, true);

  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();

  // Verify sort order by price descending: doc3(150), doc1(100), doc2(50)
  ASSERT_EQ("doc3", docs.columns().at("id_s").col_s().v(0));
  ASSERT_EQ(150, docs.columns().at("price_i").col_i().v(0));

  ASSERT_EQ("doc1", docs.columns().at("id_s").col_s().v(1));
  ASSERT_EQ(100, docs.columns().at("price_i").col_i().v(1));

  ASSERT_EQ("doc2", docs.columns().at("id_s").col_s().v(2));
  ASSERT_EQ(50, docs.columns().at("price_i").col_i().v(2));

  lreq->done();
}

TEST_F(SortCollectorTest, SortWithBatchedResponses) {
  CollectionHelper helper;
  helper.clear();

  // Add 10 documents with different prices
  for (int i = 1; i <= 10; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i * 10),
      i == 10 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  // Create a search request with small batch size to trigger multiple responses
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");

  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_limit(10);
  topDocs.set_batch_size(3); // Small batch size to get multiple responses
  topDocs.set_get_number(true);

  // Match all documents
  topDocs.mutable_query()->set_all(true);

  // Sort by price ascending
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);

  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");

  lreq->engine.submit(*lreq, true);

  // Should have multiple responses due to batch size
  ASSERT_GT(lreq->responses.size(), 1) << "Expected multiple batched responses";

  // Verify we got all documents across all responses
  int totalDocs = 0;
  std::vector<int> allPrices;

  for (size_t i = 0; i < lreq->responses.size(); i++) {
    auto& response = *lreq->responses[i];
    auto& docs = response.proto.ops().at("q").docs();

    // Check offset is correct for each batch
    EXPECT_EQ(docs.offset(), totalDocs) << "Incorrect offset for batch " << i;

    // All but last response should have more flag
    if (i < lreq->responses.size() - 1) {
      EXPECT_TRUE(response.proto.more()) << "Expected more flag on response " << i;
      EXPECT_TRUE(docs.more()) << "Expected more flag on docs " << i;
    }
    else {
      EXPECT_FALSE(response.proto.more()) << "Unexpected more flag on last response";
      EXPECT_FALSE(docs.more()) << "Unexpected more flag on last docs";
    }

    // Collect all prices to verify complete sort order
    auto& priceCol = docs.columns().at("price_i").col_i();
    for (int j = 0; j < priceCol.v_size(); j++) {
      allPrices.push_back(priceCol.v(j));
    }

    totalDocs += priceCol.v_size();
  }

  // Verify we got all 10 documents
  ASSERT_EQ(totalDocs, 10);
  ASSERT_EQ(lreq->responses.back()->proto.ops().at("q").docs().matches(), 10);

  // Verify complete sort order: 10, 20, 30, ..., 100
  ASSERT_EQ(allPrices.size(), 10);
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(allPrices[i], (i + 1) * 10) << "Incorrect price at position " << i;
  }

  lreq->done();
}

TEST_F(SortCollectorTest, SortWithMissingValues) {
  CollectionHelper helper;
  helper.clear();
  
  // Add documents with some missing the sort field
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2"), UpdateMessage::NO_COMMIT);  // Missing price
  helper.index(flatdoc("id_s", "doc3", "price_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc4"), UpdateMessage::NO_COMMIT);  // Missing price
  helper.index(flatdoc("id_s", "doc5", "price_i", 75), UpdateMessage::COMMIT);
  
  // Create a search request that sorts by price ascending
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(10);
  
  // Match all documents
  topDocs.mutable_query()->set_all(true);
  
  // Sort by price ascending (missing values should be last by default)
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);
  
  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");
  
  lreq->engine.submit(*lreq, true);
  
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(5, docs.matches());
  
  // Check order: documents with values first (50, 75, 100), then missing values
  auto& idCol = docs.columns().at("id_s").col_s();
  auto& priceCol = docs.columns().at("price_i").col_i();
  
  ASSERT_EQ(5, idCol.v_size());
  ASSERT_EQ(5, priceCol.v_size());
  
  // Documents with values come first in ascending order
  ASSERT_EQ("doc3", idCol.v(0));
  ASSERT_EQ(50, priceCol.v(0));
  
  ASSERT_EQ("doc5", idCol.v(1));
  ASSERT_EQ(75, priceCol.v(1));
  
  ASSERT_EQ("doc1", idCol.v(2));
  ASSERT_EQ(100, priceCol.v(2));
  
  // Documents with missing values come last
  // They should have the missing value substitute (min int64)
  ASSERT_EQ(std::numeric_limits<int64_t>::min(), priceCol.v(3));
  ASSERT_EQ(std::numeric_limits<int64_t>::min(), priceCol.v(4));
  
  lreq->done();
}

TEST_F(SortCollectorTest, EmptyResults) {
  CollectionHelper helper;
  helper.clear();
  
  // Add documents but search for non-existent field value
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50), UpdateMessage::COMMIT);
  
  // Create a search request that matches no documents
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(10);
  
  // Search for a field value that doesn't exist
  topDocs.mutable_query()->mutable_match()->set_field("id_s");
  topDocs.mutable_query()->mutable_match()->mutable_val()->set_s("nonexistent");
  
  // Sort by price
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);
  
  lreq->engine.submit(*lreq, true);
  
  ASSERT_GT(lreq->responses.size(), 0) << "Should receive empty response";
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  
  // Should have 0 matches but still have a valid response
  ASSERT_EQ(0, docs.matches());
  ASSERT_EQ(0, docs.columns_size()) << "Should have no columns for empty results";
  
  lreq->done();
}

TEST_F(SortCollectorTest, SingleDocument) {
  CollectionHelper helper;
  helper.clear();
  
  // Add only one document
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::COMMIT);
  
  // Create a search request
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(10);
  
  // Match all documents
  topDocs.mutable_query()->set_all(true);
  
  // Sort by price
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);
  
  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");
  
  lreq->engine.submit(*lreq, true);
  
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  
  ASSERT_EQ(1, docs.matches());
  ASSERT_EQ("doc1", docs.columns().at("id_s").col_s().v(0));
  ASSERT_EQ(100, docs.columns().at("price_i").col_i().v(0));
  
  lreq->done();
}

TEST_F(SortCollectorTest, ResultsExceedingTopCount) {
  CollectionHelper helper;
  helper.clear();
  
  // Add 20 documents
  for (int i = 1; i <= 20; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i * 10),
      i == 20 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Create a search request with limit of 5
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(5);  // Only get top 5
  
  // Match all documents
  topDocs.mutable_query()->set_all(true);
  
  // Sort by price ascending
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);
  
  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");
  
  lreq->engine.submit(*lreq, true);
  
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  
  // Should report total of 20 matches but only return 5
  ASSERT_EQ(20, docs.matches());
  
  auto& priceCol = docs.columns().at("price_i").col_i();
  ASSERT_EQ(5, priceCol.v_size()) << "Should only return top 5 documents";
  
  // Verify we got the 5 lowest prices: 10, 20, 30, 40, 50
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ((i + 1) * 10, priceCol.v(i)) << "Wrong price at position " << i;
  }
  
  lreq->done();
}

TEST_F(SortCollectorTest, LimitOne) {
  CollectionHelper helper;
  helper.clear();
  
  // Add multiple documents
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150), UpdateMessage::COMMIT);
  
  // Create a search request with limit=1
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(1);  // Only get the top 1
  
  // Match all documents
  topDocs.mutable_query()->set_all(true);
  
  // Sort by price ascending
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("price_i");
  sortSpec->set_dir(proto::SortSpec::ASC);
  
  // Request fields to return
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("price_i");
  
  lreq->engine.submit(*lreq, true);
  
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  
  // Should report 3 matches but only return 1
  ASSERT_EQ(3, docs.matches());
  
  auto& idCol = docs.columns().at("id_s").col_s();
  auto& priceCol = docs.columns().at("price_i").col_i();
  
  ASSERT_EQ(1, idCol.v_size());
  ASSERT_EQ(1, priceCol.v_size());
  
  // Should get the document with lowest price
  ASSERT_EQ("doc2", idCol.v(0));
  ASSERT_EQ(50, priceCol.v(0));
  
  lreq->done();
}

TEST_F(SortCollectorTest, DeterministicParallelSort) {
  CollectionHelper helper;
  helper.clear();
  
  // Create multiple segments to trigger parallel execution
  // First segment
  for (int i = 1; i <= 100; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i % 10),
      i == 100 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Second segment
  for (int i = 101; i <= 200; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i % 10),
      i == 200 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Third segment
  for (int i = 201; i <= 300; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i % 10),
      i == 300 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Run query multiple times to verify deterministic results
  std::vector<int64_t> fingerprints;
  
  for (int run = 0; run < 5; run++) {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.set_limit(50);
    
    // Match all documents
    topDocs.mutable_query()->set_all(true);
    
    // Sort by price ascending  
    auto* sortSpec = topDocs.add_sorts();
    sortSpec->set_field("price_i");
    sortSpec->set_dir(proto::SortSpec::ASC);
    
    // Request fields to return
    topDocs.mutable_fields()->Add("id_s");
    
    // Run in parallel mode
    lreq->engine.submit(*lreq, true);
    
    auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
    ASSERT_EQ(300, docs.matches());
    
    // Calculate fingerprint of results
    int64_t fp = docs.matches();
    const auto& idCol = docs.columns().at("id_s").col_s();
    for (int i = 0; i < idCol.v_size(); i++) {
      int64_t id = 0;
      std::from_chars(idCol.v(i).data() + 3, idCol.v(i).data() + idCol.v(i).size(), id);
      fp = fp * 31 + id;
    }
    
    fingerprints.push_back(fp);
    lreq->done();
  }
  
  // Verify all runs produced the same fingerprint
  for (size_t i = 1; i < fingerprints.size(); i++) {
    ASSERT_EQ(fingerprints[0], fingerprints[i]) 
      << "Run " << i << " produced different results (fingerprint mismatch)";
  }
}

TEST_F(SortCollectorTest, RandomValuesWithTieBreaking) {
  CollectionHelper helper;
  helper.clear();
  
  // Track expected results: tuples of (value, docid, segment)
  struct DocInfo {
    int32_t value;
    int32_t docId;
    int32_t segment;
    int32_t docInSegment;
  };
  std::vector<DocInfo> expectedOrder;
  
  // Create multiple segments with random values
  int docId = 0;
  const int numSegments = 5;
  const int docsPerSegment = 20;
  
  for (int seg = 0; seg < numSegments; seg++) {
    SplitMix64 rng(seg); // Predictable random numbers per segment
    
    // Index documents for this segment
    for (int i = 0; i < docsPerSegment; i++) {
      int32_t value = rng.rint(100); // Random values 0-99
      
      helper.index(flatdoc("id_s", std::to_string(docId), "value_i", value), 
                   UpdateMessage::NO_COMMIT);
      
      expectedOrder.push_back({value, docId, seg, i});
      
      
      docId++;
    }
    
    // Commit to create a segment
    helper.commit();
  }
  
  // Sort expected order: by value descending, then by segment/doc ascending for ties
  std::sort(expectedOrder.begin(), expectedOrder.end(), 
    [](const auto& a, const auto& b) {
      if (a.value != b.value) {
        return a.value > b.value; // Higher values first (DESC)
      }
      // For ties, sort by segment first, then by doc within segment
      if (a.segment != b.segment) {
        return a.segment < b.segment;
      }
      return a.docInSegment < b.docInSegment;
    });
  
  // Search with sorting
  auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
  lreq->proto.mutable_collection()->add_name("main");
  
  auto& ops = *lreq->proto.mutable_ops();
  auto& topDocs = *ops["q"].mutable_top_docs();
  topDocs.set_get_number(true);
  topDocs.set_limit(100); // Get all results
  
  // Match all documents
  topDocs.mutable_query()->set_all(true);
  
  // Sort by value descending
  auto* sortSpec = topDocs.add_sorts();
  sortSpec->set_field("value_i");
  sortSpec->set_dir(proto::SortSpec::DESC);
  
  // Request fields
  topDocs.mutable_fields()->Add("id_s");
  topDocs.mutable_fields()->Add("value_i");
  
  lreq->engine.submit(*lreq, true); // Run multi-threaded
  
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  
  ASSERT_EQ(docs.matches(), docId) << "Should match all documents";
  
  // Verify results are in expected order
  const auto& idCol = docs.columns().at("id_s").col_s();
  const auto& valueCol = docs.columns().at("value_i").col_i();
  
  for (int i = 0; i < std::min(idCol.v_size(), (int)expectedOrder.size()); i++) {
    int64_t actualId = 0;
    std::from_chars(idCol.v(i).data(), idCol.v(i).data() + idCol.v(i).size(), actualId);
    int64_t actualValue = valueCol.v(i);
    
    ASSERT_EQ(actualId, expectedOrder[i].docId) 
      << "Position " << i << ": Expected id=" << expectedOrder[i].docId 
      << " but got id=" << actualId << " (value=" << actualValue << ")";
    ASSERT_EQ(actualValue, expectedOrder[i].value)
      << "Position " << i << ": Expected value=" << expectedOrder[i].value 
      << " but got value=" << actualValue;
  }
  
  lreq->done();
}
