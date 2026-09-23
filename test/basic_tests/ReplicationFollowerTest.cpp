// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/SchemaBuilder.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/ReplicationFollower.h"
#include "luxir/server/ReplicationCatalog.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include "luxir/store/Manifest.h"
#include "luxir/luxir_main.h"
#include <sstream>
#include <filesystem>
#include <fstream>
#include <latch>
#include <future>

namespace luxir::test {
using namespace std::chrono_literals;

class ReplicationFollowerTest : public LuxirTest {
protected:
  std::filesystem::path path;
  LuxirConfig sourceConfig, followerConfig;
  std::unique_ptr<LuxirNode> source, follower;
  std::unique_ptr<HttpServer> sourceServer, followerServer;
  int sourcePort = 0;
  void SetUp() override {
    LuxirTest::SetUp();
    path = std::filesystem::temp_directory_path() / ("luxir-follower-" + newUuid());
    sourceConfig.store.backend = followerConfig.store.backend = "fs";
    sourceConfig.store.checked_dir.sync = followerConfig.store.checked_dir.sync = "throw";
    sourceConfig.store.data_dir = (path / "source").string();
    followerConfig.store.data_dir = (path / "follower").string();
    sourceConfig.replication.follower_timeout_ms = 1000;
  }
  void startSource() {
    source = std::make_unique<LuxirNode>(sourceConfig);
    sourceServer = std::make_unique<HttpServer>(*source, 1, sourcePort);
    sourceServer->start();
    sourcePort = sourceServer->getPort();
  }
  void stopSource() { sourceServer.reset(); source.reset(); }
  void startFollower() {
    followerConfig.replication.source = "http://127.0.0.1:" + std::to_string(sourcePort);
    follower = std::make_unique<LuxirNode>(followerConfig);
    followerServer = std::make_unique<HttpServer>(*follower, 1, 0);
    followerServer->start();
  }
  void stopFollower() { followerServer.reset(); follower.reset(); }
  void TearDown() override {
    stopFollower(); stopSource();
    std::filesystem::remove_all(path);
    LuxirTest::TearDown();
  }
  template<class Predicate> bool until(Predicate predicate) {
    auto end = std::chrono::steady_clock::now() + 10s;
    do { if (predicate()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < end);
    return false;
  }
  bool caughtUp(std::string name = "main") {
    auto target = source->getCollection(name)->getShard()->getSnapshots().snapshot()->id;
    return until([&] {
      try { return follower->getCollection(name)->getShard()->getSnapshots().snapshot()->id == target; }
      catch (const CollectionNotFoundError&) { return false; }
    });
  }
  std::string status() { return httpRequest(followerServer->getPort(), http::verb::get, "/_replication/status").body(); }
  bool stateIs(std::string_view state) {
    glz::generic json;
    if (glz::read_json(json, status()) || !json.contains("collections")) return false;
    for (const auto& row : json["collections"].get<glz::generic::array_t>()) if (row["state"].get<std::string>() == state) return true;
    return false;
  }
  std::pair<int, std::string> command(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    std::ostringstream output;
    auto cout = std::cout.rdbuf(output.rdbuf()), cerr = std::cerr.rdbuf(output.rdbuf());
    auto level = spdlog::get_level();
    auto restore = scope_guard([&] { std::cout.rdbuf(cout); std::cerr.rdbuf(cerr); spdlog::set_level(level); });
    int code = luxir_main((int)argv.size(), argv.data());
    return {code, output.str()};
  }
  std::pair<int, std::string> pull(std::string url = {}) {
    if (url.empty()) url = "http://127.0.0.1:" + std::to_string(sourcePort);
    // Both requested server ports are occupied by the source: pull must not bind.
    return command({"luxir", "--server.http.port", std::to_string(sourcePort),
        "--server.grpc.port", std::to_string(sourcePort), "pull", url, followerConfig.store.data_dir});
  }
  void exerciseSnapshots() {
    startSource(); startFollower();
    ASSERT_TRUE(caughtUp());
    EXPECT_EQ(0, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
    CollectionHelper h(*source, "main");
    ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "c")}, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(caughtUp());
    auto col = follower->getCollection("main");
    EXPECT_FALSE(col->getShard()->getIndexWriter());
    HttpReq search(followerServer->getPort());
    search.matchQuery("id", "a").withStats().execute();
    EXPECT_EQ(200, search.status()); EXPECT_EQ(1, search.found());
    EXPECT_THROW(follower->createCollection(nullptr, "blocked"), ReadOnlyError);
    EXPECT_THROW(follower->deleteCollection("main"), ReadOnlyError);
    EXPECT_THROW(col->setSchema(col->getSchema()), ReadOnlyError);
    EXPECT_EQ(403, httpRequest(followerServer->getPort(), http::verb::post, "/collections/main/_update", "{\"id\":\"x\"}\n", "application/x-ndjson").result_int());
    ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
    ASSERT_TRUE(caughtUp());
    auto beforeDelete = col->getShard()->getSnapshots().snapshot();
    ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
    ASSERT_TRUE(caughtUp());
    EXPECT_EQ(2, col->getReaderManager().getReader()->liveDocs());
    auto afterDelete = col->getShard()->getSnapshots().snapshot();
    uint64_t missingBytes = 0;
    for (const auto& file : afterDelete->files) if (std::ranges::find(beforeDelete->files, file) == beforeDelete->files.end()) missingBytes += file.size;
    std::pmr::monotonic_buffer_resource deleteArena;
    api::ReplicationStatus deleteStats;
    follower->getFollower()->stats(deleteStats, deleteArena);
    EXPECT_GT(missingBytes, 0u);
    EXPECT_EQ(missingBytes, deleteStats.collections[0].bytes_downloaded);
    auto previousReader = col->getReaderManager().getReader();
    SchemaBuilder builder; builder.field("extra_s").type = api::FieldDef::FieldClass::STRING;
    h.collection().setSchema(builder.build(h.collection().getSchema().get()));
    ASSERT_TRUE(caughtUp());
    EXPECT_EQ(&previousReader->segments()[0], &col->getReaderManager().getReader()->segments()[0]);
    std::pmr::monotonic_buffer_resource arena;
    api::ReplicationStatus stats;
    follower->getFollower()->stats(stats, arena);
    ASSERT_EQ(1u, stats.collections.size());
    EXPECT_EQ(0u, stats.collections[0].bytes_downloaded);
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(h.submit(merge).success);
    ASSERT_TRUE(caughtUp());
    EXPECT_EQ(2, col->getReaderManager().getReader()->liveDocs());
    EXPECT_EQ(200, httpRequest(followerServer->getPort(), http::verb::get, "/_stats").result_int());
  }
};

TEST_F(ReplicationFollowerTest, largeFileUsesLargeSocketReads) {
  startSource();
  CollectionHelper h(*source, "main");
  SchemaBuilder builder;
  auto& field = builder.field("payload");
  field.type = api::FieldDef::FieldClass::STRING;
  field.index = api::FieldDef::IndexMode::NONE; field.column = false; field.stored = true;
  h.collection().setSchema(builder.build(h.collection().getSchema().get()));
  std::string payload(8 * 1024 * 1024, 'a');
  uint32_t random = 1;
  for (auto& c : payload) { random = random * 1664525 + 1013904223; c = (char)(' ' + (random >> 24) % 95); }
  ASSERT_TRUE(h.index(flatdoc("id", "a", "payload", payload), UpdateMessage::COMMIT).success);
  size_t reads = 0, bytes = 0;
  auto join = scope_guard([&] { stopFollower(); });
  Signal::listen("replicationBodyRead", [&](void* received, void* length, void*) -> void* {
    if (*(uint64_t*)length >= 1024 * 1024) { reads++; bytes += *(size_t*)received; }
    return nullptr;
  });
  startFollower();
  ASSERT_TRUE(caughtUp());
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  stopFollower();
  ASSERT_GT(reads, 0u);
  EXPECT_GT(bytes, 1024 * 1024u);
  EXPECT_GE(bytes / reads, 32 * 1024u);
}

class ReplicationMatrixTest : public ReplicationFollowerTest,
                              public ::testing::WithParamInterface<std::pair<std::string, std::string>> {
  void SetUp() override {
    ReplicationFollowerTest::SetUp();
    sourceConfig.store.backend = GetParam().first;
    followerConfig.store.backend = GetParam().second;
  }
};

INSTANTIATE_TEST_SUITE_P(Storage, ReplicationMatrixTest,
    ::testing::Values(std::pair<std::string, std::string>{"fs", "fs"}, std::pair<std::string, std::string>{"fs", "ram"},
                      std::pair<std::string, std::string>{"ram", "fs"}, std::pair<std::string, std::string>{"ram", "ram"}),
    [](const auto& info) { return info.param.first + "_" + info.param.second; });

TEST_F(ReplicationFollowerTest, restartsBindingAndAcknowledgment) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  std::pmr::monotonic_buffer_resource arena;
  api::ReplicationStatus before;
  follower->getFollower()->stats(before, arena);
  stopFollower(); startFollower();
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  ASSERT_TRUE(caughtUp());
  api::ReplicationStatus after;
  follower->getFollower()->stats(after, arena);
  EXPECT_EQ(before.follower, after.follower);
  stopSource(); startSource();
  ASSERT_TRUE(until([&] { return !source->getReplication().stats(*source).empty() && !source->getReplication().stats(*source)[0].commit.incarnation.empty(); }));
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  stopFollower();
  auto different = followerConfig; different.replication.source = "http://127.0.0.1:1";
  EXPECT_NO_THROW(LuxirNode repointed(different));
  auto unbound = sourceConfig; unbound.replication.source = followerConfig.replication.source;
  stopSource();
  EXPECT_THROW(LuxirNode invalid(unbound), std::invalid_argument);
}

