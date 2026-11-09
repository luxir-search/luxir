// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <fstream>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <oneapi/tbb/task_group.h>

#include "luxir/store/Directory.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include <unistd.h>
#include "test/LuxirTest.h"

using namespace luxir;

class DirectoryTest : public luxir::LuxirTest {
protected:
  void addFile(Directory& dir, const std::string& name, const std::string& data) {
    std::unique_ptr<File> f = dir.createFile(name);
    OutputStream os;
    os.setFile(f.get());
    os.write(data.data(), data.size());
    // os.write('X');  // make sure test fails with this
    ASSERT_EQ(data.size(), os.size());
    os.close();
    ASSERT_EQ(data.size(), f->size());
    dir.finishFile(*f);

    // check if dir contents are in sorted order
    std::vector<Directory::FileInfo> resultListing;
    dir.listFiles(resultListing);
    for (uint32_t i=1; i<resultListing.size(); i++) {
      ASSERT_LT(resultListing[i-1].name, resultListing[i].name);
    }

    // test not finding a file
    auto missing = dir.openFile(name+"!!!!!!!!!!!!!");
    ASSERT_TRUE(missing.get() == nullptr);

    // test file just written
    auto input = dir.openFile(name);
    ASSERT_TRUE(input.get() != nullptr);
    ASSERT_EQ(data.size(), input->size());
    ASSERT_EQ(0, memcmp(data.data(), input->read().data(), data.size()));

    auto input2 = dir.openFile(name);  // open again... to test out shared_ptr + resource management
    ASSERT_EQ(input->read().size(), input2->read().size());
    ASSERT_EQ(input->read(), input2->read());
  }

