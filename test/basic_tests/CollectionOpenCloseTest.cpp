// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <filesystem>
#include <future>
#include <thread>
#include <gtest/gtest.h>
#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/RpcStatus.h"
#include "test/CollectionHelper.h"
#include "test/GrpcClient.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class DataDir {
  std::filesystem::path path_;
public:
  explicit DataDir(std::string_view prefix) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / (std::string(prefix) + "_" + std::to_string(stamp));
    std::filesystem::remove_all(path_);
  }
  ~DataDir() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }
};

LuxirConfig fsConfig(const DataDir& data, bool readOnly = false) {
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = data.path().string();
  config.read_only = readOnly;
  config.normalize();
  return config;
}

}

// A writer on a filesystem data directory with one committed doc in "main".
class CollectionOpenCloseTest : public LuxirTest {
protected:
  DataDir data{"luxir_open_close"};
  LuxirNode node{fsConfig(data)};
  CollectionId main = CollectionId::of("main");
  CollectionStorage storage = node.collections().storage().collection(main);
  std::string incarnation;
  std::string commit;

  void SetUp() override {
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.index(flatdoc("id", "a", "title_w", "first"), UpdateMessage::COMMIT).success);
    incarnation = *storage.current();
    commit = token();
  }
  std::string token() { return node.getCollection(main)->getShard()->getSnapshots().snapshot()->id.token(); }
  int64_t liveDocs() { return node.getCollection(main)->getReaderManager().getReader()->liveDocs(); }
  bool uncommitted() { return node.getCollection(main)->getShard()->requireIndexWriter()->hasUncommittedChanges(); }
};

TEST_F(CollectionOpenCloseTest, closeThenOpenReusesTheDirectory) {
  node.closeCollection(main);
  EXPECT_THROW(node.getCollection(main), CollectionNotFoundError);
  EXPECT_TRUE(node.collectionEntries().empty());
  EXPECT_EQ(incarnation, *storage.current());

  auto opened = node.openCollection(main);
  EXPECT_EQ(opened, node.getCollection(main));
  EXPECT_EQ(1u, node.collectionEntries().size());
  EXPECT_EQ(1, liveDocs());
  EXPECT_EQ(commit, token());
  EXPECT_FALSE(uncommitted());
  // The reopened writer keeps indexing.
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(2, liveDocs());
}

TEST_F(CollectionOpenCloseTest, openWithPromoteStartsANewIncarnation) {
  node.closeCollection(main);
  node.openCollection(main, true);
  auto promoted = *storage.current();
  EXPECT_NE(incarnation, promoted);
  EXPECT_TRUE(CommitId::validIncarnation(promoted));
  EXPECT_EQ(std::vector<std::string>{promoted}, storage.incarnations());
  EXPECT_FALSE(std::filesystem::exists(data.path() / "t" / "t0" / "main" / incarnation));
  EXPECT_EQ(1, liveDocs());
  EXPECT_NE(commit, token());
  EXPECT_TRUE(token().starts_with(promoted + ":"));
}

TEST_F(CollectionOpenCloseTest, openRefusesARegisteredNameAndAnEmptyDirectory) {
  EXPECT_THROW(node.openCollection(main), CollectionExistsError);
  EXPECT_THROW(node.openCollection(CollectionId::of("absent")), CollectionNotFoundError);
  EXPECT_EQ(1, liveDocs());
}

TEST_F(CollectionOpenCloseTest, closeWithDiscardRemovesStorage) {
  node.closeCollection(main, true);
  EXPECT_THROW(node.getCollection(main), CollectionNotFoundError);
  EXPECT_FALSE(std::filesystem::exists(data.path() / "t" / "t0" / "main"));
  EXPECT_THROW(node.openCollection(main), CollectionNotFoundError);
  // The name is free again.
  EXPECT_TRUE(node.createCollection(main));
  EXPECT_EQ(0, liveDocs());
}

TEST_F(CollectionOpenCloseTest, openSelectsAnIncarnationWhenCurrentIsAbsent) {
  node.closeCollection(main);
  node.collections().storage().container(main, false)->deleteFile("CURRENT");
  EXPECT_FALSE(storage.current());
  EXPECT_THROW(node.openCollection(main), ApiError);
  EXPECT_THROW(node.openCollection(main, false, "0123456789abcdef"), ApiError);
  node.openCollection(main, false, incarnation);
  EXPECT_EQ(incarnation, *storage.current());
  EXPECT_EQ(1, liveDocs());
  EXPECT_EQ(commit, token());
}