TEST_F(ReplicationFollowerTest, ramRestartKeepsOldUntilRealCommitAndOrphanDelete) {
  sourceConfig.store.backend = "ram";
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success); }
  { CollectionHelper h(*source, "orphan"); ASSERT_TRUE(h.index(flatdoc("id", "orphan"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp()); ASSERT_TRUE(caughtUp("orphan"));
  auto old = follower->getCollection("main");
  stopSource();
  ASSERT_TRUE(until([&] { return stateIs("stale"); }));
  EXPECT_EQ(1, old->getReaderManager().getReader()->liveDocs());
  startSource();
  ASSERT_TRUE(until([&] { return stateIs("orphan"); }));
  EXPECT_EQ(old, follower->getCollection("main"));
  { CollectionHelper h(*source, "main"); SchemaBuilder builder; builder.field("extra_s").type = api::FieldDef::FieldClass::STRING; h.collection().setSchema(builder.build(h.collection().getSchema().get())); }
  ASSERT_TRUE(until([&] { return stateIs("waiting"); }));
  EXPECT_EQ(old, follower->getCollection("main"));
  follower->deleteCollection("orphan");
  EXPECT_THROW(follower->getCollection("orphan"), CollectionNotFoundError);
  std::atomic<int> searches = 0, failures = 0;
  std::jthread queries([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      try {
        HttpReq search(followerServer->getPort());
        search.matchQuery("id", "old").withStats().execute();
        if (search.status() != 200) failures++;
      } catch (...) { failures++; }
      searches++;
    }
  });
  ASSERT_TRUE(until([&] { return searches > 0; }));
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "new"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  queries.request_stop(); queries.join();
  EXPECT_EQ(0, failures);
  EXPECT_NE(old, follower->getCollection("main"));
  // An admitted request can still be acquiring its reader from the old collection.
  EXPECT_EQ(1, old->getReaderManager().getReader()->liveDocs());
  stopFollower(); startFollower();
  ASSERT_TRUE(caughtUp());
  HttpReq search(followerServer->getPort());
  EXPECT_EQ(1, search.matchQuery("id", "new").withStats().execute().found());
  source->deleteCollection("main");
  ASSERT_TRUE(until([&] { return follower->collectionEntries().empty(); }));
}

TEST_F(ReplicationFollowerTest, ramFollowerAndReservationLossReuseVerifiedFiles) {
  followerConfig.store.backend = "ram";
  startSource();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  std::atomic<int> verified = 0;
  std::atomic<bool> expired = false;
  Signal::listen("replicationReservationGone", [&](void*, void*, void*) -> void* { expired = true; return nullptr; });
  Signal::listen("replicationFileVerified", [&](void*, void*, void*) -> void* {
    if (++verified == 1) {
      auto& snapshots = h.collection().getShard()->getSnapshots();
      snapshots.evictOldest();
    }
    return nullptr;
  });
  auto join = scope_guard([&] { stopFollower(); });
  startFollower();
  ASSERT_TRUE(until([&] { return stateIs("error"); }));
  glz::generic retry; ASSERT_FALSE(glz::read_json(retry, status()));
  EXPECT_GT(retry["collections"][0]["next_retry"].get<double>(), 0);
  ASSERT_TRUE(caughtUp());
  EXPECT_EQ(2, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(h.collection().getShard()->getSnapshots().snapshot()->files.size(), (size_t)verified.load());
  EXPECT_TRUE(expired.load());
  stopFollower(); // Join signal emitters before captured locals leave scope.
}

TEST_P(ReplicationMatrixTest, connectionFailureResumesRangeAndDroppedTransferRefetches) {
  startSource();
  CollectionHelper h(*source, "main");
  SchemaBuilder builder;
  auto& field = builder.field("payload");
  field.type = api::FieldDef::FieldClass::STRING;
  field.index = api::FieldDef::IndexMode::NONE; field.column = false; field.stored = true;
  h.collection().setSchema(builder.build(h.collection().getSchema().get()));
  std::string payload(8 * 1024 * 1024, 'a');
  uint32_t random = 1;
  for (auto& c : payload) { random = random * 1664525 + 1013904223; c = (char)(' ' + (random >> 24) % 95); }
  ASSERT_TRUE(h.index(flatdoc("id", "a", "payload", payload), UpdateMessage::COMMIT).success);
  std::atomic<int> interrupted = 0, resumed = 0;
  std::atomic<bool> gone{false};
  auto join = scope_guard([&] { stopFollower(); });
  Signal::listen("replicationRangeResume", [&](void*, void*, void*) -> void* { resumed++; return nullptr; });
  Signal::listen("replicationDownloadProgress", [&](void* progress, void*, void*) -> void* {
    if (*(uint64_t*)progress < 256 * 1024) return nullptr;
    if (interrupted.fetch_add(1) == 0) {
      sourceServer.reset();
      sourceServer = std::make_unique<HttpServer>(*source, 1, sourcePort);
      sourceServer->start();
    }
    return nullptr;
  });
  startFollower();
  ASSERT_TRUE(caughtUp());
  EXPECT_GT(resumed.load(), 0);
  stopFollower();
  Signal::unlisten("replicationDownloadProgress");
  // A fresh follower loses its reservation during the large file itself.
  followerConfig.store.data_dir = (path / "lost-reservation").string();
  interrupted = 0;
  Signal::listen("replicationReservationGone", [&](void*, void*, void*) -> void* { gone = true; return nullptr; });
  Signal::listen("replicationDownloadProgress", [&](void* progress, void*, void*) -> void* {
    if (*(uint64_t*)progress >= 256 * 1024 && interrupted.fetch_add(1) == 0) {
      auto& snapshots = h.collection().getShard()->getSnapshots();
      snapshots.evictOldest();
    }
    return nullptr;
  });
  startFollower();
  ASSERT_TRUE(caughtUp());
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  EXPECT_TRUE(gone.load());
  stopFollower();
}

TEST_P(ReplicationMatrixTest, corruptDownloadLeavesOldReaderServing) {
  startSource(); startFollower();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  auto old = follower->getCollection("main")->getReaderManager().getReader();
  auto& dir = h.getIndexWriter()->dir;
  std::mutex mutex;
  std::string name, original;
  auto write = [&](const std::string& bytes) {
    auto output = dir.createFile(name); OutputStream stream(output.get());
    stream.write(bytes.data(), bytes.size()); stream.close(); dir.finishFile(*output);
    std::array<std::string, 2> names{name, "."}; dir.sync(names);
  };
  Signal::listen("replicationDownloadStart", [&](void* descriptor, void*, void*) -> void* {
    std::lock_guard lock(mutex);
    if (!name.empty()) return nullptr;
    name = ((FileDescriptor*)descriptor)->name;
    original = dir.openFile(name)->read();
    auto corrupt = original;
    if (!corrupt.empty()) { corrupt[0] ^= 1; write(corrupt); }
    return nullptr;
  });
  auto join = scope_guard([&] { stopFollower(); });
  ASSERT_TRUE(h.index(flatdoc("id", "new"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(until([&] { return stateIs("error"); }));
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  std::pmr::monotonic_buffer_resource arena;
  api::ReplicationStatus retry;
  follower->getFollower()->stats(retry, arena);
  ASSERT_EQ(1u, retry.collections.size());
  EXPECT_GT(retry.collections[0].next_retry, retry.last_contact);
  EXPECT_NE(std::string::npos, retry.collections[0].last_error.find("checksum mismatch"));
  { std::lock_guard lock(mutex); write(original); }
  ASSERT_TRUE(caughtUp());
  EXPECT_EQ(2, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(1, old->liveDocs());
}

TEST_F(ReplicationFollowerTest, deleteWhileDownloadIsPendingCannotResurrectCollection) {
  startSource(); startFollower();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  std::latch downloading(1), resume(1);
  auto join = scope_guard([&] { if (!resume.try_wait()) resume.count_down(); stopFollower(); });
  Signal::listen("replicationFileVerified", [&](void*, void*, void*) -> void* {
    downloading.count_down(); resume.wait(); return nullptr;
  });
  ASSERT_TRUE(h.index(flatdoc("id", "new"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(until([&] { return downloading.try_wait(); }));
  source->deleteCollection("main");
  ASSERT_TRUE(until([&] { return stateIs("orphan"); }));
  resume.count_down();
  ASSERT_TRUE(until([&] { return follower->collectionEntries().empty(); }));
  stopFollower();
  EXPECT_TRUE(std::filesystem::is_empty(path / "follower" / "c"));
}

TEST_F(ReplicationFollowerTest, writerAndReadOnlyOpenFollowerLayout) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto id = follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  stopFollower();
  auto config = followerConfig; config.replication.source.clear(); config.read_only = true;
  {
    LuxirNode reader(config);
    auto col = reader.getCollection("main");
    EXPECT_EQ(id, col->getShard()->getSnapshots().snapshot()->id);
    EXPECT_EQ(1, col->getReaderManager().getReader()->liveDocs());
  }
  config.read_only = false;
  EXPECT_THROW(LuxirNode writer(config), std::runtime_error);
  config.promote = true;
  {
    LuxirNode writer(config);
    CollectionHelper h(writer, "main");
    auto promoted = h.collection().getShard()->getSnapshots().snapshot()->id;
    EXPECT_NE(id.incarnation, promoted.incarnation);
    EXPECT_FALSE(std::filesystem::exists(path / "follower" / "replication.json"));
    EXPECT_FALSE(std::filesystem::exists(path / "follower" / "c" / "main" / id.incarnation));
    EXPECT_EQ(1, h.collection().getReaderManager().getReader()->liveDocs());
    ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
    EXPECT_EQ(2, h.collection().getReaderManager().getReader()->liveDocs());
  }
}

TEST_F(ReplicationFollowerTest, unavailableSourceKeepsReplica) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto directory = source->getCollection("main")->getShard()->getDirectory();
  auto manifest = Manifest::load(*directory);
  stopSource();
  directory->deleteFile(Manifest::name(manifest.generation));
  startSource();
  source->getReplication().changed();
  ASSERT_TRUE(until([&] { return stateIs("stale"); }));
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
}

TEST_F(ReplicationFollowerTest, rootBeforeCurrentCrashKeepsOldIncarnation) {
  sourceConfig.store.backend = "ram";
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto old = follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  stopSource(); startSource();
  ASSERT_TRUE(until([&] { return stateIs("waiting"); }));
  Signal::listen("replicationRootWritten", [](void*, void*, void*) -> void* { throw std::runtime_error("interrupted before CURRENT"); });
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "new"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(until([&] { return stateIs("error"); }));
  stopFollower(); stopSource();
  Signal::unlisten("replicationRootWritten");
  startFollower();
  EXPECT_EQ(old, follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id);
  startSource();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "new"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
}

TEST_F(ReplicationFollowerTest, corruptLocalCollectionIsFetchedAgain) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto id = follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  stopFollower();
  std::filesystem::remove(path / "follower" / "c" / "main" / id.incarnation / Manifest::name(id.index_gen));
  startFollower();
  ASSERT_TRUE(caughtUp());
}

TEST_F(ReplicationFollowerTest, sameBootAbsenceSurvivesFollowerRestart) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  stopFollower();
  source->deleteCollection("main");
  startFollower();
  ASSERT_TRUE(until([&] { return follower->collectionEntries().empty(); }));
}

TEST_F(ReplicationFollowerTest, sourceRegressionIsRefused) {
  startSource(); startFollower();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  auto& sourceRegistry = h.collection().getShard()->getSnapshots();
  auto old = sourceRegistry.snapshot();
  sourceRegistry.acquire(); // preserve real files while publishing the older root
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  auto served = follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  auto latest = sourceRegistry.snapshot();
  ASSERT_TRUE(until([&] {
    return std::ranges::any_of(source->getReplication().stats(*source), [&](const auto& row) { return row.commit == latest->id; });
  }));
  sourceRegistry.publish(old);
  ASSERT_TRUE(until([&] { return stateIs("error"); }));
  EXPECT_NE(std::string::npos, status().find("source went backwards"));
  EXPECT_EQ(served, follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id);
  sourceRegistry.publish(latest);
  ASSERT_TRUE(until([&] { return stateIs("serving"); }));
  stopFollower(); startFollower();
  EXPECT_EQ(served, follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id);
}

TEST_F(ReplicationFollowerTest, sweepKeepsInstalledRootAfterInterruptedNewerRoot) {
  startSource(); startFollower();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  stopFollower();
  auto& registry = h.collection().getShard()->getSnapshots();
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  auto middle = registry.acquire();
  ASSERT_TRUE(h.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  Signal::listen("replicationRootWritten", [](void*, void*, void*) -> void* { throw std::runtime_error("interrupted newer root"); });
  startFollower();
  ASSERT_TRUE(until([&] { return stateIs("error"); }));
  Signal::unlisten("replicationRootWritten");
  registry.publish(middle);
  ASSERT_TRUE(caughtUp());
  auto& local = follower->getCollection("main")->getShard()->getSnapshots().dir;
  EXPECT_EQ(middle->id.index_gen, Manifest::load(local).generation);
  stopFollower(); startFollower();
  EXPECT_EQ(middle->id, follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id);
}

TEST_F(ReplicationFollowerTest, barriersCountFsAndRamAndReadYourWrite) {
  startSource(); startFollower();
  auto ramConfig = followerConfig; ramConfig.store.backend = "ram";
  LuxirNode ram(ramConfig);
  ASSERT_TRUE(until([&] { return source->getReplication().stats(*source).size() == 2; }));
  for (std::string wanted : {"1", "2", "all"}) {
    auto reply = httpRequest(sourcePort, http::verb::post,
        "/collections/main/_update?commit=true&wait_for_replicas=" + wanted,
        "{\"id\":\"" + wanted + "\"}\n", "application/x-ndjson");
    ASSERT_EQ(200, reply.result_int()) << reply.body();
    auto end = reply.body().find_last_not_of("\r\n ");
    auto begin = reply.body().rfind('\n', end);
    auto line = std::string_view(reply.body()).substr(begin == std::string::npos ? 0 : begin + 1);
    glz::generic json; ASSERT_FALSE(glz::read_json(json, line));
    ASSERT_TRUE(json.contains("replicas")) << reply.body();
    EXPECT_FALSE(json["replicas"]["timed_out"].get<bool>());
    EXPECT_GE(json["replicas"]["serving"].get<double>(), wanted == "1" ? 1 : 2);
    auto token = json["commit"].get<std::string>();
    auto search = httpRequest(followerServer->getPort(), http::verb::get,
        "/collections/main/_search?query=id:" + wanted + "&min_commit=" + token + "&get_number=true");
    EXPECT_EQ(200, search.result_int()) << search.body();
    EXPECT_NE(std::string::npos, search.body().find("\"found\":1"));
  }
  auto partial = httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"partial"}],"commit":{"wait_for_replicas":3,"replication_timeout_ms":100}})");
  glz::generic json; ASSERT_FALSE(glz::read_json(json, partial.body()));
  EXPECT_EQ(3, json["replicas"]["wanted"].get<double>());
  EXPECT_EQ(2, json["replicas"]["serving"].get<double>());
  EXPECT_TRUE(json["replicas"]["timed_out"].get<bool>());
  auto ndjson = httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
      "{\"id\":\"end\"}\n{\"_end_\":{\"commit\":{\"wait_for_replicas\":2,\"commit_within_ms\":60000}}}\n", "application/x-ndjson");
  ASSERT_FALSE(glz::read_json(json, ndjson.body())) << ndjson.body();
  EXPECT_FALSE(json["replicas"]["timed_out"].get<bool>());
  EXPECT_EQ(2, json["replicas"]["serving"].get<double>());
}

TEST_F(ReplicationFollowerTest, slowBarrierDoesNotHoldLaterCommitAndFloorWaits) {
  startSource(); startFollower();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  std::atomic<bool> paused{false};
  std::latch resume(1);
  auto join = scope_guard([&] { if (!resume.try_wait()) resume.count_down(); stopFollower(); });
  Signal::listen("replicationFileVerified", [&](void*, void*, void*) -> void* {
    if (!paused.exchange(true)) resume.wait();
    return nullptr;
  });
  auto first = std::async(std::launch::async, [&] {
    return httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
        R"({"docs":[{"id":"new"}],"commit":{"wait_for_replicas":1,"replication_timeout_ms":5000}})");
  });
  ASSERT_TRUE(until([&] { return paused.load(); }));
  auto second = httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
      R"({"commit":{"wait_for_replicas":0}})");
  glz::generic json; ASSERT_FALSE(glz::read_json(json, second.body()));
  auto token = json["commit"].get<std::string>();
  std::atomic<bool> parked{false};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked = true; return nullptr; });
  auto search = std::async(std::launch::async, [&] {
    return httpRequest(followerServer->getPort(), http::verb::get,
        "/collections/main/_search?query=id:new&get_number=true&min_commit=" + token + "&min_commit_timeout_ms=5000&freshness_ms=60000");
  });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  resume.count_down();
  auto searched = search.get();
  EXPECT_EQ(200, searched.result_int());
  EXPECT_NE(std::string::npos, searched.body().find("\"found\":1"));
  auto reply = first.get(); ASSERT_FALSE(glz::read_json(json, reply.body()));
  EXPECT_FALSE(json["replicas"]["timed_out"].get<bool>());
  ASSERT_TRUE(caughtUp());
  stopFollower();
}

