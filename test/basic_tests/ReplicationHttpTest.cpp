// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationCatalog.h"
#include <future>
#include <latch>
#include <filesystem>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/SchemaBuilder.h"
#include "luxir/server/HttpServer.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

namespace luxir::test {
using namespace std::chrono_literals;
using Json = glz::generic;

class ReplicationHttpTest : public LuxirTest {
protected:
  std::filesystem::path path;
  std::unique_ptr<LuxirNode> node;
  std::unique_ptr<HttpServer> server;
  std::unique_ptr<CollectionHelper> h;

  void SetUp() override { setup(); }
  void setup(std::string backend = "fs", int64_t idle = 60000) {
    LuxirTest::SetUp();
    path = std::filesystem::temp_directory_path() / ("luxir-replication-http-" + newUuid());
    LuxirConfig config;
    config.store.backend = backend;
    config.replication.pin_idle_timeout_ms = idle;
    config.store.data_dir = path.string();
    config.store.checked_dir.sync = "throw";
    node = std::make_unique<LuxirNode>(config);
    h = std::make_unique<CollectionHelper>(*node, "main");
    server = std::make_unique<HttpServer>(*node, 1, 0);
    server->start();
  }
  void TearDown() override {
    server->shutdown();
    server.reset();
    h.reset();
    node.reset();
    std::filesystem::remove_all(path);
    LuxirTest::TearDown();
  }
  std::shared_ptr<const CommitSnapshot> largeSnapshot() {
    SchemaBuilder builder;
    auto& field = builder.field("payload");
    field.type = api::FieldDef::FieldClass::STRING;
    field.index = api::FieldDef::IndexMode::NONE;
    field.column = false;
    field.stored = true;
    h->collection().setSchema(builder.build(h->collection().getSchema().get()));
    std::string payload(8 * 1024 * 1024, 'a');
    uint32_t random = 1;
    for (auto& c : payload) { random = random * 1664525 + 1013904223; c = (char)(' ' + (random >> 24) % 95); }
    EXPECT_TRUE(h->index(flatdoc("id", "large", "payload", payload), UpdateMessage::COMMIT).success);
    return snapshot();
  }
  void stalledTransfer(const std::function<void(const CommitSnapshot&)>& drop) {
    auto first = largeSnapshot();
    auto file = *std::ranges::max_element(first->files, {}, &FileDescriptor::size);
    ASSERT_GT(file.size, 4u * 1024 * 1024);
    std::atomic<bool> aborted = false;
    Signal::listen("replicationTransferAborted", [&](void*, void*, void*) -> void* { aborted = true; return nullptr; });
    std::promise<std::weak_ptr<InputFile>> opened;
    Signal::listen("replicationFileOpened", [&](void* value, void*, void*) -> void* {
      opened.set_value(*(std::shared_ptr<InputFile>*)value); return nullptr;
    });
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), (unsigned short)server->getPort()));
    stream.socket().set_option(net::socket_base::receive_buffer_size(4096));
    http::request<http::empty_body> request(http::verb::get, fileUrl(*first, file.name), 11);
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response_parser<http::empty_body> response;
    response.body_limit(32 * 1024 * 1024);
    http::read_header(stream, buffer, response);
    ASSERT_EQ(200, response.get().result_int());
    auto mapping = opened.get_future().get();
    ASSERT_FALSE(mapping.expired());
    if (drop) drop(*first);
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!mapping.expired() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(mapping.expired()); // No client reads: the server released its mmap.
    EXPECT_TRUE(aborted.load());
    EXPECT_EQ(200, get("/health").result_int());
  }
  http::response<http::string_body> get(std::string_view target) { return httpRequest(server->getPort(), http::verb::get, target); }
  Json catalog(std::string_view target = "/_replication/watch?timeout_ms=0") {
    auto response = get(target);
    EXPECT_EQ(200, response.result_int());
    Json json;
    EXPECT_FALSE(glz::read_json(json, response.body()));
    return json;
  }
  static std::string cursor(const Json& json) {
    return json["cursor"].get<std::string>();
  }
  std::shared_ptr<const CommitSnapshot> snapshot() {
    auto response = get("/_replication/main/snapshot");
    EXPECT_EQ(200, response.result_int());
    auto& body = response.body();
    auto bytes = std::make_shared<const std::vector<std::byte>>(
        (const std::byte*)body.data(), (const std::byte*)body.data() + body.size());
    auto result = CommitSnapshot::fromBytes(bytes);
    EXPECT_EQ(result->id.token(), response["X-Luxir-Commit"]);
    return result;
  }
  static std::string fileUrl(const CommitSnapshot& snapshot, std::string_view name) {
    return "/_replication/main/file/" + std::string(name) + "?commit=" + snapshot.id.incarnation +
        ":" + std::to_string(snapshot.id.index_gen);
  }
  auto download(const CommitSnapshot& snapshot, std::string_view name, std::string_view range, bool head = false) {
    net::io_context io;
    beast::tcp_stream stream(io);
    stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), (unsigned short)server->getPort()));
    http::request<http::empty_body> request(head ? http::verb::head : http::verb::get, fileUrl(snapshot, name), 11);
    request.set(http::field::range, range);
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> response;
    response.skip(head);
    http::read(stream, buffer, response);
    return response.release();
  }
};

