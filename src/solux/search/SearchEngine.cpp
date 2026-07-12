#include "SearchEngine.h"
#include "ProtobufSearchParser.h"

namespace solux {

void SearchEngine::submitBody(SearchRequest& req) {
  try {
    getResources(req);
    // LOG_DEBUG("submitBody: IndexReader commitTime={}", req.reader->commitTime());
    req.lastResponse = SearchResponse::create(req, true);

    ProtobufSearchParser parser(req);
    auto* root = parser.parse();
    root->init();
    std::unique_ptr<SearchOp::Calculator> calc(root->createCalculator(nullptr, -1));
    calc->calc(req.tg, -1, nullptr);

    if (req.tg) {
      LOG_TRACE("SearchRequest: waiting for task group to finish for req={}", (void*)&req);
      req.tg->wait();
      LOG_TRACE("SearchRequest: DONE! will call reply next. req={}", (void*)&req);
    }
  } catch (std::exception& e) {
    // TODO: distinguish between request errors and server errors.

    // Drain any in-flight tasks so they aren't writing into the response while
    // we tear down.  wait() rethrows the first exception, so swallow it here.
    if (req.tg) {
      // TODO: notify other tasks about the error?
      try { req.tg->wait(); } catch (...) {}
    }
    LOG_WARN("Search request failed: {}", e.what());
    if (req.lastResponse == nullptr) {
      req.lastResponse = SearchResponse::create(req, true);
    }
    // proto.error is a non-owning string_view; e.what() points into the exception object,
    // which is destroyed when this catch block exits. Copy it into the response arena.
    req.lastResponse->proto.error = solux::api::build::arenaStr(req.lastResponse->mr, e.what());
  }

  // Declared degradations ride on the final response (streaming responses
  // with more=true do not carry them).
  if (!req.warnings.empty()) {
    auto* warnings = solux::api::build::allocArray(req.lastResponse->proto.warnings,
                                                   req.warnings.size(), req.lastResponse->mr);
    for (size_t i = 0; i < req.warnings.size(); i++) {
      warnings[i] = req.warnings[i];
    }
  }

  // Send back the final response.  Do not access req after this point as it
  // maybe asynchronously deleted.
  req.reply(*req.lastResponse);
}

void SearchEngine::submit(SearchRequest& req, bool parallel) {
  // Ideas: we could keep track of executing requests here, and provide ways to list / cancel them?
  std::optional<oneapi::tbb::task_group> stackTg;
  if (parallel && req.tg == nullptr) {
    req.tg = &stackTg.emplace();
  }
  submitBody(req);
}

void SearchEngine::getResources(SearchRequest& req) {
  // look up the correct index reader and the associated schema
  auto& request = req.proto;
  auto& node = req.engine.node;
  auto collection = node.resolveCollection(request.collection ? &*request.collection : nullptr);

  // get the index reader
  req.schema = collection->getSchema();
  req.reader = collection->getShard()->getIndexWriter()->getIndexReader(request.freshness_us);
}


} // namespace solux
