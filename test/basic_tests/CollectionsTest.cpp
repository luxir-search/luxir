// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "luxir/server/Promotion.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include <future>
#include <latch>
#include <memory_resource>
#include "luxir/server/CommitWaits.h"
#include "luxir/server/Stats.h"
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
  TenantCatalog& storage = collections.catalog();
  std::string selected, copy;

  void SetUp() override {
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    selected = *storage.current("main");
    copy = copySnapshot(storage, "main", selected, Manifest::load(*storage.directory(selected)));
  }
  Collections::Prepared prepared() {
    auto candidate = collections.stage(CollectionId::of("main"), copy);
    return collections.prepare(candidate, CommitSnapshot::fromBytes(Manifest::load(candidate.dir()).bytes));
  }
};

TEST_F(CollectionsTest, installActivatesOnlyOverTheExpectedEntry) {
  // A changed map is detected before the catalog moves.
  EXPECT_THROW(collections.install(prepared(), nullptr), std::runtime_error);
  EXPECT_EQ(selected, storage.current("main"));

  auto installed = collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_EQ(installed, collections.get(CollectionId::of("main")));
  EXPECT_EQ(copy, storage.current("main"));
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations("main"));
  EXPECT_EQ(1, installed->getReaderManager().getReader()->liveDocs());

  // Abandoning a candidate never removes the selected incarnation.
  auto again = collections.stage(CollectionId::of("main"), copy);
  collections.discard(again);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations("main"));
}

TEST_F(CollectionsTest, ownsOneCatalogPerTenant) {
  EXPECT_EQ(&storage, &collections.catalog());
  auto& other = collections.catalog("other");
  EXPECT_NE(&storage, &other);
  EXPECT_EQ(&other, &collections.catalog("other"));
}

TEST_F(CollectionsTest, installPreservesAnUnreadableCatalog) {
  auto container = collections.storage().directory(StorageRoot::AUTHORITATIVE, "t/t0");
  container->publishFile("CATALOG", std::as_bytes(std::span("{", 1)), Directory::PublishMode::REPLACE);
  EXPECT_THROW(storage.current("main"), std::runtime_error);
  EXPECT_THROW(collections.install(prepared(), collections.get(CollectionId::of("main"))), std::runtime_error);
  EXPECT_EQ("{", container->openFile("CATALOG")->read());
}

