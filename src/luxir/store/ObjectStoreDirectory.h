// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "luxir/store/Directory.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/store/LazyInputFile.h"
#include "luxir/store/ObjectStore.h"
#include "luxir/store/StorageRegistry.h"

namespace luxir {

class Hotcache;

/// A Directory over the objects of an ObjectStore that share a key prefix.
///
/// Reads are lazy: openFile returns a LazyInputFile that fetches byte ranges on
/// first touch, and when the segment has a hotcache (see reader/Hotcache.h) the
/// first open of any of its files prefetches the ranges that opening it reads.
/// The sizes of committed files come from the manifest that opened the index
/// (knownFiles), so opening them costs no size request, and a segment that the
/// manifest lists without a hotcache is not probed for one.
///
/// Writes go to a local staging Directory: createFile hands out a staging file,
/// and finishFile closes it and uploads it with one put. The staged copy stays
/// readable until the file is deleted or the directory is destroyed, so a
/// segment that was just flushed is not fetched back. An acknowledged put is
/// durable, so sync() has nothing left to do. renameFile is a server-side copy
/// plus a remove: the target object is replaced atomically, which is all that
/// CURRENT switching needs. linkFile is a server-side copy.
class ObjectStoreDirectory : public Directory {
public:
  struct Options {
    uint64_t chunkBytes = 1 << 20;  // fetch granularity of lazy reads
    std::string stagingDir;         // local directory for staged writes; empty stages in RAM
    // With a staging directory, how large an index stream may grow in RAM
    // before it becomes an object of its own (0 keeps the writer's 4 MiB).
    // Smaller streams are folded into the segment's first file, so a smaller
    // value means more, finer objects for a lazy reader to choose between.
    uint64_t spillBytes = 0;
  };

private:
  std::shared_ptr<ObjectStore> store;
  std::string prefix;  // empty or ends with '/'
  Options options;

  // Objects a reader still has open. A delete of one waits for the last reader,
  // so a query that is mid-flight on a segment that was merged away still
  // reads it, as it would after an unlink on a filesystem. Shared with the
  // files' release callbacks, which can outlive the directory.
  struct Readers {
    std::mutex mutex;
    std::shared_ptr<ObjectStore> store;
    std::map<std::string, int, std::less<>> open;  // name -> open files
    std::set<std::string, std::less<>> doomed;     // deleted, waiting for the last reader
    void release(const std::string& name, const std::string& key);
  };
  std::shared_ptr<Readers> readers = std::make_shared<Readers>();

  std::mutex mutex;
  std::shared_ptr<Directory> staged;  // created on first write
  std::filesystem::path stagedPath;   // only when staging on a filesystem
  std::map<std::string, std::shared_ptr<const Hotcache>, std::less<>> hot;  // by segment string; null: none
  // Sizes of committed files (knownFiles), so that opening one needs no size
  // request. A name leaves when this directory deletes, writes, renames or
  // links over it.
  std::map<std::string, uint64_t, std::less<>> known;

  std::string key(std::string_view name) const { return prefix + std::string(name); }
  Directory& staging();
  std::shared_ptr<InputFile> stagedFile(std::string_view name);
  std::shared_ptr<const Hotcache> hotcacheOf(std::string_view segStr);
  std::shared_ptr<InputFile> openLazy(std::string_view name);
  std::optional<uint64_t> sizeOf(std::string_view name);  // known, else asks the store
  void forget(std::string_view name);

public:
  /// `prefix` is the key prefix of this directory; a '/' is appended when missing.
  ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix, Options options);
  ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix);
  ~ObjectStoreDirectory() override;

  Capabilities capabilities() const override { return REMOTE | ATOMIC_RENAME | DURABLE_SYNC; }
  ObjectStore& objects() { return *store; }
  const std::string& keyPrefix() const { return prefix; }

  void listFiles(std::vector<FileInfo>& target) override;
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override;
  void knownFiles(std::span<const FileDescriptor> files) override;
  std::unique_ptr<File> createFile(std::string_view name) override;
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override;
  bool deleteFile(std::string_view name) override;
  void finishFile(File& file) override;
  void renameFile(std::string_view from, std::string_view to) override;
  void sync(std::span<const std::string> filenames) override { unused(filenames); }
  void clear() override;
  void linkFile(Directory& source, std::string_view name) override;
  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override;
};

/// The storage backend over an ObjectStore: collections laid out as
/// t/<tenant>/<collection>/<incarnation>/<file> keys (the layout of the fs
/// backend), CURRENT beside the incarnations, node-level files under node/.
///
/// This is the entry point for a plugin that provides a remote store: register
/// a backend whose maker returns
/// `std::make_unique<ObjectStoreDirFactory>(std::make_shared<MyStore>(...), ctx)`.
/// It reads these options from `ctx.options` (the --store.option flags):
///   chunk_bytes  fetch granularity of lazy reads (default 1M)
///   staging_dir  local directory for staged writes (default: stage in RAM)
///   spill_bytes  with staging_dir, stream size at which a file becomes its own
///                object instead of being folded into the segment's first one
class ObjectStoreDirFactory : public DirectoryFactory {
  std::shared_ptr<ObjectStore> store_;
  ObjectStoreDirectory::Options options_;

  std::string collectionPrefix(const CollectionId& id) const { return "t/" + id.tenant + "/" + id.name + "/"; }
  std::shared_ptr<Directory> directory(std::string prefix) const;
  void ensureMarker(const std::string& key);
  bool exists(const std::string& prefix, const std::string& marker);
  void removeAll(const std::string& prefix);

public:
  ObjectStoreDirFactory(std::shared_ptr<ObjectStore> store, const StorageContext& ctx);

  ObjectStore& store() { return *store_; }

  std::vector<CollectionId> collections() override;
  uint64_t storageBytes() override;
  uint64_t collectionBytes(const CollectionId& id) override;
  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override;
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override;
  void createCollection(const CollectionId& id) override;
  std::vector<std::string> incarnations(const CollectionId& id) override;
  void removeIncarnation(const CollectionId& id, std::string_view incarnation) override;
  void remove(const CollectionId& id) override;
  std::shared_ptr<Directory> nodeDirectory() override;
  bool holdsNoCollections() override;
  std::string describe() const override;
  std::string diskPath() const override { return options_.stagingDir; }
};

/// Registers the built-in backend ram_object: an ObjectStoreDirFactory over a
/// RamObjectStore. Options: chunk_bytes, staging_dir (see ObjectStoreDirFactory)
/// and store_name, which selects the process-wide RamObjectStore (default
/// "default"), so that a node restarted in the same process finds its data and
/// tests can inspect the store with RamObjectStore::named().
void registerRamObjectBackend(StorageRegistry& registry);

}  // namespace luxir