TEST_F(ReplicationHttpTest, watchPublishCreateDeleteAndTimeout) {
  auto initial = catalog();
  auto wait = [&] {
    auto url = "/_replication/watch?since=" + cursor(initial) + "&timeout_ms=2000";
    auto parked = std::make_shared<std::promise<void>>();
    auto ready = parked->get_future();
    Signal::listen("replicationWatchParked", [parked](void*, void*, void*) -> void* {
      parked->set_value(); return nullptr;
    });
    auto result = std::async(std::launch::async, [&, url] { return catalog(url); });
    EXPECT_EQ(std::future_status::ready, ready.wait_for(1s));
    return result;
  };
  auto published = wait();
  ASSERT_TRUE(h->index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  ASSERT_EQ(std::future_status::ready, published.wait_for(1s));
  initial = published.get();
  auto created = wait();
  std::promise<void> initialized, install;
  auto resume = install.get_future();
  Signal::listen("collectionInitialized", [&](void*, void*, void*) -> void* {
    initialized.set_value(); resume.wait(); return nullptr;
  });
  auto creation = std::async(std::launch::async, [&] { node->createCollection(nullptr, "other"); });
  EXPECT_EQ(std::future_status::ready, initialized.get_future().wait_for(1s));
  EXPECT_EQ(std::future_status::timeout, created.wait_for(20ms));
  install.set_value();
  ASSERT_EQ(std::future_status::ready, created.wait_for(1s));
  initial = created.get();
  ASSERT_TRUE(initial["collections"].contains("other"));
  EXPECT_EQ(200, get("/_replication/other/snapshot").result_int());
  creation.get();
  Signal::unlisten("collectionInitialized");
  auto removed = wait();
  node->deleteCollection("other");
  ASSERT_EQ(std::future_status::ready, removed.wait_for(1s));
  initial = removed.get();
  EXPECT_FALSE(initial["collections"].contains("other"));
  Signal::unlisten("replicationWatchParked");
  auto start = std::chrono::steady_clock::now();
  auto timeout = catalog("/_replication/watch?since=" + cursor(initial) + "&timeout_ms=30");
  EXPECT_EQ(cursor(initial), cursor(timeout));
  EXPECT_GE(std::chrono::steady_clock::now() - start, 25ms);
  EXPECT_EQ(cursor(initial), cursor(catalog("/_replication/watch?since=foreign:123&timeout_ms=30000")));
  EXPECT_EQ(cursor(initial), cursor(catalog("/_replication/watch?since=" + initial["boot"].get<std::string>() + ":999999")));
}

TEST_F(ReplicationHttpTest, parkedWatchesDoNotBlockIoAndShutdownCancelsThem) {
  auto initial = catalog();
  std::latch parked(12);
  Signal::listen("replicationWatchParked", [&](void*, void*, void*) -> void* { parked.count_down(); return nullptr; });
  std::vector<std::future<void>> watches;
  for (int i = 0; i < 12; i++) {
    watches.push_back(std::async(std::launch::async, [&, url = "/_replication/watch?since=" + cursor(initial) + "&timeout_ms=30000"] {
      try { get(url); } catch (const std::exception&) {}
    }));
  }
  parked.wait();
  EXPECT_EQ(200, get("/health").result_int());
  server->shutdown();
  for (auto& watch : watches) EXPECT_EQ(std::future_status::ready, watch.wait_for(1s));
}

TEST_F(ReplicationHttpTest, snapshotFilesRangeAndSharedReservations) {
  ASSERT_TRUE(h->indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h->deleteById("a", UpdateMessage::COMMIT).success);
  auto first = snapshot();
  auto second = std::async(std::launch::async, [&] { return snapshot(); });
  EXPECT_EQ(first->id, second.get()->id);
  auto& registry = h->collection().getShard()->getSnapshots();
  EXPECT_EQ(1u, registry.stats().pins);
  for (const auto& file : first->files) {
    auto response = get(fileUrl(*first, file.name));
    ASSERT_EQ(200, response.result_int());
    EXPECT_EQ(file.size, response.body().size());
    EXPECT_EQ(file.xxh3, XXH3_64bits(response.body().data(), response.body().size()));
    EXPECT_EQ(registry.dir.openFile(file.name)->read(), response.body());
    auto partial = download(*first, file.name, "bytes=1-");
    ASSERT_EQ(206, partial.result_int());
    EXPECT_EQ(response.body().substr(1), partial.body());
    EXPECT_EQ("bytes 1-" + std::to_string(file.size - 1) + "/" + std::to_string(file.size), partial[http::field::content_range]);
    EXPECT_EQ(response.body().substr(response.body().size() - 1), download(*first, file.name, "bytes=-1").body());
    EXPECT_EQ(416, download(*first, file.name, "bytes=999999999999-").result_int());
    EXPECT_EQ(200, download(*first, file.name, "bytes=0-1,3-4").result_int());
    EXPECT_EQ(200, download(*first, file.name, "items=0-1").result_int());
    EXPECT_EQ(200, download(*first, file.name, "bytes=xyz").result_int());
    EXPECT_EQ(response.body().substr(1), download(*first, file.name, "bytes=1-999999999999999999999999").body());
  }
  auto a = std::async(std::launch::async, [&] { return get(fileUrl(*first, first->files.front().name)); });
  auto b = get(fileUrl(*first, first->files.front().name));
  EXPECT_EQ(a.get().body(), b.body());
  EXPECT_EQ(1u, registry.stats().pins);
  auto denied = get(fileUrl(*first, "../write.lock") + "&request_id=missing-file");
  EXPECT_EQ(404, denied.result_int());
  Json error;
  ASSERT_FALSE(glz::read_json(error, denied.body()));
  EXPECT_EQ("missing-file", error["request_id"].get<std::string>());
  EXPECT_EQ("file_not_in_snapshot", error["error"]["code"].get<std::string>());
  EXPECT_EQ(404, get("/_replication/missing/snapshot").result_int());
}

TEST_F(ReplicationHttpTest, expiryBudgetAndInstalledStats) {
  ASSERT_TRUE(h->index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto first = snapshot();
  auto& registry = h->collection().getShard()->getSnapshots();
  // Set a timeout already exceeded; no timing-dependent sleep is needed.
  registry.setPolicy({std::chrono::milliseconds(1), UINT64_MAX});
  auto until = std::chrono::steady_clock::now() + 2s;
  while (registry.stats().pins && std::chrono::steady_clock::now() < until) std::this_thread::yield();
  EXPECT_EQ(410, get(fileUrl(*first, first->files.front().name)).result_int());
  registry.setPolicy({60s, UINT64_MAX});
  first = snapshot();
  ASSERT_TRUE(h->index(flatdoc("id", "b"), UpdateMessage::COMMIT, false, 1).success);
  registry.setPolicy({60s, 0});
  EXPECT_EQ(410, get(fileUrl(*first, first->files.front().name)).result_int());
  auto current = snapshot();
  auto installed = httpRequest(server->getPort(), http::verb::post, "/_replication/installed",
      "{\"follower\":\"f1\",\"collection\":\"main\",\"commit\":\"" + current->id.token() + "\"}");
  ASSERT_EQ(200, installed.result_int()) << installed.body();
  h->getIndexWriter()->setSchema(Schema::createDefaultSchema());
  auto stats = get("/_replication/status");
  Json json;
  ASSERT_FALSE(glz::read_json(json, stats.body()));
  auto followers = json["followers"].get<Json::array_t>();
  ASSERT_EQ(1u, followers.size());
  EXPECT_EQ("f1", followers[0]["follower"].get<std::string>());
  EXPECT_EQ(1, followers[0]["lag"].get<double>());
  EXPECT_GT(followers[0]["last_seen"].get<double>(), 0);
}

TEST_F(ReplicationHttpTest, fullAcknowledgmentTableKeepsExistingFollowers) {
  auto current = snapshot();
  auto body = [&](int follower) {
    return "{\"follower\":\"f" + std::to_string(follower) + "\",\"collection\":\"main\",\"commit\":\"" + current->id.token() + "\"}";
  };
  for (int i = 0; i < 4096; i++) node->getReplication().installed(*node, body(i));
  EXPECT_EQ(429, httpRequest(server->getPort(), http::verb::post, "/_replication/installed", body(4096)).result_int());
  EXPECT_EQ(200, httpRequest(server->getPort(), http::verb::post, "/_replication/installed", body(0)).result_int());
  EXPECT_EQ(4096u, node->getReplication().stats(*node).size());
}

TEST_F(ReplicationHttpTest, slowDownloadSurvivesMerge) {
  auto first = largeSnapshot();
  auto file = *std::ranges::max_element(first->files, {}, &FileDescriptor::size);
  ASSERT_GT(file.size, 4u * 1024 * 1024);
  net::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), (unsigned short)server->getPort()));
  http::request<http::empty_body> request(http::verb::get, fileUrl(*first, file.name), 11);
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response_parser<http::string_body> response;
  response.body_limit(32 * 1024 * 1024);
  http::read_header(stream, buffer, response);
  ASSERT_EQ(200, response.get().result_int());
  ASSERT_TRUE(h->index(flatdoc("id", "large", "payload", "replacement"), UpdateMessage::COMMIT, false, 1).success);
  auto& registry = h->collection().getShard()->getSnapshots();
  EXPECT_GT(registry.stats().retainedBytes, 0u);
  EXPECT_EQ(206, download(*first, file.name, "bytes=0-127").result_int());
  EXPECT_EQ(200, get("/health").result_int());
  http::read(stream, buffer, response);
  auto& body = response.get().body();
  EXPECT_EQ(file.size, body.size());
  EXPECT_EQ(file.xxh3, XXH3_64bits(body.data(), body.size()));
  registry.setPolicy({60s, 0});
  EXPECT_EQ(410, get(fileUrl(*first, file.name)).result_int());
}


