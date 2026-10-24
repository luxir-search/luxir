// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>

#include "luxir/store/Directory.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/store/LazyInputFile.h"
#include "luxir/store/ObjectStore.h"
#include "luxir/store/StorageRegistry.h"

namespace luxir {

class Hotcache;

/// Threads that run the background uploads and removals of an object-store
/// backend's directories. Destroying it runs the tasks still queued, then joins.
class ObjectStoreWorkers {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> tasks;
  bool stopping = false;
  std::vector<std::thread> threads;

  void run();

public:
  explicit ObjectStoreWorkers(size_t threads);
  ~ObjectStoreWorkers();
  ObjectStoreWorkers(const ObjectStoreWorkers&) = delete;
  ObjectStoreWorkers& operator=(const ObjectStoreWorkers&) = delete;

  void submit(std::function<void()> task);
};

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
/// and finishFile closes it and queues its upload, one put, on the backend's
/// workers. So a segment is on its way to the store as soon as it is
/// finalized, and a flush or merge does not wait for the network. sync()
/// waits for the uploads of the files it names (an acknowledged put is
/// durable), uploads one itself that is still queued behind other files, and
/// throws when one failed, so a commit root, which is written
/// only after its files are synced, never names a file the store lacks. The
/// staged copy stays readable until the file is deleted or the directory is
/// destroyed, so a segment that was just flushed is not fetched back.
///
/// A `.pending` file is the first half of a publication: it is renamed into
/// place. It is not uploaded until then, and the rename puts its bytes under
/// the final name with one request. A commit root (commit_<gen>) is put only
/// if absent: a second writer that publishes the same generation fails
/// instead of replacing a root. Renaming a file that is already in the store
/// is a server-side copy plus a remove. linkFile is a server-side copy.
///
/// Removals are queued too and sent in batches (ObjectStore::removeMany); a
/// removed name is gone from this directory at once. A file deleted before
/// its upload started is never sent. The directory keeps the names of its
/// objects from its first listing on, so retiring a segment (deletePrefix)
/// lists nothing.
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
    // Runs uploads and removals; a directory without one starts its own.
    std::shared_ptr<ObjectStoreWorkers> workers;
    uint32_t workerThreads = 8;  // of a directory's own workers
    uint32_t uploadAttempts = 3;  // background attempts before an upload counts as failed
  };

