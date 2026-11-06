// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/plugin/Registries.h"
#include "luxir/reader/Hotcache.h"
#include "luxir/store/ObjectStoreDirectory.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"

// The Directory contract itself runs over ObjectStoreDirectory in DirectoryTest
// (objectStoreDir*); these tests cover what is specific to an object store.

using namespace luxir;

namespace {

void write(Directory& dir, const std::string& name, const std::string& bytes) {
  auto file = dir.createFile(name);
  OutputStream out(file.get());
  out.writeBytes(bytes);
  out.close();
  dir.finishFile(*file);
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
  write(dir, "s01.1_00", "hello object store");
  EXPECT_EQ(1u, store->requests("put", "d/s01.1_00"));
  EXPECT_EQ("hello object store", readAll(dir, "s01.1_00"));
  std::array<std::string, 2> names{"s01.1_00", "."};
  dir.sync(names);  // an acknowledged put is durable
  EXPECT_EQ(0u, store->requests("read"));
  EXPECT_FALSE(dir.openFile("s01.1_00")->lazy());

  // Another handle on the same prefix has no staged copy: it reads lazily.
  ObjectStoreDirectory other(store, "d/", chunks(8));
  auto file = other.openFile("s01.1_00");
  ASSERT_NE(nullptr, file);
  EXPECT_TRUE(file->lazy());
  EXPECT_EQ(0u, store->requests("read"));  // opening a file fetches nothing
  EXPECT_EQ(18u, file->size());
  auto is = file->rangeStream(6, 6);
  EXPECT_EQ("object", std::string(is.ptr(), 6));
  EXPECT_EQ(1u, store->requests("read"));
  EXPECT_EQ(16u, store->bytesRead());  // the two covering chunks, in one request
}

TEST_F(ObjectStoreDirectoryTest, publishIsAConditionalPutOnTheObjectVersion) {
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  auto bytes = [](std::string_view text) { return std::as_bytes(std::span(text)); };
  auto first = dir.publishFile("STATE", bytes("one"), Directory::PublishMode::CREATE_ONLY);
  ASSERT_TRUE(first);
  EXPECT_FALSE(dir.publishFile("STATE", bytes("two"), Directory::PublishMode::CREATE_ONLY));
  ObjectStoreDirectory reading(store, "d/", chunks(8));
  auto before = reading.openFile("STATE");  // a reader of the old bytes
  auto published = reading.readPublished("STATE");
  ASSERT_TRUE(published);
  EXPECT_EQ("one", published->bytes);
  EXPECT_EQ(*first, published->version);

  auto second = dir.publishFile("STATE", bytes("two"), Directory::PublishCondition::match(*first));
  ASSERT_TRUE(second);
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(dir.publishFile("STATE", bytes("three"), Directory::PublishCondition::match(*first)));
  EXPECT_FALSE(dir.publishFile("absent", bytes("x"), Directory::PublishCondition::match(*first)));
  EXPECT_EQ("two", readAll(dir, "STATE"));
  EXPECT_EQ("two", reading.readPublished("STATE")->bytes);
  EXPECT_EQ("one", before->read());  // a file that is not segment data is read whole when opened
  // Identical bytes still get a fresh version.
  auto third = dir.publishFile("STATE", bytes("two"), Directory::PublishMode::REPLACE);
  ASSERT_TRUE(third);
  EXPECT_NE(*second, *third);
  EXPECT_FALSE(dir.readPublished("absent"));

  std::array<std::string, 2> names{"STATE", "absent"};
  dir.deleteDurably(names);
  EXPECT_FALSE(store->size("d/STATE"));
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

TEST_F(ObjectStoreDirectoryTest, listingHidesOtherDirectories) {
  ObjectStoreDirectory dir(store, "a/", chunks(8));
  write(dir, "f1", "1");
  write(dir, "f2", "22");
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
  EXPECT_TRUE(store->size("a/sub/f3"));
  EXPECT_FALSE(dir.openFile("f1"));
}

TEST_F(ObjectStoreDirectoryTest, deletingAFileAReaderHoldsWaitsForTheReader) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  write(writer, "s01.1_00", std::string(100, 'x'));  // segment-data shaped: read lazily
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  auto reader = dir.openFile("s01.1_00");
  ASSERT_TRUE(reader->lazy());

  EXPECT_TRUE(dir.deleteFile("s01.1_00"));
  EXPECT_FALSE(dir.openFile("s01.1_00"));  // gone for everyone new...
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  EXPECT_TRUE(files.empty());
  EXPECT_TRUE(store->size("d/s01.1_00"));  // ...but the bytes stay until the reader is done
  auto is = reader->rangeStream(50, 10);
  EXPECT_EQ(std::string(10, 'x'), std::string(is.ptr(), 10));

  reader.reset();
  EXPECT_FALSE(store->size("d/s01.1_00"));

  // With no reader the delete is immediate; a file that is not there is not deleted.
  write(writer, "s01.1_01", "y");
  EXPECT_TRUE(dir.deleteFile("s01.1_01"));
  EXPECT_FALSE(store->size("d/s01.1_01"));
  EXPECT_FALSE(dir.deleteFile("s01.1_01"));
}

// A checksum handle is a reader too: a reader that secured a segment's files by
// handle maps them after a merge deleted them, even past the directory.
TEST_F(ObjectStoreDirectoryTest, aChecksumHandleKeepsADeletedObjectUntilItIsDestroyed) {
  std::string bytes(100, 'x');
  auto dir = std::make_unique<ObjectStoreDirectory>(store, "d/", chunks(8));
  write(*dir, "s01.1_00", bytes);
  auto handle = dir->openChecksum("s01.1_00");
  ASSERT_TRUE(handle);

  EXPECT_TRUE(dir->deleteFile("s01.1_00"));
  EXPECT_FALSE(dir->openFile("s01.1_00"));
  EXPECT_FALSE(dir->openChecksum("s01.1_00"));
  EXPECT_TRUE(store->size("d/s01.1_00"));
  EXPECT_EQ(bytes.size(), handle->checksum().size);
  EXPECT_EQ(bytes, handle->map()->read());

  dir.reset();
  auto mapped = handle->map();
  ASSERT_TRUE(mapped);
  handle.reset();
  EXPECT_TRUE(store->size("d/s01.1_00"));  // the mapped file still reads it
  EXPECT_EQ(bytes, mapped->read());
  mapped.reset();
  EXPECT_FALSE(store->size("d/s01.1_00"));
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
  ObjectStoreDirFactory factory(store, chunks(16 << 10));
  EXPECT_EQ("object:ram_object:ram", factory.describe());
  auto root = StorageRoot::AUTHORITATIVE;
  EXPECT_TRUE(factory.children(root, "").empty());

  // Opening a view creates nothing.
  auto dir = factory.directory(root, "t/t0/i/1");
  EXPECT_TRUE(store->list("").empty());
  EXPECT_EQ(dir, factory.directory(root, "t/t0/i/1"));  // live views are shared
  write(*dir, "data", "bytes");
  EXPECT_TRUE(store->size("t/t0/i/1/data"));
  write(*factory.directory(root, "t/t0"), "CATALOG", "{}");
  write(*factory.directory(root, "t/t0/i/2"), "data", "more");

  using Names = std::vector<std::pair<std::string, bool>>;
  auto names = [](const std::vector<DirectoryFactory::Child>& children) {
    Names result;
    for (const auto& child : children) result.emplace_back(child.name, child.directory);
    return result;
  };
  EXPECT_EQ((Names{{"t", true}}), names(factory.children(root, "")));
  EXPECT_EQ((Names{{"CATALOG", false}, {"i", true}}), names(factory.children(root, "t/t0")));
  EXPECT_EQ((Names{{"1", true}, {"2", true}}), names(factory.children(root, "t/t0/i")));

  // Node-private files live under node/, which the authoritative root does not show.
  write(*factory.directory(StorageRoot::NODE_PRIVATE, ""), "replication.json", "{}");
  EXPECT_TRUE(store->size("node/replication.json"));
  EXPECT_EQ((Names{{"t", true}}), names(factory.children(root, "")));
  EXPECT_EQ((Names{{"replication.json", false}}), names(factory.children(StorageRoot::NODE_PRIVATE, "")));
  EXPECT_THROW(factory.directory(root, "node"), std::invalid_argument);
  EXPECT_THROW(factory.directory(root, "t/../x"), std::invalid_argument);

  factory.removeTree(root, "t/t0/i/1");
  EXPECT_FALSE(store->size("t/t0/i/1/data"));
  EXPECT_FALSE(dir->openFile("data"));  // a held view forgets its staged copy
  EXPECT_EQ((Names{{"2", true}}), names(factory.children(root, "t/t0/i")));
  EXPECT_THROW(factory.removeTree(root, ""), std::invalid_argument);
  factory.removeTree(root, "t/missing");
}

TEST_F(ObjectStoreDirectoryTest, conditionalPutSignalsAConflict) {
  auto created = store->put("k", "1", {.ifAbsent = true, .ifMatch = {}});
  ASSERT_TRUE(created);
  EXPECT_FALSE(store->put("k", "2", {.ifAbsent = true, .ifMatch = {}}));  // a conditional create does not replace
  EXPECT_FALSE(store->put("k", "2", {.ifAbsent = false, .ifMatch = "stale"}));
  auto replaced = store->put("k", "3", {.ifAbsent = false, .ifMatch = *created});
  ASSERT_TRUE(replaced);
  EXPECT_EQ("3", store->get("k")->bytes);
  EXPECT_EQ(*replaced, store->get("k")->version);
  EXPECT_FALSE(store->get("absent"));
  EXPECT_TRUE(store->put("k", "4"));  // an unconditional put replaces
  std::string out(1, '\0');
  store->readRange("k", 0, out);
  EXPECT_EQ("4", out);
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
  auto make = [&](std::map<std::string, std::string> options) {
    config.store.options = {{"ram_object", std::move(options)}};
    StorageOptions parsed(config.store);
    auto params = parsed.reader("ram_object", "store backend 'ram_object'");
    auto factory = storage.makeBackend("ram_object", {config, params, {}});
    params.rejectUnknown();
    return factory;
  };
  auto factory = make({{"store_name", "backend_test"}, {"chunk_bytes", "2K"}});
  auto* objects = dynamic_cast<ObjectStoreDirFactory*>(factory.get());
  ASSERT_NE(nullptr, objects);
  EXPECT_EQ(RamObjectStore::named("backend_test").get(), &objects->store());
  EXPECT_EQ("object:ram_object:backend_test", factory->describe());

  EXPECT_THROW(make({{"chunk_bytes", "0"}}), ParamError);
  EXPECT_THROW(make({{"chunk_bytes", "lots"}}), ParamError);
  EXPECT_THROW(make({{"chunk_size", "2K"}}), ParamError);
  RamObjectStore::drop("backend_test");
}

// Files a loaded manifest lists open from their known sizes: no size request,
// and a segment listed without a hotcache is not probed for one.
TEST_F(ObjectStoreDirectoryTest, knownFilesOpenWithoutSizeRequests) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  Hotcache cache;
  cache.entries.push_back({0, {0, 8}});
  std::string hot = cache.encode();
  write(writer, "s01.1_00", "first segment");
  write(writer, "s01.1__hot", hot);
  write(writer, "s02.1_00", "second segment");
  std::vector<FileDescriptor> files{{"s01.1_00", 13, 0}, {"s01.1__hot", hot.size(), 0}, {"s02.1_00", 14, 0}};