  void doDir(Directory& dir) {
    std::vector<Directory::FileInfo> lst;

    dir.listFiles(lst);
    ASSERT_EQ(lst.size(), 0);

    addFile(dir, "f5", "12345");
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("f5", lst[0].name);

    // add file at end
    addFile(dir, "f5a", "123456");
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("f5a", lst[1].name);

    // add file at start
    std::string aaa_data = "123456789abcdefghijklmnopqrstuvwxyz";
    addFile(dir, "aaa", aaa_data);
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("aaa", lst[0].name);

    // add file in middle
    addFile(dir, "bbb", "qwertyuiop");
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("bbb", lst[1].name);

    // sizes match written payload lengths, and totalBytes() is their sum
    ASSERT_EQ(4, lst.size());
    ASSERT_EQ(aaa_data.size(), lst[0].size);  // aaa
    ASSERT_EQ(10, lst[1].size);               // bbb
    ASSERT_EQ(5, lst[2].size);                // f5
    ASSERT_EQ(6, lst[3].size);                // f5a
    ASSERT_EQ(aaa_data.size() + 10 + 5 + 6, dir.totalBytes());

    // remove a non-existing file
    bool found = dir.deleteFile("doesntexist");
    ASSERT_EQ(false, found);

    // remove a non-existing file that would appear at the end of the directory listing
    found = dir.deleteFile("zzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    ASSERT_EQ(false, found);

    // remove existing file from the middle
    found = dir.deleteFile("bbb");
    ASSERT_EQ(true, found);

    // make sure it's gone
    {
      auto input = dir.openFile("bbb");
      ASSERT_TRUE(input.get() == nullptr);
    }

    // Test delete by prefix
    addFile(dir, "bb1", "yeah");
    addFile(dir, "bbx2", "dude");
    addFile(dir, "bbx3", "wow");
    addFile(dir, "bb4", "zonks");

    dir.deletePrefix("bbx");
    {
      auto input = dir.openFile("bb1");
      ASSERT_TRUE(input.get() != nullptr);
      input = dir.openFile("bbx2");
      ASSERT_TRUE(input.get() == nullptr);
      input = dir.openFile("bbx3");
      ASSERT_TRUE(input.get() == nullptr);
      input = dir.openFile("bb4");
      ASSERT_TRUE(input.get() != nullptr);
    }
    // remove remaining test files for delete-prefix
    dir.deletePrefix("bb");

    lst.resize(0);
    dir.listFiles(lst);
    ASSERT_EQ(lst.size(), 3);
    ASSERT_EQ("f5", lst[1].name);

    // test delete of open file
    {
      auto input = dir.openFile("aaa");
      found = dir.deleteFile("aaa");
      ASSERT_EQ(true, found);
      auto data = input->read();
      ASSERT_EQ(data.size(), aaa_data.size());
      ASSERT_EQ(0, memcmp(data.data(), aaa_data.data(), data.size()));
    }
  }

  // create a test method to test for thread safety
  void doDirThreaded(Directory& dir) {
    // 64 reliably failed for me if I removed one of the lock guards in RAMDir
     int nFiles = 64; // this must be a power of two!

     tbb::task_group tg;
     int filenum = 0;
     for (int i=0; i<nFiles; i++) {
       filenum = (filenum + 123456789) % nFiles; // adding an odd number will do a complete traversal for a table of size power of two
       tg.run([this, &dir, filenum]() {
         std::string name = "file" + std::to_string(filenum);
         std::string data = "data" + std::to_string(filenum);
         addFile(dir, name, data);
       });
     }
     tg.wait();
  }

  void doDataTypes(Directory& dir) {
    std::unique_ptr<File> f = dir.createFile("f1");
    OutputStream os;
    os.setFile(f.get());

    Rng r = rng;  // take a snapshot for replayability
    int iterations = 5;  // results in file size of ~19K

    for (int iter=0; iter<iterations; iter++) {
      for (int i = 0; i < 65; i++) {
        uint64_t mask = std::numeric_limits<uint64_t>::max() >> (64 - i);
        uint64_t val = r.rlong() & mask;
        os.writeStr(std::to_string(val));
        os.write((char) val);
        os.writeInt((int32_t) val);
        os.writeInt(-(int32_t) val);
        os.writeVint((uint32_t) val);
        os.writeVint(-(uint32_t) val);
        os.writeLong((int64_t) val);
        os.writeLong(-(int64_t) val);
        os.writeVlong((uint64_t) val);
        os.writeVlong(-(uint64_t) val);
      }
    }

    os.close();
    dir.finishFile(*f);

    auto input = dir.openFile("f1");
    auto is = input->getInputStream();

    r = rng;  // replay same random numbers
    for (int iter=0; iter<iterations; iter++) {
      for (int i = 0; i < 65; i++) {
        uint64_t mask = std::numeric_limits<uint64_t>::max() >> (64 - i);
        uint64_t val = r.rlong() & mask;
        ASSERT_EQ(std::to_string(val), is.readStr());
        ASSERT_EQ((char) val, is.readByte());
        ASSERT_EQ((int32_t) val, is.readInt());
        ASSERT_EQ(-(int32_t) val, is.readInt());
        ASSERT_EQ((uint32_t) val, is.readVint());
        ASSERT_EQ(-(uint32_t) val, is.readVint());
        ASSERT_EQ((int64_t) val, is.readLong());
        ASSERT_EQ(-(int64_t) val, is.readLong());
        ASSERT_EQ((uint64_t) val, is.readVlong());
        ASSERT_EQ(-(uint64_t) val, is.readVlong());
      }
    }
  }

  void doAppendFile(Directory& dir) {
    RAMFile source("source");
    OutputStream sourceOut(&source);
    std::string middle(4097, 'm');
    sourceOut.writeBytes(middle);
    sourceOut.flush(true);

    auto file = dir.createFile("append");
    OutputStream out(file.get());
    out.writeBytes("before");
    out.appendFile(source);
    out.writeBytes("after");
    out.close();
    dir.finishFile(*file);

    auto input = dir.openFile("append");
    ASSERT_NE(nullptr, input);
    EXPECT_EQ(std::string("before") + middle + "after", input->read());
  }

  std::filesystem::path getTempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "luxir_test_XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) {
      throw std::runtime_error("Failed to create temp directory");
    }
    return tmpl;
  }
};


