// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class AppendOnlyTest : public LuxirTest {
public:
  LuxirNode node;
  CollectionHelper helper{node};

  void put(std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    if (!api::read_json(def, json, arena, &error)) throw std::runtime_error(error);
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  int64_t liveDocs() { return helper.getIndexWriter()->snapshots.readers.getReader()->liveDocs(); }
};

}  // namespace

TEST_F(AppendOnlyTest, rejectsIdsAndDeletes) {
  put(R"({"append_only":{"enabled":true}})");
  auto withId = helper.index(flatdoc("id", "a", "text_w", "x"), UpdateMessage::COMMIT, true);
  ASSERT_EQ(1u, withId.errors.size());
  EXPECT_NE(std::string::npos, withId.errors[0].error_message.find("append-only"));
  EXPECT_FALSE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("text_w", "x"), UpdateMessage::NO_COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("text_w", "x"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(2, liveDocs());
}

TEST_F(AppendOnlyTest, dropsIdsAndNeverOverwrites) {
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "text_w", "before"), UpdateMessage::COMMIT, true).success);
  put(R"({"append_only":{"enabled":true,"ids":"drop_ids"}})");
  // A SET that does not name it keeps it.
  put(R"({"fields":{"other_s":"string"}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "text_w", "after"), UpdateMessage::COMMIT, true).success);
  EXPECT_EQ(2, liveDocs());  // the earlier "a" stays: nothing is overwritten

  put(R"({"append_only":{"enabled":false}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "text_w", "again"), UpdateMessage::COMMIT, true).success);
  EXPECT_EQ(2, liveDocs());  // overwrites "a" again
}
