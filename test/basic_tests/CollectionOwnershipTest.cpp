// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/server/ReplicationSource.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

using namespace luxir;
using namespace luxir::test;

namespace {
RAMDirFactory* sharedStore = nullptr;

class SharedRAM : public DirectoryFactory {
  RAMDirFactory& shared;
  RAMDirFactory local;
  DirectoryFactory& root(StorageRoot root) { return root == StorageRoot::AUTHORITATIVE ? shared : local; }
public:
  explicit SharedRAM(RAMDirFactory& shared) : shared(shared) {}
  std::string describe() const override { return "ownership_test"; }
  std::shared_ptr<StorageMemory> memory() override { return shared.memory(); }
  std::shared_ptr<Directory> directory(StorageRoot r, std::string_view path, ViewOptions options = {}) override {
    return root(r).directory(r, path, std::move(options));
  }
  std::vector<Child> children(StorageRoot r, std::string_view path) override { return root(r).children(r, path); }
  void removeTree(StorageRoot r, std::string_view path) override { root(r).removeTree(r, path); }
};

const bool registered = [] {
  StorageRegistry::global().addBackend("ownership_test", [](const StorageContext&) {
    if (!sharedStore) throw std::logic_error("ownership test has no shared store");
    return std::make_unique<SharedRAM>(*sharedStore);
  });
  return true;
}();
}

class CollectionOwnershipTest : public LuxirTest {
protected:
  RAMDirFactory shared;
  std::unique_ptr<LuxirNode> a, b;
  void SetUp() override { LuxirTest::SetUp(); sharedStore = &shared; }
  void TearDown() override { b.reset(); a.reset(); sharedStore = nullptr; LuxirTest::TearDown(); }
  LuxirConfig config(std::string id) {
    LuxirConfig config;
    config.node.id = std::move(id);
    config.store.backend = "ownership_test";
    config.store.checked_dir.sync = "throw";
    config.server.http.port = 0;
    return config;
  }
  void startA() { a = std::make_unique<LuxirNode>(config("A")); }
  void startB() { b = std::make_unique<LuxirNode>(config("B")); }
  std::string catalogBytes() { return std::string(shared.directory(StorageRoot::AUTHORITATIVE, "t/t0")->openFile("CATALOG")->read()); }
  template <class F> void servedBy(F&& operation, std::string_view node = "A") {
    try { operation(); ADD_FAILURE() << "operation succeeded"; }
    catch (const CollectionUnavailableError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("served by " + std::string(node)));
    }
  }
};

