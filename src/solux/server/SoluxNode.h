#pragma once
#include "solux/store/Directory.h"
#include "solux/index/IndexWriter.h"

namespace solux {

/// There should normally be a single SoluxNode instance per process.
/// A single SoluxNode can host many indexes.
/// There still *may* be multiple SoluxNodes instances per process, but it's currently more for testing.


class Library;
class Collection;
class SoluxNode;

class Shard {
  std::shared_ptr<Directory> dir;  // does this need to be shared_ptr?  Perhaps not if we have a shared ptr to a parent object (Shard or Collection?)
  std::shared_ptr<IndexWriter> iw;
public:
  // TODO: we don't want to be in the position of having more than ine IW pointing at the index/dir... this suggests that instead of
  // having the ability for it to come and go, it should be a singleton (which could still be created on demand) that
  // can dump most of it's state for low memory usage?  Then this method would not return a shared_ptr, but a simple reference.
  std::shared_ptr<IndexWriter> getIndexWriter() {
    return iw;
  }

  // TODO: if a shard isn't currently "loaded", should it be removed from the map, or just it's size cut down?

  friend class Collection;
  friend class Library;
  friend class SoluxNode;
};



// A single logical collection of docs which may
// consist of multiple shards.
class Collection {
  std::string name;
  std::shared_ptr<Shard> shard;
  std::vector<std::shared_ptr<Shard>> shards;
public:

  std::shared_ptr<Shard> getShard() {
    return shard;
  }

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
  gtl::parallel_flat_hash_map<std::string, std::shared_ptr<Collection>> collections;
  friend class SoluxNode;
};



class SoluxNode {
public:
  SoluxNode() {
    createSingletons();
  }

  // ALTERNATIVE: instead of nested maps, we could also have a single map directly to Collection or Shard objects
  // and represent metadata in the hierarchy.  This choice needs to be informed by the external representation
  // of collections.

  std::shared_ptr<Collection> getCollection(std::string_view name) {
    unused(name);
    return collection;
  }

  std::shared_ptr<Collection> getCollection(Library* library, std::string_view name) {
    unused(library, name);
    return collection;
  }

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

  std::shared_ptr<Collection> createCollection(Library* library, std::string_view name) {
    unused(library, name);
    return {};
  }

private:

  void createSingletons() {
    collection = std::make_shared<Collection>();
    collection->shard = std::make_shared<Shard>();
    collection->shard->dir = std::make_shared<RAMDir>();
    collection->shard->iw = std::make_shared<IndexWriter>(*collection->shard->dir);
  }

  std::shared_ptr<Library> root;
  // temporary singletons
  RAMDir dir;
  std::shared_ptr<Shard> shard;
  std::shared_ptr<Collection> collection;
};

}