TEST_F(CollectionOpenCloseTest, inFlightSearchesSurviveAClose) {
  HttpServer server(node, 2, 0);
  server.start();
  int port = server.getPort();
  auto reader = node.getCollection(main)->getReaderManager().getReader();
  std::atomic<bool> stop{false};
  std::atomic<int> ok{0}, gone{0};
  auto searcher = std::async(std::launch::async, [&] {
    while (!stop.load()) {
      auto res = httpRequest(port, http::verb::post, "/collections/main/_search", "{}");
      if (res.result_int() == 200) ok++;
      else if (res.result_int() == 404) gone++;
      else ADD_FAILURE() << res.result_int() << " " << res.body();
    }
  });
  while (ok.load() < 3) std::this_thread::yield();
  node.closeCollection(main);
  while (gone.load() < 3) std::this_thread::yield();
  stop = true;
  searcher.get();
  // A reader taken before the close still reads the closed files.
  EXPECT_EQ(1, reader->liveDocs());
  reader.reset();
  server.shutdown();
}

TEST_F(CollectionOpenCloseTest, expectCommitGuardsTheClose) {
  CollectionHelper h(node, "main");
  // An uncommitted update refuses the close and leaves the collection open.
  ASSERT_TRUE(h.index(flatdoc("id", "b")).success);
  EXPECT_TRUE(uncommitted());
  EXPECT_THROW(node.closeCollection(main, false, commit), CollectionChangedError);
  EXPECT_EQ(1, liveDocs());
  ASSERT_TRUE(h.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
  EXPECT_FALSE(uncommitted());
  EXPECT_EQ(3, liveDocs());
  // A newer commit refuses the stale token.
  EXPECT_THROW(node.closeCollection(main, false, commit), CollectionChangedError);
  EXPECT_THROW(node.closeCollection(main, false, "garbage"), ApiError);
  EXPECT_NO_THROW(node.getCollection(main));
  // Unchanged since the named commit: closed.
  node.closeCollection(main, false, token());
  EXPECT_THROW(node.getCollection(main), CollectionNotFoundError);
  node.openCollection(main);
  EXPECT_EQ(3, liveDocs());
}

TEST_F(CollectionOpenCloseTest, httpOpenCloseAndStats) {
  HttpServer server(node, 2, 0);
  server.start();
  int port = server.getPort();
  auto post = [&](std::string_view target, std::string body) {
    auto res = httpRequest(port, http::verb::post, target, std::move(body));
    return std::pair<int, std::string>{res.result_int(), res.body()};
  };
  auto stats = [&] { return httpRequest(port, http::verb::get, "/collections/main/_stats?pretty=false").body(); };

  EXPECT_EQ(std::string::npos, stats().find(R"("uncommitted":true)"));
  ASSERT_EQ(200, post("/collections/main/_update", R"({"docs":[{"id":"b"}]})").first);
  EXPECT_NE(std::string::npos, stats().find(R"("uncommitted":true)"));
  auto changed = post("/collections/main/_close", R"({"expect_commit":")" + commit + R"("})");
  EXPECT_EQ(409, changed.first) << changed.second;
  EXPECT_NE(std::string::npos, changed.second.find("collection_changed"));
  ASSERT_EQ(200, post("/collections/main/_update", R"({"commit":{}})").first);
  EXPECT_EQ(std::string::npos, stats().find(R"("uncommitted":true)"));

  auto exists = post("/tenants/t0/collections/main/_open", "{}");
  EXPECT_EQ(409, exists.first) << exists.second;
  EXPECT_NE(std::string::npos, exists.second.find("collection_exists"));
  EXPECT_EQ(400, post("/collections/main/_close", R"({"name":"other"})").first);
  EXPECT_EQ(400, post("/collections/main/_close", "{bad").first);

  auto closed = post("/collections/main/_close", R"({"expect_commit":")" + token() + R"("})");
  EXPECT_EQ(200, closed.first) << closed.second;
  EXPECT_EQ(R"({"name":"main","tenant":"t0"})" "\n", closed.second);
  EXPECT_EQ(404, post("/collections/main/_search", "{}").first);
  EXPECT_EQ(404, post("/collections/main/_close", "").first);

  auto opened = post("/tenants/t0/collections/main/_open", R"({"promote":true})");
  EXPECT_EQ(200, opened.first) << opened.second;
  auto promoted = *storage.current();
  EXPECT_NE(incarnation, promoted);
  EXPECT_NE(std::string::npos, opened.second.find(R"("incarnation":")" + promoted + "\""));
  EXPECT_NE(std::string::npos, opened.second.find(R"("commit":")" + promoted + ":"));
  EXPECT_NE(std::string::npos, opened.second.find(R"("tenant":"t0")"));
  EXPECT_EQ(200, post("/collections/main/_search", "{}").first);
  EXPECT_EQ(2, liveDocs());

  auto discarded = post("/collections/main/_close", R"({"discard":true})");
  EXPECT_EQ(200, discarded.first) << discarded.second;
  EXPECT_FALSE(std::filesystem::exists(data.path() / "t" / "t0" / "main"));
  EXPECT_EQ(404, post("/collections/main/_open", "").first);
  server.shutdown();
}

TEST_F(CollectionOpenCloseTest, readOnlyNodeRefusesOpenAndClose) {
  LuxirNode reader(fsConfig(data, true));
  HttpServer server(reader, 2, 0);
  server.start();
  for (const char* target : {"/collections/main/_close", "/collections/main/_open",
                             "/tenants/t0/collections/main/_close"}) {
    auto res = httpRequest(server.getPort(), http::verb::post, target, "{}");
    EXPECT_EQ(403, res.result_int()) << target << ": " << res.body();
    EXPECT_NE(std::string::npos, res.body().find("read_only")) << res.body();
  }
  EXPECT_THROW(reader.closeCollection(main), ReadOnlyError);
  EXPECT_THROW(reader.openCollection(CollectionId::of("other")), ReadOnlyError);
  EXPECT_EQ(1, reader.getCollection(main)->getReaderManager().getReader()->liveDocs());
  server.shutdown();
}

TEST_F(CollectionOpenCloseTest, grpcOpenAndClose) {
  GRPCServer server(node, 2, 0);
  std::thread serverThread([&] { server.run(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()), grpc::InsecureChannelCredentials());

  luxir::api::CloseCollectionRequest close;
  close.name = "main";
  close.expect_commit = "0123456789abcdef:1";
  {
    grpc::ClientContext context;
    Reply<luxir::api::CloseCollectionResponse> reply;
    auto status = hppUnaryCall(channel.get(), rpc::CloseCollection, &context, close, &reply);
    EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, status.error_code()) << status.error_message();
    std::pmr::monotonic_buffer_resource arena;
    luxir::api::Error detail;
    ASSERT_TRUE(decodeRpcStatusDetails(status.error_details(), detail, arena));
    EXPECT_EQ("collection_changed", detail.code);
  }
  close.expect_commit = commit;
  {
    grpc::ClientContext context;
    Reply<luxir::api::CloseCollectionResponse> reply;
    auto status = hppUnaryCall(channel.get(), rpc::CloseCollection, &context, close, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ("main", reply.msg.name);
    EXPECT_EQ("t0", reply.msg.tenant);
  }
  EXPECT_THROW(node.getCollection(main), CollectionNotFoundError);
  {
    grpc::ClientContext context;
    Reply<luxir::api::CloseCollectionResponse> reply;
    auto status = hppUnaryCall(channel.get(), rpc::CloseCollection, &context, close, &reply);
    EXPECT_EQ(grpc::StatusCode::NOT_FOUND, status.error_code()) << status.error_message();
  }

  luxir::api::OpenCollectionRequest open;
  open.name = "main";
  open.tenant = "t0";
  {
    grpc::ClientContext context;
    Reply<luxir::api::OpenCollectionResponse> reply;
    auto status = hppUnaryCall(channel.get(), rpc::OpenCollection, &context, open, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ("main", reply.msg.name);
    EXPECT_EQ(incarnation, reply.msg.incarnation);
    EXPECT_EQ(commit, reply.msg.commit);
  }
  EXPECT_EQ(1, liveDocs());
  {
    grpc::ClientContext context;
    Reply<luxir::api::OpenCollectionResponse> reply;
    auto status = hppUnaryCall(channel.get(), rpc::OpenCollection, &context, open, &reply);
    EXPECT_EQ(grpc::StatusCode::ALREADY_EXISTS, status.error_code()) << status.error_message();
  }
  server.shutdown();
  serverThread.join();
}
