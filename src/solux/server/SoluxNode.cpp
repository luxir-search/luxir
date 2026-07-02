#include "SoluxNode.h"
#include "solux/schema/Schema.h"
#include "solux/store/InputStream.h"
#include "solux/store/CheckedDirFactory.h"
#include "solux/reader/Postings.h"
#include "solux/api/padded_input.h"
#include "solux/api/solux_types.hpp"

#include <memory_resource>
#include <span>

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

    std::vector<std::string> syncFiles = {fileName, "."};
    shard->dir->sync(syncFiles);

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
    auto file = shard->dir->openFile(lastSchemaFile, true);
    if (file) {
      InputStream is = file->getInputStream();
      std::pmr::monotonic_buffer_resource schemaArena;  // backs the non-owning SchemaDef
      solux::api::SchemaDef def;
      std::span<const char> bytes((const char*)is.ptr(), is.left());
      auto padded = solux::api::copyToPaddedInput(std::as_bytes(bytes), schemaArena);
      if (!solux::api::decode(def, padded, schemaArena)) {
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

    // File was deleted by a concurrent setSchema - rescan for the latest.
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

std::shared_ptr<Collection> SoluxNode::resolveCollection(const solux::api::Target* target) {
  std::shared_ptr<Library> library = getLibrary(nullptr, "");
  std::shared_ptr<Collection> collection;
  if (target != nullptr) {
    for (int i = 0; i < (int)target->name.size(); i++) {
      if (i == (int)target->name.size() - 1) {
        collection = getCollection(library.get(), target->name[i]);
      } else {
        library = getLibrary(library.get(), target->name[i]);
      }
    }
  }
  if (!collection) {
    collection = getCollection("");
  }
  return collection;
}

std::shared_ptr<Collection> SoluxNode::initCollection(const std::string& name) {
  auto col = std::make_shared<Collection>();
  col->name = name;
  col->shard = std::make_shared<Shard>(*col);
  col->shard->dir = dirFactory->create(name);

  // Set default schema initially (without persisting, gen=0 means not yet persisted)
  auto defaultSchema = Schema::createDefaultSchema();
  defaultSchema->gen_ = 0;
  col->schema.store(std::move(defaultSchema));

  // Pass a schemaProvider that fetches the schema from the Collection
  auto* colPtr = col.get();
  col->shard->iw = std::make_shared<IndexWriter>(*col->shard->dir,
    [colPtr]() { return colPtr->getSchema(); });

  // Load the latest persisted schema if one exists.
  col->loadSchema();
  return col;
}

void SoluxNode::createSingletons() {
  if (config.store.backend == "fs") {
    dirFactory = std::make_unique<FSDirFactory>(config.store.data_dir);
  } else {
    dirFactory = std::make_unique<RAMDirFactory>();
  }

  if (config.store.checked_dir.sync != "off") {
    auto mode = config.store.checked_dir.sync == "throw" ? CheckedDirMode::THROW : CheckedDirMode::WARN;
    dirFactory = std::make_unique<CheckedDirFactory>(std::move(dirFactory), mode);
  }

  root = std::make_shared<Library>();

  // Discover existing collections from the store, or create default "main".
  auto existing = dirFactory->listCollections();
  if (existing.empty()) {
    existing.push_back("main");
  }

  for (const auto& name : existing) {
    auto col = initCollection(name);
    LOG_INFO("Loaded collection: {}", name);
    root->collections.emplace(name, col);
  }

  // Keep backward-compat: set the singleton 'collection' to "main"
  collection = root->collections.at("main");
}

} // namespace solux
