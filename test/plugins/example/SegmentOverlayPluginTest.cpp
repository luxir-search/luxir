// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Plugin segment overlays (SchemaDef.segment_overlays) through the registry,
// with the example plugin's example_field_list kind.

#include <gtest/gtest.h>

#include <format>

#include "luxir/plugin/Registries.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr std::string_view OVERLAY = "example_field_list.fields";

class SegmentOverlayPluginTest : public LuxirTest {
protected:
  void SetUp() override { initPlugins(addConfiguredPlugins); }

  static void setSchema(CollectionHelper& helper, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, json, arena)) << json;
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  static void addSegment(CollectionHelper& helper, std::string_view prefix, int docs) {
    std::vector<Doc> batch;
    for (int i = 0; i < docs; i++) batch.push_back(flatdoc("id", std::format("{}{}", prefix, i), "a_s", "x", "b_s", "y"));
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }

  // How many segments of the durable manifest carry the overlay, of how many.
  static std::pair<int, int> covered(CollectionHelper& helper) {
    auto info = readDurableIndexInfo(helper.getIndexWriter()->dir);
    int with = 0;
    for (const auto& segment : info->segments) {
      with += std::ranges::any_of(segment.overlays, [](const auto& overlay) { return overlay.name == OVERLAY; });
    }
    return {with, (int)info->segments.size()};
  }

  static constexpr std::string_view FIELDS = R"({"segment_overlays":[{"name":"fields","kind":"example_field_list","fields":["a_s"]}]})";
};

}  // namespace

TEST_F(SegmentOverlayPluginTest, builtForNewSegmentsAndReadableOnDemand) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_new");
  setSchema(helper, FIELDS);
  addSegment(helper, "a", 3);
  helper.commit();
  EXPECT_EQ((std::pair{1, 1}), covered(helper));
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto aux = reader->segments()[0].getAuxReader(OVERLAY);
  ASSERT_NE(nullptr, aux);
  EXPECT_EQ("example_field_list", aux->getKind());
  EXPECT_FALSE(dynamic_cast<UnavailableAuxReader*>(aux.get()));
}

TEST_F(SegmentOverlayPluginTest, olderSegmentsAreBackfilledAtLaterCommits) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_backfill");
  addSegment(helper, "a", 2);
  addSegment(helper, "b", 2);
  EXPECT_EQ(0, covered(helper).first);
  setSchema(helper, FIELDS);
  addSegment(helper, "c", 2);
  helper.commit();
  EXPECT_EQ((std::pair{3, 3}), covered(helper));
}

TEST_F(SegmentOverlayPluginTest, mergedSegmentsGetTheirOwnOverlay) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_merge");
  setSchema(helper, FIELDS);
  addSegment(helper, "a", 2);
  addSegment(helper, "b", 2);
  helper.commit();
  ASSERT_EQ(2, covered(helper).second);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  EXPECT_EQ((std::pair{1, 1}), covered(helper));
}

TEST_F(SegmentOverlayPluginTest, minDocsSkipsSmallSegments) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_min_docs");
  setSchema(helper, R"({"segment_overlays":[{"name":"fields","kind":"example_field_list","fields":["a_s"],"min_docs":5}]})");
  addSegment(helper, "a", 2);
  addSegment(helper, "b", 6);
  helper.commit();
  EXPECT_EQ((std::pair{1, 2}), covered(helper));
}

TEST_F(SegmentOverlayPluginTest, aFailedBuildLeavesTheSegmentWithoutTheOverlay) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_fails");
  setSchema(helper, R"({"segment_overlays":[{"name":"fields","kind":"example_field_list","fields":["a_s"],"params":{"fail":true}}]})");
  addSegment(helper, "a", 2);
  helper.commit();
  EXPECT_EQ((std::pair{0, 1}), covered(helper));
  EXPECT_EQ(1, helper.getIndexWriter()->stats(true).segmentStats.size());
}

TEST_F(SegmentOverlayPluginTest, noCoveredFieldsMeansNoOverlay) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_no_fields");
  setSchema(helper, R"({"segment_overlays":[{"name":"fields","kind":"example_field_list","fields":["missing_*"]}]})");
  addSegment(helper, "a", 2);
  helper.commit();
  EXPECT_EQ((std::pair{0, 1}), covered(helper));
}

TEST_F(SegmentOverlayPluginTest, badDefinitionsFailTheSchemaRequest) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_bad");
  EXPECT_ANY_THROW(setSchema(helper, R"({"segment_overlays":[{"name":"f","kind":"nope","fields":["a_s"]}]})"));
  EXPECT_ANY_THROW(setSchema(helper, R"({"segment_overlays":[{"name":"F","kind":"example_field_list","fields":["a_s"]}]})"));
  EXPECT_ANY_THROW(setSchema(helper, R"({"segment_overlays":[{"name":"f","kind":"example_field_list","fields":[]}]})"));
  EXPECT_ANY_THROW(setSchema(helper, R"({"segment_overlays":[{"name":"f","kind":"example_field_list","fields":["a_s"],"params":{"nope":1}}]})"));
  EXPECT_ANY_THROW(setSchema(helper, R"({"segment_overlays":[{"name":"f","kind":"example_field_list","fields":["a_s"]},{"name":"f","kind":"example_field_list","fields":["b_s"]}]})"));
}
