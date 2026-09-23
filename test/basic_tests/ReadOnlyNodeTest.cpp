// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <memory_resource>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/store/Directory.h"
#include "test/GrpcClient.h"
#include "luxir/server/RpcStatus.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

class ReadOnlyNodeTest : public LuxirTest {};

namespace {

class DataDir {
  std::filesystem::path path_;

public:
  explicit DataDir(std::string_view prefix) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
        (std::string(prefix) + "_" + std::to_string(stamp));
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

// Seeds a data directory with one committed doc and leaves the writer running.
struct RunningWriter {
  LuxirNode node;
  HttpServer server;

  explicit RunningWriter(const DataDir& data) : node(fsConfig(data)), server(node, 2, 0) {
    server.start();
    auto indexed = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update",
        R"({"docs":[{"id":"r1","title_w":"lockless token"}],"commit":{}})");
    EXPECT_EQ(200, indexed.result_int()) << indexed.body();
  }

  ~RunningWriter() { server.shutdown(); }
};

} // namespace

// A read-only node shares a data directory with the instance that owns it: it opens
// without contending for write.lock, serves searches, and refuses every mutation.
TEST_F(ReadOnlyNodeTest, servesSearchesAlongsideTheWriter) {
  DataDir data("luxir_read_only_http");
  RunningWriter writer(data);

  // The writer is still up and still holds write.lock.
  LuxirNode reader(fsConfig(data, /*readOnly=*/true));
  EXPECT_EQ(nullptr, reader.getCollection("main")->getShard()->getIndexWriter());
  HttpServer readerServer(reader, 2, 0);
  readerServer.start();
  int port = readerServer.getPort();

  auto search = httpRequest(port, http::verb::post, "/collections/main/_search",
                            R"({"query":"title_w:lockless","fields":["id"],"get_number":true})");
  ASSERT_EQ(200, search.result_int()) << search.body();
  EXPECT_NE(search.body().find(R"("found":1)"), std::string::npos) << search.body();
  EXPECT_NE(search.body().find(R"("r1")"), std::string::npos) << search.body();

  auto expectRefused = [&](std::string_view what, http::verb method, std::string target,
                           std::string body, std::string_view contentType = "application/json") {
    auto res = httpRequest(port, method, target, std::move(body), contentType);
    EXPECT_EQ(403, res.result_int()) << what << ": " << res.body();
    EXPECT_NE(res.body().find("read-only"), std::string::npos) << what << ": " << res.body();
    glz::generic_i64 root;
    ASSERT_FALSE(glz::read_json(root, res.body())) << what << ": " << res.body();
    EXPECT_EQ("failed_precondition", root["error"]["kind"].get_string());
    EXPECT_EQ("read_only", root["error"]["code"].get_string());
  };

  expectRefused("update", http::verb::post, "/collections/main/_update",
                R"({"docs":[{"id":"r2","title_w":"rejected token"}],"commit":{}})");
  expectRefused("ndjson stream", http::verb::post, "/collections/main/_update",
                "{\"id\":\"r3\",\"title_w\":\"rejected token\"}\n", "application/x-ndjson");
  expectRefused("schema set", http::verb::post, "/collections/main/_schema",
                R"({"fields": {"title": {"type": "text"}}})");
  expectRefused("collection create", http::verb::post, "/collections/_create",
                R"({"name":"read_only_new"})");
  expectRefused("collection delete", http::verb::post, "/collections/_delete",
                R"({"name":"main"})");

  // Reads stay available, and the update to a collection that does not exist is
  // refused as read-only rather than auto-creating it.
  EXPECT_EQ(200, httpRequest(port, http::verb::get, "/_stats").result_int());
  EXPECT_EQ(200, httpRequest(port, http::verb::get, "/collections").result_int());
  EXPECT_EQ(200, httpRequest(port, http::verb::get, "/collections/main/_schema").result_int());
  expectRefused("update of absent collection", http::verb::post, "/collections/absent/_update",
                R"({"docs":[{"id":"r4"}],"commit":{}})");

  // The writer never lost its lock: a second writer is still refused, and the
  // writer itself can still index and commit while the reader serves.
  EXPECT_THROW({ LuxirNode second(fsConfig(data)); }, std::runtime_error);
  auto stillWriting = httpRequest(writer.server.getPort(), http::verb::post,
      "/collections/main/_update",
      R"({"docs":[{"id":"r5","title_w":"lockless token"}],"commit":{}})");
  EXPECT_EQ(200, stillWriting.result_int()) << stillWriting.body();

  readerServer.shutdown();
}

