// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "luxir/server/Promotion.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include <future>
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class CollectionsTest : public LuxirTest {
protected:
  LuxirNode node;
  CollectionHelper h{node, "main"};
  Collections& collections = node.collections();
  CollectionStorage storage = collections.storage().collection("main");
  std::string selected, copy;

  void SetUp() override {
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    selected = *storage.current();
    copy = copySnapshot(storage, selected, Manifest::load(*storage.open(selected)));
  }
  Collections::Prepared prepared() {
    auto candidate = collections.stage("main", copy);
    return collections.prepare(candidate, CommitSnapshot::fromBytes(Manifest::load(candidate.dir()).bytes));
  }
};

TEST_F(CollectionsTest, installActivatesOnlyOverTheExpectedEntry) {
  // A changed map is detected before CURRENT moves.
  EXPECT_THROW(collections.install(prepared(), nullptr), std::runtime_error);
  EXPECT_EQ(selected, storage.current());

  auto installed = collections.install(prepared(), collections.get("main"));
  EXPECT_EQ(installed, collections.get("main"));
  EXPECT_EQ(copy, storage.current());
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
  EXPECT_EQ(1, installed->getReaderManager().getReader()->liveDocs());

  // Abandoning a candidate never removes the selected incarnation.
  auto again = collections.stage("main", copy);
  collections.discard(again);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
}

TEST_F(CollectionsTest, installReplacesUnreadableSelection) {
  auto container = collections.storage().container("main", false);
  container->deleteFile("CURRENT");
  auto file = container->createFile("CURRENT");
  OutputStream out(file.get()); out.write("{", 1); out.close();
  container->finishFile(*file);
  EXPECT_THROW(storage.current(), std::runtime_error);
  collections.install(prepared(), collections.get("main"));
  EXPECT_EQ(copy, storage.current());
}

TEST_F(CollectionsTest, stagedCandidatesSurviveRetirementAndBlockRemoval) {
  auto staged = collections.stage("main", "pending");
  EXPECT_THROW(collections.remove("main"), CollectionUnavailableError);
  collections.install(prepared(), collections.get("main"));
  auto incarnations = storage.incarnations();
  EXPECT_EQ((std::set<std::string>{copy, "pending"}), std::set<std::string>(incarnations.begin(), incarnations.end()));
  collections.discard(staged);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
  collections.remove("main");
  EXPECT_FALSE(collections.get("main"));
}

TEST_F(CollectionsTest, creationOwnsItsNameUntilAnnounced) {
  std::promise<void> initialized, resume;
  auto resumed = resume.get_future().share();
  Signal::listen("collectionInitialized", [&](void*, void*, void*) -> void* {
    initialized.set_value(); resumed.wait(); return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("collectionInitialized"); });
  auto creation = std::async(std::launch::async, [&] { return collections.create("other"); });
  initialized.get_future().wait();
  EXPECT_THROW(collections.remove("other"), CollectionUnavailableError);
  EXPECT_THROW(collections.stage("other", "pending"), CollectionUnavailableError);
  resume.set_value();
  auto created = creation.get();
  ASSERT_TRUE(created->getShard());
  EXPECT_EQ(created, collections.get("other"));
  collections.remove("other");
  EXPECT_FALSE(collections.get("other"));
}
