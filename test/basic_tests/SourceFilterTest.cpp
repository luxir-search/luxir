// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Source includes/excludes over both captured forms: raw JSON bytes and Maps.

#include <gtest/gtest.h>

#include <memory_resource>
#include <stdexcept>
#include <vector>

#include "luxir/source/SourceFilter.h"

namespace luxir::test {

namespace {

constexpr const char* kDoc =
    R"({"id":"1","title":"t","user":{"name":"n","pw":"p","tags":["a","b"]},)"
    R"("items":[{"sku":"x","cost":1},{"cost":2},3],"name_s":"esc","empty":{}})";

source::SourceFilter includes(std::vector<std::string_view> patterns) { return {patterns, {}}; }
source::SourceFilter excludes(std::vector<std::string_view> patterns) { return {{}, patterns}; }

std::string json(const source::SourceFilter& filter, std::string_view doc = kDoc) {
  std::string out;
  EXPECT_TRUE(filter.filterJson(doc, out)) << doc;
  return out;
}

// The Map path agrees with the JSON path on a document without duplicate keys.
std::string map(const source::SourceFilter& filter, std::string_view doc) {
  std::pmr::monotonic_buffer_resource arena;
  api::Map in;
  EXPECT_TRUE(api::read_json(in, doc, arena)) << doc;
  std::string out;
  EXPECT_TRUE(api::write_json(filter.filterMap(in, arena), out));
  return out;
}

}  // namespace

TEST(SourceFilterTest, excludesDropMatchingPathsAndKeepEmptiedContainers) {
  EXPECT_EQ(R"({"id":"1","title":"t","user":{"name":"n","tags":["a","b"]},)"
            R"("items":[{"sku":"x","cost":1},{"cost":2},3],"name_s":"esc","empty":{}})",
            json(excludes({"user.pw"})));
  // Arrays add no step: items.cost reaches every object in items.
  EXPECT_EQ(R"({"id":"1","title":"t","items":[{"sku":"x"},{},3],"empty":{}})",
            json(excludes({"user", "items.cost", "*_s"})));
  // Escaped keys are matched by their text.
  EXPECT_EQ(R"({"id":"1"})", json(excludes({"t*", "u*", "i*s", "name_s", "empty"})));
}

TEST(SourceFilterTest, includesKeepMatchingPathsAndTheirAncestors) {
  EXPECT_EQ(R"({"id":"1","user":{"name":"n"}})", json(includes({"id", "user.name"})));
  EXPECT_EQ(R"({"user":{"name":"n","pw":"p","tags":["a","b"]}})", json(includes({"user"})));
  // Objects inside an array are filtered; emptied elements and scalars go.
  EXPECT_EQ(R"({"items":[{"sku":"x"}]})", json(includes({"items.sku"})));
  // A wildcard spans dots; nothing matched leaves an empty object.
  EXPECT_EQ(R"({"user":{"pw":"p"}})", json(includes({"*.pw"})));
  EXPECT_EQ("{}", json(includes({"missing", "empty.x"})));
}

TEST(SourceFilterTest, mapPathMatchesJsonPath) {
  std::string doc = R"({"id":"1","user":{"name":"n","pw":"p"},"items":[{"sku":"x","cost":1},{"cost":2}]})";
  for (const auto& filter : {includes({"user.name", "items.sku"}), excludes({"user.pw", "items.cost"}),
                             includes({"id"}), excludes({"*"})}) {
    EXPECT_EQ(json(filter, doc), map(filter, doc));
  }
}

TEST(SourceFilterTest, rejectsBothListsAndEmptyPatterns) {
  std::vector<std::string_view> some{"a"};
  std::vector<std::string_view> blank{""};
  EXPECT_THROW(source::SourceFilter(some, some), std::invalid_argument);
  EXPECT_THROW(source::SourceFilter(blank, {}), std::invalid_argument);
  std::string out;
  EXPECT_FALSE(includes({"a"}).filterJson("[1]", out));
}

}  // namespace luxir::test
