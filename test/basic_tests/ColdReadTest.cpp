// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "luxir/reader/SegmentCore.h"
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

void copyWithHeaderFlipped(Directory& from, Directory& to, const api::SegmentInfo& segment) {
  for (const auto& descriptor : segment.files) {
    auto input = from.openFile(descriptor.name);
    auto hold = input->acquire({0, input->size()});
    std::string bytes(hold.getData(), hold.getLength());
    if (descriptor.name == primaryFile(segment)) bytes[0] ^= 1;
    auto file = to.createFile(descriptor.name);
    OutputStream out(file.get());
    out.write(bytes.data(), bytes.size());
    out.close();
    to.finishFile(*file);
  }
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

// A lazy file opens without a header read; its first slice that covers the
// header checks it. A local file checks it at open.
TEST_F(ColdReadTest, aLazyFileChecksItsHeaderOnTheFirstSliceThatCoversIt) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "c_s", "x"), flatdoc("id", "b", "c_s", "y")}, UpdateMessage::COMMIT).success);
  auto& source = h.getIndexWriter()->dir;
  auto info = readDurableIndexInfo(source);
  ASSERT_EQ(1u, info->segments.size());
  const auto& segment = info->segments[0];
  RAMDir copy;
  copyWithHeaderFlipped(source, copy, segment);
  EXPECT_THROW(SegmentCore::create(copy, segment), std::runtime_error);

  ColdDirectory cold(borrow(copy));
  auto core = SegmentCore::create(cold, segment);
  ASSERT_TRUE(core);
  EXPECT_EQ(2, core->rootReader().maxDoc());
  for (const auto& file : segment.files) EXPECT_EQ(0u, cold.counters().fileStats(file.name).firstPageReads) << file.name;
  EXPECT_GT(cold.counters().fileStats(primaryFile(segment)).metadataAcquires, 0u);  // the catalog

  auto size = core->getFile(Postings::PRIMARY_FILE)->size();
  auto before = SegmentCore::wholeFileMaterializations();
  EXPECT_THROW(core->slice({{Postings::PRIMARY_FILE, 0}, size}), std::runtime_error);
  EXPECT_EQ(before + 1, SegmentCore::wholeFileMaterializations());
}
