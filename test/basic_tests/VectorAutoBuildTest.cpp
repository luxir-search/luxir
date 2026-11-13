// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <latch>
#include <thread>

using namespace luxir;
using namespace luxir::test;
using namespace std::chrono_literals;
using BuildSite = VectorIndexBuilder::BuildSite;

namespace {

constexpr int DOCS_PER_SEGMENT = 80;

struct BuildHook {
  std::function<void()> onBackgroundBuild;
  explicit BuildHook(std::function<void()> fn) : onBackgroundBuild(std::move(fn)) {
    Signal::listen("vectorBuildField", [this](void*, void* site, void*) -> void* {
      if ((BuildSite)(intptr_t)site == BuildSite::BACKGROUND) onBackgroundBuild();
      return nullptr;
    });
  }
  ~BuildHook() { Signal::unlisten("vectorBuildField"); }
};

// Holds background builds at their start until released.  Declare it after the
// node: if a test bails out early it releases the build that closing the node joins.
struct BuildGate {
  std::latch started{1}, released{1};
  BuildHook hook{[this] {
    if (!started.try_wait()) started.count_down();
    released.wait();
  }};
  ~BuildGate() { release(); }
  void release() { if (!released.try_wait()) released.count_down(); }
};

}  // namespace

class VectorAutoBuildTest : public LuxirTest {
protected:
  IvfTestGuard guard;

  void SetUp() override {
    LuxirTest::SetUp();
    VectorIndexBuilder::ivfBuildCountForTests = 0;
    VectorIndexBuilder::ivfMergeBuildCountForTests = 0;
    VectorIndexBuilder::ivfBackgroundBuildCountForTests = 0;
  }

  static void setSchema(CollectionHelper& h, bool autoBuild = true) {
    SchemaBuilder b;
    auto& f = b.templ("_v");
    f.type = api::FieldDef_::FieldClass::VECTOR;
    f.column = true;
    f.metric = api::VectorMetric::L2;
    f.dims = 4;
    auto ann = pqAnn(2, 1, 1);
    if (autoBuild) ann.auto_build = true;
    f.ann = ann;
    b.set(h.collection());
  }

  static void configure(IndexWriter& iw) {
    iw.overlayBuildDebounce = 0ms;
    iw.overlayBuildRetryMin = 10ms;
    iw.mergePolicy->setMergeFactor(100);
  }

  static void addDocs(CollectionHelper& h, const std::string& tag) {
    std::vector<Doc> docs;
    for (int i = 0; i < DOCS_PER_SEGMENT; i++) {
      docs.push_back(flatdoc("id", tag + std::to_string(i),
                             "embedding_v", std::vector<float>{(float)i, 1.0f, 2.0f, 3.0f}));
    }
    ASSERT_TRUE(h.indexAll(docs).success);
  }

  static void addSegment(CollectionHelper& h, const std::string& tag) {
    addDocs(h, tag);
    h.commit();
  }

  template <class Predicate>
  static bool until(Predicate predicate) {
    auto end = std::chrono::steady_clock::now() + 60s;
    do {
      if (predicate()) return true;
      std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
  }

  static size_t publishedOverlays(IndexWriter& iw) {
    auto info = readDurableIndexInfo(iw.dir);
    size_t count = 0;
    for (const auto& seg : info->segments) {
      for (const auto& overlay : seg.overlays) count += overlay.kind == VectorIndexBuilder::KIND;
    }
    return count;
  }

  static size_t overlayFiles(IndexWriter& iw) {
    std::vector<Directory::FileInfo> files;
    iw.dir.listFiles(files);
    return (size_t)std::ranges::count_if(files, [](const auto& file) {
      return file.name.find("__vec.") != std::string::npos;
    });
  }

  static bool overlaysBecome(IndexWriter& iw, size_t expected) {
    return until([&] { return publishedOverlays(iw) == expected && iw.testOverlayBuildsIdle(); });
  }

  static int64_t backgroundBuilds() { return VectorIndexBuilder::ivfBackgroundBuildCountForTests; }
  static int64_t commitBuilds() { return VectorIndexBuilder::ivfBuildCountForTests; }
};

TEST_F(VectorAutoBuildTest, plainCommitPublishesOverlayWithoutExplicitBuild) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  addSegment(h, "a");
  ASSERT_TRUE(overlaysBecome(*iw, 1));
  EXPECT_EQ(1, backgroundBuilds());
  EXPECT_EQ(0, commitBuilds());
  EXPECT_EQ(0u, iw->stats(false).overlayBacklogSegments);
}

