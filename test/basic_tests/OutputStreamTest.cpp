// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <iostream>

#include "luxir/store/Directory.h"
#include "luxir/store/DirectoryFactory.h"
#include "test/LuxirTest.h"

using namespace luxir;

class OutputStreamTest : public luxir::LuxirTest {
public:
  // if you run into an issue in this test, try changing to true to catch the bug earlier.
  constexpr static bool catch_early = false;
};

// TODO: when we have an InputStream that can read everything that an OutputStream can write,
// do a better test that exercizes all of the outputs+inputs.
TEST_F(OutputStreamTest, randWrite) {
  auto& r = rng;

  RAMDir dir;
  auto nwords = RAMFile::START_BUFFER_SIZE/2;  // size is 4 times amount of first heap buffer in OutputStream...
  std::vector<uint64_t> rdata(nwords);
  const char* rbegin = reinterpret_cast<char*>(rdata.data());
  const char* rend = reinterpret_cast<char*>(rdata.data()+nwords);
  auto rlen = rend-rbegin;

  for (auto& elem : rdata) {
    elem = rng();
  }

  for (int iter=0; iter<1000; iter++) {
    auto f = dir.createFile("rdata");
    OutputStream os;
    ASSERT_EQ(0, os.size());
    os.setFile(f.get());

    int targetLen = r.rint(rlen-1);
    const char* start = rbegin + r.rint(rlen - targetLen);  // start at random place in data
    const char* end = start + targetLen;  // start at random place in data
    const char* pos = start;

    // std::cout << "targetLen=" << targetLen << std::endl;

    while (pos < end) {
      auto a = os.ptr();

      switch(r.rint(4)) {
        case 0:
          os.write(*pos);
          if constexpr (catch_early) {
            if (os.ptr() - a == 1) { // same buffer, check for data written
              ASSERT_EQ(*pos, *a);
            }
          }
          pos++;
          break;
        case 1:
          os.reserve(1);
          os.unsafeWrite(*pos);
          if constexpr (catch_early) {
            if (os.ptr() - a == 1) { // same buffer, check for data written
              ASSERT_EQ(*pos, *a);
            }
          }
          pos++;
          break;
        case 2: {
          auto sz = std::min(r.rint(32), (int)(end-pos));
          os.write(pos, sz);
          if constexpr (catch_early) {
            if (os.ptr() - a == sz) { // same buffer, check for data written
              ASSERT_EQ(0, memcmp(pos, a, sz));
            }
          }
          pos += sz;
          break;
        }
        case 3: {
          auto sz = std::min(r.rint(32), (int)(end-pos));
          os.reserve(sz);
          os.unsafeWrite(pos, sz);
          if constexpr (catch_early) {
            if (os.ptr() - a == sz) { // same buffer, check for data written
              ASSERT_EQ(0, memcmp(pos, a, sz));
            }
          }
          pos+=sz;
          break;
        }
      }
    }

    os.close();

    ASSERT_EQ(pos-start, targetLen);
    dir.finishFile(*f);
    EXPECT_EQ(f->digest(), XXH3_64bits(start, targetLen));
    auto input = dir.openFile("rdata");
    ASSERT_EQ(targetLen, input->size());
    auto data = input->read();
    ASSERT_EQ(targetLen, data.size());
    ASSERT_EQ(0, memcmp(start, data.data(), targetLen));
    dir.deleteFile("rdata");
  }

}

TEST_F(OutputStreamTest, appendFileContinuesWriting) {
  RAMFile source("source");
  OutputStream sourceOut(&source);
  std::string middle(4097, 'm');
  sourceOut.write(middle.data(), middle.size());
  sourceOut.flush(true);

  RAMDir dir;
  auto file = dir.createFile("target");
  OutputStream out(file.get());
  out.writeBytes("before");
  out.appendFile(source);
  EXPECT_EQ(0u, source.size());
  out.writeBytes("after");
  out.close();
  dir.finishFile(*file);

  auto input = dir.openFile("target");
  ASSERT_NE(nullptr, input);
  EXPECT_EQ(std::string("before") + middle + "after", input->read());
}

TEST_F(OutputStreamTest, sizedRamOutputTransfersItsAllocation) {
  for (size_t size : {0, 7, 2 * 1024 * 1024}) {
    RAMDirFactory factory(size);
    auto dir = factory.create("main/first");
    Directory::FileCreateOptions options; options.expectedSize = size;
    auto file = dir->createFile("data", options);
    EXPECT_EQ(size, factory.storageBytes("main"));
    OutputStream out(file.get());
    const char* buffer = nullptr;
    if (size) { out.reserve(1); buffer = out.ptr(); }
    std::string bytes(size, 'x'); out.write(bytes.data(), bytes.size()); out.close();
    dir->finishFile(*file); // Fits an exact-size limit only if there is no flatten copy.
    auto input = dir->openFile("data");
    EXPECT_EQ(bytes, input->read());
    if (size) { EXPECT_EQ(buffer, input->read().data()); }
    EXPECT_EQ(XXH3_64bits(bytes.data(), bytes.size()), file->digest());
    file.reset();
    auto next = factory.create("main/next"); next->linkFile(*dir, "data");
    EXPECT_EQ(size, factory.storageBytes());
    dir->clear(); next->clear(); factory.remove("main");
    EXPECT_EQ(size, factory.storageBytes("main"));
    input.reset();
    EXPECT_EQ(0, factory.storageBytes());
  }
}

TEST_F(OutputStreamTest, sizedRamOutputRejectsIncompleteAndOverlongWrites) {
  RAMDir dir;
  Directory::FileCreateOptions options; options.expectedSize = 2;
  auto file = dir.createFile("short", options);
  OutputStream out(file.get()); out.write('x');
  EXPECT_THROW(out.close(), FileIOException);
  EXPECT_THROW(dir.finishFile(*file), FileIOException);
  auto other = dir.createFile("long", options);
  OutputStream over(other.get());
  EXPECT_THROW(over.reserve(3), FileIOException);
  EXPECT_THROW(over.write("abc", 3), FileIOException);
  EXPECT_EQ(4, dir.storageBytes());
  file.reset(); other.reset();
  EXPECT_EQ(0, dir.storageBytes());
}

TEST_F(OutputStreamTest, ramStorageAccountsForChunkedOutputAndLimitsAllocations) {
  RAMDirFactory factory(1030);
  auto dir = factory.create("main/first");
  auto file = dir->createFile("chunked");
  OutputStream out(file.get()); out.write("abc", 3); out.close();
  EXPECT_EQ(1024, factory.storageBytes());
  dir->finishFile(*file);
  EXPECT_EQ(1027, factory.storageBytes());
  Directory::FileCreateOptions options; options.expectedSize = 4;
  EXPECT_THROW(dir->createFile("over-limit", options), ApiError);
  EXPECT_EQ(1027, factory.storageBytes());
  file.reset();
  EXPECT_EQ(3, factory.storageBytes());
  auto pending = dir->createFile("pending", options);
  EXPECT_EQ(7, factory.storageBytes());
  pending.reset();
  auto input = dir->openFile("chunked"); dir->clear();
  EXPECT_EQ(3, factory.storageBytes());
  input.reset(); EXPECT_EQ(0, factory.storageBytes());
}
