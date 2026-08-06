#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "solux/server/GRPCServer.h"
#include "solux/server/HttpServer.h"
#include "solux/store/Directory.h"
#include "test/GrpcClient.h"
#include "test/HttpReq.h"
#include "test/SoluxTest.h"

namespace solux::test {

class ReadOnlyNodeTest : public SoluxTest {};

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

SoluxConfig fsConfig(const DataDir& data, bool readOnly = false) {
  SoluxConfig config;
  config.store.backend = "fs";
  config.store.data_dir = data.path().string();
  config.read_only = readOnly;
  config.normalize();
  return config;
}

// Seeds a data directory with one committed doc and leaves the writer running.
struct RunningWriter {
  SoluxNode node;
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
  DataDir data("solux_read_only_http");
  RunningWriter writer(data);

  // The writer is still up and still holds write.lock.
  SoluxNode reader(fsConfig(data, /*readOnly=*/true));
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
  EXPECT_EQ(200, httpRequest(port, http::verb::get, "/collections/main/_schema").result_int());
  expectRefused("update of absent collection", http::verb::post, "/collections/absent/_update",
                R"({"docs":[{"id":"r4"}],"commit":{}})");

  // The writer never lost its lock: a second writer is still refused, and the
  // writer itself can still index and commit while the reader serves.
  EXPECT_THROW({ SoluxNode second(fsConfig(data)); }, std::runtime_error);
  auto stillWriting = httpRequest(writer.server.getPort(), http::verb::post,
      "/collections/main/_update",
      R"({"docs":[{"id":"r5","title_w":"lockless token"}],"commit":{}})");
  EXPECT_EQ(200, stillWriting.result_int()) << stillWriting.body();

  readerServer.shutdown();
}

TEST_F(ReadOnlyNodeTest, grpcRefusesMutatingMethods) {
  DataDir data("solux_read_only_grpc");
  RunningWriter writer(data);

  SoluxNode reader(fsConfig(data, /*readOnly=*/true));
  GRPCServer readerServer(reader, 2, 0);
  std::thread serverThread([&] { readerServer.run(); });
  ASSERT_TRUE(readerServer.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(readerServer.getPort()),
                                     grpc::InsecureChannelCredentials());

  solux::api::UpdateRequest update;
  grpc::ClientContext updateContext;
  Reply<solux::api::UpdateResponse> updateReply;
  auto updateStatus = hppUnaryCall(channel.get(), rpc::Update, &updateContext, update, &updateReply);
  EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, updateStatus.error_code())
      << updateStatus.error_message();
  EXPECT_NE(updateStatus.error_message().find("read-only"), std::string::npos)
      << updateStatus.error_message();

  solux::api::CreateCollectionRequest create;
  create.name = "read_only_grpc_new";
  grpc::ClientContext createContext;
  Reply<solux::api::CreateCollectionResponse> createReply;
  auto createStatus =
      hppUnaryCall(channel.get(), rpc::CreateCollection, &createContext, create, &createReply);
  EXPECT_EQ(grpc::StatusCode::FAILED_PRECONDITION, createStatus.error_code())
      << createStatus.error_message();

  // Read methods are unaffected.
  solux::api::StatsRequest stats;
  grpc::ClientContext statsContext;
  Reply<solux::api::StatsResponse> statsReply;
  auto statsStatus = hppUnaryCall(channel.get(), rpc::Stats, &statsContext, stats, &statsReply);
  EXPECT_TRUE(statsStatus.ok()) << statsStatus.error_message();

  readerServer.shutdown();
  serverThread.join();
}

// A read-only node writes nothing at startup: no data dir, no lock file, no trash.
TEST_F(ReadOnlyNodeTest, startupTouchesNothing) {
  DataDir data("solux_read_only_startup");

  EXPECT_THROW({ SoluxNode reader(fsConfig(data, /*readOnly=*/true)); }, ReadOnlyError);
  EXPECT_FALSE(std::filesystem::exists(data.path()));

  { SoluxNode writer(fsConfig(data)); }
  ASSERT_TRUE(std::filesystem::exists(data.path() / "write.lock"));
  std::filesystem::remove(data.path() / "write.lock");
  std::filesystem::remove_all(data.path() / "trash");

  { SoluxNode reader(fsConfig(data, /*readOnly=*/true)); }
  EXPECT_FALSE(std::filesystem::exists(data.path() / "write.lock"));
  EXPECT_FALSE(std::filesystem::exists(data.path() / "trash"));
}

// There is no existing data directory to serve without the fs backend.
TEST_F(ReadOnlyNodeTest, readOnlyRequiresTheFsBackend) {
  SoluxConfig config;
  config.read_only = true;
  config.store.backend = "ram";
  EXPECT_THROW(config.normalize(), std::runtime_error);
}

} // namespace solux::test
