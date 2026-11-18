// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/client_unary_call.h>
#include <grpcpp/impl/rpc_method.h>
#include <grpcpp/support/byte_buffer.h>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/Readiness.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/util/Uuid.h"

namespace luxir::test {
using namespace std::chrono_literals;
using Json = glz::generic;

class ReadinessTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("luxir-ready-" + newUuid());
  std::unique_ptr<LuxirNode> node;
  std::unique_ptr<HttpServer> server;

  void start(LuxirConfig config = {}) {
    node = std::make_unique<LuxirNode>(config);
    server = std::make_unique<HttpServer>(*node, 1, 0);
    server->start();
  }
  LuxirConfig fsConfig() {
    LuxirConfig config;
    config.store.backend = "fs";
    config.store.data_dir = path.string();
    return config;
  }
  void TearDown() override {
    if (server) server->shutdown();
    server.reset();
    node.reset();
    std::filesystem::remove_all(path);
    LuxirTest::TearDown();
  }
  std::pair<int, Json> ready() {
    node->readiness().refresh();
    auto response = httpRequest(server->getPort(), http::verb::get, "/_ready");
    Json json;
    EXPECT_FALSE(glz::read_json(json, response.body())) << response.body();
    return {response.result_int(), json};
  }
  template <class Predicate> bool until(Predicate predicate) {
    auto end = std::chrono::steady_clock::now() + 10s;
    do { if (predicate()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < end);
    return false;
  }
};

TEST_F(ReadinessTest, unavailableCollectionHoldsBackReadiness) {
  start();
  CollectionHelper h(*node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(200, httpRequest(server->getPort(), http::verb::get, "/_health").result_int());
  auto [ok, json] = ready();
  EXPECT_EQ(200, ok);
  EXPECT_TRUE(json["ready"].get<bool>());
  ASSERT_EQ(1u, json["collections"].get<Json::array_t>().size());

  h.collection().fail("test failure");
  auto [status, failed] = ready();
  EXPECT_EQ(503, status);
  EXPECT_FALSE(failed.contains("ready"));  // proto3 JSON omits false
  EXPECT_NE(std::string::npos, failed["reason"].get<std::string>().find("test failure"));
  // /health is liveness only.
  EXPECT_EQ(200, httpRequest(server->getPort(), http::verb::get, "/health").result_int());
}

TEST_F(ReadinessTest, ramFilesAreResident) {
  start();
  CollectionHelper h(*node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto reader = node->getCollection("main")->getReaderManager().getReader();
  auto files = Readiness::files(*reader);
  ASSERT_FALSE(files.empty());
  auto counted = Readiness::residency(files);
  EXPECT_GT(counted.fileBytes, 0u);
  EXPECT_EQ(counted.fileBytes, counted.residentBytes);
  EXPECT_EQ(0u, counted.unknownBytes);
  auto [status, json] = ready();
  EXPECT_EQ(200, status);
  EXPECT_EQ(1.0, json["collections"][0]["resident_fraction"].get<double>());
}

TEST_F(ReadinessTest, mappedFileResidencyAfterLoad) {
  std::filesystem::create_directories(path);
  FSDirectory dir(path);
  for (size_t size : {(size_t)320 * 1024, (size_t)16 << 20}) {  // every page, then sampled
    auto name = "f" + std::to_string(size);
    {
      auto file = dir.createFile(name);
      OutputStream out(file.get());
      std::string chunk(64 * 1024, 'x');
      for (size_t written = 0; written < size; written += chunk.size()) out.write(chunk.data(), chunk.size());
      out.close();
      dir.finishFile(*file);
    }
    auto input = dir.openFile(name);
    ASSERT_EQ(size, input->size());
    input->load(0, input->size());
    auto resident = input->residentBytes(Readiness::kSamplePages);
    ASSERT_TRUE(resident.has_value());
    EXPECT_EQ(size, *resident);
  }
}

TEST_F(ReadinessTest, residencyGateWarmsAndWarmRoute) {
  auto config = fsConfig();
  config.server.ready.min_resident_fraction = 1.0;
  start(config);
  CollectionHelper h(*node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  // Below the fraction the collection warms itself; then it is ready.
  ASSERT_TRUE(until([&] { return ready().first == 200; }));
  auto json = ready().second;
  auto& row = json["collections"][0];
  EXPECT_GT(row["file_bytes"].get<double>(), 0);
  EXPECT_EQ(1.0, row["resident_fraction"].get<double>());

  auto port = server->getPort();
  ASSERT_TRUE(until([&] {
    auto row = ready().second["collections"][0];
    return !row.contains("warmup") || row["warmup"].contains("finished");
  }));
  EXPECT_EQ(202, httpRequest(port, http::verb::post, "/collections/main/_warm?budget_mb=1").result_int());
  EXPECT_EQ(400, httpRequest(port, http::verb::post, "/collections/main/_warm?budget_mb=0").result_int());
  EXPECT_EQ(404, httpRequest(port, http::verb::post, "/collections/nope/_warm").result_int());
  EXPECT_EQ(405, httpRequest(port, http::verb::get, "/collections/main/_warm").result_int());
  ASSERT_TRUE(until([&] {
    auto warmed = ready().second["collections"][0];
    return warmed.contains("warmup") && warmed["warmup"].contains("finished") && !warmed["warmup"].contains("automatic");
  }));
  auto warmup = ready().second["collections"][0]["warmup"];
  EXPECT_GT(warmup["loaded_bytes"].get<double>(), 0);
  EXPECT_EQ(warmup["loaded_bytes"].get<double>(), warmup["budget_bytes"].get<double>());

  auto metrics = httpRequest(port, http::verb::get, "/metrics").body();
  EXPECT_NE(std::string::npos, metrics.find("luxir_ready 1"));
  EXPECT_NE(std::string::npos, metrics.find("luxir_collection_resident_fraction{tenant=\"t0\",collection=\"main\"} 1"));
}

TEST_F(ReadinessTest, grpcHealthServiceFollowsReadiness) {
  start();
  CollectionHelper h(*node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  node->readiness().refresh();
  GRPCServer grpcServer(*node, 1, 0);
  std::thread thread([&] { grpcServer.run(); });
  auto stop = scope_guard([&] { grpcServer.shutdown(); thread.join(); });
  ASSERT_TRUE(grpcServer.waitForStart());
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(grpcServer.getPort()), grpc::InsecureChannelCredentials());
  // grpc.health.v1 HealthCheckRequest{service} -> HealthCheckResponse{status}, encoded by hand.
  auto check = [&](std::string_view service) {
    std::string request = std::string(1, '\x0a') + (char)service.size() + std::string(service);
    grpc::Slice slice(request);
    grpc::ByteBuffer in(&slice, 1), out;
    grpc::ClientContext context;
    grpc::internal::RpcMethod method("/grpc.health.v1.Health/Check", nullptr, grpc::internal::RpcMethod::NORMAL_RPC);
    auto status = grpc::internal::BlockingUnaryCall<grpc::ByteBuffer, grpc::ByteBuffer>(channel.get(), method, &context, in, &out);
    EXPECT_TRUE(status.ok()) << status.error_message();
    std::vector<grpc::Slice> slices;
    (void)out.Dump(&slices);
    std::string bytes;
    for (const auto& s : slices) bytes.append((const char*)s.begin(), s.size());
    return bytes.size() == 2 ? (int)bytes[1] : 0;  // 1 SERVING, 2 NOT_SERVING
  };
  EXPECT_EQ(1, check(""));
  EXPECT_EQ(1, check(Readiness::kService));
  h.collection().fail("test failure");
  node->readiness().refresh();
  EXPECT_EQ(2, check(Readiness::kService));
  EXPECT_EQ(1, check(""));
}

}
