// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "luxir/store/Manifest.h"
#include "luxir/store/ObjectStoreDirectory.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"

// The write path of an object-store backend, end to end: what a commit costs
// and when the store has it. ObjectStoreDirectoryTest covers the pieces.

using namespace luxir;
using namespace luxir::test;

namespace {

class ObjectStoreWriteTest : public ::testing::Test {
protected:
  std::string name = "write_" + newUuid();
  std::shared_ptr<RamObjectStore> store = RamObjectStore::named(name);
  LuxirConfig config;

  void SetUp() override {
    config.store.backend = "ram_object";
    config.store.options = {{"store_name", name}};
  }
  void TearDown() override {
    store->interceptRequests(nullptr);
    store.reset();
    RamObjectStore::drop(name);
  }

  static std::string keyPrefix(Collection& collection) {
    return "t/t0/main/" + collection.getShard()->getSnapshots().snapshot()->id.incarnation + "/";
  }
  static uint64_t liveDocs(LuxirNode& node) {
    return node.getCollection("main")->getReaderManager().getReader()->liveDocs();
  }
};

}  // namespace

// When a commit returns, the store holds its root and every file the root
// names, and the root went up last. The root is one put under its final name.
TEST_F(ObjectStoreWriteTest, aCommitReturnsWithItsRootAndEveryFileItNamesInTheStore) {
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  store->resetStats();
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "text_un", "alpha"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  auto snapshot = h.collection().getShard()->getSnapshots().snapshot();
  std::string prefix = keyPrefix(h.collection());
  std::string root = prefix + Manifest::name(snapshot->id.index_gen);
  for (const auto& file : snapshot->files) EXPECT_EQ(file.size, store->size(prefix + file.name)) << file.name;

  auto log = store->requestLog();
  auto rootPut = std::ranges::find_if(log, [&](const auto& r) { return r.op == "put" && r.name == root; });
  ASSERT_NE(log.end(), rootPut);
  EXPECT_TRUE(std::none_of(rootPut + 1, log.end(), [](const auto& r) { return r.op == "put"; }));
  EXPECT_EQ(1u, store->requests("put", root));
  EXPECT_EQ(0u, store->requests("put", root + ".pending"));
  EXPECT_EQ(0u, store->requests("copy"));
  EXPECT_EQ(0u, store->requests("list"));
}

