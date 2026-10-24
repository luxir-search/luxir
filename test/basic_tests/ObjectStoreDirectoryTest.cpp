// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <future>

#include <gtest/gtest.h>

#include "luxir/plugin/Registries.h"
#include "luxir/reader/Hotcache.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/ObjectStoreDirectory.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"

// The Directory contract itself runs over ObjectStoreDirectory in DirectoryTest
// (objectStoreDir*); these tests cover what is specific to an object store.

using namespace luxir;

namespace {

void finish(Directory& dir, const std::string& name, const std::string& bytes) {
  auto file = dir.createFile(name);
  OutputStream out(file.get());
  out.writeBytes(bytes);
  out.close();
  dir.finishFile(*file);
}

// A written and synced file, as a commit leaves it: in the store.
void write(Directory& dir, const std::string& name, const std::string& bytes) {
  finish(dir, name, bytes);
  std::array<std::string, 1> names{name};
  dir.sync(names);
}

std::string readAll(Directory& dir, const std::string& name) {
  auto file = dir.openFile(name);
  return file ? std::string(file->read()) : std::string("<absent>");
}

ObjectStoreDirectory::Options chunks(uint64_t bytes) {
  ObjectStoreDirectory::Options options;
  options.chunkBytes = bytes;
  return options;
}

class ObjectStoreDirectoryTest : public ::testing::Test {
protected:
  std::shared_ptr<RamObjectStore> store = std::make_shared<RamObjectStore>();
};

}  // namespace

TEST_F(ObjectStoreDirectoryTest, capabilitiesDescribeARemoteStore) {
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  auto caps = dir.capabilities();
  EXPECT_TRUE(caps & Directory::REMOTE);
  EXPECT_TRUE(caps & Directory::ATOMIC_RENAME);
  EXPECT_TRUE(caps & Directory::DURABLE_SYNC);
  EXPECT_FALSE(caps & Directory::HARD_LINK);
  EXPECT_FALSE(caps & Directory::MEMORY_MAPPED);
}

TEST_F(ObjectStoreDirectoryTest, aWrittenFileIsUploadedOnceAndReadFromTheStagedCopy) {
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  write(dir, "s01_00", "hello object store");
  EXPECT_EQ(1u, store->requests("put", "d/s01_00"));
  EXPECT_EQ("hello object store", readAll(dir, "s01_00"));
  std::array<std::string, 2> names{"s01_00", "."};
  dir.sync(names);  // an acknowledged put is durable
  EXPECT_EQ(0u, store->requests("read"));
  EXPECT_FALSE(dir.openFile("s01_00")->lazy());

  // Another handle on the same prefix has no staged copy: it reads lazily.
  ObjectStoreDirectory other(store, "d/", chunks(8));
  auto file = other.openFile("s01_00");
  ASSERT_NE(nullptr, file);
  EXPECT_TRUE(file->lazy());
  EXPECT_EQ(0u, store->requests("read"));  // opening a file fetches nothing
  EXPECT_EQ(18u, file->size());
  auto is = file->rangeStream(6, 6);
  EXPECT_EQ("object", std::string(is.ptr(), 6));
  EXPECT_EQ(1u, store->requests("read"));
  EXPECT_EQ(16u, store->bytesRead());  // the two covering chunks, in one request
}

TEST_F(ObjectStoreDirectoryTest, renameReplacesTheTargetAtomically) {
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  write(dir, "CURRENT", "one");
  ObjectStoreDirectory reading(store, "d/", chunks(8));
  auto before = reading.openFile("CURRENT");  // a reader of the old bytes
  write(dir, "CURRENT.pending", "two");
  dir.renameFile("CURRENT.pending", "CURRENT");
  EXPECT_EQ("two", readAll(dir, "CURRENT"));
  ObjectStoreDirectory fresh(store, "d/");
  EXPECT_EQ("two", readAll(fresh, "CURRENT"));
  EXPECT_EQ("<absent>", readAll(dir, "CURRENT.pending"));
  EXPECT_FALSE(store->size("d/CURRENT.pending"));
  EXPECT_EQ(0u, store->requests("put", "d/CURRENT.pending"));  // a pending file goes up under its final name
  EXPECT_EQ(0u, store->requests("copy"));
  EXPECT_EQ("one", before->read());  // a file that is not segment data is read whole when opened
  EXPECT_THROW(dir.renameFile("missing", "x"), std::runtime_error);

  // A file that is in the store already is renamed server side.
  write(dir, "x", "three");
  store->resetStats();
  dir.renameFile("x", "y");
  EXPECT_EQ(1u, store->requests("copy"));
  EXPECT_EQ("three", readAll(fresh, "y"));
  EXPECT_EQ("<absent>", readAll(dir, "x"));
}

