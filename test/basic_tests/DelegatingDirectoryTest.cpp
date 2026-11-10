// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/ScratchBytes.h"

using namespace luxir;

namespace {

void put(Directory& dir, const std::string& name, const std::string& data) {
  auto file = dir.createFile(name);
  OutputStream out(file.get());
  out.write(data.data(), data.size());
  out.close();
  dir.finishFile(*file);
}

FileDescriptor describe(Directory& dir, const std::string& name) {
  auto check = dir.checksum(name);
  return {name, check->size, check->xxh3};
}

// A delegating directory that hides the hard link capability.
class NoLinkDirectory : public DelegatingDirectory {
public:
  using DelegatingDirectory::DelegatingDirectory;
  Capabilities capabilities() const override { return DelegatingDirectory::capabilities() & ~HARD_LINK; }
};

}  // namespace

TEST(DelegatingDirectoryTest, everyVirtualForwards) {
  auto ram = std::make_shared<RAMDir>();
  DelegatingDirectory dir(ram);
  put(dir, "a.bin", "hello");
  put(dir, "ab.bin", "world!");
  put(*ram, "direct.bin", "x");  // visible through the wrapper, and vice versa

  std::vector<Directory::FileInfo> listing;
  dir.listFiles(listing);
  ASSERT_EQ(3u, listing.size());
  EXPECT_EQ("a.bin", listing[0].name);
  EXPECT_EQ(5u, listing[0].size);
  EXPECT_EQ(ram->openFile("a.bin"), dir.openFile("a.bin", true));
  EXPECT_EQ(nullptr, dir.openFile("missing"));
  EXPECT_EQ(&*ram, &dir.underlying());

  // createFile with options reaches the delegate's overload (a sized RAM file).
  Directory::FileCreateOptions options;
  options.expectedSize = 3;
  auto sized = dir.createFile("sized.bin", options);
  OutputStream out(sized.get());
  out.write("abc", 3);
  out.close();
  dir.finishFile(*sized);
  EXPECT_EQ(3u, ram->openFile("sized.bin")->size());

  FileDescriptor desc = describe(dir, "a.bin");
  EXPECT_EQ(describe(*ram, "a.bin"), desc);
  EXPECT_NO_THROW(dir.expectSynced(std::span(&desc, 1)));
  EXPECT_EQ(ram->capabilities(), dir.capabilities());

  std::array<std::string, 1> names{"a.bin"};
  dir.sync(names);
  auto bytes = std::as_bytes(std::span("published", 9));
  auto version = dir.publishFile("a.bin", bytes, Directory::PublishMode::REPLACE);
  ASSERT_TRUE(version);
  auto published = dir.readPublished("a.bin");
  ASSERT_TRUE(published);
  EXPECT_EQ("published", published->bytes);
  EXPECT_EQ(*version, published->version);
  EXPECT_EQ(*version, ram->readPublished("a.bin")->version);
  EXPECT_FALSE(dir.readPublished("missing"));
  EXPECT_FALSE(dir.publishFile("a.bin", {}, Directory::PublishMode::CREATE_ONLY));
  EXPECT_TRUE(dir.publishFile("a.bin", bytes, Directory::PublishCondition::match(*version)));
  EXPECT_FALSE(dir.publishFile("a.bin", {}, Directory::PublishCondition::match(*version)));

  DelegatingDirectory other(std::make_shared<RAMDir>());
  other.linkFile(dir, "a.bin");
  EXPECT_EQ(ram->openFile("a.bin"), other.openFile("a.bin"));  // a link shares the buffer

  EXPECT_TRUE(dir.deleteFile("a.bin"));
  EXPECT_FALSE(dir.deleteFile("a.bin"));
  dir.deletePrefix("ab");
  EXPECT_EQ(nullptr, ram->openFile("ab.bin"));
  EXPECT_NE(nullptr, ram->openFile("direct.bin"));
  std::array<std::string, 2> removed{"sized.bin", "missing"};
  dir.deleteDurably(removed);
  EXPECT_FALSE(ram->openFile("sized.bin"));
  dir.clear();
  listing.clear();
  ram->listFiles(listing);
  EXPECT_TRUE(listing.empty());
}