TEST_F(ReplicationHttpTest, evictionAbortsStalledReader) {
  stalledTransfer([&](const auto& snapshot) { h->collection().getShard()->getSnapshots().evictOldest(); });
}

TEST_F(ReplicationHttpTest, deleteAbortsStalledReader) {
  stalledTransfer([&](const auto&) { node->deleteCollection("main"); });
}

TEST_F(ReplicationHttpTest, budgetEvictionAbortsStalledReader) {
  stalledTransfer([&](const auto&) {
    ASSERT_TRUE(h->index(flatdoc("id", "large", "payload", "replacement"), UpdateMessage::COMMIT, false, 1).success);
    h->collection().getShard()->getSnapshots().setPolicy({60s, 0});
  });
}

TEST_F(ReplicationHttpTest, snapshotJsonHeadAndEmptyFile) {
  auto& registry = h->collection().getShard()->getSnapshots();
  auto file = registry.dir.createFile("zero");
  OutputStream out(file.get());
  out.close();
  registry.dir.finishFile(*file);
  std::array<std::string, 2> names{"zero", "."};
  registry.dir.sync(names);
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(registry.snapshot()->bytes, arena);
  api::FileDescriptor descriptor{"zero", 0, XXH3_64bits(nullptr, 0)};
  api::AuxIndexInfo aux;
  aux.files = {&descriptor, 1};
  info.aux_indexes = {&aux, 1};
  info.index_gen++;
  auto bytes = std::make_shared<std::vector<std::byte>>();
  ASSERT_TRUE(api::encode(info, *bytes));
  registry.publish(CommitSnapshot::fromBytes(bytes));
  auto snap = snapshot();
  EXPECT_EQ(200, get(fileUrl(*snap, "zero")).result_int());
  EXPECT_TRUE(get(fileUrl(*snap, "zero")).body().empty());
  EXPECT_EQ(416, download(*snap, "zero", "bytes=0-").result_int());
  auto head = download(*snap, "zero", {}, true);
  EXPECT_EQ(200, head.result_int());
  EXPECT_EQ("0", head[http::field::content_length]);
  auto missing = download(*snap, "missing", {}, true);
  EXPECT_EQ(404, missing.result_int());
  EXPECT_TRUE(missing.body().empty());
  auto json = get("/_replication/main/snapshot?format=json");
  ASSERT_EQ(200, json.result_int());
  EXPECT_EQ("application/json", json[http::field::content_type]);
  api::IndexInfo decoded;
  ASSERT_TRUE(api::read_json(decoded, json.body(), arena));
  ASSERT_EQ(1u, decoded.aux_indexes.size());
  EXPECT_EQ("zero", decoded.aux_indexes[0].files[0].name);
  EXPECT_EQ(descriptor.xxh3, decoded.aux_indexes[0].files[0].xxh3);
}

