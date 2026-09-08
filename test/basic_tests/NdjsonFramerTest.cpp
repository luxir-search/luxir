// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/NdjsonFramer.h"

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
  ASSERT_TRUE(framer.finish(record));
  EXPECT_EQ("{\"id\":\"tail\"}", record);
  EXPECT_FALSE(framer.finish(record));
  EXPECT_TRUE(drain(framer).empty());
  EXPECT_FALSE(framer.error());
}

TEST(NdjsonFramerTest, overCapWithoutNewlineIsError) {
  NdjsonFramer framer(4);
  framer.feed("1234");
  EXPECT_FALSE(framer.error());
  framer.feed("5");

  EXPECT_TRUE(framer.error());
  EXPECT_NE(std::string::npos, framer.message().find("maximum size"));
  EXPECT_TRUE(drain(framer).empty());
}

TEST(NdjsonFramerTest, overCapCompleteRecordIsError) {
  NdjsonFramer framer(4);
  framer.feed("12345\n");

  EXPECT_TRUE(framer.error());
  EXPECT_NE(std::string::npos, framer.message().find("maximum size"));
  EXPECT_TRUE(drain(framer).empty());
}

}  // namespace luxir