// No request here asks for a commit: flushes fill a level, and the merge's own
// publication carries the overlay of a field no commit ever selected.
TEST_F(VectorAutoBuildTest, writesWithNoCommitAtAll) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  iw->mergePolicy->setMergeFactor(2);
  iw->perInverterMaxDocs = 1;
  addDocs(h, "a");
  addDocs(h, "b");
  ASSERT_TRUE(overlaysBecome(*iw, 1));
  auto info = readDurableIndexInfo(iw->dir);
  EXPECT_EQ(1u, info->segments.size());
  EXPECT_EQ(0, commitBuilds());
}

TEST_F(VectorAutoBuildTest, deferredCommitPublishesOverlay) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  CollectionHelper::UpdateBuilder request;
  for (int i = 0; i < DOCS_PER_SEGMENT; i++) {
    request.add(flatdoc("id", "d" + std::to_string(i), "embedding_v", std::vector<float>{(float)i, 1.0f, 2.0f, 3.0f}));
  }
  request.commitWithin(20);
  ASSERT_TRUE(h.submit(request).success);
  ASSERT_TRUE(overlaysBecome(*iw, 1));
  EXPECT_EQ(0, commitBuilds());
}

TEST_F(VectorAutoBuildTest, fieldWithoutOptInBuildsNothing) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h, false);
  auto iw = h.getIndexWriter();
  configure(*iw);
  addSegment(h, "a");
  addSegment(h, "b");
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(iw->testOverlayBuildsIdle());
  EXPECT_EQ(0u, publishedOverlays(*iw));
  EXPECT_EQ(0, backgroundBuilds());
  EXPECT_FALSE(iw->testActiveVectorOverlayName("vec.embedding_v"));
}

TEST_F(VectorAutoBuildTest, newSegmentOnBuiltFieldGetsOverlay) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  addDocs(h, "a");
  h.commit({"vec.embedding_v"});
  EXPECT_EQ(1, commitBuilds());
  ASSERT_TRUE(overlaysBecome(*iw, 1));
  addSegment(h, "b");
  ASSERT_TRUE(overlaysBecome(*iw, 2));
  EXPECT_EQ(1, backgroundBuilds());
}

TEST_F(VectorAutoBuildTest, enablingOnExistingDataBuildsWithoutWrites) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h, false);
  auto iw = h.getIndexWriter();
  configure(*iw);
  addSegment(h, "a");
  setSchema(h);
  EXPECT_TRUE(overlaysBecome(*iw, 1));
}

TEST_F(VectorAutoBuildTest, commitsRunDuringBuildAndStatsShowBacklog) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  BuildGate gate;
  addSegment(h, "a");
  gate.started.wait();
  auto stats = iw->stats(false);
  EXPECT_EQ(1u, stats.overlayBacklogSegments);
  EXPECT_EQ((uint64_t)DOCS_PER_SEGMENT, stats.overlayBacklogVectors);
  addSegment(h, "b");
  addSegment(h, "c");
  EXPECT_EQ(3u, readDurableIndexInfo(iw->dir)->segments.size());
  EXPECT_EQ(0u, publishedOverlays(*iw));
  gate.release();
  // The build of a held segment comes first, then those that arrived meanwhile.
  ASSERT_TRUE(overlaysBecome(*iw, 3));
  EXPECT_EQ(0u, iw->stats(false).overlayBacklogVectors);
}

TEST_F(VectorAutoBuildTest, segmentMergedAwayDiscardsItsBuild) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  BuildGate gate;
  addSegment(h, "a");
  gate.started.wait();
  addSegment(h, "b");
  iw->mergeSegments();
  EXPECT_EQ(1u, readDurableIndexInfo(iw->dir)->segments.size());
  gate.release();
  ASSERT_TRUE(overlaysBecome(*iw, 1));
  EXPECT_TRUE(until([&] { return overlayFiles(*iw) == 1; }));
  EXPECT_EQ(1, VectorIndexBuilder::ivfMergeBuildCountForTests.load());
}

TEST_F(VectorAutoBuildTest, failedBuildLeavesCommitsWorkingAndRetries) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  ExpectLog quiet("injected background failure");
  std::atomic<int> attempts{0};
  BuildHook hook([&] {
    if (attempts++ < 2) throw std::runtime_error("injected background failure");
  });
  addSegment(h, "a");
  ASSERT_TRUE(until([&] { return attempts.load() >= 1; }));
  addSegment(h, "b");
  ASSERT_TRUE(overlaysBecome(*iw, 2));
  EXPECT_GE(attempts.load(), 3);
  EXPECT_EQ(2u, overlayFiles(*iw));
}