TEST_F(ReplicationHttpTest, installedRejectsInvalidAcknowledgments) {
  auto id = snapshot()->id;
  auto post = [&](std::string follower, std::string collection, std::string token) {
    return httpRequest(server->getPort(), http::verb::post, "/_replication/installed",
        "{\"follower\":\"" + follower + "\",\"collection\":\"" + collection + "\",\"commit\":\"" + token + "\"}");
  };
  EXPECT_EQ(400, post("", "main", id.token()).result_int());
  EXPECT_EQ(400, post(std::string(256, 'x'), "main", id.token()).result_int());
  EXPECT_EQ(400, post("f", "missing", id.token()).result_int());
  EXPECT_EQ(400, post("f", "main", "malformed").result_int());
  EXPECT_EQ(400, post("f", "main", "foreign:1").result_int());
  id.index_gen++;
  EXPECT_EQ(400, post("f", "main", id.token()).result_int());
  EXPECT_TRUE(node->getReplication().stats(*node).empty());
}

TEST_F(ReplicationHttpTest, nameDeletionSurvivesIncarnationChange) {
  auto first = catalog();
  auto old = snapshot()->id;
  h->getIndexWriter()->testDeleteAllData();
  auto recreated = catalog();
  EXPECT_NE(old.incarnation, snapshot()->id.incarnation);
  node->deleteCollection("main");
  auto removed = catalog("/_replication/watch?since=" + cursor(first));
  EXPECT_FALSE(removed["collections"].contains("main"));
  EXPECT_NE(cursor(removed), cursor(recreated));
  EXPECT_FALSE(removed.contains("deleted"));
}