TEST(DelegatingDirectoryTest, factoryForwardsEverything) {
  DelegatingDirFactory factory(std::make_unique<RAMDirFactory>(64 * 1024));
  EXPECT_EQ(factory.inner(), factory.find<RAMDirFactory>());
  EXPECT_EQ(nullptr, factory.inner()->inner());
  EXPECT_EQ(&factory, factory.find<DelegatingDirFactory>());
  EXPECT_EQ(nullptr, factory.find<FSDirFactory>());
  auto memory = factory.memory();
  ASSERT_TRUE(memory);
  EXPECT_EQ(64u * 1024, memory->limitBytes());
  auto account = memory->child("test");
  auto root = StorageRoot::AUTHORITATIVE;
  std::string path = "t/t0/i/00000000001";
  EXPECT_TRUE(factory.children(root, "").empty());
  auto dir = factory.directory(root, path, {account});
  EXPECT_TRUE(factory.children(root, "").empty()); // opening a view creates nothing
  put(*dir, "f", "bytes");
  EXPECT_EQ(dir, factory.directory(root, path));
  auto children = factory.children(root, "t/t0/i");
  ASSERT_EQ(1u, children.size());
  EXPECT_EQ("00000000001", children[0].name);
  EXPECT_TRUE(children[0].directory);
  EXPECT_GT(account->bytes(), 0u);
  EXPECT_EQ(account->bytes(), factory.storageBytes());
  EXPECT_EQ(factory.directory(StorageRoot::NODE_PRIVATE, ""), factory.directory(StorageRoot::NODE_PRIVATE, ""));
  EXPECT_EQ("ram", factory.describe());
  EXPECT_TRUE(factory.diskPath().empty());
  factory.removeTree(root, path);
  EXPECT_TRUE(factory.children(root, "").empty());
  EXPECT_EQ(0u, memory->bytes());
}

TEST(DelegatingDirectoryTest, readOnlyRejectsEveryMutatorIncludingSizedCreate) {
  auto ram = std::make_shared<RAMDir>();
  put(*ram, "f", "data");
  ReadOnlyDirectory dir(ram, "t");
  EXPECT_NE(nullptr, dir.openFile("f"));
  EXPECT_TRUE(dir.checksum("f"));
  EXPECT_THROW(dir.createFile("g"), ReadOnlyError);
  EXPECT_THROW(dir.createFile("g", {}), ReadOnlyError);
  EXPECT_THROW(dir.deleteFile("f"), ReadOnlyError);
  EXPECT_THROW(dir.deletePrefix("f"), ReadOnlyError);
  EXPECT_TRUE(dir.readPublished("f"));
  EXPECT_THROW(dir.publishFile("f", {}, Directory::PublishMode::REPLACE), ReadOnlyError);
  EXPECT_THROW(dir.publishFile("g", {}, Directory::PublishMode::CREATE_ONLY), ReadOnlyError);
  EXPECT_THROW(dir.publishFile("f", {}, Directory::PublishCondition::match(dir.readPublished("f")->version)), ReadOnlyError);
  EXPECT_THROW(dir.copyFile(*ram, "f"), ReadOnlyError);
  EXPECT_THROW(dir.clear(), ReadOnlyError);
  EXPECT_THROW(dir.linkFile(*ram, "f"), ReadOnlyError);
  std::array<std::string, 1> names{"f"};
  EXPECT_THROW(dir.sync(names), ReadOnlyError);
  EXPECT_THROW(dir.deleteDurably(names), ReadOnlyError);
  auto file = ram->createFile("h");
  EXPECT_THROW(dir.finishFile(*file), ReadOnlyError);
  EXPECT_NE(nullptr, ram->openFile("f"));
}