TEST_F(ReplicationFollowerTest, allDropsExpiredMembersAndDoesNotAddNewOnes) {
  sourceConfig.replication.follower_timeout_ms = 1000;
  sourceConfig.replication.pin_idle_timeout_ms = 100;
  std::atomic<int> ticks{0};
  auto close = scope_guard([&] { stopFollower(); stopSource(); });
  Signal::listen("replicationExpiryTick", [&](void*, void*, void*) -> void* { ticks++; return nullptr; });
  startSource(); startFollower();
  ASSERT_TRUE(until([&] { return source->getReplication().stats(*source).size() == 1; }));
  ASSERT_TRUE(caughtUp());
  auto prior = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id.token();
  httpRequest(sourcePort, http::verb::post, "/_replication/installed",
      "{\"follower\":\"leaving\",\"collection\":\"main\",\"commit\":\"" + prior + "\"}");
  std::atomic<bool> parked{false};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked = true; return nullptr; });
  auto pending = std::async(std::launch::async, [&] {
    return httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
        R"({"docs":[{"id":"a"}],"commit":{"wait_for_replicas":"all","replication_timeout_ms":5000}})");
  });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  auto seen = httpRequest(sourcePort, http::verb::get, "/_replication/watch?follower=late&timeout_ms=0");
  glz::generic catalog; ASSERT_FALSE(glz::read_json(catalog, seen.body()));
  auto cursor = catalog["cursor"].get<std::string>();
  std::jthread late([&](std::stop_token stop) {
    while (!stop.stop_requested()) httpRequest(sourcePort, http::verb::get,
        "/_replication/watch?follower=late&timeout_ms=100&since=" + cursor);
  });
  EXPECT_EQ(std::future_status::ready, pending.wait_for(3s));
  auto response = pending.get();
  EXPECT_GT(ticks.load(), 0);
  glz::generic json; ASSERT_FALSE(glz::read_json(json, response.body()));
  EXPECT_FALSE(json["replicas"]["timed_out"].get<bool>());
  EXPECT_EQ(1, json["replicas"]["wanted"].get<double>());
  EXPECT_EQ(1, json["replicas"]["serving"].get<double>());
}

