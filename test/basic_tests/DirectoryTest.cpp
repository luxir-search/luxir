
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <oneapi/tbb/task_group.h>

#include "luxir/store/Directory.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/store/CheckedDirFactory.h"
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
