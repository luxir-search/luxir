#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "solux/store/Directory.h"
#include "solux/store/DirectoryFactory.h"
#include "solux/index/IndexRamBudget.h"
#include "solux/index/IndexWriter.h"
#include "oneapi/tbb/task_arena.h"
#include "solux/search/SearchEngine.h"
#include "solux/SoluxConfig.h"
#include "solux/util/SharedLazyMap.h"

namespace solux {

namespace api { struct Target; }

/// There should normally be a single SoluxNode instance per process.
/// A single SoluxNode can host many indexes.
/// There still *may* be multiple SoluxNode instances per process, but it's currently more for testing.

class SearchEngine;
class Library;
class Collection;
class SoluxNode;

class CollectionResolutionError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class Shard {
  Collection& collection; // hard reference to the collection that owns this shard
  std::shared_ptr<Directory> dir;  // does this need to be shared_ptr?  Perhaps not if we have a shared ptr to a parent object (Shard or Collection?)
  std::shared_ptr<IndexWriter> iw;

public:
  explicit Shard(Collection& collection) : collection(collection) {
    unused(this->collection);
    // dir = std::make_shared<RAMDir>();
    // iw = std::make_shared<IndexWriter>(*dir);
  }
  // TODO: we don't want to be in the position of having more than ine IW pointing at the index/dir... this suggests that instead of
  // having the ability for it to come and go, it should be a singleton (which could still be created on demand) that
  // can dump most of it's state for low memory usage?  Then this method would not return a shared_ptr, but a simple reference.
  std::shared_ptr<IndexWriter> getIndexWriter() {
    return iw;
  }

  std::shared_ptr<Directory> getDirectory() {
    return dir;
  }

  // TODO: if a shard isn't currently "loaded", should it be removed from the map, or just it's size cut down?

  friend class Collection;
  friend class Library;
  friend class SoluxNode;
};

class Schema;

namespace api { struct SchemaDef; }
namespace api::SchemaRequest_ { enum class Mode; }

// A single logical collection of docs which may
// consist of multiple shards.
class Collection {
  std::string name;
  std::string loadError;  // non-empty means the collection failed to load at startup; resolution rejects it
  std::atomic<std::shared_ptr<Schema>> schema;  // atomic for lock-free reader access
  std::shared_ptr<Shard> shard;
  std::vector<std::shared_ptr<Shard>> shards;
  std::atomic<uint64_t> schemaGen_{1};  // starts at 1 for default schema
  std::mutex schemaMutex_;  // serializes schema read-modify-write + persistence
public:

  std::shared_ptr<Shard> getShard() {
    return shard;
  }

  // Returns current schema (lock-free read)
  std::shared_ptr<Schema> getSchema() {
    return schema.load();
  }

  // The schema mutation transaction: applies `def` to the current schema
  // (SET) or replaces it (REPLACE_ALL), persists, swaps, and returns the
  // installed schema.  All external schema mutation (gRPC, HTTP) goes through
  // here; concurrent calls serialize per collection.
  std::shared_ptr<Schema> updateSchema(const solux::api::SchemaDef& def,
                                       solux::api::SchemaRequest_::Mode mode);

  // Atomically replaces the schema and persists it to the shard's Directory.
  void setSchema(std::shared_ptr<Schema> newSchema);

  uint64_t schemaGen() const { return schemaGen_.load(); }

  // Load the latest schema from the Directory.
  // Returns true if schema was loaded, false if no schema file found.
  bool loadSchema();

private:
  void setSchemaLocked(std::shared_ptr<Schema> newSchema);

  friend class Library;
  friend class SoluxNode;
};


/// A library can contain multiple document collections.  It is meant to be the natural
/// implementation of a tenant in a multi-tenant system.
class Library {
public:
  // TODO: schemas (changes from global schemas)
  // TODO: defaults (changes from global)
  // IDEA: for even better multi-tenant support, the ability to arbitrarily nest libraries?  Akin to the ability of a VM to run in a VM.
  //       and in the grpc, rather than distinguish between library and collection, we could just introduce the notion
  //       of a fully qualified index name, represented as an array of string.

private:
  std::string name;
  SharedLazyMap<std::string, Collection> collections;
  friend class SoluxNode;
};



class SoluxNode {
public:
  static constexpr std::string_view kDefaultCollectionName = "main";

  SoluxNode() : SoluxNode(SoluxConfig{}) {}
  explicit SoluxNode(SoluxConfig config);
  ~SoluxNode();

  const SoluxConfig& getConfig() const { return config; }

  // ALTERNATIVE: instead of nested maps, we could also have a single map directly to Collection or Shard objects
  // and represent metadata in the hierarchy.  This choice needs to be informed by the external representation
  // of collections.

  std::shared_ptr<Collection> getCollection(std::string_view name);
  std::shared_ptr<Collection> getOrCreateCollection(std::string_view name);

  std::shared_ptr<Collection> resolveCollection(const solux::api::Target* target);
  std::shared_ptr<Collection> resolveOrCreateCollection(const solux::api::Target* target);

  std::shared_ptr<Collection> getCollection(Library* library, std::string_view name);
  std::shared_ptr<Collection> getOrCreateCollection(Library* library, std::string_view name);

  std::shared_ptr<Library> getLibrary(std::string_view name) {
    unused(name);
    return root; // TODO: temporary
  }

  std::shared_ptr<Library> getLibrary(Library* parent, std::string_view name) {
    unused(parent, name);
    if (parent == nullptr) {
      return root;
    }
    return root; // TODO: look up sub-library
  }

  std::shared_ptr<Library> createLibrary(std::string_view name) {
    unused(name);
    return {};
  }

  std::shared_ptr<Collection> createCollection(Library* library, std::string_view name);

  oneapi::tbb::task_arena& getTaskArena() {
    return taskArena;
  }

  IndexRamBudget& getIndexRamBudget() {
    return indexRamBudget;
  }

  SearchEngine& getSearchEngine() {
    return *searchEngine;
  }


private:

  void createSingletons();
  std::shared_ptr<Collection> initCollection(const std::string& name);
  // Returns the collection unchanged, or throws CollectionResolutionError if it is a load-failure tombstone.
  static std::shared_ptr<Collection> checkLoaded(std::shared_ptr<Collection> collection);
  static std::string normalizedCollectionName(std::string_view name);
  static void validateCollectionName(std::string_view name);

  SoluxConfig config;
  IndexRamBudget indexRamBudget;
  std::unique_ptr<SearchEngine> searchEngine;
  std::shared_ptr<Library> root;
  std::unique_ptr<DirectoryFactory> dirFactory;

  oneapi::tbb::task_arena taskArena;
};

}