TEST_F(ReplicationFollowerTest, searchFloorTimeoutMismatchAndWriterWait) {
  startSource();
  auto id = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  auto future = id; future.index_gen++;
  auto path = "/collections/main/_search?query=id:a&min_commit=" + future.token();
  auto timeout = httpRequest(sourcePort, http::verb::get, path + "&min_commit_timeout_ms=10");
  EXPECT_EQ(503, timeout.result_int()); EXPECT_NE(std::string::npos, timeout.body().find("stale_replica"));
  std::atomic<bool> parked{false};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked = true; return nullptr; });
  auto pending = std::async(std::launch::async, [&] { return httpRequest(sourcePort, http::verb::get, path + "&min_commit_timeout_ms=5000"); });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(200, pending.get().result_int());
  auto mismatch = httpRequest(sourcePort, http::verb::post, "/collections/main/_search",
      "{\"min_commit\":\"" + newUuid() + ":1\",\"query\":\"id:a\"}");
  EXPECT_EQ(409, mismatch.result_int());
  EXPECT_NE(std::string::npos, mismatch.body().find("commit_incarnation_mismatch"));
}

TEST_F(ReplicationFollowerTest, shutdownCancelsParkedWaits) {
  startSource();
  auto id = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  id.index_gen += 100;
  std::atomic<int> parked{0};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked++; return nullptr; });
  auto search = std::async(std::launch::async, [&] {
    try { httpRequest(sourcePort, http::verb::get, "/collections/main/_search?min_commit=" + id.token() + "&min_commit_timeout_ms=600000"); }
    catch (const boost::system::system_error&) {}
  });
  auto commit = std::async(std::launch::async, [&] {
    try { httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
        R"({"commit":{"wait_for_replicas":10,"replication_timeout_ms":600000}})"); }
    catch (const boost::system::system_error&) {}
  });
  ASSERT_TRUE(until([&] { return parked.load() == 2; }));
  sourceServer.reset();
  search.get(); commit.get();
}

