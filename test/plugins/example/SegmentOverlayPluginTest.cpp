// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Plugin segment overlays (SchemaDef.segment_overlays) through the registry,
// with the example plugin's example_field_list kind.

#include <gtest/gtest.h>

#include <format>
#include <set>

#include "luxir/index/CommitSnapshot.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/ReaderErrors.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/Signal.h"

#include "luxir/plugin/Registries.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/ExamplePluginHooks.h"
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
  static std::pair<int, int> covered(CollectionHelper& helper) { return covered(helper.getIndexWriter()->dir); }
  static std::pair<int, int> covered(Directory& dir) {
    auto info = readDurableIndexInfo(dir);
    int with = 0;
    for (const auto& segment : info->segments) {
      with += std::ranges::any_of(segment.overlays, [](const auto& overlay) { return overlay.name == OVERLAY; });
    }
    return {with, (int)info->segments.size()};
  }

  // The overlay files on disk, and the ones the durable manifest names.
  static std::pair<std::set<std::string>, std::set<std::string>> overlayFiles(Directory& dir) {
    std::pair<std::set<std::string>, std::set<std::string>> files;
    std::vector<Directory::FileInfo> listed;
    dir.listFiles(listed);
    for (const auto& file : listed) {
      if (file.name.contains("example_field_list")) files.first.insert(file.name);
    }
    auto info = readDurableIndexInfo(dir);
    for (const auto& segment : info->segments) {
      for (const auto& overlay : segment.overlays) {
        for (const auto& file : overlay.files) files.second.insert(std::string(file.name));
      }
    }
    return files;
  }

  static std::string meta(CollectionHelper& helper) {
    auto info = readDurableIndexInfo(helper.getIndexWriter()->dir);
    for (const auto& overlay : info->segments.back().overlays) {
      if (overlay.name == OVERLAY) return std::string((const char*)overlay.opaque_meta.data(), overlay.opaque_meta.size());
    }
    return "";
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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  EXPECT_EQ(2, reader->liveDocs());
  EXPECT_EQ(nullptr, reader->segments()[0].getAuxReader(OVERLAY));
  EXPECT_TRUE(overlayFiles(helper.getIndexWriter()->dir).first.empty());
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

// Covered segments cost nothing after a restart, so the per-commit budget goes to the rest;
// the overlay files named by the manifest survive the round trip and open lazily.
TEST_F(SegmentOverlayPluginTest, restartKeepsCoveredOverlaysAndBackfillsTheRest) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_restart");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);  // keep the segments apart
  for (int i = 0; i < 10; i++) addSegment(helper, std::format("s{}_", i), 2);
  setSchema(helper, FIELDS);
  addSegment(helper, "late", 2);
  auto before = covered(helper);
  EXPECT_EQ(11, before.second);
  EXPECT_EQ(8, before.first);  // the per-commit budget
  auto writer = helper.getIndexWriter();
  writer->close();
  CommitSnapshotRegistry reopenedSnapshots(writer->dir);
  IndexWriter reopened(reopenedSnapshots);
  reopened.commit();
  EXPECT_EQ(11, covered(writer->dir).first);
  auto files = overlayFiles(writer->dir);
  EXPECT_EQ(files.first, files.second);
  IndexReader reader(writer->dir);
  const auto& overlay = reader.segments()[0].overlays();
  ASSERT_EQ(1u, overlay.size());
  EXPECT_FALSE(overlay[0]->isOpen());
  EXPECT_NE(nullptr, reader.segments()[0].getAuxReader(OVERLAY));
  EXPECT_TRUE(overlay[0]->isOpen());
}

TEST_F(SegmentOverlayPluginTest, changedDefinitionsRebuildAndClearingStops) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_change");
  addSegment(helper, "a", 2);
  setSchema(helper, FIELDS);
  addSegment(helper, "b", 2);
  EXPECT_EQ((std::pair{2, 2}), covered(helper));
  EXPECT_EQ("1", meta(helper));
  setSchema(helper, R"({"segment_overlays":[{"name":"fields","kind":"example_field_list","fields":["a_s","b_s"],"min_docs":1}]})");
  addSegment(helper, "c", 2);
  EXPECT_EQ((std::pair{3, 3}), covered(helper));
  EXPECT_EQ("2", meta(helper));
  auto info = readDurableIndexInfo(helper.getIndexWriter()->dir);
  for (const auto& segment : info->segments) EXPECT_EQ(1u, segment.overlays.size());
  // Setting an overlay to the same content (only min_docs differs) rebuilds nothing.
  auto files = overlayFiles(helper.getIndexWriter()->dir).second;
  setSchema(helper, R"({"segment_overlays":[{"name":"fields","kind":"example_field_list","fields":["a_s","b_s"],"min_docs":2}]})");
  addSegment(helper, "d", 2);
  for (const auto& file : files) EXPECT_TRUE(overlayFiles(helper.getIndexWriter()->dir).second.contains(file));
  // An entry with no kind removes the overlay: later segments get none.
  setSchema(helper, R"({"segment_overlays":[{"name":"fields"}]})");
  EXPECT_TRUE(helper.collection().getSchema()->segmentOverlays.empty());
  addSegment(helper, "e", 2);
  EXPECT_EQ((std::pair{4, 5}), covered(helper));
}