TEST_F(DirectoryTest, ramdir) {
  RAMDir dir;
  doDir(dir);
}

TEST_F(DirectoryTest, ramdirThreads) {
  RAMDir dir;
  doDirThreaded(dir);
}

TEST_F(DirectoryTest, dataTypes) {
  RAMDir dir;
  doDataTypes(dir);
}

TEST_F(DirectoryTest, varintBoundaries) {
  RAMDir dir;
  std::unique_ptr<File> f = dir.createFile("varints");
  OutputStream os;
  os.setFile(f.get());

  uint32_t ints[] = {
    0u, 1u, 0x7fu, 0x80u, 0x3fffu, 0x4000u, 0x1fffffu, 0x200000u,
    0x0fffffffu, 0x10000000u, std::numeric_limits<uint32_t>::max()
  };
  uint64_t longs[] = {
    0ull, 1ull, 0x7full, 0x80ull, 0x3fffull, 0x4000ull,
    0x1fffffull, 0x200000ull, 0x0fffffffull, 0x10000000ull,
    0x7ffffffffull, 0x800000000ull, 0x3fffffffffffull, 0x400000000000ull,
    std::numeric_limits<uint64_t>::max()
  };

  for (uint32_t v : ints) os.writeVint(v);
  for (uint64_t v : longs) os.writeVlong(v);
  os.close();
  dir.finishFile(*f);

  auto input = dir.openFile("varints");
  auto is = input->getInputStream();
  for (uint32_t v : ints) EXPECT_EQ(is.readVint(), v);
  for (uint64_t v : longs) EXPECT_EQ(is.readVlong(), v);
  EXPECT_EQ(is.left(), 0);
}

TEST_F(DirectoryTest, fsdir) {
  auto path = getTempDir();
  FSDirectory dir(path);
  doDir(dir);
  std::filesystem::remove_all(path);
}

TEST_F(DirectoryTest, fsdirThreads) {
  auto path = getTempDir();
  FSDirectory dir(path);
  doDirThreaded(dir);
  std::filesystem::remove_all(path);
}

TEST_F(DirectoryTest, fsdirDataTypes) {
  auto path = getTempDir();
  FSDirectory dir(path);
  doDataTypes(dir);
  std::filesystem::remove_all(path);
}

TEST_F(DirectoryTest, fsdirAppendFile) {
  auto path = getTempDir();
  FSDirectory dir(path);
  doAppendFile(dir);
  std::filesystem::remove_all(path);
}

TEST_F(DirectoryTest, fsdirPersistence) {
  auto path = getTempDir();
  {
    FSDirectory dir(path);
    addFile(dir, "persist1", "hello world");
    addFile(dir, "persist2", "goodbye world");
  }
  // Reopen the directory - files should still be there
  {
    FSDirectory dir(path);
    std::vector<Directory::FileInfo> files;
    dir.listFiles(files);
    ASSERT_EQ(2, files.size());
    ASSERT_EQ("persist1", files[0].name);
    ASSERT_EQ("persist2", files[1].name);

    auto f = dir.openFile("persist1");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ("hello world", f->read());

    f = dir.openFile("persist2");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ("goodbye world", f->read());
  }
  std::filesystem::remove_all(path);
}


// --- CheckedDirectory tests ---

TEST_F(DirectoryTest, checkedDirThrowsOnUnsyncedExpectSynced) {
  auto ram = std::make_shared<RAMDir>();
  CheckedDirectory dir(ram, CheckedDirMode::THROW);

  auto f = dir.createFile("foo");
  OutputStream os;
  os.setFile(f.get());
  os.write("data", 4);
  os.close();
  dir.finishFile(*f);

  // Opening without expectSynced is fine
  ASSERT_NO_THROW(dir.openFile("foo"));

  // Opening with expectSynced on an unsynced file should throw
  ASSERT_THROW(dir.openFile("foo", true), std::runtime_error);

  // After syncing, expectSynced should succeed
  std::vector<std::string> syncFiles = {"foo"};
  dir.sync(syncFiles);
  ASSERT_NO_THROW(dir.openFile("foo", true));
}

