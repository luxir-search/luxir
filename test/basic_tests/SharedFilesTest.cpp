// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The lifetime of a file a segment holds by reference (SegInfo::sharedFiles): a
// file not under the segment's own name prefix, which another segment may list
// too. It goes with the last segment that lists it, whether that segment was
// committed (the commit's set difference) or never was (unreferenced shared
// files of a retired segment). No _source_ involved: the rule is the core's.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "luxir/index/IndexWriter.h"
#include "luxir/store/OutputStream.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

namespace {

class SharedFilesTest : public LuxirTest {
protected:
  static FileDescriptor writeFile(Directory& dir, const std::string& name) {
    auto file = dir.createFile(name);
    OutputStream out(file.get());
    std::string bytes(100, 'x');
    out.write(bytes.data(), bytes.size());
    out.close();
    dir.finishFile(*file);
    const std::string names[] = {name};
    dir.sync(names);  // a listed file must be synced before a commit publishes it
    return file->descriptor();
  }

  static bool exists(Directory& dir, std::string_view name) {
    std::vector<Directory::FileInfo> files;
    dir.listFiles(files);
    return std::ranges::any_of(files, [&](const auto& file) { return file.name == name; });
  }

  static bool waitGone(Directory& dir, std::string_view name) {
    for (int i = 0; i < 300 && exists(dir, name); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return !exists(dir, name);
  }

  // Lists `file` under every current segment, as a merge output lists an input's
  // source files. The writer is quiescent here, as in the other segInfos probes.
  static void attachToAll(IndexWriter& iw, const FileDescriptor& file) {
    for (auto& [id, seg] : iw.segInfos) {
      seg->files.push_back(file);
      seg->sharedFiles.push_back(file);
    }
  }

  static size_t listings(const api::IndexInfo& info, std::string_view name) {
    return (size_t)std::ranges::count_if(manifestFiles(info), [&](const auto& file) { return file.name == name; });
  }

  static void flushSegment(IndexWriter& iw, std::string_view id) {
    auto& inverter = iw.obtainInverter();
    inverter.startDoc();
    inverter.getIndexHandler("id").index(inverter, id);
    inverter.finishDoc();
    iw.releaseInverter(inverter, true);
  }
};

}  // namespace

TEST_F(SharedFilesTest, committedFileGoesWhenNoSegmentOfTheNextSnapshotListsIt) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  ASSERT_EQ(2u, helper.durableSegmentCount());

  auto shared = writeFile(iw->dir, "x00");
  attachToAll(*iw, shared);
  // A commit with a change, so a manifest listing x00 under both is written.
  ASSERT_TRUE(helper.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(2u, listings(*readDurableIndexInfo(iw->dir), "x00"));

  // The merge output lists only its own files, so both inputs' retirement
  // leaves nothing listing x00 and the commit retires it.
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  EXPECT_EQ(1u, helper.durableSegmentCount());
  EXPECT_EQ(0u, listings(*readDurableIndexInfo(iw->dir), "x00"));
  EXPECT_TRUE(waitGone(iw->dir, "x00"));
}

TEST_F(SharedFilesTest, committedFileSurvivesWhileAnotherSegmentListsIt) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto shared = writeFile(iw->dir, "x01");
  attachToAll(*iw, shared);  // listed by a's segment only
  ASSERT_TRUE(helper.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  attachToAll(*iw, shared);  // now by both (a's segment lists it twice, harmlessly)
  ASSERT_TRUE(helper.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  ASSERT_GE(listings(*readDurableIndexInfo(iw->dir), "x01"), 2u);

  // Deleting a empties its segment, which the next commit drops: x01 stays,
  // because b's segment still lists it.
  ASSERT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  ASSERT_EQ(2u, helper.durableSegmentCount());
  EXPECT_EQ(1u, listings(*readDurableIndexInfo(iw->dir), "x01"));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(exists(iw->dir, "x01"));

  ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
  EXPECT_EQ(0u, listings(*readDurableIndexInfo(iw->dir), "x01"));
  EXPECT_TRUE(waitGone(iw->dir, "x01"));
}

TEST_F(SharedFilesTest, uncommittedSegmentsFileGoesWhenTheyAreMergedAway) {
  CollectionHelper helper;
  auto iw = helper.getIndexWriter();
  iw->mergePolicy->setMergeFactor(100);
  flushSegment(*iw, "a");
  flushSegment(*iw, "b");
  // Flushes complete in the background; the segments appear once they do.
  for (int i = 0; i < 300 && iw->segInfos.size() < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_EQ(2u, iw->segInfos.size());
  auto shared = writeFile(iw->dir, "x02");
  attachToAll(*iw, shared);

  // Neither input was ever committed, so no snapshot difference can retire
  // x02: the retirement of the dead segments has to.
  iw->mergeSegments();
  ASSERT_EQ(1u, iw->segInfos.size());
  EXPECT_TRUE(iw->segInfos.begin()->second->sharedFiles.empty());
  EXPECT_TRUE(waitGone(iw->dir, "x02"));
}

}  // namespace luxir::test