TEST_F(ObjectStoreDirectoryTest, linkIsAServerSideCopyAndReuseFallsBackToCopyingAcrossStores) {
  ObjectStoreDirectory source(store, "src/", chunks(8)), target(store, "dst/", chunks(8));
  write(source, "f", "shared bytes");
  store->resetStats();
  target.linkFile(source, "f");
  EXPECT_EQ(1u, store->requests("copy"));
  EXPECT_EQ(0u, store->requests("read"));
  EXPECT_EQ("shared bytes", readAll(target, "f"));

  // Across stores there is nothing to copy server side: the bytes are streamed.
  auto elsewhere = std::make_shared<RamObjectStore>();
  ObjectStoreDirectory remote(elsewhere, "dst/", chunks(8));
  remote.linkFile(source, "f");
  EXPECT_EQ("shared bytes", readAll(remote, "f"));

  // reuseFiles installs wanted, unchanged files; without HARD_LINK it copies them.
  FileDescriptor f{"f", 12, source.checksum("f")->xxh3};
  ObjectStoreDirectory reused(store, "reuse/", chunks(8));
  std::vector<FileDescriptor> files{f};
  auto result = reused.reuseFiles(source, files, files);
  EXPECT_EQ(files, result);
  EXPECT_EQ("shared bytes", readAll(reused, "f"));
}

TEST_F(ObjectStoreDirectoryTest, listingHidesMarkersAndOtherDirectories) {
  ObjectStoreDirectory dir(store, "a/", chunks(8));
  write(dir, "f1", "1");
  write(dir, "f2", "22");
  store->put("a/.dir", "");
  store->put("a/sub/f3", "333");
  store->put("ab/f4", "4444");  // a different directory whose name starts the same
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  ASSERT_EQ(2u, files.size());
  EXPECT_EQ("f1", files[0].name);
  EXPECT_EQ(2u, files[1].size);
  EXPECT_EQ(3u, dir.totalBytes());

  dir.clear();
  files.clear();
  dir.listFiles(files);
  EXPECT_TRUE(files.empty());
  EXPECT_TRUE(store->size("a/.dir"));  // the directory itself stays
  EXPECT_TRUE(store->size("a/sub/f3"));
  EXPECT_FALSE(dir.openFile("f1"));
}

TEST_F(ObjectStoreDirectoryTest, deletingAFileAReaderHoldsWaitsForTheReader) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  write(writer, "s01_00", std::string(100, 'x'));  // segment-data shaped: read lazily
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  auto reader = dir.openFile("s01_00");
  ASSERT_TRUE(reader->lazy());

  EXPECT_TRUE(dir.deleteFile("s01_00"));
  EXPECT_FALSE(dir.openFile("s01_00"));  // gone for everyone new...
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  EXPECT_TRUE(files.empty());
  EXPECT_TRUE(store->size("d/s01_00"));  // ...but the bytes stay until the reader is done
  auto is = reader->rangeStream(50, 10);
  EXPECT_EQ(std::string(10, 'x'), std::string(is.ptr(), 10));

  reader.reset();
  EXPECT_FALSE(store->size("d/s01_00"));

  // With no reader the delete is immediate; a file that is not there is not deleted.
  write(writer, "s01_01", "y");
  EXPECT_TRUE(dir.deleteFile("s01_01"));
  EXPECT_FALSE(store->size("d/s01_01"));
  EXPECT_FALSE(dir.deleteFile("s01_01"));
}