TEST_F(CollectionOwnershipTest, otherNodeServesPlaceholdersWithoutChangingTheStore) {
  startA();
  CollectionHelper c(*a, "c");
  ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success);
  auto before = catalogBytes();
  startB();
  EXPECT_EQ("served by A", b->collections().get(CollectionId::of("c"))->getUnavailableReason());
  servedBy([&] { CollectionHelper update(*b, "c"); update.index(flatdoc("id", "blocked")); });
  servedBy([&] { b->getOrCreateCollection("c"); });
  EXPECT_EQ(before, catalogBytes());
  ASSERT_TRUE(c.index(flatdoc("id", "second"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(2, c.collection().getReaderManager().getReader()->liveDocs());
  a->createCollection("late");
  before = catalogBytes();
  servedBy([&] { b->getOrCreateCollection("late"); });
  EXPECT_EQ(before, catalogBytes());
}

TEST_F(CollectionOwnershipTest, detachCommitsBufferedDocumentsAndAttachOpensThem) {
  startA();
  CollectionHelper c(*a, "c");
  ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success);
  startB();
  ASSERT_TRUE(c.index(flatdoc("id", "buffered")).success);
  auto incarnation = *a->collections().catalog().current("c");
  a->detachCollection("c", "B");
  EXPECT_TRUE(c.getIndexWriter()->isClosed());
  servedBy([&] { a->getCollection("c"); }, "B");
  auto rejected = c.index(flatdoc("id", "late")); // A request retaining the old collection.
  EXPECT_FALSE(rejected.success);
  EXPECT_EQ(ErrorKind::UNAVAILABLE, rejected.error_kind);
  std::pmr::monotonic_buffer_resource arena;
  auto advertised = a->getReplication().catalog(arena);
  auto row = std::ranges::find_if(advertised.collections, [](const auto& row) { return row.collection == "c"; });
  ASSERT_NE(advertised.collections.end(), row); // Unavailable is not a deletion to followers.
  EXPECT_FALSE(row->available);
  EXPECT_EQ(2, b->attachCollection("c")->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(incarnation, b->collections().catalog().current("c"));
  CollectionHelper writing(*b, "c");
  ASSERT_TRUE(writing.index(flatdoc("id", "third"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(3, writing.collection().getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, forceAttachTakesOverTheLastCommitOfAStoppedNode) {
  startA();
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "durable"), UpdateMessage::COMMIT).success); }
  startB();
  auto before = catalogBytes();
  auto placeholder = b->collections().get(CollectionId::of("c"));
  servedBy([&] { b->attachCollection("c"); });
  EXPECT_EQ(before, catalogBytes());
  EXPECT_EQ(placeholder, b->collections().get(CollectionId::of("c")));
  a.reset();
  EXPECT_EQ(1, b->attachCollection("c", true)->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ("B", b->collections().catalog().find("c")->node);
}

TEST_F(CollectionOwnershipTest, failedFinalCommitStillClosesAndHandsOff) {
  startA();
  CollectionHelper c(*a, "c");
  ASSERT_TRUE(c.index(flatdoc("id", "durable"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(c.index(flatdoc("id", "buffered")).success);
  startB();
  auto writer = c.getIndexWriter();
  Signal::listen("commitDataSync", [&](void* source, void*, void*) -> void* {
    if (source == writer.get()) throw std::runtime_error("injected final commit failure");
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("commitDataSync"); });
  EXPECT_THROW(a->detachCollection("c", "B"), std::runtime_error);
  EXPECT_TRUE(writer->isClosed());
  EXPECT_EQ("B", b->collections().catalog().find("c")->node);
  servedBy([&] { a->getCollection("c"); }, "B");
  EXPECT_EQ(1, b->attachCollection("c")->getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, attachAndDetachValidateTheirTargetsAndLoadedState) {
  startA(); a->createCollection("c"); startB();
  EXPECT_THROW(a->attachCollection("missing"), CollectionNotFoundError);
  EXPECT_THROW(a->attachCollection("missing", true), CollectionNotFoundError);
  EXPECT_THROW(a->attachCollection("c"), CollectionExistsError);
  EXPECT_THROW(a->attachCollection("c", true), CollectionExistsError);
  EXPECT_THROW(a->detachCollection("missing", "B"), CollectionNotFoundError);
  servedBy([&] { b->detachCollection("c", "A"); });
  auto before = catalogBytes();
  for (auto target : {"A", "", ".", "../B", "B\\C"}) EXPECT_THROW(a->detachCollection("c", target), std::invalid_argument);
  EXPECT_EQ(before, catalogBytes());
}

TEST_F(CollectionOwnershipTest, detachRefusesAStagedReplacement) {
  startA(); auto collection = a->createCollection("c");
  auto candidate = a->collections().stage(CollectionId::of("c"), "00000000001");
  auto before = catalogBytes();
  EXPECT_THROW(a->detachCollection("c", "B"), CollectionUnavailableError);
  EXPECT_FALSE(collection->getShard()->getIndexWriter()->isClosed());
  EXPECT_EQ(before, catalogBytes());
  a->collections().discard(candidate);
}

TEST_F(CollectionOwnershipTest, failedAssignmentUnloadsToTheCurrentOwner) {
  startA(); auto collection = a->createCollection("c");
  auto& catalog = a->collections().catalog();
  auto incarnation = *catalog.current("c");
  Signal::listen("catalogRead", [&](void* source, void*, void*) -> void* {
    if (source == &catalog) {
      EXPECT_TRUE(collection->getShard()->getIndexWriter()->isClosed());
      TenantCatalog other(shared, "t0", "A");
      EXPECT_TRUE(other.assign(incarnation, "A", "C"));
    }
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("catalogRead"); });
  EXPECT_THROW(a->detachCollection("c", "B"), CollectionUnavailableError);
  servedBy([&] { a->getCollection("c"); }, "C");
  EXPECT_EQ("C", catalog.find("c")->node);
}

TEST_F(CollectionOwnershipTest, assignmentIoFailureStillUnloadsTheClosedWriter) {
  startA(); auto collection = a->createCollection("c");
  auto& catalog = a->collections().catalog();
  Signal::listen("catalogRead", [&](void* source, void*, void*) -> void* {
    if (source == &catalog) throw FileIOException("injected assignment failure");
    return nullptr;
  });
  EXPECT_THROW(a->detachCollection("c", "B"), FileIOException);
  Signal::unlisten("catalogRead");
  EXPECT_TRUE(collection->getShard()->getIndexWriter()->isClosed());
  servedBy([&] { a->getCollection("c"); });
  EXPECT_NE(collection, a->attachCollection("c"));
}

TEST_F(CollectionOwnershipTest, deletionAfterForceAssignmentCannotBecomeAnAutoCreate) {
  startA(); a->createCollection("c"); startB(); a.reset();
  auto& catalog = b->collections().catalog();
  auto placeholder = b->collections().get(CollectionId::of("c"));
  Signal::listen("catalogPublished", [&](void* source, void*, void*) -> void* {
    if (source == &catalog) {
      TenantCatalog owner(shared, "t0", "B");
      owner.remove("c");
    }
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("catalogPublished"); });
  EXPECT_THROW(b->attachCollection("c", true), CollectionUnavailableError);
  EXPECT_FALSE(catalog.find("c"));
  EXPECT_EQ(placeholder, b->collections().get(CollectionId::of("c")));
}

TEST_F(CollectionOwnershipTest, failedAttachKeepsThePlaceholderAndUpdatesItsReason) {
  startA(); a->createCollection("c"); startB();
  a->detachCollection("c", "B");
  auto placeholder = b->collections().get(CollectionId::of("c"));
  Signal::listen("collectionInitialized", [](void*, void*, void*) -> void* { throw std::runtime_error("injected open failure"); });
  EXPECT_THROW(b->attachCollection("c"), std::runtime_error);
  Signal::unlisten("collectionInitialized");
  EXPECT_EQ(placeholder, b->collections().get(CollectionId::of("c")));
  EXPECT_NE(std::string::npos, placeholder->getUnavailableReason().find("injected open failure"));
  EXPECT_NE(placeholder, b->attachCollection("c"));
}

TEST_F(CollectionOwnershipTest, detachAndReattachPreserveTheCachePolicy) {
  startA(); a->createCollection("c");
  a->collections().setCacheBytes("", "c", 4096);
  auto incarnation = *a->collections().catalog().current("c");
  a->detachCollection("c", "B");
  auto collection = a->attachCollection("c", true);
  EXPECT_EQ(4096u, collection->getReaderManager().filterCache->configuration().maxBytes);
  EXPECT_EQ(incarnation, a->collections().catalog().current("c"));
}

TEST_F(CollectionOwnershipTest, detachKeepsTheFilesystemCollectionForTheTarget) {
  auto cfg = config("A");
  cfg.store.backend = "fs";
  auto path = std::filesystem::temp_directory_path() / ("luxir-detach-" + newUuid());
  auto cleanup = scope_guard([&] { b.reset(); a.reset(); std::filesystem::remove_all(path); });
  cfg.store.data_dir = path.string();
  a = std::make_unique<LuxirNode>(cfg);
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "durable"), UpdateMessage::COMMIT).success); }
  {
    auto readOnly = cfg; readOnly.read_only = true;
    LuxirNode reader(readOnly);
    EXPECT_THROW(reader.detachCollection("c", "B"), ReadOnlyError);
    EXPECT_THROW(reader.attachCollection("c", true), ReadOnlyError);
  }
  auto incarnation = *a->collections().catalog().current("c");
  a->detachCollection("c", "B"); a.reset();
  cfg.node.id = "B";
  b = std::make_unique<LuxirNode>(cfg);
  EXPECT_EQ(incarnation, b->collections().catalog().current("c"));
  EXPECT_EQ(1, b->getCollection("c")->getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, followerRefusesWriterHandoffHooks) {
  LuxirConfig cfg;
  cfg.node.id = "follower";
  cfg.replication.source = "http://127.0.0.1:1";
  LuxirNode follower(cfg, LuxirNode::Mode::PULL);
  EXPECT_THROW(follower.detachCollection("main", "B"), ReadOnlyError);
  EXPECT_THROW(follower.attachCollection("main", true), ReadOnlyError);
}

TEST_F(CollectionOwnershipTest, startupLeavesAnotherNodesReservation) {
  startA();
  auto& catalog = a->collections().catalog();
  bool visited = false;
  Signal::listen("catalogPublished", [&](void* source, void*, void*) -> void* {
    if (source != &catalog || visited || !catalog.find("d")) return nullptr;
    visited = true;
    auto before = catalogBytes();
    startB();
    EXPECT_EQ(before, catalogBytes());
    EXPECT_EQ("reserved", catalog.find("d")->state);
    return nullptr;
  });
  a->createCollection("d");
  ASSERT_TRUE(visited);
  EXPECT_EQ("active", catalog.find("d")->state);
  servedBy([&] { b->getCollection("d"); });
}

TEST_F(CollectionOwnershipTest, anotherNodeWinsTheDefaultReservation) {
  TenantCatalog::initialize(shared, true);
  bool raced = false;
  Signal::listen("catalogRead", [&](void*, void*, void*) -> void* {
    if (!raced) { raced = true; startA(); }
    return nullptr;
  });
  startB();
  ASSERT_TRUE(raced);
  EXPECT_EQ("served by A", b->collections().get(CollectionId::of("main"))->getUnavailableReason());
  EXPECT_EQ("A", a->collections().catalog().find("main")->node);
}

TEST_F(CollectionOwnershipTest, conditionalClaimCannotFenceTheNewlyAssignedWriter) {
  startA();
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success); }
  auto id = *a->collections().catalog().current("c");
  a.reset();
  TenantCatalog catalog(shared, "t0", "A");
  ASSERT_TRUE(catalog.assign(id, "A", "B"));
  auto epoch = IncarnationState::read(*catalog.directory(id))->value.epoch;
  bool raced = false;
  Signal::listen("collectionClaimChecked", [&](void*, void* value, void*) -> void* {
    const auto& entry = *(TenantCatalog::Entry*)value;
    if (entry.id != id || entry.node != "B" || raced) return nullptr;
    raced = true;
    EXPECT_TRUE(catalog.assign(id, "B", "A"));
    startA();
    CollectionHelper c(*a, "c");
    EXPECT_TRUE(c.index(flatdoc("id", "second"), UpdateMessage::COMMIT).success);
    return nullptr;
  });
  startB();
  ASSERT_TRUE(raced);
  servedBy([&] { b->getCollection("c"); });
  EXPECT_EQ(epoch + 1, IncarnationState::read(*catalog.directory(id))->value.epoch);
  EXPECT_EQ(2, a->getCollection("c")->getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, lifecycleRequiresTheOwnerInsideTheCatalogMutation) {
  startA(); a->createCollection("c");
  auto replacement = a->collections().catalog().reserve("c");
  startB(); b->createCollection("b");
  auto before = catalogBytes();
  auto placeholder = b->collections().get(CollectionId::of("c"));
  servedBy([&] { b->deleteCollection("c"); });
  EXPECT_EQ(placeholder, b->collections().get(CollectionId::of("c")));
  servedBy([&] { b->renameCollection(CollectionId::of("c"), CollectionId::of("renamed")); });
  servedBy([&] { b->swapCollections(CollectionId::of("c"), CollectionId::of("b")); });
  auto& catalog = b->collections().catalog();
  servedBy([&] { catalog.remove("c"); });
  servedBy([&] { catalog.rename("c", "renamed"); });
  servedBy([&] { catalog.swap("c", "b"); });
  servedBy([&] { catalog.activate("c", replacement); });
  servedBy([&] { catalog.removeIncarnation(replacement); });
  EXPECT_FALSE(catalog.assign(replacement, "B", "B"));
  EXPECT_THROW(b->createCollection("c"), CollectionExistsError);
  EXPECT_EQ(before, catalogBytes());
}

TEST_F(CollectionOwnershipTest, sharedCleanupKeepsOtherNodesEntriesAndOrphanTrees) {
  startA();
  auto& catalog = a->collections().catalog();
  auto pending = catalog.reserve("pending_a");
  auto retiring = catalog.reserve("retiring_a");
  catalog.retire("retiring_a");
  TenantCatalog other(a->collections().storage(), "t0", "B");
  auto removed = other.reserve("pending_b");
  auto orphan = shared.directory(StorageRoot::AUTHORITATIVE, "t/t0/i/ffffffffffe");
  orphan->publishFile("data", {}, Directory::PublishMode::CREATE_ONLY);
  startB();
  EXPECT_EQ("reserved", catalog.owned(pending).state);
  EXPECT_EQ("retiring", catalog.owned(retiring).state);
  EXPECT_FALSE(other.find("pending_b"));
  EXPECT_FALSE(other.directory(removed)->readPublished("STATE"));
  EXPECT_TRUE(orphan->openFile("data"));
  a.reset(); startA();
  EXPECT_FALSE(a->collections().catalog().find("pending_a"));
  EXPECT_FALSE(a->collections().catalog().directory(retiring)->readPublished("STATE"));
  EXPECT_TRUE(orphan->openFile("data"));
}

TEST_F(CollectionOwnershipTest, failedCreateRollsBackOnlyItsOwnIncarnation) {
  startA(); startB();
  bool raced = false;
  Signal::listen("collectionInitialized", [&](void* value, void*, void*) -> void* {
    auto& collection = *(Collection*)value;
    if (collection.getId().name != "d" || raced) return nullptr;
    raced = true;
    auto& catalog = a->collections().catalog();
    catalog.retire("d", *catalog.current("d"));
    CollectionHelper replacement(*b, "d");
    EXPECT_TRUE(replacement.index(flatdoc("id", "winner"), UpdateMessage::COMMIT).success);
    throw std::runtime_error("first create failed");
  });
  EXPECT_THROW(a->createCollection("d"), std::runtime_error);
  ASSERT_TRUE(raced);
  auto entry = b->collections().catalog().find("d");
  ASSERT_TRUE(entry);
  EXPECT_EQ("B", entry->node);
  EXPECT_EQ("active", entry->state);
  EXPECT_EQ(1u, b->collections().catalog().incarnations("d").size());
  EXPECT_EQ(1, b->getCollection("d")->getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, mutationRechecksAssignmentAfterAConflict) {
  startA(); startB(); b->createCollection("b");
  auto& catalog = b->collections().catalog();
  auto id = *catalog.current("b");
  std::string assigned;
  Signal::listen("catalogRead", [&](void* source, void*, void*) -> void* {
    if (source == &catalog) {
      EXPECT_TRUE(a->collections().catalog().assign(id, "B", "A"));
      assigned = catalogBytes();
    }
    return nullptr;
  });
  servedBy([&] { catalog.rename("b", "renamed"); });
  EXPECT_EQ(assigned, catalogBytes());
  EXPECT_EQ(id, catalog.current("b"));
}

TEST_F(CollectionOwnershipTest, sameIdRestartReopensItsCollections) {
  startA();
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success); }
  auto before = catalogBytes();
  auto id = *a->collections().catalog().current("c");
  a.reset(); startA();
  EXPECT_EQ(before, catalogBytes());
  EXPECT_EQ(id, a->collections().catalog().current("c"));
  EXPECT_EQ(1, a->getCollection("c")->getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, newOwnerRetainsThePreviousOwnersSnapshots) {
  startA();
  std::shared_ptr<const CommitSnapshot> old;
  std::shared_ptr<Directory> dir;
  std::string incarnation;
  {
    CollectionHelper c(*a, "c");
    ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success);
    old = c.collection().getShard()->getSnapshots().snapshot();
    dir = c.collection().getShard()->getDirectory();
    ASSERT_TRUE(c.index(flatdoc("id", "second"), UpdateMessage::COMMIT, false, 1).success);
    incarnation = *a->collections().catalog().current("c");
  }
  ASSERT_TRUE(a->collections().catalog().assign(incarnation, "A", "B"));
  a.reset(); startB();
  ASSERT_TRUE(dir->openFile(old->manifestName()));
  CollectionHelper c(*b, "c");
  ASSERT_TRUE(c.index(flatdoc("id", "third"), UpdateMessage::COMMIT, false, 1).success);
  EXPECT_TRUE(dir->openFile(old->manifestName()));
  for (const auto& file : old->files) EXPECT_TRUE(dir->openFile(file.name));
  EXPECT_EQ(1, IndexReader(*dir, *old).liveDocs());
  EXPECT_EQ(3, c.collection().getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, sharedFilesystemReopensOnlyTheAssignedCollections) {
  auto cfg = config("A");
  cfg.store.backend = "shared_fs";
  auto path = std::filesystem::temp_directory_path() / ("luxir-shared-fs-" + newUuid());
  auto cleanup = scope_guard([&] { b.reset(); a.reset(); std::filesystem::remove_all(path); });
  cfg.store.data_dir = path.string();
  a = std::make_unique<LuxirNode>(cfg);
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success); }
  cfg.node.id = "B";
  b = std::make_unique<LuxirNode>(cfg);
  EXPECT_FALSE(b->collections().storage().exclusive());
  servedBy([&] { b->getCollection("c"); });
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "second"), UpdateMessage::COMMIT).success); }
  auto incarnation = *a->collections().catalog().current("c");
  ASSERT_TRUE(a->collections().catalog().assign(incarnation, "A", "B"));
  a.reset(); b.reset();
  b = std::make_unique<LuxirNode>(cfg);
  EXPECT_EQ(incarnation, b->collections().catalog().current("c"));
  EXPECT_EQ(2, b->getCollection("c")->getReaderManager().getReader()->liveDocs());
  { CollectionHelper c(*b, "c"); ASSERT_TRUE(c.index(flatdoc("id", "third"), UpdateMessage::COMMIT).success); }
  cfg.node.id = "A";
  a = std::make_unique<LuxirNode>(cfg);
  servedBy([&] { a->getCollection("c"); }, "B");
}