TEST_F(ReplicationHttpTest, acknowledgmentsExpireAndWatchRenewsLiveness) {
  auto time = ReplicationCatalog::Clock::now();
  ReplicationCatalog catalog(90s, [&] { return time; });
  auto id = snapshot()->id;
  auto body = [&](int follower) {
    return "{\"follower\":\"f" + std::to_string(follower) + "\",\"collection\":\"main\",\"commit\":\"" + id.token() + "\"}";
  };
  for (int i = 0; i < 4096; i++) catalog.installed(*node, body(i));
  time += 60s;
  catalog.watch({}, "f0", [] {});
  time += 30s;
  catalog.installed(*node, body(4096));
  EXPECT_EQ(2u, catalog.stats(*node).size());
  catalog.remove("main");
  for (const auto& row : catalog.stats(*node)) EXPECT_TRUE(row.collection.empty());
  catalog.installed(*node, body(0));
  h->getIndexWriter()->testDeleteAllData();
  for (const auto& row : catalog.stats(*node)) EXPECT_TRUE(row.collection.empty());
  auto response = get("/_replication/watch?follower=watch-only&timeout_ms=0");
  EXPECT_EQ(200, response.result_int());
  auto live = node->getReplication().stats(*node);
  ASSERT_EQ(1u, live.size());
  EXPECT_EQ("watch-only", live[0].follower);
}