// Readers share open segments instead of reopening their files, so it is
// publication that checks a root references only synced files.
TEST_F(DirectoryTest, checkedDirThrowsOnPublishingUnsyncedFiles) {
  auto ram = std::make_shared<RAMDir>();
  CheckedDirectory dir(ram, CheckedDirMode::THROW);
  auto f = dir.createFile("s01_00");
  OutputStream os;
  os.setFile(f.get());
  os.write("data", 4);
  os.close();
  dir.finishFile(*f);

  api::FileDescriptor file{"s01_00", 4, 0};
  api::SegmentInfo segment;
  segment.seg_id = 1;
  segment.max_doc = 1;
  segment.files = std::span<const api::FileDescriptor>(&file, 1);
  api::IndexInfo info;
  info.incarnation = "0123456789abcdef";
  info.index_gen = 1;
  info.segments = std::span<const api::SegmentInfo>(&segment, 1);
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(info, bytes));
  EXPECT_THROW(Manifest::commit(dir, 1, bytes, 1), std::runtime_error);
  EXPECT_EQ(nullptr, dir.openFile(Manifest::name(1, 1)));

  std::vector<std::string> syncFiles = {"s01_00"};
  dir.sync(syncFiles);
  EXPECT_NO_THROW(Manifest::commit(dir, 1, bytes, 1));
}

TEST_F(DirectoryTest, checkedDirWarnMode) {
  auto ram = std::make_shared<RAMDir>();
  CheckedDirectory dir(ram, CheckedDirMode::WARN);

  auto f = dir.createFile("bar");
  OutputStream os;
  os.setFile(f.get());
  os.write("data", 4);
  os.close();
  dir.finishFile(*f);

  // WARN mode should not throw even with expectSynced on unsynced file
  {
    ExpectLog quiet("expectSynced openFile on unsynced file");
    ASSERT_NO_THROW(dir.openFile("bar", true));
  }
}

TEST_F(DirectoryTest, checkedDirDeleteClearsUnsynced) {
  auto ram = std::make_shared<RAMDir>();
  CheckedDirectory dir(ram, CheckedDirMode::THROW);

  auto f = dir.createFile("gone");
  OutputStream os;
  os.setFile(f.get());
  os.write("x", 1);
  os.close();
  dir.finishFile(*f);

  // Delete the file - should clear it from unsynced tracking
  dir.deleteFile("gone");

  // Re-add and sync it, then expectSynced should work
  auto f2 = dir.createFile("gone");
  OutputStream os2;
  os2.setFile(f2.get());
  os2.write("y", 1);
  os2.close();
  dir.finishFile(*f2);

  std::vector<std::string> syncFiles = {"gone"};
  dir.sync(syncFiles);
  ASSERT_NO_THROW(dir.openFile("gone", true));
}

TEST_F(DirectoryTest, checkedDirPreExistingFilesAssumedSynced) {
  auto ram = std::make_shared<RAMDir>();

  // Write a file directly to the underlying RAMDir before wrapping
  auto f = ram->createFile("pre_existing");
  OutputStream os;
  os.setFile(f.get());
  os.write("old", 3);
  os.close();
  ram->finishFile(*f);

  // Now wrap with CheckedDirectory
  CheckedDirectory dir(ram, CheckedDirMode::THROW);

  // Pre-existing files should be assumed synced
  ASSERT_NO_THROW(dir.openFile("pre_existing", true));
}