TEST_F(ReadOnlyNodeTest, grpcRefusesMutatingMethods) {
  DataDir data("luxir_read_only_grpc");
  RunningWriter writer(data);

  LuxirNode reader(fsConfig(data, /*readOnly=*/true));
  GRPCServer readerServer(reader, 2, 0);
  std::thread serverThread([&] { readerServer.run(); });
  ASSERT_TRUE(readerServer.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(readerServer.getPort()),
                                     grpc::InsecureChannelCredentials());

  luxir::api::UpdateRequest update;
  grpc::ClientContext updateContext;
  Reply<luxir::api::UpdateResponse> updateReply;
  auto updateStatus = hppUnaryCall(channel.get(), rpc::Update, &updateContext, update, &updateReply);
  EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, updateStatus.error_code())
      << updateStatus.error_message();
  EXPECT_NE(updateStatus.error_message().find("read-only"), std::string::npos)
      << updateStatus.error_message();
  std::pmr::monotonic_buffer_resource arena;
  luxir::api::Error detail;
  ASSERT_TRUE(decodeRpcStatusDetails(updateStatus.error_details(), detail, arena));
  EXPECT_EQ("read_only", detail.code);
  EXPECT_EQ(luxir::api::Error::Kind::FAILED_PRECONDITION, detail.kind);

  luxir::api::CreateCollectionRequest create;
  create.name = "read_only_grpc_new";
  grpc::ClientContext createContext;
  Reply<luxir::api::CreateCollectionResponse> createReply;
  auto createStatus =
      hppUnaryCall(channel.get(), rpc::CreateCollection, &createContext, create, &createReply);
  EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, createStatus.error_code())
      << createStatus.error_message();

  // Read methods are unaffected.
  luxir::api::StatsRequest stats;
  grpc::ClientContext statsContext;
  Reply<luxir::api::StatsResponse> statsReply;
  auto statsStatus = hppUnaryCall(channel.get(), rpc::Stats, &statsContext, stats, &statsReply);
  EXPECT_TRUE(statsStatus.ok()) << statsStatus.error_message();

  readerServer.shutdown();
  serverThread.join();
}

// A read-only node writes nothing at startup: no data dir, no lock file, no trash.
TEST_F(ReadOnlyNodeTest, startupTouchesNothing) {
  DataDir data("luxir_read_only_startup");

  EXPECT_THROW({ LuxirNode reader(fsConfig(data, /*readOnly=*/true)); }, ReadOnlyError);
  EXPECT_FALSE(std::filesystem::exists(data.path()));

  { LuxirNode writer(fsConfig(data)); }
  ASSERT_TRUE(std::filesystem::exists(data.path() / "write.lock"));
  std::filesystem::remove(data.path() / "write.lock");
  std::filesystem::remove_all(data.path() / "trash");

  { LuxirNode reader(fsConfig(data, /*readOnly=*/true)); }
  EXPECT_FALSE(std::filesystem::exists(data.path() / "write.lock"));
  EXPECT_FALSE(std::filesystem::exists(data.path() / "trash"));
}

// There is no existing data directory to serve without the fs backend.
TEST_F(ReadOnlyNodeTest, readOnlyRequiresTheFsBackend) {
  LuxirConfig config;
  config.read_only = true;
  config.store.backend = "ram";
  EXPECT_THROW(config.normalize(), std::runtime_error);
}

} // namespace luxir::test
