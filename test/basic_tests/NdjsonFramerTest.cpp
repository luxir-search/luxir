// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/NdjsonFramer.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace luxir {

static std::vector<std::string> drain(NdjsonFramer& framer) {
  std::vector<std::string> out;
  std::string_view record;
  while (framer.next(record)) {
    out.emplace_back(record);
  }
  return out;
}

TEST(NdjsonFramerTest, splitRecordAcrossFeeds) {
  NdjsonFramer framer;
  framer.feed(R"({"id":")");
  EXPECT_TRUE(drain(framer).empty());
  framer.feed("a");
  EXPECT_TRUE(drain(framer).empty());
  framer.feed(R"("})" "\n");

  EXPECT_EQ(std::vector<std::string>({R"({"id":"a"})"}), drain(framer));
  EXPECT_FALSE(framer.error());
}

TEST(NdjsonFramerTest, multipleRecordsInOneFeed) {
  NdjsonFramer framer;
  framer.feed("{\"id\":\"a\"}\n{\"id\":\"b\"}\n{\"id\":\"c\"}\n");

  EXPECT_EQ((std::vector<std::string>{"{\"id\":\"a\"}", "{\"id\":\"b\"}", "{\"id\":\"c\"}"}),
            drain(framer));
  EXPECT_FALSE(framer.error());
}

TEST(NdjsonFramerTest, skipsEmptyLinesAndStripsCr) {
  NdjsonFramer framer;
  framer.feed("\n{\"id\":\"a\"}\r\n\r\n{\"id\":\"b\"}\n");

  EXPECT_EQ((std::vector<std::string>{"{\"id\":\"a\"}", "{\"id\":\"b\"}"}),
            drain(framer));
  EXPECT_FALSE(framer.error());
}

TEST(NdjsonFramerTest, finishFlushesFinalTail) {
  NdjsonFramer framer;
  framer.feed("{\"id\":\"tail\"}");

  std::string_view record;
  EXPECT_TRUE(drain(framer).empty());
  ASSERT_TRUE(framer.finish(record));
  EXPECT_EQ("{\"id\":\"tail\"}", record);
  EXPECT_FALSE(framer.finish(record));
  EXPECT_TRUE(drain(framer).empty());
  EXPECT_FALSE(framer.error());
}

TEST(NdjsonFramerTest, overCapWithoutNewlineIsError) {
  NdjsonFramer framer(4);
  framer.feed("1234");
  EXPECT_TRUE(drain(framer).empty());
  EXPECT_FALSE(framer.error());
  framer.feed("5");

  EXPECT_TRUE(drain(framer).empty());
  EXPECT_TRUE(framer.error());
  EXPECT_NE(std::string::npos, framer.message().find("maximum size"));
}

// Records are found in order, so the ones before an oversized record still come out.
TEST(NdjsonFramerTest, overCapCompleteRecordIsErrorAfterEarlierRecords) {
  NdjsonFramer framer(4);
  framer.feed("123\n12345\n12\n");

  EXPECT_EQ(std::vector<std::string>({"123"}), drain(framer));
  EXPECT_TRUE(framer.error());
  EXPECT_NE(std::string::npos, framer.message().find("maximum size"));
  EXPECT_TRUE(drain(framer).empty());
}

// Records are views into chunks: holding a record's chunk keeps it valid while
// later reads fill new chunks, and a record cut by a chunk boundary (or longer
// than a chunk) comes out whole.
TEST(NdjsonFramerTest, recordsOutliveLaterChunks) {
  NdjsonFramer framer(1024 * 1024, 4096);
  std::string body;
  std::vector<std::string> expected;
  for (int i = 0; i < 200; i++) {
    expected.push_back(std::string(i == 100 ? 20000 : 1 + i % 97, (char)('a' + i % 26)));
    body += expected.back() + "\n";
  }

  std::vector<std::pair<std::string_view, std::shared_ptr<NdjsonChunk>>> held;
  std::string_view record;
  for (std::size_t pos = 0; pos < body.size(); pos += 1000) {
    framer.feed(std::string_view(body).substr(pos, 1000));
    while (framer.next(record)) held.emplace_back(record, framer.chunk());
  }

  ASSERT_EQ(expected.size(), held.size());
  for (std::size_t i = 0; i < held.size(); i++) EXPECT_EQ(expected[i], held[i].first) << i;
  EXPECT_FALSE(framer.error());
}

}  // namespace luxir