TEST_F(DirectoryTest, pathViewsOpenWithoutCreating) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirFactory factory(path);
  auto dir = factory.directory(StorageRoot::AUTHORITATIVE, "missing/nested");
  EXPECT_FALSE(std::filesystem::exists(path / "missing"));
  EXPECT_EQ(nullptr, dir->openFile("absent"));
  EXPECT_TRUE(factory.children(StorageRoot::AUTHORITATIVE, "").empty());
  for (auto invalid : {"../outside", "/absolute", "node/private", "a/../b"}) {
    EXPECT_THROW(factory.directory(StorageRoot::AUTHORITATIVE, invalid), std::invalid_argument);
  }
  std::filesystem::create_directory_symlink(path.parent_path(), path / "escape");
  EXPECT_THROW(factory.directory(StorageRoot::AUTHORITATIVE, "escape"), std::invalid_argument);
  std::filesystem::create_directory_symlink(path / "node", path / "private_alias");
  EXPECT_THROW(factory.directory(StorageRoot::AUTHORITATIVE, "private_alias"), std::invalid_argument);
  addFile(*dir, "data", "bytes");
  std::array<std::string, 1> names{"data"};
  dir->sync(names);
  EXPECT_TRUE(std::filesystem::exists(path / "missing/nested/data"));
  std::filesystem::create_directory_symlink(path / "missing", path / "alias");
  factory.removeTree(StorageRoot::AUTHORITATIVE, "alias");
  EXPECT_FALSE(std::filesystem::exists(path / "alias"));
  EXPECT_TRUE(std::filesystem::exists(path / "missing/nested/data"));
}

TEST_F(DirectoryTest, checkedStorageRemembersUnsyncedFilesAcrossOpens) {
  CheckedDirFactory factory(std::make_unique<RAMDirFactory>(), CheckedDirMode::THROW);
  auto open = [&] { return factory.directory(StorageRoot::AUTHORITATIVE, "first"); };
  addFile(*open(), "data", "bytes");
  EXPECT_THROW(open()->openFile("data", true), std::runtime_error);
  std::array<std::string, 1> names{"data"};
  open()->sync(names);
  EXPECT_NE(nullptr, open()->openFile("data", true));
}

TEST_F(DirectoryTest, unreadableFileIsAnErrorNotAbsence) {
  auto path = getTempDir();
  FSDirectory dir(path);
  addFile(dir, "secret", "bytes");
  std::filesystem::permissions(path / "secret", std::filesystem::perms::none);
  if (::access((path / "secret").c_str(), R_OK) != 0) { // not as root
    EXPECT_THROW(dir.openFile("secret"), FileIOException);
    EXPECT_THROW(dir.readPublished("secret"), FileIOException);
  }
  EXPECT_EQ(nullptr, dir.openFile("absent"));
  EXPECT_FALSE(dir.readPublished("absent"));
  std::filesystem::remove_all(path);
}

TEST_F(DirectoryTest, manifestCommitKeepsAnExistingRootOnFailure) {
  RAMDir dir;
  std::vector<std::byte> original(8, std::byte{1}), replacement(8, std::byte{2});
  Manifest::write(dir, 3, original, 1);
  Signal::listen("publishWritten", [](void*, void*, void*) -> void* { throw std::runtime_error("injected"); });
  EXPECT_THROW(Manifest::commit(dir, 3, replacement, 1), std::runtime_error);
  Signal::unlisten("publishWritten");
  auto input = dir.openFile(Manifest::name(3, 1))->getInputStream();
  EXPECT_EQ(std::byte{1}, *(const std::byte*)input.ptr());
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  ASSERT_EQ(1, files.size());
  EXPECT_EQ(Manifest::name(3, 1), files[0].name);
}

