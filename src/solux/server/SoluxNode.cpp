#include "SoluxNode.h"
#include "solux/schema/Schema.h"
#include "solux/store/InputStream.h"
#include "solux/reader/Postings.h"
#include "protos/solux_types.pb.h"

namespace solux {

static constexpr std::string_view SCHEMA_PREFIX = "_schema_";

static std::string schemaFileName(uint64_t gen) {
  return std::string(SCHEMA_PREFIX) + Postings::getSortableString(gen);
}

void Collection::setSchema(std::shared_ptr<Schema> newSchema) {
  uint64_t gen = schemaGen_++;
  newSchema->gen_ = gen;

  // Persist the schema source def to the Directory
  if (shard && shard->dir && !newSchema->sourceDef_.empty()) {
    std::string fileName = schemaFileName(gen);
    auto file = shard->dir->createFile(fileName);
    OutputStream out;
    out.setFile(&*file);
    out.write(newSchema->sourceDef_.data(), newSchema->sourceDef_.size());
    out.close();
    shard->dir->finishFile(*file);

    // Delete older schema files
    std::vector<std::string> files;
    shard->dir->listFiles(files);
    for (const auto& f : files) {
      if (f.starts_with(SCHEMA_PREFIX) && f != fileName) {
        shard->dir->deleteFile(f);
      }
    }
  }

  schema.store(std::move(newSchema));
}


bool Collection::loadSchema() {
  if (!shard || !shard->dir) return false;

  // Find the latest schema file by lexicographic order (sortable naming).
  std::string lastSchemaFile;
  std::vector<std::string> files;
  shard->dir->listFiles(files);
  for (const auto& f : files) {
    if (f.starts_with(SCHEMA_PREFIX)) {
      if (f > lastSchemaFile) lastSchemaFile = f;
    }
  }

  while (!lastSchemaFile.empty()) {
    auto file = shard->dir->openFile(lastSchemaFile);
    if (file) {
      InputStream is = file->getInputStream();
      proto::SchemaDef def;
      if (!def.ParseFromArray(is.ptr(), (int)is.left())) {
        throw std::runtime_error("Failed to parse schema file: " + lastSchemaFile);
      }

      // Parse the gen back from the sortable filename suffix
      auto genStr = std::string_view(lastSchemaFile).substr(SCHEMA_PREFIX.size());
      uint64_t gen = Postings::parseSortableString(genStr);

      auto newSchema = Schema::fromProto(def);
      newSchema->gen_ = gen;
      schemaGen_ = gen + 1;  // next setSchema will use gen+1
      schema.store(std::move(newSchema));
      return true;
    }

    // File was deleted by a concurrent setSchema — rescan for the latest.
    lastSchemaFile.clear();
    files.clear();
    shard->dir->listFiles(files);
    for (const auto& f : files) {
      if (f.starts_with(SCHEMA_PREFIX)) {
        if (f > lastSchemaFile) lastSchemaFile = f;
      }
    }
  }

  return false;
}


SoluxNode::SoluxNode(SoluxConfig config) : config(std::move(config)) {
  createSingletons();
  searchEngine = std::make_unique<SearchEngine>(*this);
}

SoluxNode::~SoluxNode() {
}

void SoluxNode::createSingletons() {
  dirFactory = std::make_unique<RAMDirFactory>();

  collection = std::make_shared<Collection>();
  collection->shard = std::make_shared<Shard>(*collection);
  collection->shard->dir = dirFactory->create();

  // Set default schema initially (without persisting, gen=0 means not yet persisted)
  auto defaultSchema = Schema::createDefaultSchema();
  defaultSchema->gen_ = 0;
  collection->schema.store(std::move(defaultSchema));

  // Pass a schemaProvider that fetches the schema from the Collection
  auto* col = collection.get();
  collection->shard->iw = std::make_shared<IndexWriter>(*collection->shard->dir,
    [col]() { return col->getSchema(); });

  // Load the latest persisted schema if one exists.
  collection->loadSchema();
}

} // namespace solux