class RamReplicationHttpTest : public ReplicationHttpTest {
  void SetUp() override { setup("ram"); }
};
TEST_F(RamReplicationHttpTest, fileBytesMatchManifest) {
  ASSERT_TRUE(h->index(flatdoc("id", "ram"), UpdateMessage::COMMIT).success);
  auto snap = snapshot();
  for (const auto& file : snap->files) {
    auto response = get(fileUrl(*snap, file.name));
    ASSERT_EQ(200, response.result_int());
    EXPECT_EQ(file.size, response.body().size());
    EXPECT_EQ(file.xxh3, XXH3_64bits(response.body().data(), response.body().size()));
  }
}

class ExpiringReplicationHttpTest : public ReplicationHttpTest {
  void SetUp() override { setup("fs", 40); }
};
TEST_F(ExpiringReplicationHttpTest, timerExpiresWithoutRequests) {
  auto& registry = h->collection().getShard()->getSnapshots();
  std::stop_token cancellation;
  registry.acquire(&cancellation);
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!cancellation.stop_requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
  EXPECT_TRUE(cancellation.stop_requested());
}

TEST_F(ReplicationHttpTest, expiryAbortsStalledReader) {
  stalledTransfer([&](const auto&) {
    std::this_thread::sleep_for(5ms);
    h->collection().getShard()->getSnapshots().setPolicy({1ms, UINT64_MAX});
  });
}

TEST_F(ReplicationHttpTest, watchTimeoutIsClamped) {
  auto initial = catalog();
  // A foreign cursor returns immediately even when its requested timeout is huge.
  auto response = catalog("/_replication/watch?since=foreign:1&timeout_ms=18446744073709551615");
  EXPECT_EQ(cursor(initial), cursor(response));
  EXPECT_FALSE(response.contains("watch_timeout_ms"));
  EXPECT_FALSE(response.contains("oldest_tombstone_revision"));
}

TEST_F(ReplicationHttpTest, shutdownJoinsNotificationAlreadyRemovedFromCatalog) {
  auto initial = catalog();
  std::promise<void> parked, notifying, resume, closing;
  auto resumed = resume.get_future();
  Signal::listen("replicationWatchParked", [&](void*, void*, void*) -> void* { parked.set_value(); return nullptr; });
  auto watch = std::async(std::launch::async, [&] {
    try { get("/_replication/watch?since=" + cursor(initial)); } catch (const std::exception&) {}
  });
  ASSERT_EQ(std::future_status::ready, parked.get_future().wait_for(1s));
  Signal::listen("replicationWatchNotify", [&](void*, void*, void*) -> void* {
    notifying.set_value(); resumed.wait(); return nullptr;
  });
  Signal::listen("httpSessionsClosing", [&](void*, void*, void*) -> void* { closing.set_value(); return nullptr; });
  auto publish = std::async(std::launch::async, [&] { return h->index(flatdoc("id", "race"), UpdateMessage::COMMIT).success; });
  EXPECT_EQ(std::future_status::ready, notifying.get_future().wait_for(1s));
  auto shutdown = std::async(std::launch::async, [&] { server->shutdown(); });
  EXPECT_EQ(std::future_status::ready, closing.get_future().wait_for(1s));
  resume.set_value();
  EXPECT_EQ(std::future_status::ready, shutdown.wait_for(1s));
  shutdown.get();
  EXPECT_TRUE(publish.get());
  watch.get();
}


