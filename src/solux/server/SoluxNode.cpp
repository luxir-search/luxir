#include "SoluxNode.h"
#include "solux/schema/Schema.h"
#include "solux/store/InputStream.h"
#include "solux/store/CheckedDirFactory.h"
#include "solux/reader/Postings.h"
#include "solux/api/padded_input.h"
#include "solux/api/solux_types.hpp"
#include "solux/util/DateTime.h"

#include <cctype>
#include <memory_resource>
#include <span>

namespace solux {

static constexpr std::string_view SCHEMA_PREFIX = "_schema_";

static std::string schemaFileName(uint64_t gen) {
  return std::string(SCHEMA_PREFIX) + Postings::getSortableString(gen);
}

std::string SoluxNode::normalizedCollectionName(std::string_view name) {
  return std::string(name);
}

void SoluxNode::validateCollectionName(std::string_view name) {
  constexpr std::size_t kMaxCollectionNameBytes = 255;
  if (name.empty()) {
    throw CollectionResolutionError("collection name is empty");
  }
  if (name.size() > kMaxCollectionNameBytes) {
    throw CollectionResolutionError("collection '" + std::string(name) + "' exceeds maximum length");
  }
  if (name[0] == '_') {
    throw CollectionResolutionError("collection '" + std::string(name) + "' is reserved");
  }
  if (name == "." || name == "..") {
    throw CollectionResolutionError("collection '" + std::string(name) + "' is reserved");
  }
  if (std::isspace((unsigned char)name.front()) || std::isspace((unsigned char)name.back())) {
    throw CollectionResolutionError("collection '" + std::string(name) + "' has leading or trailing whitespace");
  }
  for (char c : name) {
    unsigned char ch = (unsigned char)c;
    if (ch < 0x20 || ch == 0x7f) {
      throw CollectionResolutionError("collection '" + std::string(name) + "' contains a control character");
    }
    if (c == '/' || c == '\\') {
      throw CollectionResolutionError("collection '" + std::string(name) + "' must be a single path component");
    }
  }
}

std::shared_ptr<Schema> Collection::updateSchema(const solux::api::SchemaDef& def,
                                                 solux::api::SchemaRequest_::Mode mode) {
  // One transaction per collection: read-current -> apply -> resolve ->
  // persist -> swap.  Without the lock, two concurrent SETs could each build
  // from the same base and the second swap would silently drop the first's
  // fields (and their persistence passes would delete each other's files).
  std::lock_guard<std::mutex> lock(schemaMutex_);
  std::shared_ptr<Schema> newSchema;
  if (mode == solux::api::SchemaRequest_::Mode::SET) {
    auto current = getSchema();
    newSchema = Schema::fromProto(def, current.get());
  } else {
    newSchema = Schema::fromProto(def);
  }
  setSchemaLocked(newSchema);
  return newSchema;
}

void Collection::setSchema(std::shared_ptr<Schema> newSchema) {
  std::lock_guard<std::mutex> lock(schemaMutex_);
  setSchemaLocked(std::move(newSchema));
}

void Collection::setSchemaLocked(std::shared_ptr<Schema> newSchema) {
  uint64_t gen = schemaGen_++;
  newSchema->gen_ = gen;

  // Persist the schema source def to the Directory.  The durable file IS the
  // publication point: once it is synced, restart would load this generation,
  // so the in-memory swap below must happen (and the caller must see success)
  // regardless of anything after the sync.  A failure BEFORE the sync removes
  // the staged file so a partial write can never be selected at startup.
  if (shard && shard->dir && !newSchema->sourceDef_.empty()) {
    std::string fileName = schemaFileName(gen);
    try {
      auto file = shard->dir->createFile(fileName);
      OutputStream out;
      out.setFile(&*file);
      out.write(newSchema->sourceDef_.data(), newSchema->sourceDef_.size());
      out.close();
      shard->dir->finishFile(*file);

      std::vector<std::string> syncFiles = {fileName, "."};
      shard->dir->sync(syncFiles);
    } catch (...) {
      try {
        shard->dir->deleteFile(fileName);
      } catch (...) {
        LOG_WARN("failed to remove staged schema file {} after write failure", fileName);
      }
      throw;
    }

    schema.store(std::move(newSchema));

    // Best-effort cleanup of older generations: the new schema is already
    // durable and published, so a cleanup failure must not fail the request
    // (loadSchema always picks the highest generation anyway).
    try {
      std::vector<std::string> files;
      shard->dir->listFiles(files);
      for (const auto& f : files) {
        if (f.starts_with(SCHEMA_PREFIX) && f != fileName) {
          shard->dir->deleteFile(f);
        }
      }
    } catch (const std::exception& e) {
      LOG_WARN("failed to clean up old schema files: {}", e.what());
    }
    return;
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


SoluxNode::SoluxNode(SoluxConfig config)
  : config(std::move(config)),
    indexRamBudget(this->config.index.max_index_ram_mb * 1024 * 1024),
    searchPool((size_t)this->config.server.resolveSearchThreads()) {
  preWarmTimeZoneDatabase();
  createSingletons();
  searchEngine = std::make_unique<SearchEngine>(*this);
}

SoluxNode::~SoluxNode() {
}

std::shared_ptr<Collection> SoluxNode::getCollection(std::string_view name) {
  return getCollection(root.get(), name);
}

std::shared_ptr<Collection> SoluxNode::checkLoaded(std::shared_ptr<Collection> collection) {
  if (collection && !collection->loadError.empty()) {
    throw CollectionResolutionError(
        "collection '" + collection->name + "' failed to load: " + collection->loadError);
  }
  return collection;
}

std::shared_ptr<Collection> SoluxNode::getCollection(Library* library, std::string_view name) {
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError("root library is not initialized");
  }

  std::string collectionName = normalizedCollectionName(name);
  validateCollectionName(collectionName);

  if (auto collection = targetLibrary->collections.get(collectionName)) {
    return checkLoaded(std::move(collection));
  }

  throw CollectionResolutionError("collection '" + collectionName + "' does not exist");
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
    collection = getCollection(kDefaultCollectionName);
  }
  return collection;
}

std::shared_ptr<Collection> SoluxNode::getOrCreateCollection(std::string_view name) {
  return getOrCreateCollection(root.get(), name);
}

std::shared_ptr<Collection> SoluxNode::getOrCreateCollection(Library* library, std::string_view name) {
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError("root library is not initialized");
  }