private:
  std::shared_ptr<ObjectStore> store;
  std::string prefix;  // empty or ends with '/'
  Options options;

  // Objects a reader still has open. A delete of one waits for the last reader,
  // so a query that is mid-flight on a segment that was merged away still
  // reads it, as it would after an unlink on a filesystem. Shared with the
  // files' release callbacks and with checksum readers, which can outlive the
  // directory: `dir` is this directory while it lives, null once destroyed,
  // and is read under a shared lock of `owner`.
  struct Readers {
    std::mutex mutex;
    std::shared_ptr<ObjectStore> store;
    std::map<std::string, int, std::less<>> open;  // name -> open files
    std::set<std::string, std::less<>> doomed;     // deleted, waiting for the last reader
    std::shared_mutex owner;
    ObjectStoreDirectory* dir = nullptr;
    void release(const std::string& name, const std::string& key);
  };
  std::shared_ptr<Readers> readers = std::make_shared<Readers>();

  // A segment's hotcache, fetched once by the first opener while others wait
  // on the slot, not on the directory. `cache` is null when the segment has
  // none (or its hotcache is unusable).
  struct HotSlot {
    std::mutex mutex;
    bool loaded = false;
    std::shared_ptr<const Hotcache> cache;
  };

  // Files that are staged but not in the store yet, and objects whose removal
  // is queued. Shared with the queued tasks, which can outlive the directory.
  struct Upload {
    enum State { QUEUED, RUNNING, FAILED } state = QUEUED;
    uint64_t size = 0;
    std::exception_ptr error;  // FAILED
  };
  struct Writes {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, std::shared_ptr<Upload>, std::less<>> uploads;  // by name
    std::map<std::string, uint64_t, std::less<>> deferred;  // .pending files, uploaded by their rename
    std::set<std::string, std::less<>> removals;  // queued
    std::set<std::string, std::less<>> removing;  // in the batch being sent
    bool removalQueued = false;  // a removal task is queued
    bool closed = false;
    bool abandoned = false;  // the objects under the prefix are being removed
    std::set<std::string, std::less<>> unsendable;  // finished after abandon(): a sync of one fails
    // Every name that may have an object, once a listing has been seen: the
    // listed names, plus what this directory wrote, minus what it removed.
    std::set<std::string, std::less<>> inventory;
    bool inventoryComplete = false;
    // Commit roots another writer published where this directory tried to:
    // never removed here, not even by the cleanup of the commit that lost.
    std::set<std::string, std::less<>> foreign;
  };
  std::shared_ptr<Writes> writes = std::make_shared<Writes>();
  std::shared_ptr<ObjectStoreWorkers> workers;

  std::mutex mutex;
  std::shared_ptr<Directory> staged;  // created on first write
  std::filesystem::path stagedPath;   // only when staging on a filesystem
  std::map<std::string, std::shared_ptr<HotSlot>, std::less<>> hot;  // by segment string
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
  // Drops the known size of `name` and, for a hotcache file, the cached
  // hotcache of its segment: the name is about to hold another object.
  void forget(std::string_view name);
  // Waits until no upload of `name` and no removal batch holding it runs.
  void settle(std::unique_lock<std::mutex>& lock, std::string_view name);
  // Before `name` is written again: a queued upload or removal of the old
  // bytes is dropped, a running one is waited for.
  void supersede(std::unique_lock<std::mutex>& lock, std::string_view name);
  // Puts the staged file; the caller marked `entry` RUNNING. Settles it.
  static void upload(const std::shared_ptr<Writes>& writes, ObjectStore& store, Directory& staging,
                     const std::string& name, const std::string& objectKey, const std::shared_ptr<Upload>& entry,
                     uint32_t attempts);
  void queueRemoval(std::unique_lock<std::mutex>& lock, const std::string& name);
  static void sendRemovals(const std::shared_ptr<Writes>& writes, ObjectStore& store, const std::string& prefix);
  void publish(std::string_view name, std::string_view bytes);  // put under the final name of a rename
  void ensureLive();  // throws once abandoned

public:
  /// `prefix` is the key prefix of this directory; a '/' is appended when missing.
  ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix, Options options);
  ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix);
  ~ObjectStoreDirectory() override;

  Capabilities capabilities() const override { return REMOTE | ATOMIC_RENAME | DURABLE_SYNC; }
  ObjectStore& objects() { return *store; }
  const std::string& keyPrefix() const { return prefix; }
  /// True while a removal of `name` is queued or being sent: the name is gone
  /// from this directory though the store may still hold its object. A layer
  /// that reads the store directly (a cache) treats the name as absent.
  bool removalPending(std::string_view name);
  /// The objects under this directory's prefix are about to be removed (a
  /// collection or incarnation is deleted): drops the uploads and removals
  /// that are queued, waits for the ones in flight, and sends nothing to the
  /// store from then on, so nothing lands after the removal. Reads go on.
  void abandon();

  void listFiles(std::vector<FileInfo>& target) override;
  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override;
  void knownFiles(std::span<const FileDescriptor> files) override;
  std::unique_ptr<File> createFile(std::string_view name) override;
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override;
  bool deleteFile(std::string_view name) override;
  void finishFile(File& file) override;
  void renameFile(std::string_view from, std::string_view to) override;
  void sync(std::span<const std::string> filenames) override;
  void deletePrefix(std::string_view namePrefix) override;
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
///   upload_threads   uploads and removal batches in flight (default 8)
///   upload_attempts  tries of a background upload before sync reports it (default 3)
class ObjectStoreDirFactory : public DirectoryFactory {
  std::shared_ptr<ObjectStore> store_;
  ObjectStoreDirectory::Options options_;
  std::mutex directoriesMutex;
  std::vector<std::weak_ptr<ObjectStoreDirectory>> directories;  // handed out, abandoned by a removal

  std::string collectionPrefix(const CollectionId& id) const { return "t/" + id.tenant + "/" + id.name + "/"; }
  std::shared_ptr<Directory> directory(std::string prefix);
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
