// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/ObjectStoreDirectory.h"

#include <algorithm>
#include <set>

#include "luxir/reader/Hotcache.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

// Marker objects make an empty "directory" exist. They are hidden from listings.
constexpr std::string_view kCollectionMarker = ".collection";
constexpr std::string_view kIncarnationMarker = ".dir";

bool isHidden(std::string_view name) { return name.starts_with('.'); }

bool alreadyExists(const std::exception& e) {
  return std::string_view(e.what()).find("already exists") != std::string_view::npos;
}

// One sequential checksum pass over an object: ranged reads of a bounded
// buffer, never the mapping, so verification streams without materializing.
class ObjectChecksumReader : public ChecksumReader {
  std::shared_ptr<ObjectStore> store;
  std::string key;
  uint64_t size_;
  uint64_t chunkBytes;

public:
  ObjectChecksumReader(std::shared_ptr<ObjectStore> store, std::string key, uint64_t size, uint64_t chunkBytes)
      : store(std::move(store)), key(std::move(key)), size_(size),
        chunkBytes(std::clamp<uint64_t>(chunkBytes, 4096, 8 << 20)) {}

  uint64_t size() const override { return size_; }

  std::shared_ptr<InputFile> map() override {
    return std::make_shared<LazyInputFile>(store, key, size_, chunkBytes);
  }

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


ObjectStoreDirectory::ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix, Options options)
    : store(std::move(store)), prefix(std::move(prefix)), options(std::move(options)) {
  if (!this->prefix.empty() && !this->prefix.ends_with('/')) this->prefix.push_back('/');
  readers->store = this->store;
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
  std::lock_guard lock(mutex);
  auto it = hot.find(segStr);
  if (it != hot.end()) return it->second;
  std::shared_ptr<const Hotcache> cache;
  try {
    std::string fileName = Hotcache::fileName(segStr);
    std::string name = key(fileName);
    auto knownSize = known.find(fileName);
    auto size = knownSize != known.end() ? std::optional(knownSize->second) : store->size(name);
    if (size) {
      std::string bytes((size_t)*size, '\0');
      store->readRange(name, 0, std::span<char>(bytes.data(), bytes.size()));
      cache = std::make_shared<const Hotcache>(Hotcache::decode(bytes));
    }
  } catch (const std::exception& e) {
    // An optimization only: without it the segment opens with small requests.
    LOG_WARN("ignoring hotcache of segment {}: {}", segStr, e.what());
  }
  hot.emplace(std::string(segStr), cache);
  return cache;
}

void ObjectStoreDirectory::listFiles(std::vector<FileInfo>& target) {
  auto objects = store->list(prefix);
  std::lock_guard lock(readers->mutex);
  for (const auto& object : objects) {
    std::string_view name = std::string_view(object.name).substr(prefix.size());
    if (name.empty() || isHidden(name) || name.find('/') != std::string_view::npos) continue;
    if (readers->doomed.contains(name)) continue;
    target.push_back({std::string(name), object.size});
  }
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
      if (!hotFiles.contains(Hotcache::fileName(data->segStr))) hot.try_emplace(std::string(data->segStr), nullptr);
    }
  }
}

std::shared_ptr<InputFile> ObjectStoreDirectory::openFile(std::string_view name, bool expectSynced) {
  unused(expectSynced);
  if (auto file = stagedFile(name)) return file;
  return openLazy(name);
}

