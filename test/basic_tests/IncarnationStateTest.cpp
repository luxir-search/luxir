// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"
#include "test/StorageTestUtils.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/util/Uuid.h"

using namespace luxir;
using namespace luxir::test;

class IncarnationStateTest : public LuxirTest {};

TEST_F(IncarnationStateTest, laterWritersNameLiveDocsAndOverlaysWithBothEpochs) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-epochs-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  SearchOverridesGuard guard(TestOverlayAuxReader::enabledForTests);
  TestOverlayAuxReader::enabledForTests = true;
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path;
  {
    LuxirNode node(config);
    CollectionHelper h(node);
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b"), flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
    EXPECT_EQ(1u, IncarnationState::read(h.getIndexWriter()->dir)->value.epoch);
  }
  {
    LuxirNode node(config);
    CollectionHelper h(node);
    auto& dir = h.getIndexWriter()->dir;
    EXPECT_EQ(2u, IncarnationState::read(dir)->value.epoch);
    ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
    h.commit({std::string(TestOverlayAuxReader::NAME)});
    auto info = readDurableIndexInfo(dir);
    ASSERT_EQ(1u, info->segments.size());
    const auto& segment = info->segments[0];
    EXPECT_EQ(1u, segment.creator_epoch);
    EXPECT_EQ(2u, segment.live_epoch);
    EXPECT_TRUE(dir.openFile(Postings::getLiveDocsFileName(Postings::segmentKey(segment.seg_id, 1), segment.live_gen, 2)));
    // The test overlay, beside the segment summary written with the segment.
    auto overlay = std::ranges::find(segment.overlays, TestOverlayAuxReader::KIND, &api::AuxIndexInfo::kind);
    ASSERT_NE(segment.overlays.end(), overlay);
    EXPECT_EQ(Postings::getSegmentOverlayFileName(segment.seg_id, TestOverlayAuxReader::NAME, overlay->gen, 0, 1, 2), overlay->files[0].name);
    auto reader = std::make_shared<IndexReader>(dir);
    EXPECT_EQ(2, reader->liveDocs());
    EXPECT_TRUE(reader->segments()[0].getAuxReader(TestOverlayAuxReader::NAME));
  }
  {
    LuxirNode node(config);
    CollectionHelper h(node);
    EXPECT_EQ(3u, IncarnationState::read(h.getIndexWriter()->dir)->value.epoch);
    EXPECT_EQ(2, h.collection().getReaderManager().getReader()->liveDocs());
    ASSERT_TRUE(h.deleteById("b", UpdateMessage::COMMIT).success);
    EXPECT_EQ(3u, readDurableIndexInfo(h.getIndexWriter()->dir)->segments[0].live_epoch);
    EXPECT_EQ(1, IndexReader(h.getIndexWriter()->dir).liveDocs());
  }
}

TEST_F(IncarnationStateTest, removalAndRecreationFencePriorVersions) {
  RAMDir dir;
  reserveIncarnation(dir);
  auto stale = IncarnationState::claim(dir);
  std::array<std::string, 1> names{"STATE"};
  dir.deleteDurably(names);
  EXPECT_THROW(stale.advance(dir, "1.1"), WriterSupersededError);
  EXPECT_THROW(IncarnationState::claim(dir), WriterSupersededError);
  EXPECT_THROW(IncarnationState::install(dir, "1.1"), WriterSupersededError);
  EXPECT_THROW(Manifest::load(dir), WriterSupersededError);
  EXPECT_FALSE(dir.readPublished("STATE"));
  reserveIncarnation(dir);
  auto fresh = IncarnationState::read(dir).value();
  EXPECT_EQ(0u, fresh.value.epoch);
  EXPECT_NE(stale.value.owner, fresh.value.owner);
  EXPECT_FALSE(dir.publishFile("STATE", {}, Directory::PublishCondition::match(stale.version)));
  EXPECT_THROW(stale.advance(dir, "1.1"), WriterSupersededError);
  EXPECT_EQ(1u, IncarnationState::claim(dir).value.epoch);
}