TEST_F(VectorAutoBuildTest, closingDuringBuildIsPromptAndLeavesNothing) {
  std::shared_ptr<IndexWriter> iw;
  std::latch started(1);
  std::atomic<IndexWriter*> writer{nullptr};
  BuildHook hook([&] {
    started.count_down();
    // A build that never reached a cancel point would hold close() here forever,
    // so this only returns because close() marked the writer first.
    while (!writer.load()->isClosed()) std::this_thread::sleep_for(1ms);
  });
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  iw = h.getIndexWriter();
  writer = iw.get();
  configure(*iw);
  addSegment(h, "a");
  started.wait();
  auto begin = std::chrono::steady_clock::now();
  iw->close();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 10s);
  EXPECT_EQ(0, backgroundBuilds());  // cancelled before training started
  EXPECT_EQ(0u, publishedOverlays(*iw));
  EXPECT_EQ(0u, overlayFiles(*iw));
}

TEST_F(VectorAutoBuildTest, deletingCollectionDuringBuildCancelsIt) {
  std::latch started(1);
  std::atomic<IndexWriter*> writer{nullptr};
  std::atomic<bool> giveUp{false};
  BuildHook hook([&] {
    started.count_down();
    while (!writer.load()->isClosed() && !giveUp.load()) std::this_thread::sleep_for(1ms);
  });
  LuxirNode node;
  std::shared_ptr<IndexWriter> iw;
  {
    CollectionHelper h(node, "doomed");
    setSchema(h);
    iw = h.getIndexWriter();
    writer = iw.get();
    configure(*iw);
    addSegment(h, "a");
  }
  started.wait();
  node.deleteCollection("doomed");
  EXPECT_TRUE(until([&] { return iw->isClosed(); }));
  giveUp = true;
  EXPECT_EQ(0, backgroundBuilds());
}

TEST_F(VectorAutoBuildTest, restartSeedsIntentAndBuildsWithoutWrites) {
  auto path = std::filesystem::temp_directory_path() / ("luxir-autobuild-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  {
    // A later restart still owes this overlay: the build is not allowed to finish.
    VectorIndexBuilder::buildFaissAuxIndexes = false;
    auto restore = scope_guard([] { VectorIndexBuilder::buildFaissAuxIndexes = true; });
    LuxirNode node(config);
    CollectionHelper h(node);
    setSchema(h);
    addSegment(h, "a");
    EXPECT_EQ(0u, publishedOverlays(*h.getIndexWriter()));
  }
  LuxirNode node(config);
  CollectionHelper h(node);
  auto iw = h.getIndexWriter();
  EXPECT_TRUE(until([&] { return publishedOverlays(*iw) == 1; }));
  EXPECT_TRUE(iw->testActiveVectorOverlayName("vec.embedding_v"));
}

TEST_F(VectorAutoBuildTest, explicitBuildDuringBackgroundBuildPublishesOnce) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  BuildGate gate;
  addSegment(h, "a");
  gate.started.wait();
  h.commit({"vec.embedding_v"});
  EXPECT_EQ(1, commitBuilds());
  EXPECT_EQ(1u, publishedOverlays(*iw));
  gate.release();
  ASSERT_TRUE(until([&] { return iw->testOverlayBuildsIdle(); }));
  h.commit();
  EXPECT_EQ(1u, publishedOverlays(*iw));
  EXPECT_EQ(1u, overlayFiles(*iw));
  EXPECT_EQ(0, backgroundBuilds());
}

// An explicit build finding a finished background build uses it, whichever got
// there first; none of it may reach the manifest twice.
TEST_F(VectorAutoBuildTest, explicitBuildAfterBackgroundBuildBuildsNothing) {
  LuxirNode node;
  CollectionHelper h(node);
  setSchema(h);
  auto iw = h.getIndexWriter();
  configure(*iw);
  addSegment(h, "a");
  ASSERT_TRUE(overlaysBecome(*iw, 1));
  h.commit({"vec.embedding_v"});
  EXPECT_EQ(0, commitBuilds());
  EXPECT_EQ(1u, publishedOverlays(*iw));
  EXPECT_EQ(1u, overlayFiles(*iw));
}

TEST_F(VectorAutoBuildTest, nodeLimitHoldsSecondCollectionBack) {
  LuxirNode node;
  CollectionHelper a(node, "a_col"), b(node, "b_col");
  for (auto* h : {&a, &b}) {
    setSchema(*h);
    configure(*h->getIndexWriter());
  }
  BuildGate gate;
  addSegment(a, "a");
  gate.started.wait();
  addSegment(b, "b");
  std::this_thread::sleep_for(700ms);  // longer than the limit's retry interval
  EXPECT_EQ(0u, publishedOverlays(*b.getIndexWriter()));
  EXPECT_FALSE(b.getIndexWriter()->testOverlayBuildsRunning());
  gate.release();
  EXPECT_TRUE(overlaysBecome(*a.getIndexWriter(), 1));
  EXPECT_TRUE(overlaysBecome(*b.getIndexWriter(), 1));
}