  ObjectStoreDirectory dir(store, "d/", chunks(8));
  dir.knownFiles(files);
  store->resetStats();
  auto first = dir.openFile("s01.1_00");
  ASSERT_NE(nullptr, first);
  EXPECT_EQ(13u, first->size());
  auto second = dir.openFile("s02.1_00");
  ASSERT_NE(nullptr, second);
  EXPECT_EQ(0u, store->requests("size"));
  EXPECT_EQ(1u, store->requests("read", "d/s01.1__hot"));  // the hotcache, then its range of s01_00
  EXPECT_EQ(1u, store->requests("read", "d/s01.1_00"));
  EXPECT_EQ(2u, store->requests());
  EXPECT_EQ("second segment", second->read());
}

// A known size stands only while the name holds the same object.
TEST_F(ObjectStoreDirectoryTest, aKnownFileIsForgottenWhenDeletedOrReplaced) {
  ObjectStoreDirectory writer(store, "d/", chunks(8));
  write(writer, "s01.1_00", "segment");
  write(writer, "s01.1_01", "other");
  ObjectStoreDirectory dir(store, "d/", chunks(8));
  std::vector<FileDescriptor> files{{"s01.1_00", 7, 0}, {"s01.1_01", 5, 0}};
  dir.knownFiles(files);

  EXPECT_TRUE(dir.deleteFile("s01.1_00"));
  EXPECT_EQ(nullptr, dir.openFile("s01.1_00"));  // asks the store, which has no such object

  auto replacement = std::string_view("replacement");
  ASSERT_TRUE(dir.publishFile("s01.1_01", std::as_bytes(std::span(replacement)), Directory::PublishMode::REPLACE));
  auto replaced = dir.openFile("s01.1_01");
  ASSERT_NE(nullptr, replaced);
  EXPECT_EQ("replacement", replaced->read());
}

// Opening a collection loads its commit manifest, which lists every committed
// file with its size: the reopened collection opens its segments without a
// single size request.
TEST(ObjectStoreBackendTest, aReopenedCollectionOpensItsFilesWithoutSizeRequests) {
  using namespace luxir::test;
  std::string name = "reopen_" + newUuid();
  LuxirConfig config;
  config.store.backend = "ram_object";
  config.store.options = {{"ram_object", {{"store_name", name}}}};
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
    std::string key = "t/t0/i/" + collection->getShard()->getSnapshots().snapshot()->id.incarnation + "/";
    EXPECT_GT(store->requests("read", key + "s"), 0u);   // the segments were opened...
    EXPECT_EQ(0u, store->requests("size", key + "s"));  // ...from the sizes the manifest lists
  }
  store.reset();
  RamObjectStore::drop(name);
}