TEST_F(ObjectStoreDirectoryTest, checksumStreamsRangesWithoutMaterializing) {
  RAMDir ram;
  std::string bytes(10000, 'q');
  for (size_t i = 0; i < bytes.size(); i++) bytes[i] = (char)('a' + i % 26);
  write(ram, "f", bytes);
  ObjectStoreDirectory dir(store, "d/", chunks(4096));
  write(dir, "f", bytes);

  store->resetStats();
  size_t seen = 0;
  auto check = dir.checksum("f", [&](size_t n) { seen += n; });
  ASSERT_TRUE(check);
  EXPECT_EQ(ram.checksum("f")->xxh3, check->xxh3);
  EXPECT_EQ(bytes.size(), check->size);
  EXPECT_EQ(bytes.size(), seen);
  EXPECT_EQ(3u, store->requests("read"));  // 4096 + 4096 + 1808
  for (const auto& r : store->requestLog()) EXPECT_LE(r.length, 4096u);
  EXPECT_FALSE(dir.checksum("absent"));

  // map() hands out a lazy file for the same object.
  auto reader = dir.openChecksum("f");
  EXPECT_EQ(bytes.size(), reader->size());
  EXPECT_EQ(bytes, reader->map()->read());
}

TEST_F(ObjectStoreDirectoryTest, factoryLaysOutKeysLikeTheFsBackend) {
  LuxirConfig config;
  config.store.options = {{"chunk_bytes", "16K"}};
  StorageContext ctx{config, config.store.options, {}};
  ObjectStoreDirFactory factory(store, ctx);
  EXPECT_EQ("object:ram_object:ram", factory.describe());
  EXPECT_TRUE(factory.holdsNoCollections());
  EXPECT_TRUE(factory.collections().empty());

  auto id = CollectionId::of("main");
  auto storage = factory.collection(id);
  EXPECT_THROW(storage.open("i1"), std::runtime_error);
  EXPECT_THROW(storage.current(), std::runtime_error);
  EXPECT_TRUE(factory.collections().empty());

  auto dir = storage.create("i1");
  write(*dir, "data", "bytes");
  EXPECT_TRUE(store->size("t/t0/main/i1/data"));
  EXPECT_EQ(std::vector<CollectionId>{id}, factory.collections());
  EXPECT_EQ(std::vector<std::string>{"i1"}, storage.incarnations());
  EXPECT_FALSE(factory.holdsNoCollections());
  EXPECT_FALSE(storage.current());
  EXPECT_FALSE(storage.hasFiles());
  storage.select("i1");
  EXPECT_EQ("i1", storage.current());
  EXPECT_TRUE(storage.hasFiles());
  EXPECT_TRUE(store->size("t/t0/main/CURRENT"));
  EXPECT_EQ("bytes", readAll(*storage.open("i1"), "data"));
  EXPECT_EQ(5u, factory.collectionBytes(id) - store->storedBytes("t/t0/main/CURRENT"));
  EXPECT_GE(factory.storageBytes(), 5u);

  // Creation is exclusive, as a directory creation is.
  EXPECT_THROW(storage.createExclusive(), std::runtime_error);
  EXPECT_NO_THROW(factory.collection(CollectionId{"t1", "other"}).createExclusive());
  EXPECT_EQ(2u, factory.collections().size());

  storage.create("i2");
  EXPECT_EQ((std::vector<std::string>{"i1", "i2"}), storage.incarnations());
  storage.retainOnly("i1");
  EXPECT_EQ(std::vector<std::string>{"i1"}, storage.incarnations());
  EXPECT_THROW(storage.open("i2"), std::runtime_error);
  storage.remove();
  ASSERT_EQ(1u, factory.collections().size());
  EXPECT_EQ("other", factory.collections()[0].name);
  EXPECT_TRUE(store->list("t/t0/").empty());
}

TEST_F(ObjectStoreDirectoryTest, nodeFilesLiveUnderNodeAndAPendingFileIsNotACollection) {
  LuxirConfig config;
  StorageContext ctx{config, config.store.options, {}};
  ObjectStoreDirFactory factory(store, ctx);
  auto node = factory.nodeDirectory();
  write(*node, "replication.json.pending", "{}");
  EXPECT_FALSE(store->size("node/replication.json.pending"));  // uploaded by its rename
  store->put("node/replication.json.pending", "{}");  // left by a crash
  EXPECT_TRUE(factory.holdsNoCollections());
  write(*node, "replication.json", "{}");
  EXPECT_EQ("{}", readAll(*factory.nodeDirectory(), "replication.json"));
  EXPECT_FALSE(factory.holdsNoCollections());
}

