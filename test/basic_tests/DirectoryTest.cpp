
#include <gtest/gtest.h>
#include <iostream>
#include <oneapi/tbb/task_group.h>

#include "solux/store/Directory.h"
#include "test/SoluxTest.h"

using namespace solux;

class DirectoryTest : public solux::SoluxTest {
protected:
  void addFile(Directory& dir, const std::string& name, const std::string& data) {
    std::unique_ptr<File> f = dir.createFile(name);
    OutputStream os;
    char arr[6];
    // sometimes start off with a user supplied buffer for the output stream
    if (rng.rbool()) {
      os = OutputStream(arr, arr+sizeof(arr));
    }

    os.setFile(f.get());
    os.write(data.data(), data.size());
    // os.write('X');  // make sure test fails with this
    ASSERT_EQ(data.size(), os.size());
    os.close();
    ASSERT_EQ(data.size(), f->size());
    dir.finishFile(*f);

    // check if dir contents are in sorted order
    std::vector<std::string> resultListing;
    dir.listFiles(resultListing);
    for (uint32_t i=1; i<resultListing.size(); i++) {
      ASSERT_LT(resultListing[i-1], resultListing[i]);
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
    ASSERT_EQ(input->read().data(), input2->read().data());
  }

  void doDir(Directory& dir) {
    std::vector<std::string> lst;

    dir.listFiles(lst);
    ASSERT_EQ(lst.size(), 0);

    addFile(dir, "f5", "12345");
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("f5", lst[0]);

    // add file at end
    addFile(dir, "f5a", "123456");
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("f5a", lst[1]);

    // add file at start
    std::string aaa_data = "123456789abcdefghijklmnopqrstuvwxyz";
    addFile(dir, "aaa", aaa_data);
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("aaa", lst[0]);

    // add file in middle
    addFile(dir, "bbb", "qwertyuiop");
    lst.resize(0); dir.listFiles(lst);
    ASSERT_EQ("bbb", lst[1]);

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
    auto input = dir.openFile("bbb");
    ASSERT_TRUE(input.get() == nullptr);

    lst.resize(0);
    dir.listFiles(lst);
    ASSERT_EQ(lst.size(), 3);
    ASSERT_EQ("f5", lst[1]);

    // test delete of open file
    input = dir.openFile("aaa");
    found = dir.deleteFile("aaa");
    ASSERT_EQ(true, found);
    auto data = input->read();
    ASSERT_EQ(data.size(), aaa_data.size());
    ASSERT_EQ(0, memcmp(data.data(), aaa_data.data(), data.size()));
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
  std::unique_ptr<File> f = dir.createFile("f1");
  OutputStream os;
  os.setFile(f.get());

  Rng r = rng;  // take a snapshot for replayability
  // write a bunch of different width integers and longs

  int iterations = 5;  // results in file size of ~19K

  for (int iter=0; iter<iterations; iter++) {
    for (int i = 0; i < 65; i++) {
      // max value of uint64_t defined in C++ headers is std::numeric_limits<uint64_t>::max()
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
  LOG_ERROR("file size={}", is.size());

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