TEST_F(ReplicationFollowerTest, eofCommitsOnlyTouchedCollections) {
  startSource(); startFollower();
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "main"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(caughtUp());
  source->createCollection(nullptr, "other");
  std::latch resume(1);
  auto join = scope_guard([&] { resume.count_down(); stopFollower(); });
  Signal::listen("replicationFileVerified", [&](void*, void*, void*) -> void* { resume.wait(); return nullptr; });
  auto response = httpRequest(sourcePort, http::verb::post,
      "/collections/main/_update?commit=true&wait_for_replicas=1&replication_timeout_ms=500",
      "{\"_update_\":{\"collection\":\"other\"}}\n{\"id\":\"other\"}\n", "application/x-ndjson");
  ASSERT_EQ(200, response.result_int()) << response.body();
  auto end = response.body().find_last_not_of("\r\n ");
  auto begin = response.body().rfind('\n', end);
  glz::generic json;
  ASSERT_FALSE(glz::read_json(json, std::string_view(response.body()).substr(begin == std::string::npos ? 0 : begin + 1)));
  EXPECT_EQ(source->getCollection("other")->getShard()->getSnapshots().snapshot()->id.token(), json["commit"].get<std::string>());
  EXPECT_EQ(0, json["replicas"]["serving"].get<double>());
  EXPECT_TRUE(json["replicas"]["timed_out"].get<bool>());
}

