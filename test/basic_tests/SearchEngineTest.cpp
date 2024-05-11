
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
  helper.index(flatdoc("foo_w","how now brown cow", "foo_i", 17), UpdateMessage::COMMIT);

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

  /* phrase query not yet parsed
  auto& terms = *ops["q"].mutable_top_docs()->mutable_query()->mutable_phrase()->mutable_terms();
  terms.Add("foo");
  terms.Add("bar");
   */


  lreq->engine.submit(*lreq);
  // LOG_DEBUG("ENGINE REQ: {}", lreq->toString());

  ASSERT_EQ(lreq->proto.request_id(),  lreq->responses[0]->proto.request_id());
  auto& docs = lreq->responses[0]->proto.ops().at("q").docs();
  ASSERT_EQ(1, docs.matches());
  ASSERT_EQ(2, docs.columns_size());
  ASSERT_EQ(17, docs.columns().at("foo_i").col_i().v(0));

  lreq->done();
}