TEST_F(CollectionsTest, stagedCandidatesSurviveRetirementAndBlockRemoval) {
  auto staged = collections.stage(CollectionId::of("main"), "00000000001");
  EXPECT_THROW(collections.remove(CollectionId::of("main")), CollectionUnavailableError);
  collections.install(prepared(), collections.get(CollectionId::of("main")));
  auto incarnations = storage.incarnations("main");
  EXPECT_EQ((std::set<std::string>{copy, "00000000001"}), std::set<std::string>(incarnations.begin(), incarnations.end()));
  collections.discard(staged);
  EXPECT_EQ(std::vector<std::string>{copy}, storage.incarnations("main"));
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
  EXPECT_THROW(collections.stage(CollectionId::of("other"), "00000000001"), CollectionUnavailableError);
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

TEST_F(CollectionsTest, cachePolicyPrecedesReplacementAndCallbacksCanSetPolicy) {
  auto replacement = prepared();
  auto old = collections.get(CollectionId::of("main"));
  collections.setCacheBytes("", "main", 0);
  Signal::listen("collectionPublishing", [&](void* object, void*, void*) -> void* {
    auto* collection = (Collection*)object;
    EXPECT_EQ(0u, collection->getReaderManager().filterCache->configuration().maxBytes);
    EXPECT_EQ(old, collections.get(CollectionId::of("main")));
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("collectionPublishing"); });
  auto floor = h.getIndexWriter()->snapshots.snapshot()->id;
  floor.index_gen++;
  bool called = false;
  node.getCommitWaits().awaitCommit(CollectionId::of("main"), floor,
      CommitWaits::Clock::now() + std::chrono::minutes(1), {},
      [&](CommitWaits::CommitResult) {
        called = true;
        collections.setCacheBytes("", "main", 8192);
      });
  EXPECT_FALSE(called);
  auto installed = collections.install(std::move(replacement), old);
  EXPECT_TRUE(called);
  EXPECT_EQ(8192u, installed->getReaderManager().filterCache->configuration().maxBytes);
  EXPECT_EQ(0u, old->getReaderManager().filterCache->configuration().maxBytes);
}

TEST_F(CollectionsTest, cachePolicyDefaultAndTenantSelection) {
  collections.setCacheBytes("", "", 8192);
  auto later = collections.create(CollectionId::of("later"));
  EXPECT_EQ(8192u, later->getReaderManager().filterCache->configuration().maxBytes);
  collections.setCacheBytes("", "main", 16384);
  auto acme = collections.create({"acme", "main"});
  EXPECT_EQ(8192u, acme->getReaderManager().filterCache->configuration().maxBytes);
  collections.setCacheBytes("acme", "", 32768);
  EXPECT_EQ(32768u, acme->getReaderManager().filterCache->configuration().maxBytes);
  EXPECT_EQ(16384u, h.getIndexWriter()->snapshots.readers.filterCache->configuration().maxBytes);
  auto future = collections.create({"acme", "future"});
  EXPECT_EQ(8192u, future->getReaderManager().filterCache->configuration().maxBytes);
  collections.setCacheBytes("", "", 4096);
  EXPECT_EQ(4096u, acme->getReaderManager().filterCache->configuration().maxBytes);
  EXPECT_EQ(4096u, h.getIndexWriter()->snapshots.readers.filterCache->configuration().maxBytes);
  // Reinstallation must inherit the cleared exception, too.
  auto installed = collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_EQ(4096u, installed->getReaderManager().filterCache->configuration().maxBytes);
}

TEST_F(CollectionsTest, cachePolicyUnknownNamesAndDeletion) {
  collections.setCacheBytes("", "", 8192);
  EXPECT_THROW(collections.setCacheBytes("", "unknown", 0), CollectionNotFoundError);
  auto created = collections.create(CollectionId::of("unknown"));
  EXPECT_EQ(8192u, created->getReaderManager().filterCache->configuration().maxBytes);
  collections.setCacheBytes("", "main", 0);
  auto candidate = collections.stage(CollectionId::of("main"), "00000000001");
  EXPECT_THROW(collections.remove(CollectionId::of("main")), CollectionUnavailableError);
  collections.discard(candidate);
  auto installed = collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_FALSE(installed->getReaderManager().filterCache->enabled());
  collections.remove(CollectionId::of("main"));
  EXPECT_THROW(collections.setCacheBytes("", "main", 1), CollectionNotFoundError);
  created = collections.create(CollectionId::of("main"));
  EXPECT_EQ(8192u, created->getReaderManager().filterCache->configuration().maxBytes);
}

TEST_F(CollectionsTest, cachePolicyAcceptsUnavailableNamesAndSurvivesRepair) {
  auto old = collections.get(CollectionId::of("main"));
  collections.unload(CollectionId::of("main"), old, "offline");
  api::CacheControlRequest request;
  request.collection = "main";
  request.max_bytes = 0;
  request.dump = true;
  api::CacheControlResponse response;
  std::pmr::monotonic_buffer_resource resource;
  gatherCacheControl(node, request, response, resource);
  ASSERT_EQ(1u, response.collections.size());
  ASSERT_TRUE(response.collections[0].error.has_value());
  EXPECT_EQ(api::Error::Kind::UNAVAILABLE, response.collections[0].error->kind);
  EXPECT_TRUE(response.collections[0].shards.empty());
  auto installed = collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_FALSE(installed->getReaderManager().filterCache->enabled());
}

TEST_F(CollectionsTest, cachePolicyAppliesToCreationAlreadyInFlight) {
  std::latch initialized(1), resume(1);
  Signal::listen("collectionInitialized", [&](void*, void*, void*) -> void* {
    initialized.count_down(); resume.wait(); return nullptr;
  });
  auto creation = std::async(std::launch::async, [&] {
    return collections.getOrCreate(CollectionId::of("later"), true);
  });
  initialized.wait();
  collections.setCacheBytes("", "", 0);
  resume.count_down();
  auto created = creation.get();
  Signal::unlisten("collectionInitialized");
  EXPECT_FALSE(created->getReaderManager().filterCache->enabled());
}

TEST_F(CollectionsTest, concurrentCachePolicySettersPreserveLastAssignment) {
  std::latch disabled(1), resume(1), started(1);
  Signal::listen("filterCacheDisabled", [&](void*, void*, void*) -> void* {
    disabled.count_down(); resume.wait(); return nullptr;
  });
  auto first = std::async(std::launch::async, [&] { collections.setCacheBytes("", "main", 0); });
  disabled.wait();
  auto second = std::async(std::launch::async, [&] {
    started.count_down(); collections.setCacheBytes("", "main", 8192);
  });
  started.wait();
  EXPECT_EQ(std::future_status::timeout, second.wait_for(std::chrono::milliseconds(0)));
  resume.count_down();
  first.get(); second.get();
  Signal::unlisten("filterCacheDisabled");
  auto installed = collections.install(prepared(), collections.get(CollectionId::of("main")));
  EXPECT_EQ(8192u, installed->getReaderManager().filterCache->configuration().maxBytes);
}

TEST(CollectionsMappingTest, swapsPreserveDataAndPublishOneMapping) {
  LuxirNode node;
  auto& collections = node.collections();
  CollectionHelper a(node, "a"), b(node, "b");
  ASSERT_TRUE(a.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(b.indexAll({flatdoc("id", "b"), flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
  auto first = node.getCollection("a"), second = node.getCollection("b");
  auto bytesA = node.storageBytes(CollectionId::of("a")), bytesB = node.storageBytes(CollectionId::of("b"));
  collections.setCacheBytes("", "a", 4096); collections.setCacheBytes("", "b", 8192);
  std::atomic<bool> stop{false}, valid{true};
  auto lookup = std::async(std::launch::async, [&] {
    while (!stop) {
      auto left = collections.get(CollectionId::of("a"));
      auto right = collections.get(CollectionId::of("b"));
      if ((left != first && left != second) || (right != first && right != second)) valid = false;
    }
  });
  auto stopLookup = scope_guard([&] { stop = true; });
  for (int i = 0; i < 11; i++) node.swapCollections(CollectionId::of("a"), CollectionId::of("b"));
  stop = true; lookup.get();
  EXPECT_TRUE(valid);
  EXPECT_EQ(second, node.getCollection("a")); EXPECT_EQ(first, node.getCollection("b"));
  EXPECT_EQ(CollectionId::of("b"), first->getId());
  EXPECT_EQ(bytesB, node.storageBytes(CollectionId::of("a"))); EXPECT_EQ(bytesA, node.storageBytes(CollectionId::of("b")));
  EXPECT_EQ(4096u, second->getReaderManager().filterCache->configuration().maxBytes);
  EXPECT_EQ(8192u, first->getReaderManager().filterCache->configuration().maxBytes);
  std::pmr::monotonic_buffer_resource arena;
  api::StatsRequest request; request.collections = true;
  api::StatsResponse stats;
  gatherStats(node, request, stats, arena);
  for (const auto& row : stats.collections) if (row.name == "a" || row.name == "b") {
    EXPECT_EQ(row.name == "a" ? bytesB : bytesA, row.storage_ram_bytes);
    EXPECT_EQ(row.name == "a" ? 2u : 1u, row.totals.live_docs);
    ASSERT_TRUE(row.activity);
    EXPECT_EQ(row.name == "a" ? 2u : 1u, row.activity->documents_accepted);
  }
  auto advertised = node.getReplication().catalog(arena);
  ASSERT_EQ(1u, advertised.tenants.size());
  EXPECT_EQ(collections.catalog().read().rev, advertised.tenants[0].revision);
  for (const auto& row : advertised.collections) if (row.collection == "a" || row.collection == "b") {
    EXPECT_EQ((row.collection == "a" ? second : first)->getShard()->getSnapshots().snapshot()->id.token(), row.commit);
  }
  node.renameCollection(CollectionId::of("a"), CollectionId::of("renamed"));
  EXPECT_EQ(second, node.getCollection("renamed")); EXPECT_EQ(CollectionId::of("renamed"), second->getId());
  EXPECT_THROW(node.getCollection("a"), CollectionNotFoundError);
  EXPECT_THROW(node.renameCollection(CollectionId::of("b"), CollectionId::of("renamed")), CollectionExistsError);
  EXPECT_THROW(node.swapCollections(CollectionId::of("b"), CollectionId::of("absent")), CollectionNotFoundError);
}
