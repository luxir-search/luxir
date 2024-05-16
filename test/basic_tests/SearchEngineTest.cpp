
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "test/SoluxTest.h"
#include "test/TestUtils.h"
#include "solux/server/GRPCServer.h"

using namespace solux;
using namespace solux::test;

class SearchEngineTest : public SoluxTest {
public:
  SearchEngineTest() {
  }

  Collection& getCollection() {
    return *soluxNode->getCollection("main");
  }

  IndexWriter& getIndexWriter() {
    return *getCollection().getShard()->getIndexWriter();
  }

  std::shared_ptr<IndexReader> getIndexReader() {
    return getIndexWriter().getIndexReader();
  }
};


class LocalReq : public SearchEngine::Request {
public:
  std::vector<SearchEngine::Response*> responses;

  /// Heap allocate an Arena (if null) and use it to create a LocalReq object and proto::SearchRequest
  static LocalReq* create(SearchEngine& engine, google::protobuf::Arena* arena = nullptr) {
    arena = arena ? arena : createArena();
    // NOTE: using Create() instead of CreateMessage() for the protobuf message resulted in failure when deallocating!
    auto* SearchRequestProto = google::protobuf::Arena::CreateMessage<solux::proto::SearchRequest>(arena);
    auto* localReq = google::protobuf::Arena::Create<LocalReq>(arena, engine, *SearchRequestProto);
    return localReq;
  }

  LocalReq(SearchEngine& engine, solux::proto::SearchRequest& proto) : Request(engine, proto) {
  }

  virtual ~LocalReq() {
    for (auto* response : responses) {
      if (&response->arena != &arena) {
        LOG_DEBUG("releasing response arena!");
        releaseArena(&response->arena);
      }
    }
  }

  int reply(SearchEngine::Response& response) override {
    responses.push_back(&response);
    /*
    std::string reqStr;
    google::protobuf::TextFormat::PrintToString(response.proto, &reqStr);
    LOG_DEBUG("\tresponse:{}", reqStr);
     */
    return 0;
  }

  // will never be called
  void replyCallback(SearchEngine::Response& response) override {
    unused(response);
  }

  // should be called by user
  void done() override {
    releaseArena(&arena);
  }

  std::string toString() {
    std::string ret;
    ret += "Request:" + proto.DebugString() + "\n";
    for (auto* response : responses) {
      ret += "\tResponse:" + response->proto.DebugString() + "\n";
    }
    return ret;
  }
};



TEST_F(SearchEngineTest, basic) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("foo_w","how now brown cow", "foo_i", 17, "color_s","red"),UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w","charlie brown", "foo_i", 23, "color_s","blue"),UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w","brown", "foo_i", 5, "color_s","brown"),UpdateMessage::COMMIT);
  // should be 2 segments now.

  {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("myrequestid");

    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.set_get_scores(true);
    auto& query = *topDocs.mutable_query()->mutable_match();
    query.set_field("foo_w");
    query.mutable_val()->set_s("brown");
    topDocs.mutable_fields()->Add("foo_i");
    topDocs.mutable_fields()->Add("color_s");
    // topDocs.mutable_fields()->Add("noexist_i");
    // topDocs.mutable_fields()->Add("noexist_s");

    /* phrase query not yet parsed
    auto& terms = *ops["q"].mutable_top_docs()->mutable_query()->mutable_phrase()->mutable_terms();
    terms.Add("foo");
    terms.Add("bar");
     */


    lreq->engine.submit(*lreq, false);
    // LOG_DEBUG("ENGINE REQ: {}", lreq->toString());

    ASSERT_EQ(lreq->proto.request_id(), lreq->responses[0]->proto.request_id());
    auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
    ASSERT_EQ(3, docs.matches());
    ASSERT_EQ(3, docs.columns_size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(5, docs.columns().at("foo_i").col_i().v(0));
    ASSERT_EQ("brown", docs.columns().at("color_s").col_s().v(0));
    ASSERT_EQ(23, docs.columns().at("foo_i").col_i().v(1));
    ASSERT_EQ("blue", docs.columns().at("color_s").col_s().v(1));
    ASSERT_EQ(17, docs.columns().at("foo_i").col_i().v(2));
    ASSERT_EQ("red", docs.columns().at("color_s").col_s().v(2));

    lreq->done();
  }

  // now lets do the same request, but try to get multiple responses.
  {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("myrequestid");
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.set_get_scores(true);
    auto& query = *topDocs.mutable_query()->mutable_match();
    query.set_field("foo_w");
    query.mutable_val()->set_s("brown");
    topDocs.mutable_fields()->Add("foo_i");
    topDocs.mutable_fields()->Add("color_s");
    topDocs.set_batch_size(2);

    lreq->engine.submit(*lreq, false);
// LOG_DEBUG("ENGINE REQ: {}", lreq->toString());

    ASSERT_EQ(lreq->proto.request_id(), lreq->responses[0]->proto.request_id());
    auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
    ASSERT_EQ(3, docs.matches());
    ASSERT_EQ(3, docs.columns_size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(2, docs.columns().at("foo_i").col_i().v_size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(2, docs.columns().at("color_s").col_s().v_size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(5, docs.columns().at("foo_i").col_i().v(0));
    ASSERT_EQ("brown", docs.columns().at("color_s").col_s().v(0));
    ASSERT_EQ(23, docs.columns().at("foo_i").col_i().v(1));
    ASSERT_EQ("blue", docs.columns().at("color_s").col_s().v(1));
    // check that "more" flags are set both at DocList level and at Response level
    ASSERT_TRUE(docs.more());
    ASSERT_TRUE(lreq->responses[0]->proto.more());


    ASSERT_EQ(lreq->proto.request_id(), lreq->responses[1]->proto.request_id());
    auto& docs2 = lreq->responses[1]->proto.ops().at("q").docs();
    ASSERT_EQ(3, docs2.matches());
    ASSERT_EQ(2, docs2.offset());
    ASSERT_EQ(3, docs2.columns_size());
    ASSERT_EQ(1, docs2.columns().at("foo_i").col_i().v_size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(1, docs2.columns().at("color_s").col_s().v_size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(17, docs2.columns().at("foo_i").col_i().v(0));
    ASSERT_EQ("red", docs2.columns().at("color_s").col_s().v(0));
    // check that more flags are false at DocList level and at Response level
    ASSERT_FALSE(docs2.more());
    ASSERT_FALSE(lreq->responses[1]->proto.more());

    lreq->done();



  }

}

