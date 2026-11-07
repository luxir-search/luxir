// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Per-collection merge policies (SchemaDef.merge_policy) through the
// registry, with the example plugin's example_bounds_overlap policy: it merges
// segments whose ts_i ranges overlap.

#include <gtest/gtest.h>

#include <algorithm>
#include <format>

#include "luxir/plugin/Registries.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class MergePolicyPluginTest : public LuxirTest {
protected:
  void SetUp() override { initPlugins(addConfiguredPlugins); }

  // Sets the collection's merge_policy (a MergePolicyDef in JSON).
  static void setMergePolicy(CollectionHelper& helper, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string text = R"({"merge_policy":)" + std::string(json) + "}";
    ASSERT_TRUE(api::read_json(def, text, arena)) << text;
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  // One segment with a document per ts_i value; waits for the merges it starts.
  static void addSegment(CollectionHelper& helper, std::string_view prefix, std::initializer_list<int64_t> values) {
    CollectionHelper::UpdateBuilder request;
    int i = 0;
    for (int64_t value : values) request.add(flatdoc("id", std::format("{}{}", prefix, i++), "ts_i", value));
    request.commit(true);
    ASSERT_TRUE(helper.submit(request).success);
  }

  // maxDoc of each live segment, ascending.
  static std::vector<int32_t> segmentSizes(CollectionHelper& helper) {
    std::vector<int32_t> sizes;
    for (const auto& segment : helper.getIndexWriter()->stats(true).segmentStats) sizes.push_back(segment.maxDoc);
    std::ranges::sort(sizes);
    return sizes;
  }
};

}  // namespace

TEST_F(MergePolicyPluginTest, pluginPolicyChoosesMergesAndCanBeChanged) {
  CollectionHelper helper(*LuxirTest::luxirNode, "merge_policy_overlap");
  setMergePolicy(helper, R"({"name":"example_bounds_overlap","params":{"field":"ts_i"}})");
  addSegment(helper, "a", {100, 110});
  addSegment(helper, "b", {500, 510, 520});
  EXPECT_EQ((std::vector<int32_t>{2, 3}), segmentSizes(helper));
  // Overlaps a only: a and c merge, b stays.
  addSegment(helper, "c", {105, 120, 130, 140});
  EXPECT_EQ((std::vector<int32_t>{3, 6}), segmentSizes(helper));
  // d overlaps nothing, so it stays alone under the plugin policy, but the
  // tiered policy now in force merges it with b (level 1), and their output
  // with a+c (level 2).
  setMergePolicy(helper, R"({"name":"tiered","params":{"merge_factor":2}})");
  addSegment(helper, "d", {600, 610});
  EXPECT_EQ((std::vector<int32_t>{11}), segmentSizes(helper));
}

TEST_F(MergePolicyPluginTest, tieredParamOverridesNodeMergeFactor) {
  CollectionHelper helper(*LuxirTest::luxirNode, "merge_policy_tiered");
  helper.getIndexWriter()->setMergeFactor(1000);
  addSegment(helper, "a", {1});
  addSegment(helper, "b", {2});
  EXPECT_EQ((std::vector<int32_t>{1, 1}), segmentSizes(helper));
  setMergePolicy(helper, R"({"name":"tiered","params":{"merge_factor":3}})");
  addSegment(helper, "c", {3});
  EXPECT_EQ((std::vector<int32_t>{3}), segmentSizes(helper));

  api::SchemaDef def;
  std::pmr::monotonic_buffer_resource arena;
  helper.collection().getSchema()->toProto(&def, arena);
  ASSERT_TRUE(def.merge_policy);
  EXPECT_EQ("tiered", def.merge_policy->name);
}

TEST_F(MergePolicyPluginTest, badMergePolicyFailsTheSchemaRequest) {
  CollectionHelper helper(*LuxirTest::luxirNode, "merge_policy_errors");
  auto expectError = [&](std::string_view json, std::string_view message) {
    try {
      setMergePolicy(helper, json);
      ADD_FAILURE() << "accepted " << json;
    } catch (const SchemaError& e) {
      EXPECT_NE(std::string(e.what()).find(message), std::string::npos) << e.what();
    }
  };
  expectError(R"({"name":"nope"})", "unknown merge policy 'nope'; valid policies: example_bounds_overlap, tiered");
  expectError(R"({"name":"tiered","params":{"merge_factor":1}})", "'merge_factor' must be an integer from 2");
  expectError(R"({"name":"tiered","params":{"factor":3}})", "unknown parameter 'factor'; valid: merge_factor");
  expectError(R"({"name":"example_bounds_overlap"})", "'field'");
  EXPECT_EQ(nullptr, helper.collection().getSchema()->mergePolicy);

  // Creation compiles the same way.
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  ASSERT_TRUE(api::read_json(def, R"({"merge_policy":{"name":"nope"}})", arena));
  EXPECT_THROW(Schema::fromProto(def), SchemaError);
}

TEST(MergePolicyRegistryTest, validatesNamesAndFreezes) {
  MergePolicyRegistry r;
  auto maker = [](const MergePolicyContext&) { return std::make_unique<TieredMergePolicy>(); };
  r.add("mine", maker);
  EXPECT_THROW(r.add("mine", maker), std::invalid_argument);
  EXPECT_THROW(r.add("Bad-Name", maker), std::invalid_argument);
  EXPECT_THROW(r.make("other", {}, 10), std::invalid_argument);
  r.freeze();
  EXPECT_THROW(r.add("later", maker), std::logic_error);
  EXPECT_TRUE(MergePolicyRegistry::global().contains("tiered"));
}