TEST_F(ObjectStoreDirectoryTest, conditionalPutSignalsAConflict) {
  store->put("k", "1", true);
  try {
    store->put("k", "2", true);
    FAIL() << "a conditional create replaced an object";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("already exists"));
  }
  store->put("k", "3");  // an unconditional put replaces
  std::string out(1, '\0');
  store->readRange("k", 0, out);
  EXPECT_EQ("3", out);
  std::string tooMuch(5, '\0');
  EXPECT_THROW(store->readRange("k", 0, tooMuch), std::runtime_error);
  EXPECT_THROW(store->copy("absent", "x"), std::runtime_error);
  EXPECT_EQ((std::vector<std::string>{"b", "c"}), [&] {
    store->put("p/b/1", ""); store->put("p/c/1", ""); store->put("p/c/2", ""); store->put("p/loose", "");
    return store->listDirectories("p/");
  }());
}

TEST(ObjectStoreBackendTest, ramObjectIsRegisteredAndReadsItsOptions) {
  initPlugins(addConfiguredPlugins);
  auto& storage = Registries::global().storage;
  auto names = storage.backendNames();
  EXPECT_NE(names.end(), std::ranges::find(names, std::string("ram_object")));

  LuxirConfig config;
  config.store.options = {{"store_name", "backend_test"}, {"chunk_bytes", "2K"}};
  StorageContext ctx{config, config.store.options, {}};
  auto factory = storage.makeBackend("ram_object", ctx);
  auto* objects = dynamic_cast<ObjectStoreDirFactory*>(factory.get());
  ASSERT_NE(nullptr, objects);
  EXPECT_EQ(RamObjectStore::named("backend_test").get(), &objects->store());
  EXPECT_EQ("object:ram_object:backend_test", factory->describe());

  config.store.options = {{"chunk_bytes", "0"}};
  EXPECT_THROW(storage.makeBackend("ram_object", ctx), std::invalid_argument);
  config.store.options = {{"chunk_bytes", "lots"}};
  EXPECT_THROW(storage.makeBackend("ram_object", ctx), std::invalid_argument);
  RamObjectStore::drop("backend_test");
}

// Files a loaded manifest lists open from their known sizes: no size request,
// and a segment listed without a hotcache is not probed for one.
TEST_F(ObjectStoreDirectoryTest, knownFilesOpenWithoutSizeRequests) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  Hotcache cache;
  cache.entries.push_back({0, {0, 8}});
  std::string hot = cache.encode();
  write(writer, "s01_00", "first segment");
  write(writer, "s01__hot", hot);
  write(writer, "s02_00", "second segment");
  std::vector<FileDescriptor> files{{"s01_00", 13, 0}, {"s01__hot", hot.size(), 0}, {"s02_00", 14, 0}};

  ObjectStoreDirectory dir(store, "d/", chunks(8));
  dir.knownFiles(files);
  store->resetStats();
  auto first = dir.openFile("s01_00");
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(13u, first->size());
  auto second = dir.openFile("s02_00");
  ASSERT_NE(nullptr, second);
  EXPECT_EQ(0u, store->requests("size"));
  EXPECT_EQ(1u, store->requests("read", "d/s01__hot"));  // the hotcache, then its range of s01_00
  EXPECT_EQ(1u, store->requests("read", "d/s01_00"));
  EXPECT_EQ(2u, store->requests());
  EXPECT_EQ("second segment", second->read());
}

// A known size stands only while the name holds the same object.
TEST_F(ObjectStoreDirectoryTest, aKnownFileIsForgottenWhenDeletedOrReplaced) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  write(writer, "s01_00", "segment");
  write(writer, "s01_01", "other");
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  std::vector<FileDescriptor> files{{"s01_00", 7, 0}, {"s01_01", 5, 0}};
  dir.knownFiles(files);

  EXPECT_TRUE(dir.deleteFile("s01_00"));
  EXPECT_EQ(nullptr, dir.openFile("s01_00"));  // asks the store, which has no such object

  write(writer, "s01_02", "replacement");
  dir.renameFile("s01_02", "s01_01");
  auto renamed = dir.openFile("s01_01");
  ASSERT_NE(nullptr, renamed);
  EXPECT_EQ("replacement", renamed->read());
}

