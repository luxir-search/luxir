// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/ObjectStoreDirectory.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <shared_mutex>

#include "luxir/reader/Hotcache.h"
#include "luxir/reader/Postings.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/log.h"
#include "luxir/util/thread.h"

namespace luxir {

namespace {

// Marker objects make an empty "directory" exist. They are hidden from listings.
constexpr std::string_view kCollectionMarker = ".collection";
constexpr std::string_view kIncarnationMarker = ".dir";

bool isHidden(std::string_view name) { return name.starts_with('.'); }

bool alreadyExists(const std::exception& e) {
  return std::string_view(e.what()).find("already exists") != std::string_view::npos;
}

// Published by a rename, so uploaded under its final name then (Manifest::commit,
// the layout file and replication.json write `<name>.pending` and rename it).
bool publishedByRename(std::string_view name) { return name.ends_with(".pending"); }

std::string describe(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "unknown error";
  }
}

// Removals a retirement issues one after another wait this long to share a batch.
constexpr auto kRemovalDelay = std::chrono::milliseconds(10);

// One sequential checksum pass over an object: ranged reads of a bounded
// buffer, never the mapping, so verification streams without materializing.
// map() opens the file the way the directory does, so a verified open still
// prefetches the hotcache and is counted as a reader.
class ObjectChecksumReader : public ChecksumReader {
  std::shared_ptr<ObjectStore> store;
  std::string key;
  uint64_t size_;
  uint64_t chunkBytes;
  std::function<std::shared_ptr<InputFile>()> mapper;

public:
  ObjectChecksumReader(std::shared_ptr<ObjectStore> store, std::string key, uint64_t size, uint64_t chunkBytes,
                       std::function<std::shared_ptr<InputFile>()> mapper)
      : store(std::move(store)), key(std::move(key)), size_(size),
        chunkBytes(std::clamp<uint64_t>(chunkBytes, 4096, 8 << 20)), mapper(std::move(mapper)) {}

  uint64_t size() const override { return size_; }

  std::shared_ptr<InputFile> map() override { return mapper(); }

  FileCheck checksum(const std::function<void(size_t)>& progress) override {
    std::vector<char> buffer((size_t)std::min(size_, chunkBytes));
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (uint64_t offset = 0; offset < size_;) {
      size_t length = (size_t)std::min<uint64_t>(buffer.size(), size_ - offset);
      store->readRange(key, offset, std::span<char>(buffer.data(), length));
      XXH3_64bits_update(&state, buffer.data(), length);
      offset += length;
      if (progress) progress(length);
    }
    return {size_, XXH3_64bits_digest(&state)};
  }
};

}  // namespace


ObjectStoreWorkers::ObjectStoreWorkers(size_t count) {
  for (size_t i = 0; i < std::max<size_t>(count, 1); i++) threads.emplace_back([this] { run(); });
}

ObjectStoreWorkers::~ObjectStoreWorkers() {
  {
    std::lock_guard lock(mutex);
    stopping = true;
  }
  wake.notify_all();
  for (auto& thread : threads) thread.join();
}

void ObjectStoreWorkers::submit(std::function<void()> task) {
  {
    std::lock_guard lock(mutex);
    tasks.push_back(std::move(task));
  }
  wake.notify_one();
}

void ObjectStoreWorkers::run() {
  nameThisThread("luxir-objstore");
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, [&] { return stopping || !tasks.empty(); });
      if (tasks.empty()) return;
      task = std::move(tasks.front());
      tasks.pop_front();
    }
    task();  // a task reports its own failures
  }
}


ObjectStoreDirectory::ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix, Options options)
    : store(std::move(store)), prefix(std::move(prefix)), options(std::move(options)) {
  if (!this->prefix.empty() && !this->prefix.ends_with('/')) this->prefix.push_back('/');
  readers->store = this->store;
  readers->dir = this;
  workers = this->options.workers ? this->options.workers
                                  : std::make_shared<ObjectStoreWorkers>(this->options.workerThreads);
}

void ObjectStoreDirectory::Readers::release(const std::string& name, const std::string& key) {
  bool remove = false;
  {
    std::lock_guard lock(mutex);
    auto it = open.find(name);
    if (it == open.end() || --it->second > 0) return;
    open.erase(it);
    remove = doomed.erase(name) > 0;
  }
  if (remove) store->remove(key);
}

ObjectStoreDirectory::ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix)
    : ObjectStoreDirectory(std::move(store), std::move(prefix), Options{}) {}

