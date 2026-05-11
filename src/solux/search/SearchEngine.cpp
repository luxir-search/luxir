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
    LOG_DEBUG("Search request failed: {}", e.what());
    if (req.lastResponse == nullptr) {
      req.lastResponse = SearchResponse::create(req, true);
    }
    req.lastResponse->proto.set_error(e.what());
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
  std::shared_ptr<Collection> collection;
  auto& request = req.proto;
  auto& node = req.engine.node;

  if (request.collection().name_size() == 0) {
    // TODO: do we support default collections (implicitly defined by something like an api-key?)
  }

  std::shared_ptr<Library> library = node.getLibrary(nullptr, "");
  for (int i = 0; i < request.collection().name_size(); i++) {
    // TODO: walk from our implicit root to find the correct collection.
    if (i == request.collection().name_size() - 1) {
      // LOG_DEBUG("Looking up collection name '{}'", request.collection().name(i));

      // last element in path, so get collection.
      collection = node.getCollection(library.get(), request.collection().name(i));
      // TODO: handle lookup failure
    } else {
      // not last element... get sub-library
      library = node.getLibrary(library.get(), request.collection().name(i));
      // TODO: handle lookup failure
    }
  }

  // get the index reader
  req.schema = collection->getSchema();
  req.reader = collection->getShard()->getIndexWriter()->getIndexReader(request.freshness_us());
}


} // namespace solux