// Opening a collection loads its commit manifest, which lists every committed
// file with its size: the reopened collection opens its segments without a
// single size request.
TEST(ObjectStoreBackendTest, aReopenedCollectionOpensItsFilesWithoutSizeRequests) {
  using namespace luxir::test;
  std::string name = "reopen_" + newUuid();
  LuxirConfig config;
  config.store.backend = "ram_object";
  config.store.options = {{"store_name", name}};
  auto store = RamObjectStore::named(name);
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "text_un", "alpha"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(h.indexAll({flatdoc("id", "c", "text_un", "beta")}, UpdateMessage::COMMIT).success);
  }
  store->resetStats();
  {
    LuxirNode node(config);
    auto collection = node.getCollection("main");
    EXPECT_EQ(3, collection->getReaderManager().getReader()->liveDocs());
    std::string key = "t/t0/main/" + collection->getShard()->getSnapshots().snapshot()->id.incarnation + "/";
    EXPECT_GT(store->requests("read", key + "s"), 0u);   // the segments were opened...
    EXPECT_EQ(0u, store->requests("size", key + "s"));  // ...from the sizes the manifest lists
  }
  store.reset();
  RamObjectStore::drop(name);
}

namespace {

// Holds a RamObjectStore's puts in flight until released, as a slow network would.
class HeldPuts {
  RamObjectStore& store;
  std::mutex mutex;
  std::condition_variable changed;
  bool held = true;
  int waiting = 0;

public:
  explicit HeldPuts(RamObjectStore& store) : store(store) {
    store.interceptRequests([this](std::string_view op, std::string_view) {
      if (op != "put") return;
      std::unique_lock lock(mutex);
      waiting++;
      changed.notify_all();
      changed.wait(lock, [&] { return !held; });
      waiting--;
    });
  }
  ~HeldPuts() {
    release();
    store.interceptRequests(nullptr);
  }
  void awaitHeld(int puts) {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return waiting >= puts; });
  }
  void release() {
    std::lock_guard lock(mutex);
    held = false;
    changed.notify_all();
  }
};

void syncOne(Directory& dir, const std::string& name) {
  std::array<std::string, 1> names{name};
  dir.sync(names);
}

std::vector<std::byte> rootBytes(std::string_view text) {
  auto data = (const std::byte*)text.data();
  return {data, data + text.size()};
}

}  // namespace

// A finished file is on its way to the store at once, and sync is what waits
// for it: a flush does not wait for the network, a commit does.
TEST_F(ObjectStoreDirectoryTest, anUploadStartsWhenAFileIsFinishedAndSyncWaitsForIt) {
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  HeldPuts held(*store);
  finish(dir, "s01_00", "segment bytes");
  held.awaitHeld(1);
  EXPECT_EQ("segment bytes", readAll(dir, "s01_00"));  // the staged copy
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  ASSERT_EQ(1u, files.size());
  EXPECT_EQ(13u, files[0].size);

  auto synced = std::async(std::launch::async, [&] { syncOne(dir, "s01_00"); });
  EXPECT_EQ(std::future_status::timeout, synced.wait_for(std::chrono::milliseconds(50)));
  EXPECT_FALSE(store->size("d/s01_00"));
  held.release();
  synced.get();
  EXPECT_TRUE(store->size("d/s01_00"));
  EXPECT_EQ(1u, store->requests("put", "d/s01_00"));
}

// Background attempts that fail leave the file staged; sync tries once more
// and reports the failure, so a commit never names a file the store lacks,
// and the next sync after the outage goes through.
TEST_F(ObjectStoreDirectoryTest, aFailedUploadFailsSyncUntilAnAttemptSucceeds) {
  auto options = chunks(8);
  options.uploadAttempts = 1;
  ObjectStoreDirectory dir(store, "d/", options);
  std::atomic<bool> down{true};
  store->interceptRequests([&](std::string_view op, std::string_view) {
    if (op == "put" && down) throw std::runtime_error("network down");
  });
  finish(dir, "s01_00", "bytes");
  for (int i = 0; i < 2; i++) {
    try {
      syncOne(dir, "s01_00");
      FAIL() << "sync of a file that is not in the store returned";
    } catch (const std::runtime_error& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("network down")) << e.what();
    }
  }
  EXPECT_FALSE(store->size("d/s01_00"));
  EXPECT_EQ("bytes", readAll(dir, "s01_00"));  // still staged
  down = false;
  syncOne(dir, "s01_00");
  EXPECT_TRUE(store->size("d/s01_00"));
  store->interceptRequests(nullptr);
}