ObjectStoreDirectory::~ObjectStoreDirectory() {
  {
    // A checksum reader that outlives the directory maps nothing.
    std::unique_lock lock(readers->owner);
    readers->dir = nullptr;
  }
  {
    // A queued upload is of a file nothing synced, so nothing references it.
    // A running one reads the staged copy, which goes below.
    std::unique_lock lock(writes->mutex);
    writes->closed = true;
    std::erase_if(writes->uploads, [](const auto& entry) { return entry.second->state != Upload::RUNNING; });
    // A running upload ends done or FAILED (a closing directory does not retry).
    writes->changed.wait(lock, [&] {
      return std::ranges::none_of(writes->uploads, [](const auto& e) { return e.second->state == Upload::RUNNING; })
          && writes->removing.empty();
    });
    writes->uploads.clear();
  }
  writes->changed.notify_all();
  sendRemovals(writes, *store, prefix);
  if (!stagedPath.empty()) {
    staged.reset();
    std::error_code ignored;
    std::filesystem::remove_all(stagedPath, ignored);
  }
}

Directory& ObjectStoreDirectory::staging() {
  std::lock_guard lock(mutex);
  if (!staged) {
    if (options.stagingDir.empty()) {
      staged = std::make_shared<RAMDir>();
    } else {
      stagedPath = std::filesystem::path(options.stagingDir) / newUuid();
      staged = std::make_shared<FSDirectory>(stagedPath, true);
    }
  }
  return *staged;
}

std::shared_ptr<InputFile> ObjectStoreDirectory::stagedFile(std::string_view name) {
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  return dir ? dir->openFile(name) : nullptr;
}

std::shared_ptr<const Hotcache> ObjectStoreDirectory::hotcacheOf(std::string_view segStr) {
  // The directory lock only finds or makes the segment's slot; the fetch runs
  // under the slot's own lock, so other segments open meanwhile.
  std::shared_ptr<HotSlot> slot;
  {
    std::lock_guard lock(mutex);
    auto it = hot.find(segStr);
    if (it == hot.end()) it = hot.emplace(std::string(segStr), std::make_shared<HotSlot>()).first;
    slot = it->second;
  }
  std::lock_guard slotLock(slot->mutex);
  if (slot->loaded) return slot->cache;
  try {
    std::string fileName = Hotcache::fileName(segStr);
    if (auto size = sizeOf(fileName)) {
      std::string bytes((size_t)*size, '\0');
      store->readRange(key(fileName), 0, std::span<char>(bytes.data(), bytes.size()));
      slot->cache = std::make_shared<const Hotcache>(Hotcache::decode(bytes));
    }
  } catch (const std::exception& e) {
    // An optimization only: without it the segment opens with small requests.
    LOG_WARN("ignoring hotcache of segment {}: {}", segStr, e.what());
  }
  slot->loaded = true;
  return slot->cache;
}