TEST_F(ReplicationFollowerTest, emptySnapshotCannotParkANewerCommit) {
  startSource();
  std::latch resume(1);
  std::atomic<bool> received{false};
  Signal::listen("replicationEmptySnapshotReceived", [&](void*, void*, void*) -> void* {
    if (!received.exchange(true)) resume.wait();
    return nullptr;
  });
  auto join = scope_guard([&] { if (!resume.try_wait()) resume.count_down(); stopFollower(); });
  startFollower();
  ASSERT_TRUE(until([&] { return received.load(); }));
  CollectionHelper h(*source, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto token = h.collection().getShard()->getSnapshots().snapshot()->id.token();
  ASSERT_TRUE(until([&] { return status().find(token) != std::string::npos; }));
  resume.count_down();
  EXPECT_TRUE(caughtUp());
  stopFollower();
}

TEST_F(ReplicationFollowerTest, promotionRetriesOnlyUnpromotedCollections) {
  startSource(); startFollower();
  for (auto name : {"main", "broken"}) {
    CollectionHelper h(*source, name);
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    ASSERT_TRUE(caughtUp(name)) << status();
  }
  auto old = follower->getCollection("broken")->getShard()->getSnapshots().snapshot()->id;
  stopFollower();
  bool fail = true;
  Signal::listen("replicationPromotingCollection", [&](void* name, void*, void*) -> void* {
    if (fail && *(std::string*)name == "broken") throw std::runtime_error("transient promotion failure");
    return nullptr;
  });
  auto config = followerConfig; config.replication.source.clear(); config.promote = true;
  CommitId main;
  {
    LuxirNode promoted(config);
    auto collection = promoted.getCollection("main");
    main = collection->getShard()->getSnapshots().snapshot()->id;
    EXPECT_EQ(1, collection->getReaderManager().getReader()->liveDocs());
    EXPECT_THROW(promoted.getCollection("broken"), CollectionUnavailableError);
    EXPECT_TRUE(std::filesystem::exists(path / "follower" / "replication.json"));
  }
  fail = false;
  for (int retry = 0; retry < 2; retry++) {
    LuxirNode promoted(config);
    EXPECT_EQ(main, promoted.getCollection("main")->getShard()->getSnapshots().snapshot()->id);
    auto collection = promoted.getCollection("broken");
    EXPECT_NE(old.incarnation, collection->getShard()->getSnapshots().snapshot()->id.incarnation);
    EXPECT_EQ(1, collection->getReaderManager().getReader()->liveDocs());
    EXPECT_FALSE(std::filesystem::exists(path / "follower" / "replication.json"));
  }
}


TEST_F(ReplicationFollowerTest, deleteAndRecreateCompletesPendingWaits) {
  startSource();
  auto id = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  id.index_gen += 100;
  std::atomic<int> parked{0};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked++; return nullptr; });
  auto floor = std::async(std::launch::async, [&] {
    return httpRequest(sourcePort, http::verb::get, "/collections/main/_search?min_commit=" + id.token());
  });
  auto barrier = std::async(std::launch::async, [&] {
    return httpRequest(sourcePort, http::verb::post, "/collections/main/_update", R"({"commit":{"wait_for_replicas":1}})");
  });
  ASSERT_TRUE(until([&] { return parked == 2; }));
  source->deleteCollection("main"); source->createCollection(nullptr, "main");
  EXPECT_EQ(std::future_status::ready, floor.wait_for(1s));
  EXPECT_EQ(std::future_status::ready, barrier.wait_for(1s));
  EXPECT_EQ(503, floor.get().result_int());
  EXPECT_NE(std::string::npos, barrier.get().body().find("replica_wait_cancelled"));
}