TEST_F(CollectionOwnershipTest, sharedFilesystemRefusesToFollow) {
  auto cfg = config("A");
  cfg.store.backend = "shared_fs";
  cfg.replication.source = "http://127.0.0.1:1";
  EXPECT_THROW(LuxirNode node(cfg), std::invalid_argument);
}

TEST_F(CollectionOwnershipTest, exclusiveStartupAdoptsAllEntriesInOneMutation) {
  auto cfg = config("A");
  cfg.store.backend = "fs";
  auto path = std::filesystem::temp_directory_path() / ("luxir-ownership-" + newUuid());
  auto cleanup = scope_guard([&] { b.reset(); a.reset(); std::filesystem::remove_all(path); });
  cfg.store.data_dir = path.string();
  a = std::make_unique<LuxirNode>(cfg);
  { CollectionHelper c(*a, "c"); ASSERT_TRUE(c.index(flatdoc("id", "first"), UpdateMessage::COMMIT).success); }
  auto id = *a->collections().catalog().current("c");
  auto rev = a->collections().catalog().read().rev;
  a.reset(); cfg.node.id = "B";
  b = std::make_unique<LuxirNode>(cfg);
  EXPECT_TRUE(b->collections().storage().exclusive());
  EXPECT_EQ(rev + 1, b->collections().catalog().read().rev);
  for (const auto& entry : b->collections().catalog().read().entries) EXPECT_EQ("B", entry.node);
  EXPECT_EQ(id, b->collections().catalog().current("c"));
  EXPECT_EQ(1, b->getCollection("c")->getReaderManager().getReader()->liveDocs());
}

TEST_F(CollectionOwnershipTest, sharedStoreRefusesOnlyTheRandomFallback) {
  EXPECT_THROW(LuxirNode node(config("")), std::invalid_argument);
  EXPECT_TRUE(shared.children(StorageRoot::AUTHORITATIVE, "").empty());
  a = std::make_unique<LuxirNode>(config(newUuid()));
  EXPECT_FALSE(a->getConfig().node.random);
  EXPECT_FALSE(a->collections().storage().exclusive());
}
