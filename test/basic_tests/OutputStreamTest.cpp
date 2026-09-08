// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <iostream>

#include "luxir/store/Directory.h"
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