TEST_F(DirectoryTest, publicationAndDurableDeletion) {
  for (bool fs : {false, true}) for (bool checked : {false, true}) {
    SCOPED_TRACE(std::format("fs={} checked={}", fs, checked));
    auto path = getTempDir();
    auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
    std::shared_ptr<Directory> dir;
    if (fs) dir = std::make_shared<FSDirectory>(path);
    else dir = std::make_shared<RAMDir>();
    if (checked) dir = std::make_shared<CheckedDirectory>(dir, CheckedDirMode::THROW);
    auto first = std::as_bytes(std::span("first", 5));
    auto next = std::as_bytes(std::span("next", 4));
    EXPECT_FALSE(dir->readPublished("root"));
    ASSERT_TRUE(dir->publishFile("root", first, Directory::PublishMode::CREATE_ONLY));
    EXPECT_FALSE(dir->publishFile("root", next, Directory::PublishMode::CREATE_ONLY));
    auto published = dir->readPublished("root");
    ASSERT_TRUE(published);
    EXPECT_EQ("first", published->bytes);
    auto changed = dir->publishFile("root", first, Directory::PublishCondition::match(published->version));
    ASSERT_TRUE(changed);
    EXPECT_NE(published->version, *changed);
    EXPECT_EQ(*changed, dir->readPublished("root")->version);
    auto repeated = dir->publishFile("root", first, Directory::PublishMode::REPLACE);
    ASSERT_TRUE(repeated);
    EXPECT_NE(*changed, *repeated);
    EXPECT_FALSE(dir->publishFile("root", next, Directory::PublishCondition::match(published->version)));
    EXPECT_THROW(addFile(*dir, "root", "other"), FileIOException);
    EXPECT_EQ("first", dir->openFile("root", true)->read());
    std::array<FileDescriptor, 1> expected{FileDescriptor{"root", 5, 0}};
    EXPECT_NO_THROW(dir->expectSynced(expected));
    dir->deleteFile("root");
    addFile(*dir, "root", "unsynced");
    if (checked) {
      EXPECT_THROW(dir->expectSynced(expected), std::runtime_error);
    }
    ASSERT_TRUE(dir->publishFile("root", next, Directory::PublishMode::REPLACE));
    EXPECT_EQ("next", dir->openFile("root", true)->read());
    EXPECT_NO_THROW(dir->expectSynced(expected));
    dir->deleteFile("root");
    addFile(*dir, "root", "unsynced");
    std::array<std::string, 2> names{"root", "absent"};
    dir->deleteDurably(names);
    EXPECT_EQ(nullptr, dir->openFile("root", true));
    EXPECT_NO_THROW(dir->expectSynced(expected));
    EXPECT_NO_THROW(dir->deleteDurably(names));
    EXPECT_TRUE(dir->publishFile("root", {}, Directory::PublishMode::CREATE_ONLY));
    EXPECT_TRUE(dir->openFile("root")->read().empty());
  }
}

TEST_F(DirectoryTest, publicationFailureBeforeInstallKeepsOriginal) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirectory dir(path);
  auto original = std::as_bytes(std::span("original", 8));
  ASSERT_TRUE(dir.publishFile("root", original, Directory::PublishMode::REPLACE));
  for (auto signal : {"publishWritten", "publishSynced"}) {
    Signal::listen(signal, [](void*, void*, void*) -> void* { throw FileIOException("injected"); });
    auto unlisten = scope_guard([&] { Signal::unlisten(signal); });
    EXPECT_THROW(dir.publishFile("root", {}, Directory::PublishMode::REPLACE), FileIOException);
    EXPECT_EQ("original", dir.openFile("root")->read());
    for (const auto& entry : std::filesystem::directory_iterator(path)) EXPECT_EQ("root", entry.path().filename());
  }
}

TEST_F(DirectoryTest, concurrentPublicationsHaveOneWinner) {
  for (bool fs : {false, true}) for (bool match : {false, true}) {
    auto path = getTempDir();
    auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
    std::shared_ptr<Directory> first, second;
    if (fs) {
      first = std::make_shared<FSDirectory>(path);
      second = std::make_shared<FSDirectory>(path);
    } else first = second = std::make_shared<RAMDir>();
    auto condition = Directory::PublishCondition(Directory::PublishMode::CREATE_ONLY);
    if (match) condition = Directory::PublishCondition::match(*first->publishFile("root", {}, condition));
    std::latch ready(2);
    Signal::listen("publishSynced", [&](void*, void*, void*) -> void* {
      ready.count_down(); ready.wait();
      return nullptr;
    });
    auto unlisten = scope_guard([] { Signal::unlisten("publishSynced"); });
    auto publish = [&](Directory& dir, std::string_view text) {
      return dir.publishFile("root", std::as_bytes(std::span(text)), condition);
    };
    auto a = std::async(std::launch::async, [&] { return publish(*first, "first"); });
    auto b = std::async(std::launch::async, [&] { return publish(*second, "second"); });
    bool won = a.get().has_value();
    EXPECT_NE(won, b.get().has_value());
    EXPECT_EQ(won ? "first" : "second", first->openFile("root")->read());
    if (fs) {
      for (const auto& entry : std::filesystem::directory_iterator(path)) EXPECT_EQ("root", entry.path().filename());
    }
  }
}

