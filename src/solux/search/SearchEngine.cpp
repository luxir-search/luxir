#include "SearchEngine.h"

namespace solux {



void SearchEngine::getResources(SearchEngine::Request& req) {
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