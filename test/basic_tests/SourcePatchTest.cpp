// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Patch operations on a stored source (SourcePatch), without an index.

#include <gtest/gtest.h>

#include <memory_resource>

#include "luxir/source/SourcePatch.h"

namespace luxir::test {

namespace {

using Status = api::PatchResult::Status;

struct Applied {
  Status status;
  std::string body;
  std::string error;
};

// Applies the first patch of an UpdateRequest written as JSON.
Applied apply(std::string_view requestJson, std::optional<std::string> base, uint64_t baseVersion = 1) {
  std::pmr::monotonic_buffer_resource arena;
  api::UpdateRequest req;
  std::string err;
  EXPECT_TRUE(api::read_json(req, requestJson, arena, &err)) << err;
  EXPECT_EQ(1u, req.patches.size());
  auto out = source::applyPatch(req.patches[0], base, baseVersion);
  return {out.status, out.body, out.error};
}

std::string patch(std::string_view body) {
  return R"({"patches":[{"id":"a",)" + std::string(body) + "}]}";
}

}  // namespace

TEST(SourcePatchTest, setKeepsUntouchedBytes) {
  auto out = apply(patch(R"("ops":[{"op":"set","path":"price","value":12}])"),
                   R"({"id":"a","n":"0042","price":10,"b":2,"b":3,"x":1.50})");
  EXPECT_EQ(Status::UPDATED, out.status);
  EXPECT_EQ(R"({"id":"a","n":"0042","price":12,"b":2,"b":3,"x":1.50})", out.body);
}

TEST(SourcePatchTest, setCreatesObjectsAndReplacesDuplicates) {
  auto out = apply(patch(R"("ops":[{"op":"set","path":"meta.k","value":"v"},{"op":"set","path":"b","value":9}])"),
                   R"({"id":"a","b":2,"b":3})");
  EXPECT_EQ(R"({"id":"a","b":9,"meta":{"k":"v"}})", out.body);
  out = apply(patch(R"("ops":[{"op":"set","path":"b.c","value":1}])"), R"({"id":"a","b":2})");
  EXPECT_EQ(Status::ERROR, out.status);
}

TEST(SourcePatchTest, unsetRemovesEveryDuplicate) {
  auto out = apply(patch(R"("ops":[{"op":"unset","path":"b"},{"op":"unset","path":"nope.x"}])"),
                   R"({"id":"a","b":2,"c":1,"b":3})");
  EXPECT_EQ(R"({"id":"a","c":1})", out.body);
}

TEST(SourcePatchTest, mergeFollowsRfc7396) {
  auto out = apply(patch(R"("ops":[{"op":"merge","value":{"t":null,"m":{"x":1,"y":null},"n":{"z":null,"w":2}}}])"),
                   R"({"id":"a","t":"x","m":{"y":2,"k":3}})");
  EXPECT_EQ(R"({"id":"a","m":{"k":3,"x":1},"n":{"w":2}})", out.body);
}

TEST(SourcePatchTest, arithmetic) {
  auto out = apply(patch(R"("ops":[{"op":"increment","path":"n","value":-3},{"op":"increment","path":"m","value":2},)"
                         R"({"op":"multiply","path":"f","value":2.5},{"op":"min","path":"lo","value":4},)"
                         R"({"op":"max","path":"hi","value":4},{"op":"max","path":"new","value":7}])"),
                   R"({"id":"a","n":10,"f":2,"lo":5,"hi":5})");
  EXPECT_EQ(R"({"id":"a","n":7,"f":5.0,"lo":4,"hi":5,"m":2,"new":7})", out.body);
  EXPECT_EQ(Status::ERROR, apply(patch(R"("ops":[{"op":"increment","path":"s","value":1}])"),
                                 R"({"id":"a","s":"x"})").status);
  EXPECT_EQ(Status::ERROR, apply(patch(R"("ops":[{"op":"increment","path":"n","value":1}])"),
                                 R"({"id":"a","n":9223372036854775807})").status);
  EXPECT_EQ(Status::ERROR, apply(patch(R"("ops":[{"op":"increment","path":"n","value":1}])"),
                                 R"({"id":"a","n":[1,2]})").status);
}

TEST(SourcePatchTest, lists) {
  auto out = apply(patch(R"("ops":[{"op":"append","path":"t","value":["x","y"]},{"op":"append_unique","path":"t","value":["y","z"]},)"
                         R"({"op":"append","path":"s","value":"b"},{"op":"remove_values","path":"r","value":[1,3]},)"
                         R"({"op":"append","path":"new","value":1}])"),
                   R"({"id":"a","t":["w"],"s":"a","r":[1,2,3,1]})");
  EXPECT_EQ(R"({"id":"a","t":["w","x","y","z"],"s":["a","b"],"r":[2],"new":[1]})", out.body);
  out = apply(patch(R"("ops":[{"op":"remove_values","path":"s","value":"a"}])"), R"({"id":"a","s":"a"})");
  EXPECT_EQ(R"({"id":"a"})", out.body);
}

TEST(SourcePatchTest, preconditions) {
  EXPECT_EQ(Status::NOT_FOUND, apply(patch(R"("ops":[{"op":"set","path":"x","value":1}])"), std::nullopt).status);
  auto created = apply(patch(R"("upsert":true,"ops":[{"op":"increment","path":"n","value":1}])"), std::nullopt);
  EXPECT_EQ(Status::CREATED, created.status);
  EXPECT_EQ(R"({"id":"a","n":1})", created.body);
  created = apply(patch(R"("upsert_doc":{"n":5},"ops":[{"op":"increment","path":"n","value":1}])"), std::nullopt);
  EXPECT_EQ(R"({"n":5,"id":"a"})", created.body);
  EXPECT_EQ(Status::CONFLICT, apply(patch(R"("if_version":3,"ops":[])"), R"({"id":"a"})", 2).status);
  EXPECT_EQ(Status::NOOP, apply(patch(R"("if_version":2,"ops":[])"), R"({"id":"a"})", 2).status);
  EXPECT_EQ(Status::CONFLICT, apply(patch(R"("exists":"must_not","ops":[])"), R"({"id":"a"})").status);
  EXPECT_EQ(Status::DELETED, apply(patch(R"("delete_doc":true)"), R"({"id":"a"})").status);
  EXPECT_EQ(Status::NOT_FOUND, apply(patch(R"("delete_doc":true)"), std::nullopt).status);
}

TEST(SourcePatchTest, conditions) {
  std::string base = R"({"id":"a","n":3,"s":"b"})";
  EXPECT_EQ(Status::UPDATED, apply(patch(R"("when":[{"path":"n","op":"eq","value":3},{"path":"s","op":"lt","value":"c"}],)"
                                         R"("ops":[{"op":"set","path":"x","value":1}])"), base).status);
  EXPECT_EQ(Status::SKIPPED, apply(patch(R"("when":[{"path":"n","op":"gt","value":3}],"ops":[{"op":"set","path":"x","value":1}])"),
                                   base).status);
  // A missing field fails comparisons unless the condition names a value for it.
  EXPECT_EQ(Status::SKIPPED, apply(patch(R"("when":[{"path":"m","op":"eq","value":0}],"ops":[{"op":"set","path":"x","value":1}])"),
                                   base).status);
  EXPECT_EQ(Status::UPDATED, apply(patch(R"("when":[{"path":"m","op":"eq","value":0,"missing":0}],"ops":[{"op":"set","path":"x","value":1}])"),
                                   base).status);
  EXPECT_EQ(Status::UPDATED, apply(patch(R"("when":[{"path":"m","op":"missing"},{"path":"n","op":"exists"}],"ops":[{"op":"set","path":"x","value":1}])"),
                                   base).status);
}

TEST(SourcePatchTest, nullCountsAsMissing) {
  auto out = apply(patch(R"("ops":[{"op":"increment","path":"n","value":2},{"op":"append","path":"t","value":"x"}])"),
                   R"({"id":"a","n":null,"t":null})");
  EXPECT_EQ(R"({"id":"a","n":2,"t":["x"]})", out.body);
}

TEST(SourcePatchTest, idCannotChange) {
  EXPECT_EQ(Status::ERROR, apply(patch(R"("ops":[{"op":"merge","value":{"id":"b"}}])"), R"({"id":"a"})").status);
  auto out = apply(patch(R"("ops":[{"op":"replace","value":{"x":1}}])"), R"({"id":"a","y":2})");
  EXPECT_EQ(R"({"x":1,"id":"a"})", out.body);
  EXPECT_EQ(Status::NOOP, apply(R"({"patches":[{"id":"7","ops":[{"op":"merge","value":{"id":7}}]}]})", R"({"id":7})").status);
  EXPECT_EQ(Status::UPDATED, apply(R"({"patches":[{"id":"100","ops":[{"op":"set","path":"x","value":1}]}]})", R"({"id":1e2})").status);
}

TEST(SourcePatchTest, paths) {
  EXPECT_TRUE(source::invalidPatchPath("id"));
  EXPECT_TRUE(source::invalidPatchPath("id.x"));
  EXPECT_TRUE(source::invalidPatchPath("_version_"));
  EXPECT_TRUE(source::invalidPatchPath("a..b"));
  EXPECT_FALSE(source::invalidPatchPath("a.idx"));
}

TEST(SourcePatchTest, changedKeysCompareTheLastValueByBytes) {
  auto changed = source::changedKeys(R"({"id":"a","n":1,"d":1,"d":2,"x":1.0,"gone":3})",
                                     R"({"id":"a","d":5,"d":2,"x":1,"n":1,"new":[1]})");
  ASSERT_EQ(3u, changed.size());
  EXPECT_EQ("gone", changed[0].key);
  EXPECT_FALSE(changed[0].value);
  EXPECT_EQ("new", changed[1].key);
  EXPECT_EQ("[1]", changed[1].value);
  EXPECT_EQ("x", changed[2].key);  // equal numbers, different bytes
  EXPECT_EQ(2u, source::valueShape("[1, 2]").values);
  EXPECT_TRUE(source::valueShape("[1,[2]]").nestedArray);
  EXPECT_EQ(1u, source::valueShape(R"({"a":[1]})").values);
}

TEST(SourcePatchTest, compareJson) {
  EXPECT_EQ(0, source::compareJson("1", "1.0"));
  EXPECT_EQ(-1, source::compareJson("\"a\"", "\"b\""));
  EXPECT_EQ(0, source::compareJson("\"\\u00e9\"", "\"\xc3\xa9\""));
  EXPECT_FALSE(source::compareJson("1", "\"1\""));
  EXPECT_EQ(0, source::compareJson("[1, 2]", "[1,2]"));
}

}  // namespace luxir::test