TEST_F(DirectoryTest, replacePublicationsDoNotWaitForOtherDirectoriesSyncs) {
  auto path = getTempDir();
  auto root = std::filesystem::weakly_canonical(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirFactory factory(path);
  auto first = factory.directory(StorageRoot::AUTHORITATIVE, "first/leaf");
  auto second = factory.directory(StorageRoot::AUTHORITATIVE, "second/leaf");
  std::promise<void> blocked, resume;
  auto resumed = resume.get_future().share();
  Signal::listen("fsSynced", [&](void* directory, void* file, void*) -> void* {
    if (!file && *(std::filesystem::path*)directory == root / "first") {
      blocked.set_value();
      resumed.wait();
    }
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("fsSynced"); });
  auto pending = std::async(std::launch::async, [&] { return first->publishFile("root", {}, Directory::PublishMode::REPLACE); });
  {
    std::future<std::optional<Directory::PublicationVersion>> other;
    auto unblock = scope_guard([&] { resume.set_value(); });
    ASSERT_EQ(std::future_status::ready, blocked.get_future().wait_for(std::chrono::seconds(5)));
    other = std::async(std::launch::async, [&] { return second->publishFile("root", {}, Directory::PublishMode::REPLACE); });
    auto status = other.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(std::future_status::ready, status);
    if (status == std::future_status::ready) { EXPECT_TRUE(other.get()); }
  }
  EXPECT_TRUE(pending.get());
}

TEST_F(DirectoryTest, publicationVersionIncludesContent) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirectory dir(path);
  auto first = dir.publishFile("root", std::as_bytes(std::span("first", 5)), Directory::PublishMode::CREATE_ONLY);
  auto modified = std::filesystem::last_write_time(path / "root");
  std::ofstream(path / "root") << "other"; // same inode, size, and restored mtime
  std::filesystem::last_write_time(path / "root", modified);
  auto current = dir.readPublished("root");
  ASSERT_TRUE(current);
  EXPECT_EQ("other", current->bytes);
  EXPECT_NE(*first, current->version);
  EXPECT_FALSE(dir.publishFile("root", {}, Directory::PublishCondition::match(*first)));
  EXPECT_TRUE(dir.publishFile("root", {}, Directory::PublishCondition::match(current->version)));
}

TEST_F(DirectoryTest, durableOperationsSyncDirectoryLast) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirectory dir(path);
  addFile(dir, "a", "first");
  addFile(dir, "b", "second");
  std::vector<std::string> synced;
  Signal::listen("fsSynced", [&](void* directory, void* file, void*) -> void* {
    if (*(std::filesystem::path*)directory != path) return nullptr;
    synced.push_back(file ? *(std::string*)file : "directory");
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("fsSynced"); });
  std::array<std::string, 2> names{"a", "b"};
  dir.sync(names);
  EXPECT_EQ((std::vector<std::string>{"a", "b", "directory"}), synced);
  synced.clear();
  dir.deleteDurably(names);
  EXPECT_EQ((std::vector<std::string>{"directory"}), synced);
  synced.clear();
  EXPECT_TRUE(dir.publishFile("root", {}, Directory::PublishMode::REPLACE));
  EXPECT_EQ((std::vector<std::string>{"root", "directory"}), synced);
}

