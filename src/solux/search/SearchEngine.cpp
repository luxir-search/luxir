#include "SearchEngine.h"
#include "ProtobufSearchParser.h"

namespace solux {

void SearchEngine::submitBody(SearchRequest& req) {
  getResources(req);
  // LOG_DEBUG("submitBody: IndexReader commitTime={}", req.reader->commitTime());
  req.lastResponse = SearchResponse::create(req, true);

  ProtobufSearchParser parser(req);
  auto* root = parser.parse();
  std::unique_ptr<SearchOp::Calculator> calc(root->createCalculator(nullptr, -1));
  calc->calc(req.tg, -1, nullptr);

  if (req.tg) {
    req.tg->wait();
  }

  // TODO: error handling here?

  // Send back the final response.  Do not access req after this point as it
  // maybe asynchronously deleted.
  req.reply(*req.lastResponse);
}

void SearchEngine::submit(SearchRequest& req, bool parallel) {
  // Ideas: we could keep track of executing requests here, and provide ways to list / cancel them?
  try {
    std::optional<oneapi::tbb::task_group> stackTg;
    if (parallel && req.tg == nullptr) {
      req.tg = &stackTg.emplace();
    }
    submitBody(req);
  } catch (std::exception& e) {
    LOG_ERROR("Unexpected exception: {}", e.what());
    LOG_ERROR("Stack trace:\n{}", solux::getStackTrace());
  }
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