TEST_F(ReplicationFollowerTest, promotionReusesFilesAcrossIncarnations) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto ramConfig = followerConfig; ramConfig.store.backend = "ram";
  auto fsConfig = followerConfig; fsConfig.store.data_dir = (path / "second").string();
  LuxirNode ram(ramConfig), fs(fsConfig);
  HttpServer ramServer(ram, 1, 0); ramServer.start();
  auto old = source->getCollection("main")->getShard()->getSnapshots().snapshot();
  ASSERT_TRUE(until([&] { return ram.collectionEntries().size() == 1 && fs.collectionEntries().size() == 1; }));
  auto ramFile = ram.getCollection("main")->getShard()->getSnapshots().dir.openFile(old->files.front().name);
  stopFollower(); stopSource();
  sourceConfig = followerConfig; sourceConfig.replication.source.clear(); sourceConfig.promote = true;
  source = std::make_unique<LuxirNode>(sourceConfig);
  auto promoted = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  sourceServer = std::make_unique<HttpServer>(*source, 1, sourcePort); sourceServer->start();
  for (auto* replica : {&ram, &fs}) {
    ASSERT_TRUE(until([&] { return replica->getCollection("main")->getShard()->getSnapshots().snapshot()->id == promoted; }));
    api::ReplicationStatus status; std::pmr::monotonic_buffer_resource arena;
    replica->getFollower()->stats(status, arena);
    ASSERT_EQ(1, status.collections.size());
    EXPECT_EQ(0, status.collections.front().bytes_downloaded);
  }
  EXPECT_EQ(ramFile, ram.getCollection("main")->getShard()->getSnapshots().dir.openFile(old->files.front().name));
}

TEST_F(ReplicationFollowerTest, eofReportsEachTouchedCollection) {
  startSource();
  for (bool wait : {false, true}) {
    auto target = std::string("/collections/main/_update?commit=true") + (wait ? "&wait_for_replicas=1&replication_timeout_ms=0" : "");
    auto response = httpRequest(sourcePort, http::verb::post, target,
        "{\"id\":\"main\"}\n{\"_update_\":{\"collection\":\"other\"}}\n{\"id\":\"other\"}\n", "application/x-ndjson");
    auto end = response.body().find_last_not_of("\r\n ");
    auto begin = response.body().rfind('\n', end);
    glz::generic json;
    ASSERT_FALSE(glz::read_json(json, std::string_view(response.body()).substr(begin == std::string::npos ? 0 : begin + 1))) << response.body();
    for (auto name : {"main", "other"}) {
      auto& result = json["commits"][name];
      EXPECT_EQ(source->getCollection(name)->getShard()->getSnapshots().snapshot()->id.token(), result["commit"].get<std::string>());
      EXPECT_FALSE(json.contains("replicas"));
      EXPECT_EQ(wait, result.contains("replicas"));
      if (wait) {
        EXPECT_EQ(1, result["replicas"]["wanted"].get<double>());
        EXPECT_TRUE(result["replicas"]["timed_out"].get<bool>());
      }
    }
  }
}

TEST_F(ReplicationFollowerTest, shutdownBarrierStillReportsSuccessfulCommit) {
  startSource();
  std::atomic<bool> parked{false};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked = true; return nullptr; });
  auto response = std::async(std::launch::async, [&] {
    return httpRequest(sourcePort, http::verb::post, "/collections/main/_update", R"({"commit":{"wait_for_replicas":1}})");
  });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  source->getReplication().closeWaits();
  glz::generic json;
  ASSERT_FALSE(glz::read_json(json, response.get().body()));
  EXPECT_EQ("ok", json["status"].get<std::string>());
  EXPECT_FALSE(json["replicas"]["timed_out"].get<bool>());
}


TEST_F(ReplicationFollowerTest, followerFloorWaitsAcrossIncarnations) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  sourceServer.reset();
  source->deleteCollection("main");
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success); }
  auto next = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  std::atomic<bool> parked{false};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked = true; return nullptr; });
  auto response = std::async(std::launch::async, [&] {
    return httpRequest(followerServer->getPort(), http::verb::get, "/collections/main/_search?min_commit=" + next.token());
  });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  sourceServer = std::make_unique<HttpServer>(*source, 1, sourcePort); sourceServer->start();
  EXPECT_EQ(200, response.get().result_int());
}