  std::string collectionName = normalizedCollectionName(name);
  validateCollectionName(collectionName);

  auto collection = targetLibrary->collections.getOrCreate(collectionName, [&]() -> std::shared_ptr<Collection> {
    if (!config.ingest.auto_create_collection) {
      return nullptr;
    }
    auto created = initCollection(collectionName);
    LOG_INFO("Created collection: {}", collectionName);
    return created;
  });
  if (!collection) {
    throw CollectionResolutionError("collection '" + collectionName + "' does not exist");
  }
  return checkLoaded(std::move(collection));
}

std::shared_ptr<Collection> SoluxNode::resolveOrCreateCollection(const solux::api::Target* target) {
  std::shared_ptr<Library> library = getLibrary(nullptr, "");
  std::shared_ptr<Collection> collection;
  if (target != nullptr) {
    for (int i = 0; i < (int)target->name.size(); i++) {
      if (i == (int)target->name.size() - 1) {
        collection = getOrCreateCollection(library.get(), target->name[i]);
      } else {
        library = getLibrary(library.get(), target->name[i]);
      }
    }
  }
  if (!collection) {
    collection = getOrCreateCollection(kDefaultCollectionName);
  }
  return collection;
}

std::shared_ptr<Collection> SoluxNode::createCollection(Library* library, std::string_view name) {
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError("root library is not initialized");
  }

  std::string collectionName = normalizedCollectionName(name);
  validateCollectionName(collectionName);

  auto collection = targetLibrary->collections.getOrCreate(collectionName, [&]() {
    auto created = initCollection(collectionName);
    LOG_INFO("Created collection: {}", collectionName);
    return created;
  });
  return checkLoaded(std::move(collection));
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
    [colPtr]() { return colPtr->getSchema(); }, &indexRamBudget);
  col->shard->iw->perInverterRamBytes = (size_t)config.index.max_inverter_ram_mb * 1024 * 1024;
  col->shard->iw->perInverterMaxDocs = (size_t)config.index.max_inverter_docs;

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
    existing.emplace_back(kDefaultCollectionName);
  }

  for (const auto& name : existing) {
    try {
      validateCollectionName(name);
      root->collections.getOrCreate(name, [&]() {
        return initCollection(name);
      });
      LOG_INFO("Loaded collection: {}", name);
    } catch (const std::exception& e) {
      // Keep the node up: register a tombstone so the name resolves to a clear
      // error instead of "does not exist", and can't be silently re-created
      // over the on-disk data.
      LOG_ERROR("Failed to load collection '{}': {}", name, e.what());
      auto tombstone = std::make_shared<Collection>();
      tombstone->name = name;
      tombstone->loadError = e.what();
      root->collections.getOrCreate(name, [&]() { return tombstone; });
    }
  }
}

} // namespace solux
