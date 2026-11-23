// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/ColdDirectory.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

std::shared_ptr<Directory> borrow(Directory& dir) { return std::shared_ptr<Directory>(std::shared_ptr<Directory>(), &dir); }

std::string primaryFile(const api::SegmentInfo& segment) {
  return Postings::getIndexFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), Postings::PRIMARY_FILE);
}

} // namespace

class ColdReadTest : public LuxirTest {};

TEST_F(ColdReadTest, aLoadedManifestNamesEachSegmentsCatalog) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "c_s", "x"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  ColdDirectory cold(borrow(h.getIndexWriter()->dir));
  auto info = readDurableIndexInfo(cold);  // Manifest::load passes the committed files on
  ASSERT_FALSE(info->segments.empty());
  for (const auto& segment : info->segments) {
    auto primary = primaryFile(segment);
    for (const auto& file : segment.files) {
      auto known = cold.counters().knownFile(file.name);
      ASSERT_TRUE(known) << file.name;
      EXPECT_EQ(file.size, known->file.size);
      EXPECT_EQ(file.xxh3, known->file.xxh3);
      if (file.name != primary) {
        EXPECT_FALSE(known->catalog) << file.name;
      }
    }
    auto known = cold.counters().knownFile(primary);
    ASSERT_TRUE(known && known->catalog);
    EXPECT_EQ(segment.catalog.offset, known->catalog->offset);
    EXPECT_EQ(segment.catalog.length, known->catalog->length);
    EXPECT_EQ(ReadClass::METADATA, known->catalog->readClass);
    EXPECT_EQ(segment.catalog.xxh3, known->catalogXxh3);
  }
  EXPECT_TRUE(cold.counters().knownFile(info->schema_file.name));
}