TEST_F(ReplicationFollowerTest, followerFloorSurvivesRemovalAndIncarnationSwitch) {
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto old = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  std::atomic<bool> parked{false};
  Signal::listen("replicationWaitParked", [&](void*, void*, void*) -> void* { parked = true; return nullptr; });
  old.index_gen += 100;
  auto removed = std::async(std::launch::async, [&] {
    return httpRequest(followerServer->getPort(), http::verb::get,
        "/collections/main/_search?min_commit=" + old.token() + "&min_commit_timeout_ms=5000");
  });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  source->deleteCollection("main");
  ASSERT_TRUE(until([&] { return follower->collectionEntries().empty(); }));
  EXPECT_EQ(std::future_status::timeout, removed.wait_for(0ms));
  sourceServer.reset();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success); }
  auto next = source->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  parked = false;
  auto switched = std::async(std::launch::async, [&] {
    return httpRequest(followerServer->getPort(), http::verb::get, "/collections/main/_search?min_commit=" + next.token());
  });
  ASSERT_TRUE(until([&] { return parked.load(); }));
  sourceServer = std::make_unique<HttpServer>(*source, 1, sourcePort); sourceServer->start();
  EXPECT_EQ(200, switched.get().result_int());
  EXPECT_EQ(409, removed.get().result_int());
}





















TEST_F(ReplicationFollowerTest, emptyCreateAndSameBootRecreateInstallImmediately) {
  startSource(); startFollower();
  ASSERT_TRUE(caughtUp());
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  auto old = follower->getCollection("main")->getShard()->getSnapshots().snapshot()->id;
  source->deleteCollection("main");
  source->createCollection(nullptr, "main");
  ASSERT_TRUE(caughtUp());
  auto current = follower->getCollection("main");
  EXPECT_NE(old.incarnation, current->getShard()->getSnapshots().snapshot()->id.incarnation);
  EXPECT_EQ(0, current->getReaderManager().getReader()->liveDocs());
}

TEST_F(ReplicationFollowerTest, waitingSurvivesFollowerRestartThenSameBootRecreateWins) {
  sourceConfig.store.backend = "ram";
  startSource(); startFollower();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success); }
  ASSERT_TRUE(caughtUp());
  stopSource(); startSource();
  ASSERT_TRUE(until([&] { return stateIs("waiting"); }));
  stopFollower(); startFollower();
  ASSERT_TRUE(until([&] { return stateIs("waiting"); }));
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
  source->deleteCollection("main"); source->createCollection(nullptr, "main");
  ASSERT_TRUE(caughtUp());
  EXPECT_EQ(0, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
}

TEST_F(ReplicationFollowerTest, refusesInvalidPromotionModes) {
  LuxirConfig config;
  config.promote = true;
  EXPECT_THROW(LuxirNode node(config), std::invalid_argument);
  config.store.backend = "fs"; config.store.data_dir = (path / "invalid").string();
  config.replication.source = "http://127.0.0.1:1";
  EXPECT_THROW(LuxirNode node(config), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(config.store.data_dir));
}

TEST_F(ReplicationFollowerTest, watchOnlyFollowersDoNotCountForAll) {
  startSource();
  httpRequest(sourcePort, http::verb::get, "/_replication/watch?follower=watcher&timeout_ms=0");
  auto response = httpRequest(sourcePort, http::verb::post, "/collections/main/_update",
      R"({"commit":{"wait_for_replicas":"all","replication_timeout_ms":0}})");
  glz::generic json; ASSERT_FALSE(glz::read_json(json, response.body()));
  EXPECT_EQ(0, json["replicas"]["wanted"].get<double>());
  EXPECT_FALSE(json["replicas"]["timed_out"].get<bool>());
}

TEST_F(ReplicationFollowerTest, restartReusesVerifiedCandidateFiles) {
  startSource();
  { CollectionHelper h(*source, "main"); ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success); }
  Signal::listen("replicationRootWritten", [](void*, void*, void*) -> void* { throw std::runtime_error("stop before CURRENT"); });
  startFollower();
  ASSERT_TRUE(until([&] { return stateIs("error"); }));
  stopFollower(); Signal::unlisten("replicationRootWritten");
  startFollower(); ASSERT_TRUE(caughtUp());
  api::ReplicationStatus status; std::pmr::monotonic_buffer_resource arena;
  follower->getFollower()->stats(status, arena);
  for (const auto& row : status.collections) EXPECT_EQ(0, row.bytes_downloaded);
}

TEST_F(ReplicationFollowerTest, fallbackWriterChangesIncarnation) {
  startSource();
  std::shared_ptr<Directory> dir;
  Manifest old, newer;
  CommitId before;
  {
    CollectionHelper h(*source, "main");
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    dir = h.collection().getShard()->getDirectory();
    old = Manifest::load(*dir);
    h.commit(); newer = Manifest::load(*dir);
    ASSERT_GT(newer.generation, old.generation);
    before = h.collection().getShard()->getSnapshots().snapshot()->id;
  }
  startFollower(); ASSERT_TRUE(caughtUp());
  stopSource();
  Manifest::write(*dir, old.generation, *old.bytes);
  auto file = dir->createFile(Manifest::name(newer.generation));
  OutputStream out(file.get()); out.write((char)0); out.close(); dir->finishFile(*file);
  std::array<std::string, 3> names{Manifest::name(old.generation), Manifest::name(newer.generation), "."};
  dir->sync(names);
  startSource(); ASSERT_TRUE(caughtUp());
  EXPECT_NE(before.incarnation, source->getCollection("main")->getShard()->getSnapshots().snapshot()->id.incarnation);
  EXPECT_EQ(1, follower->getCollection("main")->getReaderManager().getReader()->liveDocs());
}





}
