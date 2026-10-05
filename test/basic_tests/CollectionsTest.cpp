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
  CollectionStorage storage = collections.storage().collection(CollectionId::of("main"));
  std::string selected, copy;

  void SetUp() override {
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    selected = *storage.current();
    copy = copySnapshot(storage, selected, Manifest::load(*storage.open(selected)));
  }
  Collections::Prepared prepared() {
    auto candidate = collections.stage(CollectionId::of("main"), copy);
    return collections.prepare(candidate, CommitSnapshot::fromBytes(Manifest::load(candidate.dir()).bytes));
  }
};

TEST_F(CollectionsTest, installActivatesOnlyOverTheExpectedEntry) {
  // A changed map is detected before CURRENT moves.
  EXPECT_THROW(collections.install(prepared(), nullptr), std::runtime_error);
  EXPECT_EQ(selected, storage.current());

  auto installed = collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_EQ(installed, collections.get(CollectionId::of("main")));
  EXPECT_EQ(copy, storage.current());
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
  EXPECT_EQ(1, installed->getReaderManager().getReader()->liveDocs());

  // Abandoning a candidate never removes the selected incarnation.
  auto again = collections.stage(CollectionId::of("main"), copy);
  collections.discard(again);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
}

TEST_F(CollectionsTest, installReplacesUnreadableSelection) {
  auto container = collections.storage().container(CollectionId::of("main"), false);
  container->deleteFile("CURRENT");
  auto file = container->createFile("CURRENT");
  OutputStream out(file.get()); out.write("{", 1); out.close();
  container->finishFile(*file);
  EXPECT_THROW(storage.current(), std::runtime_error);
  collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_EQ(copy, storage.current());
}

TEST_F(CollectionsTest, stagedCandidatesSurviveRetirementAndBlockRemoval) {
  auto staged = collections.stage(CollectionId::of("main"), "pending");
  EXPECT_THROW(collections.remove(CollectionId::of("main")), CollectionUnavailableError);
  collections.install(prepared(), collections.get(CollectionId::of("main")));
  auto incarnations = storage.incarnations();
  EXPECT_EQ((std::set<std::string>{copy, "pending"}), std::set<std::string>(incarnations.begin(), incarnations.end()));
  collections.discard(staged);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations());
  collections.remove(CollectionId::of("main"));
  EXPECT_FALSE(collections.get(CollectionId::of("main")));
}

TEST_F(CollectionsTest, creationOwnsItsNameUntilAnnounced) {
  std::promise<void> initialized, resume;
  auto resumed = resume.get_future().share();
  Signal::listen("collectionInitialized", [&](void*, void*, void*) -> void* {
    initialized.set_value(); resumed.wait(); return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("collectionInitialized"); });
  auto creation = std::async(std::launch::async, [&] { return collections.create(CollectionId::of("other")); });
  initialized.get_future().wait();
  EXPECT_THROW(collections.remove(CollectionId::of("other")), CollectionUnavailableError);
  EXPECT_THROW(collections.stage(CollectionId::of("other"), "pending"), CollectionUnavailableError);
  resume.set_value();
  auto created = creation.get();
  ASSERT_TRUE(created->getShard());
  EXPECT_EQ(created, collections.get(CollectionId::of("other")));
  collections.remove(CollectionId::of("other"));
  EXPECT_FALSE(collections.get(CollectionId::of("other")));
}

TEST_F(CollectionsTest, tenantsSeparateSameNamedCollections) {
  CollectionId acme{"acme", "main"};
  CollectionHelper other(node, acme);
  ASSERT_TRUE(other.indexAll({flatdoc("id", "x"), flatdoc("id", "y")}, UpdateMessage::COMMIT).success);
  EXPECT_NE(collections.get(CollectionId::of("main")), collections.get(acme));
  EXPECT_EQ(1, node.getCollection("main")->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(2, node.getCollection(acme)->getReaderManager().getReader()->liveDocs());
  EXPECT_THROW(node.getCollection(CollectionId{"Bad", "main"}), InvalidCollectionNameError);
  EXPECT_THROW(node.getCollection(CollectionId{"absent", "main"}), CollectionNotFoundError);
  node.deleteCollection(acme);
  EXPECT_EQ(1, node.getCollection("main")->getReaderManager().getReader()->liveDocs());
}