// A commit whose files cannot be uploaded fails instead of publishing a root
// the store cannot serve. The writer goes on: the next commit uploads them.
TEST_F(ObjectStoreWriteTest, aFailedUploadFailsTheCommitAndTheNextCommitRecovers) {
  config.store.options["upload_attempts"] = "1";
  std::atomic<bool> down{false};
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
    store->interceptRequests([&](std::string_view op, std::string_view) {
      if (op == "put" && down) throw std::runtime_error("network down");
    });
    down = true;
    auto failed = h.indexAll({flatdoc("id", "b")}, UpdateMessage::COMMIT);
    EXPECT_FALSE(failed.success);
    EXPECT_EQ(1u, liveDocs(node));
    down = false;
    ASSERT_TRUE(h.indexAll({flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
    EXPECT_EQ(3u, liveDocs(node));
  }
  LuxirNode reopened(config);  // nothing local: everything comes from the store
  EXPECT_EQ(3u, liveDocs(reopened));
}

// A merge retires its sources: the writer knows their objects, so it lists
// nothing, and their removals go out in batches.
TEST_F(ObjectStoreWriteTest, aMergeRetiresItsSourcesWithoutListing) {
  std::string prefix;
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    for (const char* id : {"a", "b", "c"}) ASSERT_TRUE(h.indexAll({flatdoc("id", id, "text_un", id)}, UpdateMessage::COMMIT).success);
    store->resetStats();
    ASSERT_TRUE(h.indexAll({flatdoc("id", "d")}, UpdateMessage::COMMIT, false, 1).success);
    prefix = keyPrefix(h.collection());
    EXPECT_EQ(0u, store->requests("list"));
  }
  EXPECT_EQ(0u, store->requests("remove"));
  EXPECT_LE(1u, store->requests("remove_many"));
  // Only the merged segment and the newest root are left.
  std::set<std::string> segments;
  for (const auto& object : store->list(prefix)) {
    std::string_view file = std::string_view(object.name).substr(prefix.size());
    if (file.starts_with("s")) segments.insert(std::string(file.substr(0, file.find('_'))));
  }
  EXPECT_EQ(1u, segments.size());
  LuxirNode reopened(config);
  EXPECT_EQ(4u, liveDocs(reopened));
}

// Two writers on the same keys: the second to publish a generation fails
// instead of replacing the first one's root, and its cleanup leaves that root
// alone, so the first writer's acknowledged commit stays.
TEST_F(ObjectStoreWriteTest, aSecondWriterCannotReplaceOrRemoveAnAcknowledgedCommit) {
  {
    LuxirNode first(config);
    CollectionHelper a(first, "main");
    ASSERT_TRUE(a.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
    LuxirNode second(config);  // opens the same collection at the same generation
    CollectionHelper b(second, "main");
    ASSERT_TRUE(a.indexAll({flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    EXPECT_FALSE(b.indexAll({flatdoc("id", "x")}, UpdateMessage::COMMIT).success);
  }
  LuxirNode reopened(config);
  EXPECT_EQ(2u, liveDocs(reopened));
}

namespace {

// Holds every put while closed, as a slow store would.
class Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool open = true;
  int waiting = 0;

public:
  explicit Gate(RamObjectStore& store) {
    store.interceptRequests([this](std::string_view op, std::string_view) {
      if (op != "put") return;
      std::unique_lock lock(mutex);
      waiting++;
      changed.notify_all();
      changed.wait(lock, [&] { return open; });
      waiting--;
    });
  }
  void close() {
    std::lock_guard lock(mutex);
    open = false;
  }
  void reopen() {
    std::lock_guard lock(mutex);
    open = true;
    changed.notify_all();
  }
  void awaitHeld() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return waiting > 0; });
  }
};

}  // namespace

// A commit waits for the store; updates that do not ask for one do not wait
// for it, and are not held up by the commit that does.
TEST_F(ObjectStoreWriteTest, indexingGoesOnWhileACommitWaitsForTheStore) {
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
  Gate gate(*store);
  gate.close();
  auto committing = std::async(std::launch::async, [&] {
    return h.indexAll({flatdoc("id", "b")}, UpdateMessage::COMMIT).success;
  });
  gate.awaitHeld();
  EXPECT_EQ(std::future_status::timeout, committing.wait_for(std::chrono::milliseconds(50)));
  auto indexing = std::async(std::launch::async, [&] { return h.indexAll({flatdoc("id", "c")}).success; });
  EXPECT_EQ(std::future_status::ready, indexing.wait_for(std::chrono::seconds(10)));
  gate.reopen();
  EXPECT_TRUE(committing.get());
  EXPECT_TRUE(indexing.get());
  ASSERT_TRUE(h.indexAll({flatdoc("id", "d")}, UpdateMessage::COMMIT).success);
  EXPECT_EQ(4u, liveDocs(node));
  store->interceptRequests(nullptr);
}

// A root that reached the store though its put reported a failure (a lost
// response) fails the commit; its generation is never reused, and the next
// commit publishes everything.
TEST_F(ObjectStoreWriteTest, aRootWhoseResponseWasLostFailsTheCommitAndTheNextOneGoesOn) {
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
    std::atomic<int> lost{1};
    store->interceptRequests([&](std::string_view op, std::string_view name) {
      if (op == "put_done" && name.find("/commit_") != std::string_view::npos && lost.fetch_sub(1) > 0) {
        throw std::runtime_error("connection reset");
      }
    });
    EXPECT_FALSE(h.indexAll({flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(h.indexAll({flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
    EXPECT_EQ(3u, liveDocs(node));
  }
  store->interceptRequests(nullptr);
  LuxirNode reopened(config);
  EXPECT_EQ(3u, liveDocs(reopened));
}

// The segment of a commit whose root failed is in the store with nothing
// naming it; a writer that opens the index after a restart removes it.
TEST_F(ObjectStoreWriteTest, aFailedCommitLeavesNothingBehindOnceTheWriterReopens) {
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a")}, UpdateMessage::COMMIT).success);
    store->interceptRequests([&](std::string_view op, std::string_view name) {
      if (op == "put" && name.find("/commit_") != std::string_view::npos) throw std::runtime_error("network down");
    });
    EXPECT_FALSE(h.indexAll({flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  }
  store->interceptRequests(nullptr);
  std::string prefix;
  std::set<std::string> committed;
  {
    LuxirNode reopened(config);
    EXPECT_EQ(1u, liveDocs(reopened));
    auto& collection = *reopened.getCollection("main");
    prefix = keyPrefix(collection);
    auto snapshot = collection.getShard()->getSnapshots().snapshot();
    committed.insert(Manifest::name(snapshot->id.index_gen));
    for (const auto& file : snapshot->files) committed.insert(file.name);
  }
  for (const auto& object : store->list(prefix)) {
    std::string name = object.name.substr(prefix.size());
    if (Manifest::indexFile(name)) {
      EXPECT_TRUE(committed.contains(name)) << name;
    }
  }
}

// Commits from several clients at once each return with their documents in
// the store.
TEST_F(ObjectStoreWriteTest, concurrentCommitsAreEachDurable) {
  constexpr int kClients = 4, kCommits = 10;
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    std::atomic<int> failed{0};
    std::vector<std::thread> clients;
    for (int c = 0; c < kClients; c++) {
      clients.emplace_back([&, c] {
        for (int i = 0; i < kCommits; i++) {
          auto id = std::to_string(c) + "_" + std::to_string(i);
          if (!h.indexAll({flatdoc("id", id)}, UpdateMessage::COMMIT).success) failed++;
        }
      });
    }
    for (auto& client : clients) client.join();
    EXPECT_EQ(0, failed.load());
    EXPECT_EQ((uint64_t)(kClients * kCommits), liveDocs(node));
  }
  LuxirNode reopened(config);
  EXPECT_EQ((uint64_t)(kClients * kCommits), liveDocs(reopened));
}

// A commit that only deletes ships its live docs and its root, nothing else.
TEST_F(ObjectStoreWriteTest, aDeleteOnlyCommitShipsItsLiveDocsAndItsRoot) {
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    store->resetStats();
    ASSERT_TRUE(h.deleteByIds({"a"}, UpdateMessage::COMMIT).success);
    for (const auto& r : store->requestLog()) {
      if (r.op == "put") {
        EXPECT_TRUE(r.name.find("/commit_") != std::string::npos || r.name.find("__L") != std::string::npos) << r.name;
      }
    }
    EXPECT_EQ(0u, store->requests("list"));
  }
  LuxirNode reopened(config);
  EXPECT_EQ(1u, liveDocs(reopened));
}

// Staged on disk: the same, and a closed node leaves its staging directory empty.
TEST_F(ObjectStoreWriteTest, stagingOnDiskShipsTheSameAndCleansUp) {
  auto staging = std::filesystem::temp_directory_path() / ("luxir-staging-" + newUuid());
  std::filesystem::create_directories(staging);
  config.store.options["staging_dir"] = staging.string();
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    for (const char* id : {"a", "b", "c"}) ASSERT_TRUE(h.indexAll({flatdoc("id", id, "text_un", id)}, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(h.indexAll({flatdoc("id", "d")}, UpdateMessage::COMMIT, false, 1).success);
    EXPECT_EQ(4u, liveDocs(node));
  }
  EXPECT_TRUE(std::filesystem::is_empty(staging));
  {
    LuxirNode reopened(config);
    EXPECT_EQ(4u, liveDocs(reopened));
  }
  std::filesystem::remove_all(staging);
}

// A collection deleted and created again under the same name starts empty and
// takes commits: the directories of the deleted one send nothing more.
TEST_F(ObjectStoreWriteTest, aCollectionDeletedAndCreatedAgainStartsEmpty) {
  {
    LuxirNode node(config);
    {
      CollectionHelper h(node, "main");
      ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
    }
    node.deleteCollection("main");
    EXPECT_TRUE(store->list("t/t0/main/").empty());
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
    EXPECT_EQ(1u, liveDocs(node));
  }
  LuxirNode reopened(config);
  EXPECT_EQ(1u, liveDocs(reopened));
}

// A read-only node serves the commit it opened while the writer goes on, merges
// and retires the segments that commit names: opening them fetched what the
// queries read (the hotcache ranges), so they still answer. A query that
// needs a range the node never fetched fails once the writer retired it.
TEST_F(ObjectStoreWriteTest, aReadOnlyNodeServesWhileTheWriterMerges) {
  config.store.options["chunk_bytes"] = "4K";
  LuxirNode writer(config);
  CollectionHelper w(writer, "main");
  for (const char* id : {"a", "b", "c"}) {
    std::vector<Doc> docs;
    docs.push_back(flatdoc("id", id));
    for (int i = 0; i < 500; i++) docs.push_back(flatdoc("id", std::string(id) + std::to_string(i), "text_un", "w" + std::to_string(i * 7919)));
    ASSERT_TRUE(w.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto readOnly = config;
  readOnly.read_only = true;
  LuxirNode reader(readOnly);
  auto count = [&](auto query) {
    auto request = localReq(reader.getSearchEngine());
    query(request->collection("main").topDocs("q")).fields({"id"}).countLimit();
    request->execute();
    EXPECT_EQ("", request->errorCode()) << request->errorMsg();
    return request->getMatchCount();
  };
  ASSERT_TRUE(w.indexAll({flatdoc("id", "d")}, UpdateMessage::COMMIT, false, 1).success);
  EXPECT_EQ(1503, count([](OpCursor& q) -> OpCursor& { return q.allQuery(); }));
  EXPECT_EQ(3, count([](OpCursor& q) -> OpCursor& { return q.matchQuery("text_un", "w39595"); }));
  EXPECT_EQ(1, count([](OpCursor& q) -> OpCursor& { return q.matchQuery("id", "c499"); }));
  EXPECT_EQ(1, count([](OpCursor& q) -> OpCursor& { return q.matchQuery("id", "a250"); }));
}