void ObjectStoreDirectory::listFiles(std::vector<FileInfo>& target) {
  // Files on their way to the store, taken before the listing: one whose
  // upload completes meanwhile is in either.
  std::map<std::string, uint64_t, std::less<>> unsent;
  {
    std::lock_guard lock(writes->mutex);
    for (const auto& [name, entry] : writes->uploads) unsent.emplace(name, entry->size);
    for (const auto& [name, size] : writes->deferred) unsent.emplace(name, size);
  }
  auto objects = store->list(prefix);
  std::vector<FileInfo> found;
  {
    std::scoped_lock lock(readers->mutex, writes->mutex);
    for (const auto& object : objects) {
      std::string_view name = std::string_view(object.name).substr(prefix.size());
      if (name.empty() || isHidden(name) || name.find('/') != std::string_view::npos) continue;
      if (writes->removals.contains(name) || writes->removing.contains(name)) continue;
      writes->inventory.emplace(name);
      if (readers->doomed.contains(name) || unsent.contains(name)) continue;
      found.push_back({std::string(name), object.size});
    }
    for (const auto& [name, size] : unsent) {
      if (!writes->removals.contains(name) && !writes->removing.contains(name)) found.push_back({name, size});
    }
    writes->inventoryComplete = true;
  }
  std::ranges::sort(found, {}, &FileInfo::name);
  target.insert(target.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
}

std::optional<uint64_t> ObjectStoreDirectory::sizeOf(std::string_view name) {
  {
    std::lock_guard lock(mutex);
    if (auto it = known.find(name); it != known.end()) return it->second;
  }
  return store->size(key(name));
}

void ObjectStoreDirectory::forget(std::string_view name) {
  std::lock_guard lock(mutex);
  if (auto it = known.find(name); it != known.end()) known.erase(it);
  // s<segStr>__hot: the segment's hotcache changes with the object.
  constexpr std::string_view kHotSuffix = "__hot";
  if (name.ends_with(kHotSuffix) && name.starts_with(Postings::PREFIX_FNAME)) {
    size_t start = Postings::PREFIX_FNAME.size();
    auto it = hot.find(name.substr(start, name.size() - start - kHotSuffix.size()));
    if (it != hot.end()) hot.erase(it);
  }
}

void ObjectStoreDirectory::knownFiles(std::span<const FileDescriptor> files) {
  std::set<std::string_view> hotFiles;
  for (const auto& file : files) {
    if (file.name.ends_with("__hot")) hotFiles.insert(file.name);
  }
  std::lock_guard lock(mutex);
  for (const auto& file : files) {
    known.insert_or_assign(file.name, file.size);
    // A hotcache is written before its segment is first committed and listed
    // with it, so a segment listed without one has none.
    if (auto data = Hotcache::parseDataFile(file.name)) {
      if (!hotFiles.contains(Hotcache::fileName(data->segStr)) && !hot.contains(data->segStr)) {
        auto slot = std::make_shared<HotSlot>();
        slot->loaded = true;
        hot.emplace(std::string(data->segStr), std::move(slot));
      }
    }
  }
}

std::shared_ptr<InputFile> ObjectStoreDirectory::openFile(std::string_view name, bool expectSynced) {
  unused(expectSynced);
  {
    std::lock_guard lock(writes->mutex);
    if (writes->removals.contains(name) || writes->removing.contains(name)) return {};
  }
  if (auto file = stagedFile(name)) return file;
  return openLazy(name);
}

std::shared_ptr<InputFile> ObjectStoreDirectory::openLazy(std::string_view name) {
  std::string objectKey = key(name);
  // Count the reader before anything else: a delete that arrives from here on
  // sees it and dooms the object instead of removing it, so the size lookup
  // and the file see the object or (deleted before) nothing at all.
  {
    std::lock_guard lock(readers->mutex);
    if (readers->doomed.contains(name)) return {};
    readers->open[std::string(name)]++;
  }
  std::shared_ptr<LazyInputFile> file;
  try {
    auto size = sizeOf(name);
    if (!size) {
      readers->release(std::string(name), objectKey);
      return {};
    }
    file = std::make_shared<LazyInputFile>(store, objectKey, *size, options.chunkBytes,
        [readers = readers, name = std::string(name), objectKey] { readers->release(name, objectKey); });
  } catch (...) {
    readers->release(std::string(name), objectKey);
    throw;
  }
  auto data = Hotcache::parseDataFile(name);
  if (!data) {
    // Not segment data (CURRENT, a commit manifest, live docs, an overlay): it
    // is read whole and may be replaced in place, so never leave it half
    // fetched.
    file->read();
  } else if (auto cache = hotcacheOf(data->segStr)) {
    auto ranges = cache->rangesOf(data->fileNum);
    if (!ranges.empty()) file->prefetchRanges(ranges);
  }
  return file;
}

std::unique_ptr<File> ObjectStoreDirectory::createFile(std::string_view name) {
  return staging().createFile(name);
}

std::unique_ptr<File> ObjectStoreDirectory::createFile(std::string_view name, FileCreateOptions createOptions) {
  if (options.spillBytes && createOptions.ramDelegating) createOptions.ramSpillBytes = (size_t)options.spillBytes;
  return staging().createFile(name, std::move(createOptions));
}

void ObjectStoreDirectory::settle(std::unique_lock<std::mutex>& lock, std::string_view name) {
  writes->changed.wait(lock, [&] {
    auto it = writes->uploads.find(name);
    return (it == writes->uploads.end() || it->second->state != Upload::RUNNING) && !writes->removing.contains(name);
  });
}

void ObjectStoreDirectory::supersede(std::unique_lock<std::mutex>& lock, std::string_view name) {
  settle(lock, name);
  if (auto it = writes->uploads.find(name); it != writes->uploads.end()) writes->uploads.erase(it);
  if (auto it = writes->deferred.find(name); it != writes->deferred.end()) writes->deferred.erase(it);
  if (auto it = writes->removals.find(name); it != writes->removals.end()) writes->removals.erase(it);
}

void ObjectStoreDirectory::upload(const std::shared_ptr<Writes>& writes, ObjectStore& store, Directory& staging,
                                  const std::string& name, const std::string& objectKey,
                                  const std::shared_ptr<Upload>& entry, uint32_t attempts) {
  std::exception_ptr error;
  for (uint32_t attempt = 0; attempt < attempts; attempt++) {
    if (attempt > 0) {
      // Backs off 200 ms, then 800 ms, ...; a closing directory stops retrying.
      auto backoff = std::chrono::milliseconds(200) * (1 << std::min<uint32_t>(2 * (attempt - 1), 8));
      std::unique_lock lock(writes->mutex);
      if (writes->changed.wait_for(lock, backoff, [&] { return writes->closed || writes->abandoned; })) break;
    }
    try {
      auto input = staging.openFile(name);
      if (!input) throw std::runtime_error("staged file vanished: " + name);
      store.put(objectKey, input->read());
      error = nullptr;
      break;
    } catch (...) {
      error = std::current_exception();
    }
  }
  {
    std::lock_guard lock(writes->mutex);
    if (error) {
      entry->state = Upload::FAILED;
      entry->error = error;
    } else if (auto it = writes->uploads.find(name); it != writes->uploads.end() && it->second == entry) {
      writes->uploads.erase(it);
    }
  }
  writes->changed.notify_all();
  if (error) LOG_WARN("upload of {} failed after {} attempt(s): {}", objectKey, attempts, describe(error));
}

void ObjectStoreDirectory::queueRemoval(std::unique_lock<std::mutex>& lock, const std::string& name) {
  unused(lock);
  if (auto it = writes->inventory.find(name); it != writes->inventory.end()) writes->inventory.erase(it);
  if (writes->abandoned) return;  // the removal of the whole prefix takes it
  writes->removals.insert(name);
  if (writes->removalQueued) return;
  writes->removalQueued = true;
  workers->submit([writes = writes, store = store, prefix = prefix] {
    std::this_thread::sleep_for(kRemovalDelay);
    sendRemovals(writes, *store, prefix);
  });
}

void ObjectStoreDirectory::sendRemovals(const std::shared_ptr<Writes>& writes, ObjectStore& store,
                                        const std::string& prefix) {
  std::vector<std::string> names;
  {
    std::lock_guard lock(writes->mutex);
    writes->removalQueued = false;
    names.assign(writes->removals.begin(), writes->removals.end());
    writes->removing.insert(names.begin(), names.end());
    writes->removals.clear();
  }
  if (names.empty()) return;
  std::vector<std::string> keys;
  keys.reserve(names.size());
  for (const auto& name : names) keys.push_back(prefix + name);
  for (int attempt = 0;; attempt++) {
    try {
      store.removeMany(keys);
      break;
    } catch (const std::exception& e) {
      if (attempt == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      // Unreferenced objects; the startup sweep of unreferenced index files removes them.
      LOG_WARN("removing {} object(s) under {} failed: {}", keys.size(), prefix, e.what());
      break;
    }
  }
  {
    std::lock_guard lock(writes->mutex);
    for (const auto& name : names) writes->removing.erase(name);
  }
  writes->changed.notify_all();
}

bool ObjectStoreDirectory::removalPending(std::string_view name) {
  std::lock_guard lock(writes->mutex);
  return writes->removals.contains(name) || writes->removing.contains(name);
}

void ObjectStoreDirectory::abandon() {
  {
    std::unique_lock lock(writes->mutex);
    writes->abandoned = true;
    std::erase_if(writes->uploads, [](const auto& entry) { return entry.second->state != Upload::RUNNING; });
    writes->deferred.clear();
    writes->removals.clear();
    // A running upload in its backoff gives up; it ends FAILED and is dropped below.
    writes->changed.notify_all();
    writes->changed.wait(lock, [&] {
      return std::ranges::none_of(writes->uploads, [](const auto& e) { return e.second->state == Upload::RUNNING; })
          && writes->removing.empty();
    });
    writes->uploads.clear();
  }
  writes->changed.notify_all();
}

void ObjectStoreDirectory::ensureLive() {
  std::lock_guard lock(writes->mutex);
  if (writes->abandoned) throw std::runtime_error("directory " + prefix + " was removed");
}

bool ObjectStoreDirectory::deleteFile(std::string_view name) {
  {
    std::lock_guard lock(writes->mutex);
    if (writes->foreign.contains(name)) return false;
  }
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  forget(name);
  {
    std::unique_lock lock(writes->mutex);
    settle(lock, name);
    if (writes->abandoned) {
      // The removal of the whole prefix takes its object.
      writes->unsendable.erase(std::string(name));
      lock.unlock();
      return dir && dir->deleteFile(name);
    }
    bool unsent = false;
    if (auto it = writes->deferred.find(name); it != writes->deferred.end()) {
      writes->deferred.erase(it);
      unsent = true;
    }
    if (auto it = writes->uploads.find(name); it != writes->uploads.end()) {
      // A failed upload may still have landed (a lost response): remove it below.
      unsent = it->second->state == Upload::QUEUED;
      writes->uploads.erase(it);
    }
    if (unsent) {
      // Never sent, so there is no object to remove.
      if (auto it = writes->inventory.find(name); it != writes->inventory.end()) writes->inventory.erase(it);
      lock.unlock();
      if (dir) dir->deleteFile(name);
      return true;
    }
  }
  bool local = dir && dir->deleteFile(name);
  std::unique_lock lock(writes->mutex);
  if (!writes->inventoryComplete || !writes->inventory.contains(name)) {
    // Not a name this directory knows of (another handle wrote it, or it was
    // never written): ask the store, which also says whether it was there.
    lock.unlock();
    {
      std::lock_guard readersLock(readers->mutex);
      auto it = readers->open.find(name);
      if (it != readers->open.end() && it->second > 0) return readers->doomed.emplace(name).second || local;
    }
    return store->remove(key(name)) || local;
  }
  {
    std::lock_guard readersLock(readers->mutex);
    auto it = readers->open.find(name);
    if (it != readers->open.end() && it->second > 0) {
      // A reader is mid-flight: the last one to finish removes the object.
      if (auto known = writes->inventory.find(name); known != writes->inventory.end()) writes->inventory.erase(known);
      return readers->doomed.emplace(name).second || local;
    }
  }
  queueRemoval(lock, std::string(name));
  return true;
}

void ObjectStoreDirectory::deletePrefix(std::string_view namePrefix) {
  std::vector<std::string> names;
  bool complete;
  {
    std::lock_guard lock(writes->mutex);
    complete = writes->inventoryComplete;
    if (complete) {
      for (auto it = writes->inventory.lower_bound(namePrefix);
           it != writes->inventory.end() && it->starts_with(namePrefix); ++it) {
        names.push_back(*it);
      }
    }
  }
  if (!complete) {
    std::vector<FileInfo> files;
    listFiles(files);
    for (auto& file : files) {
      if (file.name.starts_with(namePrefix)) names.push_back(std::move(file.name));
    }
  }
  for (const auto& name : names) deleteFile(name);
}

void ObjectStoreDirectory::finishFile(File& file) {
  Directory& dir = staging();
  dir.finishFile(file);
  std::string name = file.name();
  forget(name);  // the name is reused: its object changes
  {
    std::lock_guard lock(readers->mutex);
    readers->doomed.erase(name);  // the name is reused: the new object wins
  }
  auto entry = std::make_shared<Upload>();
  entry->size = file.size();
  std::shared_ptr<Directory> stagingDir;
  {
    std::lock_guard lock(mutex);
    stagingDir = staged;
  }
  {
    std::unique_lock lock(writes->mutex);
    supersede(lock, name);
    if (writes->abandoned) {
      // Stays staged and is never sent; a sync of it fails.
      writes->unsendable.insert(name);
      return;
    }
    writes->inventory.insert(name);
    if (publishedByRename(name)) {
      writes->deferred.insert_or_assign(name, entry->size);
      return;
    }
    writes->uploads.insert_or_assign(name, entry);
  }
  workers->submit([writes = writes, store = store, stagingDir, name, objectKey = key(name), entry,
                   attempts = options.uploadAttempts] {
    {
      std::lock_guard lock(writes->mutex);
      auto it = writes->uploads.find(name);
      // Deleted, renamed or written again before its turn.
      if (it == writes->uploads.end() || it->second != entry || entry->state != Upload::QUEUED) return;
      entry->state = Upload::RUNNING;
    }
    upload(writes, *store, *stagingDir, name, objectKey, entry, attempts);
  });
}

void ObjectStoreDirectory::sync(std::span<const std::string> filenames) {
  for (const auto& name : filenames) {
    std::unique_lock lock(writes->mutex);
    for (;;) {
      if (writes->unsendable.contains(name)) {
        throw std::runtime_error("cannot sync " + key(name) + ": its directory was removed");
      }
      auto it = writes->uploads.find(name);
      if (it == writes->uploads.end()) break;  // in the store, or never written
      auto entry = it->second;
      if (entry->state == Upload::RUNNING) {
        writes->changed.wait(lock);
        continue;
      }
      // Still queued: the workers are busy with other files (a large merge),
      // so upload it here rather than wait behind them; its task finds it
      // sent. Failed: the background attempts failed, and one more here lets
      // a commit after an outage go through, while a failure fails the commit.
      uint32_t attempts = entry->state == Upload::QUEUED ? options.uploadAttempts : 1;
      entry->state = Upload::RUNNING;
      lock.unlock();
      std::shared_ptr<Directory> stagingDir;
      {
        std::lock_guard dirLock(mutex);
        stagingDir = staged;
      }
      upload(writes, *store, *stagingDir, name, key(name), entry, attempts);
      lock.lock();
      if (entry->state == Upload::FAILED) {
        throw std::runtime_error("upload of " + key(name) + " failed: " + describe(entry->error));
      }
    }
  }
}

void ObjectStoreDirectory::publish(std::string_view name, std::string_view bytes) {
  ensureLive();
  std::string objectKey = key(name);
  if (!Manifest::generationOf(name)) {
    store->put(objectKey, bytes);
    return;
  }
  // A commit root is created, never replaced: two writers cannot both
  // publish one generation.
  try {
    store->put(objectKey, bytes, true);
    return;
  } catch (const std::exception& e) {
    if (!alreadyExists(e)) throw;
  }
  // The generation exists: the same commit published before (an interrupted
  // attempt), a damaged root that a repair replaces, or another writer's.
  std::string existing;
  if (auto size = store->size(objectKey)) {
    existing.resize((size_t)*size);
    store->readRange(objectKey, 0, std::span<char>(existing.data(), existing.size()));
    if (existing == bytes) return;
    if (Manifest::intact(existing)) {
      {
        std::lock_guard lock(writes->mutex);
        writes->foreign.insert(std::string(name));
      }
      throw std::runtime_error("commit root " + objectKey + " already exists with other content: another writer " +
                               "published this generation");
    }
  }
  store->put(objectKey, bytes);
}

void ObjectStoreDirectory::renameFile(std::string_view from, std::string_view to) {
  forget(from);
  forget(to);
  bool unsent = false;
  bool failed = false;
  std::optional<uint64_t> deferredSize;
  std::shared_ptr<Upload> upload;
  {
    std::unique_lock lock(writes->mutex);
    supersede(lock, to);
    settle(lock, from);
    if (auto it = writes->deferred.find(from); it != writes->deferred.end()) {
      deferredSize = it->second;
      writes->deferred.erase(it);
      unsent = true;
    }
    if (auto it = writes->uploads.find(from); it != writes->uploads.end()) {
      upload = it->second;
      failed = upload->state == Upload::FAILED;
      unsent = true;
      writes->uploads.erase(it);
    }
  }
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  if (unsent) {
    // Its bytes go straight to the final name: one put, no copy.
    try {
      auto input = dir ? dir->openFile(from) : nullptr;
      if (!input) throw std::runtime_error("staged file vanished: " + std::string(from));
      publish(to, input->read());
    } catch (...) {
      // `from` is still staged and unsent: a retry or its cleanup finds it so.
      std::lock_guard lock(writes->mutex);
      if (deferredSize) writes->deferred.try_emplace(std::string(from), *deferredSize);
      if (upload) {
        upload->state = Upload::FAILED;
        upload->error = std::current_exception();
        writes->uploads.try_emplace(std::string(from), upload);
      }
      throw;
    }
    std::unique_lock lock(writes->mutex);
    writes->inventory.insert(std::string(to));
    if (failed) queueRemoval(lock, std::string(from));  // the failed put may have landed
    else if (auto it = writes->inventory.find(from); it != writes->inventory.end()) writes->inventory.erase(it);
  } else {
    ensureLive();
    store->copy(key(from), key(to));
    std::unique_lock lock(writes->mutex);
    writes->inventory.insert(std::string(to));
    queueRemoval(lock, std::string(from));
  }
  {
    std::lock_guard lock(readers->mutex);
    readers->doomed.erase(std::string(to));  // the name holds a new object now
  }
  if (dir) {
    dir->deleteFile(to);
    if (dir->openFile(from)) dir->renameFile(from, to);
  }
}

void ObjectStoreDirectory::clear() {
  {
    std::unique_lock lock(writes->mutex);
    writes->changed.wait(lock, [&] {
      return std::ranges::none_of(writes->uploads, [](const auto& e) { return e.second->state == Upload::RUNNING; })
          && writes->removing.empty();
    });
    writes->uploads.clear();
    writes->deferred.clear();
  }
  std::vector<FileInfo> files;
  listFiles(files);
  std::vector<std::string> keys;
  for (const auto& file : files) keys.push_back(key(file.name));
  store->removeMany(keys);
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
    hot.clear();
    known.clear();
  }
  {
    std::lock_guard lock(writes->mutex);
    writes->removals.clear();
    writes->inventory.clear();
  }
  if (dir) dir->clear();
}

void ObjectStoreDirectory::linkFile(Directory& source, std::string_view name) {
  auto* other = dynamic_cast<ObjectStoreDirectory*>(&source.underlying());
  if (!other || other->store != store) {
    copyFile(source, name);
    return;
  }
  std::array<std::string, 1> names{std::string(name)};
  other->sync(names);  // the copy reads the source's object
  ensureLive();
  forget(name);
  {
    std::unique_lock lock(writes->mutex);
    supersede(lock, name);
    writes->inventory.insert(std::string(name));
  }
  store->copy(other->key(name), key(name));
  {
    std::lock_guard lock(readers->mutex);
    readers->doomed.erase(std::string(name));  // the name holds a new object now
  }
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  if (dir) dir->deleteFile(name);
}

std::shared_ptr<ChecksumReader> ObjectStoreDirectory::openChecksum(std::string_view name, bool direct) {
  bool unsent;
  {
    std::lock_guard lock(writes->mutex);
    if (writes->removals.contains(name) || writes->removing.contains(name)) return {};
    unsent = writes->uploads.contains(name) || writes->deferred.contains(name);
  }
  if (unsent) {
    // Not in the store yet: the staged copy is what will be.
    std::shared_ptr<Directory> dir;
    {
      std::lock_guard lock(mutex);
      dir = staged;
    }
    if (auto reader = dir ? dir->openChecksum(name, direct) : nullptr) return reader;
  }
  auto size = sizeOf(name);
  if (!size) return {};
  // The reader may outlive this directory: map() takes the shared owner lock
  // and finds the directory gone, then.
  auto mapper = [readers = readers, name = std::string(name)]() -> std::shared_ptr<InputFile> {
    std::shared_lock lock(readers->owner);
    if (!readers->dir) return {};
    return readers->dir->openLazy(name);
  };
  return std::make_shared<ObjectChecksumReader>(store, key(name), *size, options.chunkBytes, std::move(mapper));
}


ObjectStoreDirFactory::ObjectStoreDirFactory(std::shared_ptr<ObjectStore> store, const StorageContext& ctx)
    : store_(std::move(store)) {
  if (!store_) throw std::invalid_argument("object store backend needs an ObjectStore");
  options_.chunkBytes = ctx.optionBytes("chunk_bytes", options_.chunkBytes);
  if (options_.chunkBytes == 0) throw std::invalid_argument("store option 'chunk_bytes' must be positive");
  options_.stagingDir = ctx.optionOr("staging_dir", "");
  options_.spillBytes = ctx.optionBytes("spill_bytes", 0);
  options_.workerThreads = (uint32_t)ctx.optionBytes("upload_threads", options_.workerThreads);
  options_.uploadAttempts = (uint32_t)ctx.optionBytes("upload_attempts", options_.uploadAttempts);
  if (options_.workerThreads == 0 || options_.uploadAttempts == 0) {
    throw std::invalid_argument("store options 'upload_threads' and 'upload_attempts' must be positive");
  }
  options_.workers = std::make_shared<ObjectStoreWorkers>(options_.workerThreads);
}

std::shared_ptr<Directory> ObjectStoreDirFactory::directory(std::string prefix) {
  auto dir = std::make_shared<ObjectStoreDirectory>(store_, std::move(prefix), options_);
  std::lock_guard lock(directoriesMutex);
  std::erase_if(directories, [](const auto& handed) { return handed.expired(); });
  directories.push_back(dir);
  return dir;
}

void ObjectStoreDirFactory::ensureMarker(const std::string& key) {
  if (store_->size(key)) return;
  store_->put(key, {});
}

bool ObjectStoreDirFactory::exists(const std::string& prefix, const std::string& marker) {
  return store_->size(prefix + marker) || !store_->list(prefix).empty();
}

void ObjectStoreDirFactory::removeAll(const std::string& prefix) {
  // An upload of a directory under the prefix that landed after the listing
  // below would bring the removed collection back.
  std::vector<std::shared_ptr<ObjectStoreDirectory>> affected;
  {
    std::lock_guard lock(directoriesMutex);
    for (const auto& handed : directories) {
      if (auto dir = handed.lock(); dir && dir->keyPrefix().starts_with(prefix)) affected.push_back(std::move(dir));
    }
  }
  for (const auto& dir : affected) dir->abandon();
  std::vector<std::string> names;
  for (auto& object : store_->list(prefix)) names.push_back(std::move(object.name));
  store_->removeMany(names);
}

std::vector<CollectionId> ObjectStoreDirFactory::collections() {
  std::vector<CollectionId> ids;
  for (const auto& tenant : store_->listDirectories("t/")) {
    for (auto& name : store_->listDirectories("t/" + tenant + "/")) ids.emplace_back(tenant, std::move(name));
  }
  return ids;
}

uint64_t ObjectStoreDirFactory::storageBytes() {
  uint64_t total = 0;
  for (const auto& object : store_->list("t/")) total += object.size;
  return total;
}

uint64_t ObjectStoreDirFactory::collectionBytes(const CollectionId& id) {
  uint64_t total = 0;
  for (const auto& object : store_->list(collectionPrefix(id))) total += object.size;
  return total;
}

std::shared_ptr<Directory> ObjectStoreDirFactory::container(const CollectionId& id, bool create) {
  std::string prefix = collectionPrefix(id);
  if (create) ensureMarker(prefix + std::string(kCollectionMarker));
  else if (!exists(prefix, std::string(kCollectionMarker))) {
    throw std::runtime_error("collection directory does not exist: " + id.label());
  }
  return directory(std::move(prefix));
}

std::shared_ptr<Directory> ObjectStoreDirFactory::incarnation(const CollectionId& id, std::string_view incarnation,
                                                              bool create) {
  std::string prefix = collectionPrefix(id) + std::string(incarnation) + "/";
  if (create) {
    ensureMarker(collectionPrefix(id) + std::string(kCollectionMarker));
    ensureMarker(prefix + std::string(kIncarnationMarker));
  } else if (!exists(prefix, std::string(kIncarnationMarker))) {
    throw std::runtime_error("incarnation directory does not exist: " + id.label() + "/" + std::string(incarnation));
  }
  return directory(std::move(prefix));
}

void ObjectStoreDirFactory::createCollection(const CollectionId& id) {
  std::string prefix = collectionPrefix(id);
  try {
    store_->put(prefix + std::string(kCollectionMarker), {}, true);
  } catch (const std::exception& e) {
    if (alreadyExists(e)) throw std::runtime_error("collection directory already exists");
    throw;
  }
}

std::vector<std::string> ObjectStoreDirFactory::incarnations(const CollectionId& id) {
  return store_->listDirectories(collectionPrefix(id));
}

void ObjectStoreDirFactory::removeIncarnation(const CollectionId& id, std::string_view incarnation) {
  removeAll(collectionPrefix(id) + std::string(incarnation) + "/");
}

void ObjectStoreDirFactory::remove(const CollectionId& id) {
  std::string prefix = collectionPrefix(id);
  store_->remove(prefix + "CURRENT");  // first, as the fs backend does: nothing selects half-removed data
  removeAll(prefix);
}

std::shared_ptr<Directory> ObjectStoreDirFactory::nodeDirectory() { return directory("node/"); }

bool ObjectStoreDirFactory::holdsNoCollections() {
  if (!collections().empty()) return false;
  for (const auto& object : store_->list("node/")) {
    if (object.name != "node/replication.json.pending") return false;
  }
  return true;
}

std::string ObjectStoreDirFactory::describe() const { return "object:" + store_->describe(); }


void registerRamObjectBackend(StorageRegistry& registry) {
  registry.addBackend("ram_object", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
    return std::make_unique<ObjectStoreDirFactory>(RamObjectStore::named(ctx.optionOr("store_name", "default")), ctx);
  });
}

}  // namespace luxir