std::shared_ptr<InputFile> ObjectStoreDirectory::openLazy(std::string_view name) {
  std::string objectKey = key(name);
  {
    std::lock_guard lock(readers->mutex);
    if (readers->doomed.contains(name)) return {};
  }
  auto size = sizeOf(name);
  if (!size) return {};
  auto data = Hotcache::parseDataFile(name);
  // Count the reader before it can be handed out; a failed construction uncounts it.
  {
    std::lock_guard lock(readers->mutex);
    readers->open[std::string(name)]++;
  }
  std::shared_ptr<LazyInputFile> file;
  try {
    file = std::make_shared<LazyInputFile>(store, objectKey, *size, options.chunkBytes,
        [readers = readers, name = std::string(name), objectKey] { readers->release(name, objectKey); });
  } catch (...) {
    readers->release(std::string(name), objectKey);
    throw;
  }
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

bool ObjectStoreDirectory::deleteFile(std::string_view name) {
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
    if (name.ends_with("__hot")) hot.clear();
    if (auto it = known.find(name); it != known.end()) known.erase(it);
  }
  bool local = dir && dir->deleteFile(name);
  {
    std::lock_guard lock(readers->mutex);
    auto it = readers->open.find(name);
    if (it != readers->open.end() && it->second > 0) {
      // A reader is mid-flight: the last one to finish removes the object.
      return readers->doomed.emplace(name).second || local;
    }
  }
  return store->remove(key(name)) || local;
}

void ObjectStoreDirectory::finishFile(File& file) {
  Directory& dir = staging();
  dir.finishFile(file);
  forget(file.name());  // the name is reused: its object changes
  {
    std::lock_guard lock(readers->mutex);
    readers->doomed.erase(file.name());  // the name is reused: the new object wins
  }
  auto input = dir.openFile(file.name());
  if (!input) throw std::runtime_error("staged file vanished: " + file.name());
  store->put(key(file.name()), input->read());
}

void ObjectStoreDirectory::renameFile(std::string_view from, std::string_view to) {
  forget(from);
  forget(to);
  store->copy(key(from), key(to));
  store->remove(key(from));
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  if (dir) {
    dir->deleteFile(to);
    if (dir->openFile(from)) dir->renameFile(from, to);
  }
}

void ObjectStoreDirectory::clear() {
  std::vector<FileInfo> files;
  listFiles(files);
  for (const auto& file : files) store->remove(key(file.name));
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
    hot.clear();
    known.clear();
  }
  if (dir) dir->clear();
}

void ObjectStoreDirectory::linkFile(Directory& source, std::string_view name) {
  auto* other = dynamic_cast<ObjectStoreDirectory*>(&source.underlying());
  if (!other || other->store != store) {
    copyFile(source, name);
    return;
  }
  forget(name);
  store->copy(other->key(name), key(name));
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  if (dir) dir->deleteFile(name);
}

std::shared_ptr<ChecksumReader> ObjectStoreDirectory::openChecksum(std::string_view name, bool direct) {
  unused(direct);
  std::string objectKey = key(name);
  auto size = store->size(objectKey);
  if (!size) return {};
  return std::make_shared<ObjectChecksumReader>(store, std::move(objectKey), *size, options.chunkBytes);
}


ObjectStoreDirFactory::ObjectStoreDirFactory(std::shared_ptr<ObjectStore> store, const StorageContext& ctx)
    : store_(std::move(store)) {
  if (!store_) throw std::invalid_argument("object store backend needs an ObjectStore");
  options_.chunkBytes = ctx.optionBytes("chunk_bytes", options_.chunkBytes);
  if (options_.chunkBytes == 0) throw std::invalid_argument("store option 'chunk_bytes' must be positive");
  options_.stagingDir = ctx.optionOr("staging_dir", "");
  options_.spillBytes = ctx.optionBytes("spill_bytes", 0);
}

std::shared_ptr<Directory> ObjectStoreDirFactory::directory(std::string prefix) const {
  return std::make_shared<ObjectStoreDirectory>(store_, std::move(prefix), options_);
}

void ObjectStoreDirFactory::ensureMarker(const std::string& key) {
  if (store_->size(key)) return;
  store_->put(key, {});
}

bool ObjectStoreDirFactory::exists(const std::string& prefix, const std::string& marker) {
  return store_->size(prefix + marker) || !store_->list(prefix).empty();
}

void ObjectStoreDirFactory::removeAll(const std::string& prefix) {
  for (const auto& object : store_->list(prefix)) store_->remove(object.name);
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
