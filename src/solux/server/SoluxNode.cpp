#include "SoluxNode.h"
#include "solux/schema/Schema.h"

namespace solux {

SoluxNode::SoluxNode() {
  createSingletons();
  searchEngine = std::make_unique<SearchEngine>(*this);
}

SoluxNode::~SoluxNode() {
}

void SoluxNode::createSingletons() {
  collection = std::make_shared<Collection>();
  collection->setSchema(Schema::createDefaultSchema());
  collection->shard = std::make_shared<Shard>(*collection);
  collection->shard->dir = std::make_shared<RAMDir>();
  // Pass a schemaProvider that fetches the schema from the Collection
  auto* col = collection.get();
  collection->shard->iw = std::make_shared<IndexWriter>(*collection->shard->dir,
    [col]() { return col->getSchema(); });
}

} // namespace solux