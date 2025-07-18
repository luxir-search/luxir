#include <gtest/gtest.h>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/SortField.h"

using namespace solux;
using namespace solux::test;

class SortCollectorTest : public SoluxTest {
};

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

TEST_F(SortCollectorTest, SortByMultipleFields) {
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
    topDocs.set_batch_size(3);  // Small batch size to get multiple responses
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
        } else {
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