TEST_F(SegmentOverlayPluginTest, aPluginOpenThatFailsReadsAsAbsentOrFailsPerPolicy) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_open_fails");
  setSchema(helper, R"({"segment_overlays":[
      {"name":"soft","kind":"example_field_list","fields":["a_s"]},
      {"name":"hard","kind":"example_field_list_required","fields":["a_s"]}]})");
  addSegment(helper, "a", 2);
  ASSERT_EQ(2u, readDurableIndexInfo(helper.getIndexWriter()->dir)->segments[0].overlays.size());
  for (int mode : {1, 2}) {
    exampleSetOverlayOpenMode(mode);
    auto reset = scope_guard([] { exampleSetOverlayOpenMode(0); });
    IndexReader reader(helper.getIndexWriter()->dir);
    const auto& segment = reader.segments()[0];
    for (int use = 0; use < 2; use++) {
      EXPECT_EQ(nullptr, segment.getAuxReader("example_field_list.soft"));
      EXPECT_THROW(segment.getAuxReader("example_field_list_required.hard"), SegmentCorruptError);
    }
    try {
      segment.getAuxReader("example_field_list_required.hard");
    } catch (const SegmentCorruptError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("did not open")) << e.what();
    }
    for (const auto& overlay : segment.overlays()) {
      EXPECT_EQ(overlay->name() == "example_field_list.soft", overlay->isUnusable());
    }
  }
  IndexReader healthy(helper.getIndexWriter()->dir);
  EXPECT_NE(nullptr, healthy.segments()[0].getAuxReader("example_field_list.soft"));
}

// A kind nobody registers is skipped on open, but the writer keeps its manifest entry across commits.
TEST_F(SegmentOverlayPluginTest, anUnregisteredKindIsSkippedAndKept) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_ghost");
  setSchema(helper, FIELDS);
  addSegment(helper, "a", 2);
  auto writer = helper.getIndexWriter();
  AuxInfo ghost;  // the writer is idle between commits
  ghost.kind = "ghost_kind";
  ghost.name = "ghost_kind.x";
  writer->segInfos.begin()->second->auxOverlays.push_back(std::move(ghost));
  addSegment(helper, "b", 2);
  auto info = readDurableIndexInfo(writer->dir);
  int ghosts = 0;
  for (const auto& segment : info->segments) {
    ghosts += (int)std::ranges::count_if(segment.overlays, [](const auto& o) { return o.kind == "ghost_kind"; });
  }
  EXPECT_EQ(1, ghosts);
  IndexReader reader(writer->dir);
  for (const auto& segment : reader.segments()) {
    EXPECT_EQ(nullptr, segment.getAuxReader("ghost_kind.x"));
    for (const auto& overlay : segment.overlays()) EXPECT_NE("ghost_kind", overlay->kind());
  }
}

// Overlay files follow their segment: a merge replaces them, and a merge that fails leaves none behind.
TEST_F(SegmentOverlayPluginTest, overlayFilesFollowTheirSegments) {
  CollectionHelper helper(*LuxirTest::luxirNode, "overlay_files");
  setSchema(helper, FIELDS);
  addSegment(helper, "a", 2);
  addSegment(helper, "b", 2);
  auto writer = helper.getIndexWriter();
  auto files = overlayFiles(writer->dir);
  ASSERT_EQ(2u, files.second.size());
  EXPECT_EQ(files.first, files.second);
  {
    Signal::listen("mergeVectorOverlayPrepare", [](void*, void*, void*) -> void* {
      throw std::runtime_error("merge fails after its overlays are built");
    });
    auto unlisten = scope_guard([] { Signal::unlisten("mergeVectorOverlayPrepare"); });
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    helper.submit(merge);
  }
  EXPECT_EQ(2, covered(helper).second);
  auto failed = overlayFiles(writer->dir);
  EXPECT_EQ(files.second, failed.first);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  addSegment(helper, "c", 1);
  auto merged = overlayFiles(writer->dir);
  EXPECT_EQ(merged.first, merged.second);
  for (const auto& file : files.second) EXPECT_FALSE(merged.first.contains(file));
}
