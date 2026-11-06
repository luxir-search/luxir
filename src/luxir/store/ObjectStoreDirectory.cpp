// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/ObjectStoreDirectory.h"

#include <algorithm>
#include <functional>
#include <set>
#include <shared_mutex>

#include "luxir/reader/Hotcache.h"
#include "luxir/reader/Postings.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

// One sequential checksum pass over an object: ranged reads of a bounded
// buffer, never the mapping, so verification streams without materializing.
// The handle counts as a reader of the object until it is destroyed, so a
// delete in the meantime leaves the object for it. map() opens the file the
// way the directory does, so a verified open still prefetches the hotcache
// and is counted as a reader.
class ObjectChecksumReader : public ChecksumReader {
  std::shared_ptr<ObjectStore> store;
  std::string key;
  uint64_t size_;
  uint64_t chunkBytes;
  std::function<std::shared_ptr<InputFile>()> mapper;
  std::function<void()> onRelease;

public:
  ObjectChecksumReader(std::shared_ptr<ObjectStore> store, std::string key, uint64_t size, uint64_t chunkBytes,
                       std::function<std::shared_ptr<InputFile>()> mapper, std::function<void()> onRelease)
      : store(std::move(store)), key(std::move(key)), size_(size),
        chunkBytes(std::clamp<uint64_t>(chunkBytes, 4096, 8 << 20)), mapper(std::move(mapper)),
        onRelease(std::move(onRelease)) {}
  ~ObjectChecksumReader() override {
    try { onRelease(); } catch (...) {}
  }

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


ObjectStoreDirectory::ObjectStoreDirectory(std::shared_ptr<ObjectStore> store, std::string prefix, Options options)
    : store(std::move(store)), prefix(std::move(prefix)), options(std::move(options)) {
  if (!this->prefix.empty() && !this->prefix.ends_with('/')) this->prefix.push_back('/');
  readers->store = this->store;
  readers->dir = this;
}

bool ObjectStoreDirectory::Readers::acquire(std::string_view name, bool held) {
  std::lock_guard lock(mutex);
  if (!held && doomed.contains(name)) return false;
  if (auto it = open.find(name); it != open.end()) it->second++;
  else open.emplace(std::string(name), 1);
  return true;
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
      staged = options.account ? std::make_shared<RAMDir>(options.account) : std::make_shared<RAMDir>();
    } else {
      stagedPath = std::filesystem::path(options.stagingDir) / newUuid();
      std::filesystem::create_directories(stagedPath);
      staged = std::make_shared<FSDirectory>(stagedPath);
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
  auto objects = store->list(prefix);
  std::lock_guard lock(readers->mutex);
  for (const auto& object : objects) {
    std::string_view name = std::string_view(object.name).substr(prefix.size());
    if (name.empty() || name.find('/') != std::string_view::npos) continue;
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
  if (auto file = stagedFile(name)) return file;
  return openLazy(name);
}

std::shared_ptr<InputFile> ObjectStoreDirectory::openLazy(std::string_view name) {
  // Count the reader before anything else: a delete that arrives from here on
  // sees it and dooms the object instead of removing it, so the size lookup
  // and the file see the object or (deleted before) nothing at all.
  if (!readers->acquire(name)) return {};
  std::optional<uint64_t> size;
  try {
    size = sizeOf(name);
  } catch (...) {
    readers->release(std::string(name), key(name));
    throw;
  }
  if (!size) {
    readers->release(std::string(name), key(name));
    return {};
  }
  return openCounted(name, *size);
}

std::shared_ptr<InputFile> ObjectStoreDirectory::openCounted(std::string_view name, uint64_t size) {
  std::string objectKey = key(name);
  std::shared_ptr<LazyInputFile> file;
  try {
    file = std::make_shared<LazyInputFile>(store, objectKey, size, options.chunkBytes,
        [readers = readers, name = std::string(name), objectKey] { readers->release(name, objectKey); });
  } catch (...) {
    readers->release(std::string(name), objectKey);
    throw;
  }
  auto data = Hotcache::parseDataFile(name);
  if (!data) {
    // Not segment data (STATE, a commit manifest, live docs, an overlay): it
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
  }
  forget(name);
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

void ObjectStoreDirectory::deleteDurably(std::span<const std::string> names) {
  // An acknowledged remove is durable.
  for (const auto& name : names) deleteFile(name);
}

std::optional<Directory::PublicationVersion> ObjectStoreDirectory::publishFile(
    std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) {
  forget(name);  // the name is about to hold another object
  ObjectStore::PutCondition put;
  put.ifAbsent = condition.mode == PublishMode::CREATE_ONLY;
  put.ifMatch = std::move(condition.version);
  auto version = store->put(key(name), std::string_view((const char*)bytes.data(), bytes.size()), put);
  if (!version) return std::nullopt;
  {
    std::lock_guard lock(readers->mutex);
    readers->doomed.erase(name);  // the name is reused: the new object wins
  }
  std::shared_ptr<Directory> dir;
  {
    std::lock_guard lock(mutex);
    dir = staged;
  }
  if (dir) dir->deleteFile(name);  // a staged copy would shadow the published bytes
  return version;
}

std::optional<Directory::Publication> ObjectStoreDirectory::readPublished(std::string_view name) {
  if (auto object = store->get(key(name))) return Publication{std::move(object->bytes), std::move(object->version)};
  return std::nullopt;
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
  // The handle is a reader, counted before the size lookup as openLazy does:
  // a delete from here on dooms the object, which stays until the handle and
  // every file it mapped are gone.
  if (!readers->acquire(name)) return {};
  auto release = [readers = readers, name = std::string(name), objectKey] { readers->release(name, objectKey); };
  std::optional<uint64_t> size;
  try {
    size = sizeOf(name);
  } catch (...) {
    release();
    throw;
  }
  if (!size) {
    release();
    return {};
  }
  // The handle may outlive this directory: map() takes the shared owner lock
  // and, with the directory gone, maps the object without its hotcache.
  auto mapper = [readers = readers, store = store, name = std::string(name), objectKey, size = *size,
                 chunkBytes = options.chunkBytes]() -> std::shared_ptr<InputFile> {
    readers->acquire(name, true);  // this handle holds it, deleted or not
    std::shared_lock lock(readers->owner);
    if (readers->dir) return readers->dir->openCounted(name, size);
    try {
      return std::make_shared<LazyInputFile>(store, objectKey, size, chunkBytes,
                                             [readers, name, objectKey] { readers->release(name, objectKey); });
    } catch (...) {
      readers->release(name, objectKey);
      throw;
    }
  };
  return std::make_shared<ObjectChecksumReader>(store, objectKey, *size, options.chunkBytes, std::move(mapper),
                                                std::move(release));
}


ObjectStoreDirFactory::ObjectStoreDirFactory(std::shared_ptr<ObjectStore> store, ObjectStoreDirectory::Options options)
    : store_(std::move(store)), options_(std::move(options)) {
  if (!store_) throw std::invalid_argument("object store backend needs an ObjectStore");
  if (options_.chunkBytes == 0) throw std::invalid_argument("object store chunk size must be positive");
}

namespace {
ObjectStoreDirectory::Options optionsOf(const StorageContext& ctx) {
  ObjectStoreDirectory::Options options;
  options.chunkBytes = ctx.params.getBytes("chunk_bytes", options.chunkBytes);
  if (options.chunkBytes == 0) ctx.params.fail("chunk_bytes", "must be positive");
  options.stagingDir = std::string(ctx.params.getString("staging_dir"));
  options.spillBytes = ctx.params.getBytes("spill_bytes", 0);
  return options;
}
}  // namespace

ObjectStoreDirFactory::ObjectStoreDirFactory(std::shared_ptr<ObjectStore> store, const StorageContext& ctx)
    : ObjectStoreDirFactory(std::move(store), optionsOf(ctx)) {}

std::string ObjectStoreDirFactory::prefixOf(StorageRoot root, std::string_view path) {
  std::string key = checkedPath(root, path);
  std::string prefix = root == StorageRoot::NODE_PRIVATE ? "node/" : "";
  return key.empty() ? prefix : prefix + key + "/";
}

std::shared_ptr<Directory> ObjectStoreDirFactory::directory(StorageRoot root, std::string_view path,
                                                            ViewOptions viewOptions) {
  std::string prefix = prefixOf(root, path);
  std::lock_guard lock(mutex);
  std::erase_if(views, [](const auto& view) { return view.second.expired(); });
  if (auto it = views.find(prefix); it != views.end()) {
    if (auto live = it->second.lock()) return live;
  }
  auto options = options_;
  options.account = std::move(viewOptions.account);
  auto dir = std::make_shared<ObjectStoreDirectory>(store_, prefix, std::move(options));
  views.insert_or_assign(prefix, dir);
  return dir;
}

std::vector<DirectoryFactory::Child> ObjectStoreDirFactory::children(StorageRoot root, std::string_view path) {
  std::string prefix = prefixOf(root, path);
  std::map<std::string, bool, std::less<>> found;
  for (const auto& object : store_->list(prefix)) {
    std::string_view rest = std::string_view(object.name).substr(prefix.size());
    auto slash = rest.find('/');
    if (slash == std::string_view::npos) {
      if (!rest.empty()) found.emplace(std::string(rest), false);
      continue;
    }
    // The authoritative root does not show the node-private one.
    std::string_view name = rest.substr(0, slash);
    if (name.empty() || (root == StorageRoot::AUTHORITATIVE && prefix.empty() && name == "node")) continue;
    found[std::string(name)] = true;
  }
  std::vector<Child> result;
  for (auto& [name, isDirectory] : found) result.push_back({name, isDirectory});
  return result;
}

void ObjectStoreDirFactory::removeTree(StorageRoot root, std::string_view path) {
  if (checkedPath(root, path).empty()) throw std::invalid_argument("cannot remove a storage root");
  std::string prefix = prefixOf(root, path);
  std::vector<std::shared_ptr<ObjectStoreDirectory>> live;
  {
    std::lock_guard lock(mutex);
    for (auto& [viewPrefix, view] : views) {
      if (!viewPrefix.starts_with(prefix)) continue;
      if (auto dir = view.lock()) live.push_back(std::move(dir));
    }
  }
  // Views still held forget what they cached and staged.
  for (const auto& dir : live) dir->clear();
  for (const auto& object : store_->list(prefix)) store_->remove(object.name);
}

std::string ObjectStoreDirFactory::describe() const { return "object:" + store_->describe(); }


void registerRamObjectBackend(StorageRegistry& registry) {
  registry.addBackend("ram_object", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
    return std::make_unique<ObjectStoreDirFactory>(RamObjectStore::named(ctx.params.getString("store_name", "default")), ctx);
  });
}

}  // namespace luxir
