#pragma once

#include "TestUtils.h"

namespace solux::test {


class LocalReq : public SearchEngine::Request {
public:
  std::vector<SearchEngine::Response*> responses;

  /// Heap allocate an Arena (if null) and use it to create a LocalReq object and proto::SearchRequest
  static LocalReq* create(SearchEngine& engine, google::protobuf::Arena* arena = nullptr) {
    arena = arena ? arena : createArena();
    auto* SearchRequestProto = google::protobuf::Arena::Create<solux::proto::SearchRequest>(arena);
    auto* localReq = google::protobuf::Arena::Create<LocalReq>(arena, engine, *SearchRequestProto);
    return localReq;
  }

  LocalReq(SearchEngine& engine, solux::proto::SearchRequest& proto) : Request(engine, proto) {
  }

  virtual ~LocalReq() {
    for (auto* response : responses) {
      if (&response->arena != &arena) {
        LOG_TRACE("releasing response arena!");
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


} // solux::test