TEST_F(DirectoryTest, publicationFailureAfterInstallNeedsSync) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  CheckedDirectory dir(std::make_shared<FSDirectory>(path), CheckedDirMode::THROW);
  Signal::listen("fsSynced", [](void*, void* file, void*) -> void* {
    if (!file) throw FileIOException("injected after directory sync");
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("fsSynced"); });
  EXPECT_THROW(dir.publishFile("root", {}, Directory::PublishMode::REPLACE), FileIOException);
  EXPECT_NE(nullptr, dir.openFile("root"));
  EXPECT_THROW(dir.openFile("root", true), std::runtime_error);
  Signal::unlisten("fsSynced");
  std::array<std::string, 1> names{"root"};
  dir.sync(names);
  EXPECT_NE(nullptr, dir.openFile("root", true));
}

TEST_F(DirectoryTest, publicationTempsAreHiddenAndLeftAlone) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  std::filesystem::create_directories(path);
  auto temp = path / ".luxir-publish-0-0";
  std::ofstream(temp) << "interrupted publication";
  FSDirectory dir(path);
  std::vector<Directory::FileInfo> files;
  dir.listFiles(files);
  EXPECT_TRUE(files.empty());
  EXPECT_TRUE(std::filesystem::exists(temp));
  EXPECT_TRUE(dir.publishFile("root", {}, Directory::PublishMode::CREATE_ONLY));
  dir.listFiles(files);
  ASSERT_EQ(1, files.size());
  EXPECT_EQ("root", files[0].name);
}

TEST_F(DirectoryTest, publicationPersistsNewAncestors) {
  auto path = getTempDir();
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirFactory factory(path);
  auto dir = factory.directory(StorageRoot::AUTHORITATIVE, "a/b");
  std::vector<std::filesystem::path> synced;
  Signal::listen("fsSynced", [&](void* directory, void* file, void*) -> void* {
    if (!file) synced.push_back(*(std::filesystem::path*)directory);
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("fsSynced"); });
  dir->publishFile("root", {}, Directory::PublishMode::CREATE_ONLY);
  auto root = std::filesystem::weakly_canonical(path);
  EXPECT_EQ((std::vector<std::filesystem::path>{root / "a/b", root / "a", root}), synced);
  synced.clear();
  auto reopened = factory.directory(StorageRoot::AUTHORITATIVE, "a/b");
  reopened->publishFile("root", {}, Directory::PublishMode::REPLACE);
  EXPECT_EQ((std::vector<std::filesystem::path>{root / "a/b"}), synced);

  factory.removeTree(StorageRoot::AUTHORITATIVE, "a");
  synced.clear();
  reopened = factory.directory(StorageRoot::AUTHORITATIVE, "a/b");
  reopened->publishFile("root", {}, Directory::PublishMode::CREATE_ONLY);
  EXPECT_EQ((std::vector<std::filesystem::path>{root / "a/b", root / "a", root}), synced);
}

TEST_F(DirectoryTest, deletionSerializesWithMatchedPublication) {
  for (bool durable : {false, true}) {
    auto path = getTempDir();
    auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
    FSDirectory dir(path), other(path);
    auto version = dir.publishFile("root", {}, Directory::PublishMode::CREATE_ONLY).value();
    std::promise<void> matched, deleting;
    std::latch resume(1);
    Signal::listen("publishMatched", [&](void*, void*, void*) -> void* {
      matched.set_value(); resume.wait(); return nullptr;
    });
    auto unlisten = scope_guard([] { Signal::unlisten("publishMatched"); });
    auto publication = std::async(std::launch::async, [&] {
      return dir.publishFile("root", {}, Directory::PublishCondition::match(version));
    });
    matched.get_future().wait();
    auto removal = std::async(std::launch::async, [&] {
      deleting.set_value();
      if (durable) {
        std::array<std::string, 1> names{"root"};
        other.deleteDurably(names);
      } else EXPECT_TRUE(other.deleteFile("root"));
    });
    deleting.get_future().wait();
    EXPECT_EQ(std::future_status::timeout, removal.wait_for(std::chrono::milliseconds(20)));
    resume.count_down();
    EXPECT_TRUE(publication.get());
    removal.get();
    EXPECT_FALSE(dir.readPublished("root"));
  }
}