// A segment merged away before its upload started is never sent, and its
// delete costs no request.
TEST_F(ObjectStoreDirectoryTest, aFileDeletedBeforeItsUploadStartsIsNeverSent) {
  auto options = chunks(8);
  options.workerThreads = 1;
  ObjectStoreDirectory dir(store, "d/", options);
  {
    HeldPuts held(*store);
    finish(dir, "s01_00", "first");  // occupies the only worker
    held.awaitHeld(1);
    finish(dir, "s02_00", "merged away");
    EXPECT_TRUE(dir.deleteFile("s02_00"));
    EXPECT_EQ("<absent>", readAll(dir, "s02_00"));
  }
  syncOne(dir, "s01_00");
  syncOne(dir, "s02_00");  // nothing to wait for
  EXPECT_EQ(0u, store->requests("put", "d/s02_00"));
  EXPECT_EQ(0u, store->requests("remove"));
  EXPECT_FALSE(store->size("d/s02_00"));
}

// A commit root goes up with one conditional put under its final name. The
// same root again (a retried commit) is fine, another writer's root for the
// generation is never replaced, and a damaged one is, as a repair does.
TEST_F(ObjectStoreDirectoryTest, aCommitRootIsCreatedOnceAndNeverReplacedByAnother) {
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  auto publish = [&](uint64_t gen, std::string_view text) {
    Manifest::write(dir, Manifest::name(gen) + ".pending", rootBytes(text));
    syncOne(dir, Manifest::name(gen) + ".pending");
    dir.renameFile(Manifest::name(gen) + ".pending", Manifest::name(gen));
  };
  publish(5, "root five");
  EXPECT_EQ(1u, store->requests("put"));
  EXPECT_EQ(0u, store->requests("copy"));
  EXPECT_EQ(0u, store->requests("remove"));
  auto published = readAll(dir, "commit_5");
  EXPECT_TRUE(Manifest::intact(published));

  publish(5, "root five");
  try {
    publish(5, "another writer's five");
    FAIL() << "a commit root was replaced";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("another writer")) << e.what();
  }
  ObjectStoreDirectory fresh(store, "d/", chunks(8));
  EXPECT_EQ(published, readAll(fresh, "commit_5"));
  // The commit that lost cleans up after itself, but never removes the root
  // it lost to.
  EXPECT_FALSE(dir.deleteFile("commit_5"));
  EXPECT_EQ(published, readAll(fresh, "commit_5"));

  store->put("d/commit_6", "torn");
  publish(6, "root six");
  EXPECT_TRUE(Manifest::intact(readAll(fresh, "commit_6")));
}

// Removals are queued and sent in batches, and a directory that has listed
// its objects retires a segment without listing again.
TEST_F(ObjectStoreDirectoryTest, removalsAreBatchedAndRetiringASegmentListsNothing) {
  {
    ObjectStoreDirectory dir(store, "d/", chunks(8));
    for (const char* name : {"s01_00", "s01_01", "s01__hot", "s02_00"}) write(dir, name, name);
    std::vector<Directory::FileInfo> files;
    dir.listFiles(files);  // as opening an index does
    store->resetStats();
    dir.deletePrefix("s01_");
    EXPECT_TRUE(dir.deleteFile("s02_00"));
    EXPECT_EQ(nullptr, dir.openFile("s01_00"));  // gone for this directory at once
    EXPECT_EQ(nullptr, dir.openFile("s02_00"));
    EXPECT_EQ(0u, store->requests("list"));
  }  // a closing directory sends what is still queued
  EXPECT_EQ(0u, store->requests("remove"));
  EXPECT_GE(2u, store->requests("remove_many"));
  EXPECT_EQ(0u, store->storedBytes("d/"));
}