TEST(DelegatingDirectoryTest, capabilities) {
  RAMDir ram;
  EXPECT_EQ(Directory::HARD_LINK | Directory::ATOMIC_RENAME | Directory::MEMORY_MAPPED, ram.capabilities());
  std::string path = (std::filesystem::temp_directory_path() / "luxir_caps_XXXXXX").string();
  ASSERT_NE(nullptr, ::mkdtemp(path.data()));
  {
    FSDirectory fs(path);
    EXPECT_EQ(Directory::HARD_LINK | Directory::ATOMIC_RENAME | Directory::DURABLE_SYNC | Directory::MEMORY_MAPPED,
              fs.capabilities());
    EXPECT_EQ(0u, fs.capabilities() & Directory::REMOTE);
  }
  std::filesystem::remove_all(path);
  NoLinkDirectory masked(std::make_shared<RAMDir>());
  EXPECT_EQ(Directory::ATOMIC_RENAME | Directory::MEMORY_MAPPED, masked.capabilities());
}

TEST(DelegatingDirectoryTest, reuseFilesLinksOrCopiesByCapability) {
  RAMDir source;
  put(source, "one", "first file");
  put(source, "two", "second file");
  std::vector<FileDescriptor> files{describe(source, "one"), describe(source, "two")};

  RAMDir linked;
  EXPECT_EQ(files, linked.reuseFiles(source, files, files));
  EXPECT_EQ(source.openFile("one"), linked.openFile("one"));  // hard link: one shared buffer

  NoLinkDirectory copied(std::make_shared<RAMDir>());
  EXPECT_EQ(files, copied.reuseFiles(source, files, files));
  for (const auto& file : files) {
    auto copy = copied.openFile(file.name);
    ASSERT_NE(nullptr, copy);
    EXPECT_NE(source.openFile(file.name), copy);  // a distinct buffer with the same bytes
    EXPECT_EQ(source.openFile(file.name)->read(), copy->read());
    EXPECT_EQ(file, describe(copied, file.name));
  }
  // Present files are left to the caller to verify, as before.
  EXPECT_TRUE(copied.reuseFiles(source, files, files).empty());
  EXPECT_THROW(copied.copyFile(source, "absent"), std::runtime_error);
}

// Scratch spills where the backend has local disk; RAM and read-only
// directories keep it in RAM, wrappers forward.
TEST(DelegatingDirectoryTest, scratchPathAndScratchBytes) {
  auto ram = std::make_shared<RAMDir>();
  EXPECT_EQ("", ram->scratchPath());
  std::string path = (std::filesystem::temp_directory_path() / "luxir_scratch_XXXXXX").string();
  ASSERT_NE(nullptr, ::mkdtemp(path.data()));
  {
    auto fs = std::make_shared<FSDirectory>(path);
    EXPECT_EQ(path, fs->scratchPath());
    EXPECT_EQ(path, DelegatingDirectory(fs).scratchPath());
    EXPECT_EQ("", ReadOnlyDirectory(fs, "r").scratchPath());

    for (std::string dir : {std::string(), path}) {
      SCOPED_TRACE(dir);
      ScratchBytes scratch(dir, 100);
      std::vector<std::pair<uint64_t, std::string>> records;
      for (int i = 0; i < 2000; i++) {
        std::string record(1 + (size_t)(i * 37) % 3000, (char)('a' + i % 26));
        records.push_back({scratch.append(record), record});
      }
      scratch.finish();
      EXPECT_EQ(!dir.empty(), scratch.spilled());
      for (const auto& [offset, record] : records) ASSERT_EQ(record, scratch.view(offset, record.size()));
    }
    // The spill file has no name.
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      EXPECT_FALSE(entry.path().filename().string().starts_with(".luxir-scratch")) << entry.path();
    }
  }
  std::filesystem::remove_all(path);
}