TEST_F(ReplicationHttpTest, headAndMonotonicAcknowledgment) {
  auto& registry = h->collection().getShard()->getSnapshots();
  auto old = registry.snapshot()->id;
  auto head = httpRequest(server->getPort(), http::verb::head, "/_replication/main/snapshot");
  EXPECT_EQ(old.token(), head["X-Luxir-Commit"]);
  EXPECT_EQ(0u, registry.stats().pins);
  ASSERT_TRUE(h->index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto current = registry.snapshot()->id;
  auto ack = [&](const CommitId& id) {
    node->getReplication().installed(*node, "{\"follower\":\"f\",\"collection\":\"main\",\"commit\":\"" + id.token() + "\"}");
  };
  ack(current);
  ack(old);
  EXPECT_EQ(current, node->getReplication().stats(*node)[0].commit);
  EXPECT_EQ(200, get("/_replication/main/snapshot?follower=downloader").result_int());
  auto snap = registry.snapshot();
  EXPECT_EQ(200, get(fileUrl(*snap, snap->files.front().name) + "&follower=file-reader").result_int());
  EXPECT_EQ(3u, node->getReplication().stats(*node).size());
}


TEST_F(ReplicationHttpTest, socketDeadlineAbortsStalledSharedReservation) {
  Signal::listen("httpWriteIdleTimeout", [](void* timeout, void*, void*) -> void* {
    *(std::chrono::milliseconds*)timeout = 100ms; return nullptr;
  });
  std::atomic<bool> timedOut = false;
  Signal::listen("httpWriteTimedOut", [&](void*, void*, void*) -> void* { timedOut = true; return nullptr; });
  std::jthread active;
  stalledTransfer([&](const auto&) {
    active = std::jthread([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        get("/_replication/main/snapshot"); // Another client renews the same reservation.
        std::this_thread::sleep_for(10ms);
      }
    });
  });
  active.request_stop(); active.join();
  EXPECT_TRUE(timedOut);
  EXPECT_EQ(1u, h->collection().getShard()->getSnapshots().stats().pins);
}

TEST_F(ReplicationHttpTest, socketDeadlineCoversOrdinarySearchResponses) {
  largeSnapshot();
  Signal::listen("httpWriteIdleTimeout", [](void* timeout, void*, void*) -> void* {
    *(std::chrono::milliseconds*)timeout = 100ms; return nullptr;
  });
  std::promise<void> timedOut;
  Signal::listen("httpWriteTimedOut", [&](void*, void*, void*) -> void* { timedOut.set_value(); return nullptr; });
  net::io_context io;
  beast::tcp_stream stream(io);
  stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), (unsigned short)server->getPort()));
  stream.socket().set_option(net::socket_base::receive_buffer_size(4096));
  http::request<http::string_body> request(http::verb::post, "/collections/main/_search", 11);
  request.set(http::field::content_type, "application/json");
  request.body() = R"({"query":"id:large","fields":["payload"]})"; request.prepare_payload();
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response_parser<http::empty_body> response;
  http::read_header(stream, buffer, response);
  ASSERT_EQ(200, response.get().result_int());
  EXPECT_EQ(std::future_status::ready, timedOut.get_future().wait_for(3s));
  EXPECT_EQ(200, get("/health").result_int());
}

}