// A directory closing while an upload fails does not wait for it forever:
// the closing directory gives up on the file, which nothing synced.
TEST_F(ObjectStoreDirectoryTest, closingWhileAnUploadFailsReturns) {
  auto options = chunks(8);
  options.uploadAttempts = 3;
  auto dir = std::make_unique<ObjectStoreDirectory>(store, "d/", options);
  std::promise<void> release;
  auto released = release.get_future().share();
  std::promise<void> started;
  store->interceptRequests([&, released](std::string_view op, std::string_view) {
    if (op != "put") return;
    started.set_value();
    released.wait();
    throw std::runtime_error("network down");
  });
  finish(*dir, "s01_00", "bytes");
  started.get_future().wait();
  auto closed = std::async(std::launch::async, [&] { dir.reset(); });
  EXPECT_EQ(std::future_status::timeout, closed.wait_for(std::chrono::milliseconds(20)));  // waits for the put
  release.set_value();
  EXPECT_EQ(std::future_status::ready, closed.wait_for(std::chrono::seconds(30)));
  store->interceptRequests(nullptr);
}

// A name a reader still held when it was deleted, then renamed onto: the
// reader's last release does not remove the new object.
TEST_F(ObjectStoreDirectoryTest, aNameRenamedOntoAfterADeferredDeleteKeepsItsNewObject) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  write(writer, "s01_00", std::string(40, 'o'));
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  auto reader = dir.openFile("s01_00");
  ASSERT_TRUE(reader->lazy());
  EXPECT_TRUE(dir.deleteFile("s01_00"));  // deferred: the reader holds it
  write(dir, "s09_00", "new bytes");
  dir.renameFile("s09_00", "s01_00");
  reader.reset();
  ObjectStoreDirectory fresh(store, "d/", chunks(8));
  EXPECT_EQ("new bytes", readAll(fresh, "s01_00"));
  EXPECT_EQ("new bytes", readAll(dir, "s01_00"));
}

// A commit's files are uploaded by the commit itself when the workers are busy
// with others (a large merge), so a commit never queues behind them.
TEST_F(ObjectStoreDirectoryTest, syncUploadsAQueuedFileItselfInsteadOfWaitingBehindOthers) {
  auto options = chunks(8);
  options.workerThreads = 1;
  ObjectStoreDirectory dir(store, "d/", options);
  std::promise<void> release;
  auto released = release.get_future().share();
  std::promise<void> started;
  store->interceptRequests([&, released](std::string_view op, std::string_view name) {
    if (op != "put" || name != "d/s01_00") return;
    started.set_value();
    released.wait();
  });
  finish(dir, "s01_00", "a large merge");  // occupies the only worker
  started.get_future().wait();
  finish(dir, "s02_00", "a small flush");
  auto synced = std::async(std::launch::async, [&] { syncOne(dir, "s02_00"); });
  EXPECT_EQ(std::future_status::ready, synced.wait_for(std::chrono::seconds(10)));
  release.set_value();
  synced.get();
  syncOne(dir, "s01_00");
  EXPECT_EQ(1u, store->requests("put", "d/s02_00"));  // its queued task found it sent
  EXPECT_EQ("a small flush", readAll(dir, "s02_00"));
  store->interceptRequests(nullptr);
}

// Removing a collection while one of its directories still has uploads in
// flight or queued leaves nothing behind: none of them lands after the removal,
// and the directory sends nothing more.
TEST_F(ObjectStoreDirectoryTest, aRemovedCollectionStaysRemovedWhileItsUploadsFinish) {
  LuxirConfig config;
  config.store.options = {{"upload_threads", "1"}};
  StorageContext ctx{config, config.store.options, {}};
  ObjectStoreDirFactory factory(store, ctx);
  auto storage = factory.collection(CollectionId::of("main"));
  auto dir = storage.create("i1");
  std::future<void> removing;
  {
    HeldPuts held(*store);
    finish(*dir, "s01_00", "flushed, never committed");
    held.awaitHeld(1);
    finish(*dir, "s02_00", "queued behind it");
    removing = std::async(std::launch::async, [&] { storage.remove(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  removing.get();
  finish(*dir, "s03_00", "written after the removal");
  EXPECT_THROW(syncOne(*dir, "s03_00"), std::runtime_error);
  dir.reset();
  EXPECT_TRUE(store->list("t/t0/main/").empty());
  EXPECT_TRUE(factory.collections().empty());
}
