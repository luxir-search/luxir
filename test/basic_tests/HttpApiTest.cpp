#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/HttpReq.h"
#include "test/SchemaBuilder.h"
#include "luxir/api/build.h"
#include "luxir/reader/Postings.h"
#include "luxir/schema/Schema.h"
#include "luxir/server/HttpServer.h"

namespace luxir::test {

// End-to-end coverage of the Phase 0 HTTP/JSON API (POST /collections/{c}/query
// + GET /health), exercising the async Beast plumbing against the same in-process
// node the rest of the suite uses.  No mocks; RAMDir.
class HttpApiTest : public LuxirTest {
protected:
  std::optional<HttpServer> server;
  CollectionHelper helper{"main"};

  void SetUp() override {
    server.emplace(*LuxirTest::luxirNode, 2 /*threads*/, 0 /*OS-assigned port*/);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
  }

  int port() { return server->getPort(); }

  static bool waitForShardThreads(HttpServer& server, int expected,
                                  std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (server.getRunningShardThreads() == expected) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return server.getRunningShardThreads() == expected;
  }

  static std::set<std::string> idsOf(const std::vector<Doc>& docs) {
    std::set<std::string> s;
    for (auto& d : docs) {
      if (auto* v = find(d, "id")) {
        if (auto* str = std::get_if<std::string>(v)) s.insert(*str);
      }
    }
    return s;
  }

  static std::vector<std::string> splitLines(const std::string& body) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < body.size()) {
      std::size_t nl = body.find('\n', start);
      if (nl == std::string::npos) {
        lines.push_back(body.substr(start));
        break;
      }
      if (nl > start) lines.push_back(body.substr(start, nl - start));
      start = nl + 1;
    }
    return lines;
  }

  static std::vector<std::string> idsInUpdateLine(const std::string& line) {
    std::vector<std::string> out;
    glz::generic_i64 root;
    if (glz::read_json(root, line)) return out;
    if (!root.is_object() || !root.contains("ids")) return out;
    auto* ids = root["ids"].get_if<glz::generic_i64::array_t>();
    if (ids == nullptr) return out;
    for (const auto& id : *ids) {
      if (auto* s = id.get_if<std::string>()) out.push_back(*s);
    }
    return out;
  }

  static std::optional<int64_t> updateVersionInLine(const std::string& line) {
    glz::generic_i64 root;
    if (glz::read_json(root, line) || !root.is_object() ||
        !root.contains("update_version")) {
      return std::nullopt;
    }
    auto* version = root["update_version"].get_if<int64_t>();
    if (version == nullptr) return std::nullopt;
    return *version;
  }

  static void writeRawHttpChunk(beast::tcp_stream& stream, std::string_view body) {
    std::ostringstream os;
    os << std::hex << body.size();
    std::string frame = os.str();
    frame += "\r\n";
    frame += body;
    frame += "\r\n";
    net::write(stream, net::buffer(frame));
  }

  static bool readNextBodyLine(beast::tcp_stream& stream, beast::flat_buffer& buffer,
                               http::response_parser<http::buffer_body>& parser,
                               std::string& pending, std::string& line, std::string& err) {
    for (;;) {
      std::size_t nl = pending.find('\n');
      if (nl != std::string::npos) {
        line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        return true;
      }
      if (parser.is_done()) {
        err = "response ended before the next NDJSON line";
        return false;
      }

      std::array<char, 4096> body{};
      parser.get().body().data = body.data();
      parser.get().body().size = body.size();
      beast::error_code ec;
      http::read_some(stream, buffer, parser, ec);
      std::size_t produced = body.size() - parser.get().body().size;
      pending.append(body.data(), produced);
      if (ec == http::error::need_buffer) continue;
      if (ec) {
        err = ec.message();
        return false;
      }
    }
  }

  static bool drainBody(beast::tcp_stream& stream, beast::flat_buffer& buffer,
                        http::response_parser<http::buffer_body>& parser,
                        std::string& pending, std::string& err) {
    while (!parser.is_done()) {
      std::array<char, 4096> body{};
      parser.get().body().data = body.data();
      parser.get().body().size = body.size();
      beast::error_code ec;
      http::read_some(stream, buffer, parser, ec);
      std::size_t produced = body.size() - parser.get().body().size;
      pending.append(body.data(), produced);
      if (ec == http::error::need_buffer) continue;
      if (ec) {
        err = ec.message();
        return false;
      }
    }
    return true;
  }
};

TEST_F(HttpApiTest, health) {
  auto res = httpRequest(port(), http::verb::get, "/health");
  EXPECT_EQ(200, res.result_int());
  EXPECT_NE(res.body().find(R"("status")"), std::string::npos);
}

// Four connections accepted in sequence must land on four different shards,
// and a persistent connection must never migrate between shard threads.
TEST_F(HttpApiTest, connectionsStayPinnedToIoShards) {
  helper.index(flatdoc("id", std::string("shard-affinity"),
                       "affinity_s", std::string("shard-affinity")),
               UpdateMessage::COMMIT);

  HttpServer shardServer(*LuxirTest::luxirNode, 4, 0);
  shardServer.start();

  struct Connection {
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    explicit Connection(net::io_context& ioc) : stream(ioc) {}
  };

  net::io_context clientIoc;
  tcp::resolver resolver(clientIoc);
  auto endpoint = resolver.resolve("127.0.0.1", std::to_string(shardServer.getPort()));
  std::vector<std::unique_ptr<Connection>> connections;
  for (int i = 0; i < 4; i++) {
    auto connection = std::make_unique<Connection>(clientIoc);
    connection->stream.connect(endpoint);
    connections.push_back(std::move(connection));
  }

  auto requestThreadId = [](Connection& connection) -> std::optional<int64_t> {
    http::request<http::string_body> request(
        http::verb::post, "/collections/main/_search", 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.keep_alive(true);
    request.body() =
        R"({"profile":true,"max_parallel":0,"ops":{"affinity":{"field_facet":{"field":"affinity_s","limit":-1}}}})";
    request.prepare_payload();
    http::write(connection.stream, request);

    http::response<http::string_body> response;
    http::read(connection.stream, connection.buffer, response);
    if (response.result() != http::status::ok) {
      ADD_FAILURE() << response;
      return std::nullopt;
    }

    glz::generic_i64 root;
    if (glz::read_json(root, response.body()) || !root.is_object() ||
        !root.contains("profile")) {
      ADD_FAILURE() << response.body();
      return std::nullopt;
    }
    auto* ops = root["profile"]["ops"].get_if<glz::generic_i64::array_t>();
    if (ops == nullptr || ops->empty()) {
      ADD_FAILURE() << response.body();
      return std::nullopt;
    }
    auto* pieces = (*ops)[0]["pieces"].get_if<glz::generic_i64::array_t>();
    if (pieces == nullptr || pieces->empty()) {
      ADD_FAILURE() << response.body();
      return std::nullopt;
    }
    auto* threadId = (*pieces)[0]["thread_id"].get_if<int64_t>();
    if (threadId == nullptr) ADD_FAILURE() << response.body();
    return threadId == nullptr ? std::nullopt : std::optional<int64_t>(*threadId);
  };

  std::set<int64_t> shardThreads;
  std::vector<int64_t> firstThreads;
  for (auto& connection : connections) {
    auto threadId = requestThreadId(*connection);
    ASSERT_TRUE(threadId.has_value());
    firstThreads.push_back(*threadId);
    shardThreads.insert(*threadId);
  }
  EXPECT_EQ(4u, shardThreads.size());

  for (std::size_t i = 0; i < connections.size(); i++) {
    auto threadId = requestThreadId(*connections[i]);
    ASSERT_TRUE(threadId.has_value());
    EXPECT_EQ(firstThreads[i], *threadId);
  }

  for (auto& connection : connections) {
    beast::error_code ec;
    connection->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  }
  connections.clear();
  shardServer.shutdown();
}

TEST_F(HttpApiTest, idleIoShardsExitAndRespawn) {
  HttpServer idleServer(*LuxirTest::luxirNode, 4, 0, -1,
                        std::chrono::milliseconds(30));
  idleServer.start();

  struct Connection {
    beast::tcp_stream stream;
    beast::flat_buffer buffer;
    explicit Connection(net::io_context& ioc) : stream(ioc) {}
  };

  net::io_context clientIoc;
  tcp::resolver resolver(clientIoc);
  auto endpoint = resolver.resolve("127.0.0.1", std::to_string(idleServer.getPort()));
  std::vector<std::unique_ptr<Connection>> connections;
  for (int i = 0; i < 4; i++) {
    auto connection = std::make_unique<Connection>(clientIoc);
    connection->stream.connect(endpoint);
    connections.push_back(std::move(connection));
  }
  ASSERT_TRUE(waitForShardThreads(idleServer, 4, std::chrono::seconds(3)));

  for (auto& connection : connections) {
    http::request<http::empty_body> request(http::verb::get, "/health", 11);
    request.set(http::field::host, "127.0.0.1");
    request.keep_alive(true);
    http::write(connection->stream, request);
    http::response<http::string_body> response;
    http::read(connection->stream, connection->buffer, response);
    ASSERT_EQ(200, response.result_int()) << response.body();
  }

  for (auto& connection : connections) {
    beast::error_code ec;
    connection->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    connection->stream.socket().close(ec);
  }
  connections.clear();
  ASSERT_TRUE(waitForShardThreads(idleServer, 1, std::chrono::seconds(3)))
      << "running shard threads=" << idleServer.getRunningShardThreads();

  // Occupy the warm floor so the next connection must revive a parked shard.
  auto floorConnection = std::make_unique<Connection>(clientIoc);
  floorConnection->stream.connect(endpoint);
  http::request<http::empty_body> floorRequest(http::verb::get, "/health", 11);
  floorRequest.set(http::field::host, "127.0.0.1");
  floorRequest.keep_alive(true);
  http::write(floorConnection->stream, floorRequest);
  http::response<http::string_body> floorResponse;
  http::read(floorConnection->stream, floorConnection->buffer, floorResponse);
  ASSERT_EQ(200, floorResponse.result_int()) << floorResponse.body();

  auto revived = httpRequest(idleServer.getPort(), http::verb::get, "/health");
  EXPECT_EQ(200, revived.result_int()) << revived.body();

  beast::error_code ec;
  floorConnection->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  floorConnection->stream.socket().close(ec);
  idleServer.shutdown();
}

TEST_F(HttpApiTest, idleIoShardChurn) {
  HttpServer idleServer(*LuxirTest::luxirNode, 4, 0, -1,
                        std::chrono::milliseconds(1));
  idleServer.start();

  // Keep shard 0 occupied so sequential connections repeatedly exercise the
  // exit/reap/respawn window on the non-floor shards.
  net::io_context holderIoc;
  tcp::resolver holderResolver(holderIoc);
  beast::tcp_stream holder(holderIoc);
  holder.connect(holderResolver.resolve(
      "127.0.0.1", std::to_string(idleServer.getPort())));
  http::request<http::empty_body> holderRequest(http::verb::get, "/health", 11);
  holderRequest.set(http::field::host, "127.0.0.1");
  holderRequest.keep_alive(true);
  http::write(holder, holderRequest);
  beast::flat_buffer holderBuffer;
  http::response<http::string_body> holderResponse;
  http::read(holder, holderBuffer, holderResponse);
  ASSERT_EQ(200, holderResponse.result_int()) << holderResponse.body();

  for (int i = 0; i < 300; i++) {
    auto response = httpRequest(idleServer.getPort(), http::verb::get, "/health");
    ASSERT_EQ(200, response.result_int()) << "request=" << i << ' ' << response.body();
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> clients;
  for (int i = 0; i < 24; i++) {
    clients.emplace_back([&idleServer, &failures] {
      try {
        auto response = httpRequest(idleServer.getPort(), http::verb::get, "/health");
        if (response.result_int() != 200) failures.fetch_add(1, std::memory_order_relaxed);
      } catch (...) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& client : clients) client.join();
  EXPECT_EQ(0, failures.load(std::memory_order_relaxed));

  beast::error_code ec;
  holder.socket().shutdown(tcp::socket::shutdown_both, ec);
  holder.socket().close(ec);
  idleServer.shutdown();
}

TEST_F(HttpApiTest, statsNodeWideAndPerCollection) {
  auto nodeStats = httpRequest(port(), http::verb::get, "/_stats");
  ASSERT_EQ(200, nodeStats.result_int()) << nodeStats.body();
  glz::generic_i64 nodeJson;
  ASSERT_FALSE(glz::read_json(nodeJson, nodeStats.body())) << nodeStats.body();
  EXPECT_TRUE(nodeJson.contains("totals"));
  EXPECT_TRUE(nodeJson.contains("collections"));
  EXPECT_TRUE(nodeJson.contains("indexing_ram"));

  auto collectionStats =
      httpRequest(port(), http::verb::get, "/collections/main/_stats");
  ASSERT_EQ(200, collectionStats.result_int()) << collectionStats.body();
  glz::generic_i64 collectionJson;
  ASSERT_FALSE(glz::read_json(collectionJson, collectionStats.body()))
      << collectionStats.body();
  auto* collections =
      collectionJson["collections"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, collections);
  ASSERT_EQ(1, collections->size());
  auto* name = (*collections)[0]["name"].get_if<std::string>();
  ASSERT_NE(nullptr, name);
  EXPECT_EQ("main", *name);

  auto* shards = (*collections)[0]["shards"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, shards);
  ASSERT_EQ(1, shards->size());
  EXPECT_FALSE((*shards)[0]["index"].contains("segments"));
}

TEST_F(HttpApiTest, statsSegmentsAfterCommit) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"stats-1","title_w":"segment stats"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  auto response =
      httpRequest(port(), http::verb::get, "/collections/main/_stats?segments=true");
  ASSERT_EQ(200, response.result_int()) << response.body();
  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, response.body())) << response.body();

  auto* segmentCount = root["totals"]["segments"].get_if<int64_t>();
  auto* committedCount = root["totals"]["committed_segments"].get_if<int64_t>();
  ASSERT_NE(nullptr, segmentCount);
  ASSERT_NE(nullptr, committedCount);
  EXPECT_GT(*segmentCount, 0);
  EXPECT_EQ(*segmentCount, *committedCount);
  auto* totalBytes = root["totals"]["bytes"].get_if<int64_t>();
  ASSERT_NE(nullptr, totalBytes);
  EXPECT_GT(*totalBytes, 0);

  auto* collections = root["collections"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, collections);
  auto* shards = (*collections)[0]["shards"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, shards);
  auto* segments =
      (*shards)[0]["index"]["segments"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, segments);
  EXPECT_EQ((size_t)*segmentCount, segments->size());
  auto* committed = (*segments)[0]["committed"].get_if<bool>();
  ASSERT_NE(nullptr, committed);
  EXPECT_TRUE(*committed);
  auto* segmentBytes = (*segments)[0]["bytes"].get_if<int64_t>();
  ASSERT_NE(nullptr, segmentBytes);
  EXPECT_GT(*segmentBytes, 0);

  // Filesystem-visible ids use their on-disk spelling: "seg" is the segment's
  // data-file prefix, and live_gen is absent when there are no deletes.
  auto* seg = (*segments)[0]["seg"].get_if<std::string>();
  ASSERT_NE(nullptr, seg);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(Postings::getIndexFileNamePrefix(reader->segments()[0].segInfo.seg_id), *seg);
  EXPECT_FALSE((*segments)[0].contains("live_gen"));
}

TEST_F(HttpApiTest, statsRoutingErrors) {
  auto missing =
      httpRequest(port(), http::verb::get, "/collections/http_missing_stats/_stats");
  EXPECT_EQ(404, missing.result_int()) << missing.body();

  auto wrongMethod = httpRequest(port(), http::verb::post, "/_stats");
  EXPECT_EQ(405, wrongMethod.result_int()) << wrongMethod.body();
  EXPECT_EQ("GET", wrongMethod[http::field::allow]);
}

TEST_F(HttpApiTest, unknownRouteIs404) {
  auto res = httpRequest(port(), http::verb::get, "/nope");
  EXPECT_EQ(404, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos);
}

TEST_F(HttpApiTest, malformedJsonIs400) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search", "{not json");
  EXPECT_EQ(400, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos);
}

// No "fields" in the request: every retrievable field of each hit comes back
// (engine fields such as _version_ only when named).
TEST_F(HttpApiTest, searchWithoutFieldsReturnsEveryRetrievableField) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"dp1","title_w":"default projection","rank_i":7}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "projection").execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  auto docs = hreq.getDocs();
  ASSERT_EQ(1u, docs.size()) << hreq.rawResponse();
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("dp1"),
                                    "title_w", std::string("default projection"),
                                    "rank_i", (int64_t)7));
  EXPECT_EQ(std::string::npos, hreq.rawResponse().find("_version_")) << hreq.rawResponse();
}

TEST_F(HttpApiTest, updateIndexesAndQueryRoundTrip) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"u1","title_w":"hello world","title_s":"Hello"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  EXPECT_NE(update.body().find(R"("update_version")"), std::string::npos) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "hello").fields({"id"}).execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"u1"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, facetResponseUsesBucketRows) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"f1","http_facet_s":"x"},{"id":"f2","http_facet_s":"x"},{"id":"f3","http_facet_s":"y"},{"id":"f4"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  auto response = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"ops":{"cats":{"field_facet":{"field":"http_facet_s","limit":-1,"missing":true}}}})");
  ASSERT_EQ(200, response.result_int()) << response.body();
  EXPECT_EQ(
      R"({"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}],"missing":1}}})" "\n",
      response.body());
}

TEST_F(HttpApiTest, shorthandCarriesRequestLevelKeys) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"sh1","http_sh_s":"x","title_w":"dune saga"},{"id":"sh2","http_sh_s":"y","title_w":"dune"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  // request-level keys (max_parallel, time_zone) mix with shorthand TopDocs keys
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":"title_w:dune","limit":0,"get_number":true,"max_parallel":-1,"time_zone":"UTC"})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("found":2)"), std::string::npos) << res.body();

  // "ops" beside shorthand keys = the op's sub-ops (facets over the query domain)
  auto facet = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":"title_w:dune","limit":0,"max_parallel":1,"ops":{"cats":{"field_facet":{"field":"http_sh_s","limit":-1}}}})");
  ASSERT_EQ(200, facet.result_int()) << facet.body();
  EXPECT_NE(facet.body().find(R"("buckets":[{"val":"x","count":1},{"val":"y","count":1}])"),
            std::string::npos) << facet.body();
}

TEST_F(HttpApiTest, maxParallelModesAllAnswer) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"mp1","http_mp_s":"x"},{"id":"mp2","http_mp_s":"x"},{"id":"mp3","http_mp_s":"y"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  // 0 = inline on the shard, 1 = serial on the arena, -1 = parallel on the
  // arena; same answer from every lane.
  for (std::string mp : {"-1", "0", "1"}) {
    auto response = httpRequest(port(), http::verb::post, "/collections/main/_search",
        R"({"max_parallel":)" + mp +
        R"(,"ops":{"cats":{"field_facet":{"field":"http_mp_s","limit":-1}}}})");
    ASSERT_EQ(200, response.result_int()) << response.body();
    EXPECT_EQ(
        R"({"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}]}}})" "\n",
        response.body()) << "max_parallel=" << mp;
  }
}

TEST_F(HttpApiTest, multiCollectionRoutingIsIsolated) {
  LuxirTest::clearCollection("http_route_a");
  LuxirTest::clearCollection("http_route_b");

  auto updateA = httpRequest(port(), http::verb::post, "/collections/http_route_a/_update",
      R"({"docs":[{"id":"route-a","title_w":"routeshared token"}],"commit":{}})");
  ASSERT_EQ(200, updateA.result_int()) << updateA.body();
  auto updateB = httpRequest(port(), http::verb::post, "/collections/http_route_b/_update",
      R"({"docs":[{"id":"route-b","title_w":"routeshared token"}],"commit":{}})");
  ASSERT_EQ(200, updateB.result_int()) << updateB.body();

  HttpReq reqA(port());
  reqA.collection("http_route_a").matchQuery("title_w", "routeshared").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, reqA.status()) << reqA.rawResponse();
  EXPECT_EQ(std::set<std::string>({"route-a"}), idsOf(reqA.getDocs())) << reqA.rawResponse();

  HttpReq reqB(port());
  reqB.collection("http_route_b").matchQuery("title_w", "routeshared").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, reqB.status()) << reqB.rawResponse();
  EXPECT_EQ(std::set<std::string>({"route-b"}), idsOf(reqB.getDocs())) << reqB.rawResponse();

  LuxirTest::clearCollection("http_route_a");
  LuxirTest::clearCollection("http_route_b");
}

TEST_F(HttpApiTest, autoCreateCollectionDefaultOn) {
  auto update = httpRequest(port(), http::verb::post, "/collections/http_auto_create_on/_update",
      R"({"docs":[{"id":"auto-on","title_w":"autocreateon token"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  std::shared_ptr<Collection> created;
  EXPECT_NO_THROW(created = LuxirTest::luxirNode->getCollection("http_auto_create_on"));
  ASSERT_NE(nullptr, created);

  HttpReq hreq(port());
  hreq.collection("http_auto_create_on").matchQuery("title_w", "autocreateon")
      .fields({"id"}).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"auto-on"}), idsOf(hreq.getDocs())) << hreq.rawResponse();

  LuxirTest::clearCollection("http_auto_create_on");
}

TEST_F(HttpApiTest, autoCreateCollectionCanBeDisabled) {
  LuxirConfig config;
  config.ingest.auto_create_collection = false;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto update = httpRequest(localServer.getPort(), http::verb::post,
      "/collections/http_auto_create_off/_update",
      R"({"docs":[{"id":"auto-off","title_w":"autocreateoff token"}],"commit":{}})");
  localServer.shutdown();

  EXPECT_EQ(404, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("collection 'http_auto_create_off' does not exist"),
            std::string::npos) << update.body();
  EXPECT_THROW(node.getCollection("http_auto_create_off"), CollectionResolutionError);
}

TEST_F(HttpApiTest, searchMissingCollectionErrorsWithoutCreating) {
  std::string autoOnName = "http_missing_search_auto_on";
  HttpReq autoOnReq(port());
  autoOnReq.collection(autoOnName).matchQuery("title_w", "missingtoken")
      .fields({"id"}).execute();
  EXPECT_EQ(200, autoOnReq.status()) << autoOnReq.rawResponse();
  EXPECT_NE(autoOnReq.rawResponse().find("collection '" + autoOnName + "' does not exist"),
            std::string::npos) << autoOnReq.rawResponse();
  EXPECT_THROW(LuxirTest::luxirNode->getCollection(autoOnName), CollectionResolutionError);

  LuxirConfig config;
  config.ingest.auto_create_collection = false;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  std::string autoOffName = "http_missing_search_auto_off";
  HttpReq autoOffReq(localServer.getPort());
  autoOffReq.collection(autoOffName).matchQuery("title_w", "missingtoken")
      .fields({"id"}).execute();
  localServer.shutdown();

  EXPECT_EQ(200, autoOffReq.status()) << autoOffReq.rawResponse();
  EXPECT_NE(autoOffReq.rawResponse().find("collection '" + autoOffName + "' does not exist"),
            std::string::npos) << autoOffReq.rawResponse();
  EXPECT_THROW(node.getCollection(autoOffName), CollectionResolutionError);
}

TEST_F(HttpApiTest, leadingUnderscoreCollectionNameIsRejected) {
  auto update = httpRequest(port(), http::verb::post, "/collections/_reserved/_update",
      R"({"docs":[{"id":"bad-reserved","title_w":"reserved token"}],"commit":{}})");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("collection '_reserved' is reserved"), std::string::npos)
      << update.body();
  EXPECT_THROW(LuxirTest::luxirNode->getCollection("_reserved"), CollectionResolutionError);
}

TEST_F(HttpApiTest, unsafeCollectionNamesAreRejectedBeforeCreate) {
  auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path base = std::filesystem::temp_directory_path() / ("luxir_unsafe_names_" + stamp);
  std::filesystem::path absolute = std::filesystem::temp_directory_path() / ("luxir_abs_collection_" + stamp);
  std::filesystem::remove_all(base);
  std::filesystem::remove_all(absolute);

  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = base.string();
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto expectRejected = [&](std::string target, std::string_view message) {
    auto update = httpRequest(localServer.getPort(), http::verb::post, target,
        R"({"docs":[{"id":"bad-name","title_w":"badname token"}],"commit":{}})");
    EXPECT_EQ(400, update.result_int()) << target << " " << update.body();
    EXPECT_NE(update.body().find(message), std::string::npos) << target << " " << update.body();
  };

  expectRejected("/collections/_reserved/_update", "reserved");
  expectRejected("/collections/MyCollection/_update", "must start with a lowercase letter");
  expectRejected("/collections/my-collection/_update", "may only contain lowercase letters");
  expectRejected("/collections/9lives/_update", "must start with a lowercase letter");
  expectRejected("/collections/unsafe/slash/_update", "may only contain lowercase letters");
  expectRejected("/collections/../_update", "must start with a lowercase letter");
  expectRejected("/collections/" + absolute.string() + "/_update", "must start with a lowercase letter");
  expectRejected("/collections//_update", "empty");

  localServer.shutdown();

  EXPECT_FALSE(std::filesystem::exists(base / "c" / "unsafe"));
  EXPECT_FALSE(std::filesystem::exists(absolute));
  std::filesystem::remove_all(base);
}

TEST_F(HttpApiTest, corruptCollectionTombstonedAtStartup) {
  auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path base = std::filesystem::temp_directory_path() / ("luxir_corrupt_col_" + stamp);
  std::filesystem::remove_all(base);

  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = base.string();

  {
    LuxirNode node(config);
    HttpServer localServer(node, 2, 0);
    localServer.start();
    auto good = httpRequest(localServer.getPort(), http::verb::post, "/collections/good/_update",
        R"({"docs":[{"id":"g1","title_w":"good token"}],"commit":{}})");
    ASSERT_EQ(200, good.result_int()) << good.body();
    auto bad = httpRequest(localServer.getPort(), http::verb::post, "/collections/bad/_update",
        R"({"docs":[{"id":"b1","title_w":"bad token"}],"commit":{}})");
    ASSERT_EQ(200, bad.result_int()) << bad.body();
    localServer.shutdown();
  }

  {
    std::ofstream out(base / "c" / "bad" / std::string(Postings::INDEX_INFO_FILE),
                      std::ios::binary | std::ios::trunc);
    out << "\xff\xff\xff\xff\xff\xff\xff\xff";
  }

  // Node startup must survive the corrupt collection and serve the good one.
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto query = httpRequest(localServer.getPort(), http::verb::post, "/collections/good/_search",
      R"({"query":{"match":{"title_w":"good"}},"fields":["id"]})");
  EXPECT_EQ(200, query.result_int()) << query.body();
  EXPECT_NE(query.body().find(R"("g1")"), std::string::npos) << query.body();

  auto badQuery = httpRequest(localServer.getPort(), http::verb::post, "/collections/bad/_search",
      R"({"query":{"match":{"title_w":"bad"}},"fields":["id"]})");
  EXPECT_NE(badQuery.body().find("failed to load"), std::string::npos) << badQuery.body();

  // Updates resolve to the tombstone too: no silent re-create over the corrupt data.
  auto badUpdate = httpRequest(localServer.getPort(), http::verb::post, "/collections/bad/_update",
      R"({"docs":[{"id":"b2","title_w":"more"}],"commit":{}})");
  EXPECT_NE(badUpdate.body().find("failed to load"), std::string::npos) << badUpdate.body();
  EXPECT_THROW(node.getCollection("bad"), CollectionResolutionError);

  localServer.shutdown();
  std::filesystem::remove_all(base);
}

TEST_F(HttpApiTest, simpleQueryOverJson) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"s1","title_w":"blade runner"},{"id":"s2","title_w":"running man"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  // the simple_query arm parses mechanically from the dialect (no sugar needed)
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"simple_query":{"q":"blade | man","fields":["title_w"]}},"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("s1")"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("s2")"), std::string::npos) << res.body();

  // declared degradations are visible on the wire (clamp-and-declare)
  auto warned = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"simple_query":{"q":"blade~9","fields":["title_w"]}},"fields":["id"]})");
  ASSERT_EQ(200, warned.result_int()) << warned.body();
  EXPECT_NE(warned.body().find(R"("warnings")"), std::string::npos) << warned.body();
  EXPECT_NE(warned.body().find(R"("fuzzy_clamped")"), std::string::npos) << warned.body();

  // never-fails: garbage user input is still a 200 with results, not an error
  auto garbage = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"simple_query":{"q":"re: \"unbalanced ((man","fields":["title_w"]}},"fields":["id"]})");
  ASSERT_EQ(200, garbage.result_int()) << garbage.body();
  EXPECT_NE(garbage.body().find(R"("s2")"), std::string::npos) << garbage.body();
}

TEST_F(HttpApiTest, geoDistanceQueryOverJson) {
  SchemaBuilder b;
  auto& field = b.field("geo");
  field.type = api::FieldDef::FieldClass::GEO_POINT;
  field.index = api::FieldDef::IndexMode::RANGE;
  b.set(helper.collection());

  auto update = httpRequest(port(), http::verb::post,
      "/collections/main/_update",
      R"({"docs":[{"id":"ny","geo":[-74.0060,40.7128]},{"id":"la","geo":[-118.2437,34.0522]}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  auto result = httpRequest(port(), http::verb::post,
      "/collections/main/_search",
      R"({"query":{"geo_distance":{"field":"geo","lat":40.7128,"lon":-74.0060,"radius_meters":1000}},"fields":["id"]})");
  ASSERT_EQ(200, result.result_int()) << result.body();
  EXPECT_NE(result.body().find(R"("ny")"), std::string::npos) << result.body();
  EXPECT_EQ(result.body().find(R"("la")"), std::string::npos) << result.body();
}

TEST_F(HttpApiTest, vectorDocumentsRoundTripOverJson) {
  auto schema = httpRequest(port(), http::verb::post, "/collections/main/_schema", R"({
    "fields": {
      "embedding": {"type":"vector", "dims":3, "metric":"l2"},
      "neighbors_vs": {"type":"vector", "dims":2, "metric":"l2", "multi":true}
    }
  })");
  ASSERT_EQ(200, schema.result_int()) << schema.body();

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"vec-http","embedding":[1.25,2.5,3.75],)"
      R"("neighbors_vs":[[0.25,0.75],[0.5,0.5]]}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  auto search = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"knn":{"field":"embedding","query":[1.25,2.5,3.75],)"
      R"("k":1,"exact":true}},"fields":["id","embedding","neighbors_vs"]})");
  ASSERT_EQ(200, search.result_int()) << search.body();
  EXPECT_NE(search.body().find(R"("embedding":[1.25,2.5,3.75])"),
            std::string::npos) << search.body();
  EXPECT_NE(search.body().find(R"("neighbors_vs":[[0.25,0.75],[0.5,0.5]])"),
            std::string::npos) << search.body();

  auto partial = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"vec-good","embedding":[1,2,3]},)"
      R"({"id":"vec-bad","embedding":[1,2]}]})");
  ASSERT_EQ(200, partial.result_int()) << partial.body();
  EXPECT_NE(partial.body().find(R"("status":"partial")"), std::string::npos)
      << partial.body();
  EXPECT_NE(partial.body().find(R"("errors":[)"), std::string::npos) << partial.body();
  EXPECT_NE(partial.body().find("embedding"), std::string::npos) << partial.body();
}

TEST_F(HttpApiTest, updateDeleteIds) {
  auto index = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"u1","title_w":"delete token"},{"id":"u2","title_w":"delete token"}],"commit":{}})");
  ASSERT_EQ(200, index.result_int()) << index.body();

  auto del = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"delete_ids":["u1"],"commit":{}})");
  ASSERT_EQ(200, del.result_int()) << del.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "token").fields({"id"}).execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"u2"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, malformedUpdateJsonIs400) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update", "{not json");
  EXPECT_EQ(400, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos) << res.body();
}

TEST_F(HttpApiTest, updateResponseUsesSnakeCase) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"request_id":"req-1","docs":[{"id":"shape1","title_w":"shape"}],"commit":{}})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("update_version")"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("request_id":"req-1")"), std::string::npos) << res.body();
}

TEST_F(HttpApiTest, bufferedMaxSegmentsCommitDurablyPublishesMergedLayout) {
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  for (int i = 0; i < 3; i++) {
    helper.index(flatdoc("id", "http-buffered-merge-" + std::to_string(i)), UpdateMessage::COMMIT);
  }
  ASSERT_EQ(3u, helper.durableSegmentCount());

  auto response = httpRequest(port(), http::verb::post, "/collections/main/_update",
                              R"({"commit":{"max_segments":1}})");
  ASSERT_EQ(200, response.result_int()) << response.body();
  EXPECT_EQ(1u, helper.durableSegmentCount());
}

TEST_F(HttpApiTest, ndjsonEndMaxSegmentsCommitDurablyPublishesMergedLayout) {
  helper.getIndexWriter()->mergePolicy->setMergeFactor(100);
  for (int i = 0; i < 3; i++) {
    helper.index(flatdoc("id", "http-ndjson-merge-" + std::to_string(i)), UpdateMessage::COMMIT);
  }
  ASSERT_EQ(3u, helper.durableSegmentCount());

  auto response = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"_end_":{"commit":{"max_segments":1}}})" "\n", "application/x-ndjson");
  ASSERT_EQ(200, response.result_int()) << response.body();
  EXPECT_EQ(1u, helper.durableSegmentCount());
}

TEST_F(HttpApiTest, ndjsonStreamIndexesAndQueries) {
  std::string body =
      R"({"id":"n1","title_w":"streamtoken alpha","title_s":"Alpha"})" "\n"
      R"({"id":"n2","title_w":"streamtoken beta","title_s":"Beta"})" "\n"
      R"({"id":"n3","title_w":"streamtoken gamma","title_s":"Gamma"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  EXPECT_NE(update.body().find(R"("update_version")"), std::string::npos) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "streamtoken").fields({"id"})
      .limit(10).withStats().execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)3, hreq.found());
  EXPECT_EQ(std::set<std::string>({"n1", "n2", "n3"}), idsOf(hreq.getDocs()))
      << hreq.rawResponse();
}

TEST_F(HttpApiTest, bufferedUpdateAppliesFieldMap) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"fm1","headline":"fieldmaptoken alpha"}],)"
      R"("field_map":{"headline":"title_w"},"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "fieldmaptoken").fields({"id"}).limit(10).execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"fm1"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonUpdateControlAppliesFieldMap) {
  // The group's field_map picks two keys out of a foreign doc shape and drops the rest.
  std::string body =
      R"({"_update_":{"field_map":{"doc_id":"id","headline":"title_w"},"drop_unmapped":true}})" "\n"
      R"({"doc_id":"fs1","headline":"streamfmtoken alpha","noise":"boom"})" "\n"
      R"({"doc_id":"fs2","headline":"streamfmtoken beta","noise":"boom"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "streamfmtoken").fields({"id"}).limit(10).execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"fs1", "fs2"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

// The marquee shape: an existing NDJSON dump indexes as-is, mapping on the URL.
TEST_F(HttpApiTest, ndjsonUrlFieldMapIndexesForeignFile) {
  std::string body =
      R"({"bookId":"uf1","headline":"urlfmtoken alpha","junk":"boom"})" "\n"
      R"({"bookId":"uf2","headline":"urlfmtoken beta","junk":"boom"})" "\n";

  auto update = httpRequest(port(), http::verb::post,
      "/collections/main/_update?field_map=bookId:id,headline:title_w&drop_unmapped=true&commit=true",
      std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "urlfmtoken").fields({"id"}).limit(10).execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"uf1", "uf2"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

// A group that sets its own field_map owns the pair for its docs; a later implicit
// group falls back to the URL default.
TEST_F(HttpApiTest, urlFieldMapYieldsToGroupMap) {
  std::string body =
      R"({"_update_":{"field_map":{"alt_id":"id","alt_head":"title_w"},"drop_unmapped":true}})" "\n"
      R"({"alt_id":"gm1","alt_head":"groupfmtoken alpha","bookId":"ignored"})" "\n"
      R"({"_end_":{}})" "\n"
      R"({"bookId":"gm2","headline":"groupfmtoken beta","junk":"boom"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post,
      "/collections/main/_update?field_map=bookId:id,headline:title_w&drop_unmapped=true",
      std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "groupfmtoken").fields({"id"}).limit(10).execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"gm1", "gm2"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, bufferedUpdateUrlFieldMap) {
  auto update = httpRequest(port(), http::verb::post,
      "/collections/main/_update?field_map=headline:title_w",
      R"({"docs":[{"id":"bu1","headline":"buffurltoken alpha"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "buffurltoken").fields({"id"}).limit(10).execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"bu1"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, urlFieldMapRejectsMalformedEntry) {
  auto update = httpRequest(port(), http::verb::post,
      "/collections/main/_update?field_map=noseparator",
      R"({"id":"x1","title_w":"whatever"})" "\n", "application/x-ndjson");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("field_map entry 'noseparator'"), std::string::npos)
      << update.body();
}

TEST_F(HttpApiTest, ndjsonEndRejectsFieldMap) {
  std::string body =
      R"({"_end_":{"field_map":{"a":"b"}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("_end_ control cannot carry submit-time field 'field_map'"),
            std::string::npos) << update.body();
}

// "found" is opt-in: it appears only when get_number is requested (an exact count
// forgoes dynamic pruning).  A default query carries no count, and absence must be
// omitted rather than rendered as found:0 (indistinguishable from zero matches).
TEST_F(HttpApiTest, foundIsOptInWithGetNumber) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("f1"), "title_w", std::string("foundtoken alpha")),
    flatdoc("id", std::string("f2"), "title_w", std::string("foundtoken beta")),
  }, UpdateMessage::COMMIT);

  // Default request (no get_number): "found" omitted, docs still returned.
  HttpReq noCount(port());
  noCount.collection("main").matchQuery("title_w", "foundtoken").fields({"id"}).limit(10).execute();
  ASSERT_EQ(200, noCount.status()) << noCount.rawResponse();
  EXPECT_EQ(noCount.rawResponse().find(R"("found")"), std::string::npos) << noCount.rawResponse();
  EXPECT_EQ((std::size_t)2, noCount.getDocs().size()) << noCount.rawResponse();

  // With get_number: "found" present and accurate.
  HttpReq withCount(port());
  withCount.collection("main").matchQuery("title_w", "foundtoken").fields({"id"}).limit(10).withStats().execute();
  ASSERT_EQ(200, withCount.status()) << withCount.rawResponse();
  EXPECT_NE(withCount.rawResponse().find(R"("found")"), std::string::npos) << withCount.rawResponse();
  EXPECT_EQ((int64_t)2, withCount.found()) << withCount.rawResponse();
}

TEST_F(HttpApiTest, ndjsonStreamFlushesMultipleBatches) {
  static constexpr int kDocCount = 180;
  std::string payload(20 * 1024, 'x');
  std::string body;
  body.reserve((payload.size() + 80) * (std::size_t)kDocCount);
  for (int i = 0; i < kDocCount; i++) {
    body += R"({"id":"nb)";
    body += std::to_string(i);
    body += R"(","title_w":"ndbatch","blob_sc":")";
    body += payload;
    body += R"("})";
    body += '\n';
  }
  body += R"({"_end_":{"commit":{}}})";
  body += '\n';

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "ndbatch").fields({"id"})
      .limit(kDocCount).withStats().execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)kDocCount, hreq.found());
  EXPECT_EQ((std::size_t)kDocCount, hreq.ids().size()) << hreq.rawResponse();
}

// A single group spanning multiple internal batches retains exactly kMaxRetainedIds
// (100) ids: once the interval fills, later slices set return_ids=false, but the cap
// and every doc's indexing must be unaffected.
TEST_F(HttpApiTest, streamGroupCapsRetainedIdsAcrossBatches) {
  static constexpr int kDocCount = 150;
  std::string payload(20 * 1024, 'y');  // ~20 KiB each -> several 1 MiB batches
  std::string body;
  body.reserve((payload.size() + 80) * (std::size_t)kDocCount);
  body += R"({"_update_":{"return_ids":true}})";
  body += '\n';
  for (int i = 0; i < kDocCount; i++) {
    body += R"({"id":"cap)";
    body += std::to_string(i);
    body += R"(","title_w":"captoken","blob_sc":")";
    body += payload;
    body += R"("})";
    body += '\n';
  }
  body += R"({"_end_":{"commit":{}}})";
  body += '\n';

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  // One explicit group -> one response line; ids capped at 100 even though 150 indexed.
  std::size_t retained = 0;
  for (const auto& line : splitLines(update.body())) retained += idsInUpdateLine(line).size();
  EXPECT_EQ((std::size_t)100, retained) << update.body().substr(0, 200);

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "captoken").fields({"id"})
      .limit(kDocCount).withStats().execute();
  ASSERT_EQ(200, hreq.status());
  EXPECT_EQ((int64_t)kDocCount, hreq.found()) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonPipelinedMatchesSerialIncludingOverwrites) {
  struct RunResult {
    int64_t plainCount = 0;
    std::map<std::string, std::string> overwriteDocs;
    std::string plainResponse;
    std::string overwriteResponse;
  };

  auto run = [&](int64_t maxInFlight, RunResult& result) {
    LuxirConfig config;
    config.ingest.stream_batch_docs = 3;
    config.ingest.max_inflight_batches = maxInFlight;
    LuxirNode node(config);
    HttpServer localServer(node, 2, 0);
    localServer.start();

    std::string plainBody;
    for (int i = 0; i < 128; i++) {
      plainBody += R"({"id":"plain-)" + std::to_string(i) +
          R"(","title_w":"plainpipeline"})" "\n";
    }
    plainBody += R"({"_end_":{"commit":{}}})" "\n";
    auto plainUpdate = httpRequest(localServer.getPort(), http::verb::post,
        "/collections/plain/_update", std::move(plainBody), "application/x-ndjson");
    ASSERT_EQ(200, plainUpdate.result_int()) << plainUpdate.body();
    result.plainResponse = plainUpdate.body();

    HttpReq plainQuery(localServer.getPort());
    plainQuery.collection("plain").matchQuery("title_w", "plainpipeline")
        .fields({"id"}).limit(128).withStats().execute();
    ASSERT_EQ(200, plainQuery.status()) << plainQuery.rawResponse();
    result.plainCount = plainQuery.found();

    static constexpr int kIds = 12;
    static constexpr int kRounds = 20;
    std::string overwriteBody = R"({"_update_":{"allow_dups":false}})" "\n";
    for (int round = 0; round < kRounds; round++) {
      for (int id = 0; id < kIds; id++) {
        overwriteBody += R"({"id":"overwrite-)" + std::to_string(id) +
            R"(","title_w":"overwriteparity","title_s":"round-)" +
            std::to_string(round) + R"("})" "\n";
      }
    }
    overwriteBody += R"({"_end_":{"commit":{}}})" "\n";
    auto overwriteUpdate = httpRequest(localServer.getPort(), http::verb::post,
        "/collections/overwrite/_update", std::move(overwriteBody), "application/x-ndjson");
    ASSERT_EQ(200, overwriteUpdate.result_int()) << overwriteUpdate.body();
    result.overwriteResponse = overwriteUpdate.body();

    HttpReq overwriteQuery(localServer.getPort());
    overwriteQuery.collection("overwrite").matchQuery("title_w", "overwriteparity")
        .fields({"id", "title_s"}).limit(kIds).withStats().execute();
    ASSERT_EQ(200, overwriteQuery.status()) << overwriteQuery.rawResponse();
    ASSERT_EQ((int64_t)kIds, overwriteQuery.found()) << overwriteQuery.rawResponse();
    for (const auto& doc : overwriteQuery.getDocs()) {
      const auto* id = find(doc, "id");
      const auto* value = find(doc, "title_s");
      ASSERT_NE(nullptr, id);
      ASSERT_NE(nullptr, value);
      result.overwriteDocs.emplace(std::get<std::string>(*id), std::get<std::string>(*value));
    }

    localServer.shutdown();
  };

  RunResult serial;
  RunResult pipelined;
  run(1, serial);
  run(8, pipelined);

  EXPECT_EQ((int64_t)128, serial.plainCount);
  EXPECT_EQ(serial.plainCount, pipelined.plainCount);
  EXPECT_EQ(serial.plainResponse, pipelined.plainResponse);
  EXPECT_EQ(serial.overwriteResponse, pipelined.overwriteResponse);
  EXPECT_EQ(serial.overwriteDocs, pipelined.overwriteDocs);
  ASSERT_EQ((std::size_t)12, pipelined.overwriteDocs.size());
  for (int id = 0; id < 12; id++) {
    EXPECT_EQ("round-19", pipelined.overwriteDocs.at("overwrite-" + std::to_string(id)));
  }
}

TEST_F(HttpApiTest, ndjsonPipelinedInputFailureDrainsSubmittedPrefix) {
  LuxirConfig config;
  config.ingest.stream_batch_docs = 1;
  config.ingest.max_inflight_batches = 8;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  static constexpr int kDocCount = 8;
  std::string body;
  for (int i = 0; i < kDocCount; i++) {
    body += R"({"id":"deferred-fail-)" + std::to_string(i) +
        R"(","title_w":"deferredfailure"})" "\n";
  }
  body += "{not json\n";

  auto update = httpRequest(localServer.getPort(), http::verb::post,
      "/collections/deferred_failure/_update", std::move(body), "application/x-ndjson");
  ASSERT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(std::string::npos,
            update.body().find("docs_indexed_so_far=" + std::to_string(kDocCount)))
      << update.body();

  // The response is itself the drain fence.  Commit the already-finished
  // updates, then verify every valid record preceding the malformed one.
  auto commit = httpRequest(localServer.getPort(), http::verb::post,
      "/collections/deferred_failure/_update", R"({"commit":{}})");
  ASSERT_EQ(200, commit.result_int()) << commit.body();

  HttpReq query(localServer.getPort());
  query.collection("deferred_failure").matchQuery("title_w", "deferredfailure")
      .fields({"id"}).limit(kDocCount).withStats().execute();
  ASSERT_EQ(200, query.status()) << query.rawResponse();
  EXPECT_EQ((int64_t)kDocCount, query.found()) << query.rawResponse();

  localServer.shutdown();
}

TEST_F(HttpApiTest, ndjsonHeaderNoopPreservesSingleBatchAndPipelineParity) {
  struct RunResult {
    std::string response;
    int64_t found = 0;
    std::optional<int64_t> updateVersion;
  };

  auto run = [&](int64_t maxInFlight, RunResult& result) {
    LuxirConfig config;
    config.ingest.stream_batch_docs = 16;
    config.ingest.max_inflight_batches = maxInFlight;
    LuxirNode node(config);
    HttpServer localServer(node, 2, 0);
    localServer.start();

    std::string body =
        R"({"id":"header-a","title_w":"headerparity"})" "\n"
        R"({"_header_":{"found":2}})" "\n"
        R"({"id":"header-b","title_w":"headerparity"})" "\n"
        R"({"_end_":{"commit":{}}})" "\n";
    auto update = httpRequest(localServer.getPort(), http::verb::post,
        "/collections/header_parity/_update", std::move(body), "application/x-ndjson");
    ASSERT_EQ(200, update.result_int()) << update.body();
    auto lines = splitLines(update.body());
    ASSERT_EQ((std::size_t)1, lines.size()) << update.body();
    result.response = update.body();
    result.updateVersion = updateVersionInLine(lines[0]);

    HttpReq query(localServer.getPort());
    query.collection("header_parity").matchQuery("title_w", "headerparity")
        .fields({"id"}).limit(2).withStats().execute();
    ASSERT_EQ(200, query.status()) << query.rawResponse();
    result.found = query.found();

    localServer.shutdown();
  };

  RunResult serial;
  RunResult pipelined;
  run(1, serial);
  run(8, pipelined);

  // A fresh writer assigns version 1 to the sole A+B data message.  Treating
  // _header_ as a barrier would split it and make the response version 2.
  ASSERT_TRUE(serial.updateVersion.has_value()) << serial.response;
  EXPECT_EQ((int64_t)1, *serial.updateVersion) << serial.response;
  EXPECT_EQ((int64_t)2, serial.found);
  EXPECT_EQ(serial.response, pipelined.response);
  EXPECT_EQ(serial.updateVersion, pipelined.updateVersion);
  EXPECT_EQ(serial.found, pipelined.found);
}

TEST_F(HttpApiTest, ndjsonPipelinedBarriersPreserveIntervalsAndCommit) {
  LuxirConfig config;
  config.ingest.stream_batch_docs = 2;
  config.ingest.max_inflight_batches = 8;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  std::vector<std::string> expectedFirst;
  std::vector<std::string> expectedSecond;
  std::string body = R"({"_update_":{"request_id":"first","return_ids":true}})" "\n";
  for (int i = 0; i < 12; i++) {
    std::string id = "barrier-a-" + std::to_string(i);
    expectedFirst.push_back(id);
    body += R"({"id":")" + id + R"(","title_w":"pipelinebarrier"})" "\n";
  }
  body += "{}\n";
  body += R"({"_update_":{"request_id":"second","return_ids":true}})" "\n";
  for (int i = 0; i < 9; i++) {
    std::string id = "barrier-b-" + std::to_string(i);
    expectedSecond.push_back(id);
    body += R"({"id":")" + id + R"(","title_w":"pipelinebarrier"})" "\n";
  }
  body += R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(localServer.getPort(), http::verb::post,
      "/collections/barriers/_update", std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto lines = splitLines(update.body());
  ASSERT_EQ((std::size_t)2, lines.size()) << update.body();
  EXPECT_NE(lines[0].find(R"("request_id":"first")"), std::string::npos);
  EXPECT_NE(lines[1].find(R"("request_id":"second")"), std::string::npos);
  EXPECT_EQ(expectedFirst, idsInUpdateLine(lines[0])) << update.body();
  EXPECT_EQ(expectedSecond, idsInUpdateLine(lines[1])) << update.body();

  HttpReq query(localServer.getPort());
  query.collection("barriers").matchQuery("title_w", "pipelinebarrier")
      .fields({"id"}).limit(21).withStats().execute();
  ASSERT_EQ(200, query.status()) << query.rawResponse();
  EXPECT_EQ((int64_t)21, query.found()) << query.rawResponse();
  auto collection = node.getCollection("barriers");
  EXPECT_FALSE(readDurableIndexInfo(collection->getShard()->getIndexWriter()->dir)->segments.empty());

  localServer.shutdown();
}

TEST_F(HttpApiTest, ndjsonDisconnectDrainsPipelinedBatches) {
  LuxirConfig config;
  config.ingest.stream_batch_docs = 1;
  config.ingest.max_inflight_batches = 8;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  std::string payload(8 * 1024, 'z');
  std::string body;
  for (int i = 0; i < 128; i++) {
    body += R"({"id":"disconnect-)" + std::to_string(i) +
        R"(","title_w":"disconnectpipeline","blob_sc":")" + payload + R"("})" "\n";
  }

  net::io_context cioc;
  beast::tcp_stream stream(cioc);
  tcp::resolver resolver(cioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(localServer.getPort())));
  http::request<http::string_body> request{http::verb::post, "/collections/disconnect/_update", 11};
  request.set(http::field::host, "127.0.0.1");
  request.set(http::field::content_type, "application/x-ndjson");
  request.keep_alive(false);
  request.body() = std::move(body);
  request.prepare_payload();
  http::write(stream, request);

  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  stream.socket().close(ec);
  EXPECT_NO_THROW(localServer.shutdown());
}

TEST_F(HttpApiTest, ndjsonMalformedRecordIs400) {
  std::string body =
      R"({"id":"bad1","title_w":"badtoken"})" "\n"
      "{not json\n";

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
                         std::move(body), "application/x-ndjson");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find("docs_indexed_so_far"), std::string::npos) << res.body();
}

// A single document larger than the debug streaming read buffer forces the framer
// to carry a partial record across multiple real socket reads.
TEST_F(HttpApiTest, ndjsonDocLargerThanReadBuffer) {
  std::string big(200 * 1024, 'x');  // ~200 KiB > 64 KiB read buffer, < record cap
  std::string body =
      R"({"id":"big1","title_w":"bigtoken","blob_sc":")" + big + R"("})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body().substr(0, 200);

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "bigtoken").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status());
  EXPECT_EQ((int64_t)1, hreq.found()) << hreq.rawResponse().substr(0, 200);
}

// A buffered (non-streaming) request body past ingest.max_request_body_mb is rejected
// with a clean 413.  We declare an oversized Content-Length; Beast rejects at header
// parse, so no body is actually sent (this is what bounds a single atomic /update).
TEST_F(HttpApiTest, oversizedBufferedBodyIs413) {
  net::io_context cioc;
  beast::tcp_stream stream(cioc);
  tcp::resolver resolver(cioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port())));
  stream.expires_after(std::chrono::seconds(10));

  std::string header =
      "POST /collections/main/_update HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 99000000\r\n"   // ~94 MiB, well past the 32 MiB default
      "Connection: close\r\n"
      "\r\n";
  net::write(stream, net::buffer(header));

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  beast::error_code ec;
  http::read(stream, buffer, res, ec);
  ASSERT_EQ(413, res.result_int()) << ec.message() << " body=" << res.body();
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos) << res.body();
}

TEST_F(HttpApiTest, ndjsonRequestIdControlEchoed) {
  std::string body =
      R"({"_update_":{"request_id":"stream-req-1"}})" "\n"
      R"({"id":"rid1","title_w":"ridtoken"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
                         std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("request_id":"stream-req-1")"), std::string::npos)
      << res.body();
}

TEST_F(HttpApiTest, ndjsonMultipleGroupsReturnMultipleLines) {
  std::string body =
      R"({"_update_":{"request_id":"g1"}})" "\n"
      R"({"id":"mg1","title_w":"mgroup token","title_s":"Group 1"})" "\n"
      R"({"_update_":{"request_id":"g2"}})" "\n"
      R"({"id":"mg2","title_w":"mgroup token","title_s":"Group 2"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto lines = splitLines(update.body());
  ASSERT_EQ(2u, lines.size()) << update.body();
  EXPECT_NE(lines[0].find(R"("request_id":"g1")"), std::string::npos) << update.body();
  EXPECT_NE(lines[1].find(R"("request_id":"g2")"), std::string::npos) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "mgroup").fields({"id"})
      .limit(10).withStats().execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"mg1", "mg2"}), idsOf(hreq.getDocs()))
      << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonNoGroupReturnsOneLine) {
  std::string body =
      R"({"id":"nog1","title_w":"nogroup token"})" "\n"
      R"({"id":"nog2","title_w":"nogroup token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto lines = splitLines(update.body());
  ASSERT_EQ(1u, lines.size()) << update.body();
  EXPECT_NE(lines[0].find(R"("update_version")"), std::string::npos) << update.body();
}

TEST_F(HttpApiTest, ndjsonCheckpointMarkerEmitsLineMidStream) {
  std::string body =
      R"({"_update_":{"request_id":"g1","return_ids":true}})" "\n"
      R"({"id":"cp1","title_w":"checkpoint token"})" "\n"
      "{}\n"
      R"({"_update_":{"return_ids":true}})" "\n"
      R"({"id":"cp2","title_w":"checkpoint token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto lines = splitLines(update.body());
  ASSERT_EQ(2u, lines.size()) << update.body();
  EXPECT_NE(lines[0].find(R"("request_id":"g1")"), std::string::npos) << update.body();
  EXPECT_EQ(lines[1].find(R"("request_id":"g1")"), std::string::npos) << update.body();
  EXPECT_EQ(std::vector<std::string>({"cp1"}), idsInUpdateLine(lines[0])) << update.body();
  EXPECT_EQ(std::vector<std::string>({"cp2"}), idsInUpdateLine(lines[1])) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "checkpoint").fields({"id"})
      .limit(10).withStats().execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"cp1", "cp2"}), idsOf(hreq.getDocs()))
      << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonCheckpointStatsAreDeltaNotCumulative) {
  std::string body =
      R"({"_update_":{"request_id":"delta","return_ids":true}})" "\n"
      R"({"id":"dlt1","title_w":"deltatoken"})" "\n"
      R"({"id":"dlt2","title_w":"deltatoken"})" "\n"
      "{}\n"
      R"({"_update_":{"return_ids":true}})" "\n"
      R"({"id":"dlt3","title_w":"deltatoken"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto lines = splitLines(update.body());
  ASSERT_EQ(2u, lines.size()) << update.body();
  auto firstIds = idsInUpdateLine(lines[0]);
  auto secondIds = idsInUpdateLine(lines[1]);
  EXPECT_EQ((std::vector<std::string>{"dlt1", "dlt2"}), firstIds) << update.body();
  EXPECT_EQ((std::vector<std::string>{"dlt3"}), secondIds) << update.body();
}

TEST_F(HttpApiTest, ndjsonCheckpointAckArrivesBeforeRequestBodyEnds) {
  net::io_context cioc;
  beast::tcp_stream stream(cioc);
  tcp::resolver resolver(cioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port())));
  stream.expires_after(std::chrono::seconds(10));

  std::string header =
      "POST /collections/main/_update HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: application/x-ndjson\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  net::write(stream, net::buffer(header));

  writeRawHttpChunk(stream,
      R"({"_update_":{"request_id":"g1","return_ids":true}})" "\n"
      R"({"id":"bd1","title_w":"bidirectional token"})" "\n"
      "{}\n");

  beast::flat_buffer buffer;
  http::response_parser<http::buffer_body> parser;
  beast::error_code ec;
  http::read_header(stream, buffer, parser, ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_EQ(200, parser.get().result_int());

  std::string pending;
  std::string err;
  std::string firstLine;
  ASSERT_TRUE(readNextBodyLine(stream, buffer, parser, pending, firstLine, err)) << err;
  EXPECT_NE(firstLine.find(R"("request_id":"g1")"), std::string::npos) << firstLine;
  EXPECT_EQ(std::vector<std::string>({"bd1"}), idsInUpdateLine(firstLine)) << firstLine;

  writeRawHttpChunk(stream,
      R"({"_update_":{"return_ids":true}})" "\n"
      R"({"id":"bd2","title_w":"bidirectional token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n");

  std::string secondLine;
  ASSERT_TRUE(readNextBodyLine(stream, buffer, parser, pending, secondLine, err)) << err;
  EXPECT_EQ(secondLine.find(R"("request_id":"g1")"), std::string::npos) << secondLine;
  EXPECT_EQ(std::vector<std::string>({"bd2"}), idsInUpdateLine(secondLine)) << secondLine;

  net::write(stream, net::buffer(std::string("0\r\n\r\n")));
  ASSERT_TRUE(drainBody(stream, buffer, parser, pending, err)) << err;
  EXPECT_TRUE(pending.empty()) << pending;

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "bidirectional").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"bd1", "bd2"}), idsOf(hreq.getDocs()))
      << hreq.rawResponse();

  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

TEST_F(HttpApiTest, ndjsonMidStreamErrorAfterGroupEmittedIsFinalLine) {
  std::string body =
      R"({"_update_":{"request_id":"g1"}})" "\n"
      R"({"id":"eg1","title_w":"egroup token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n"
      R"({"_update_":{"request_id":"g2"}})" "\n"
      "{not json\n";

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
                         std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(2u, lines.size()) << res.body();
  EXPECT_NE(lines[0].find(R"("request_id":"g1")"), std::string::npos) << res.body();
  EXPECT_NE(lines.back().find(R"("status":"error")"), std::string::npos) << res.body();
  EXPECT_NE(lines.back().find("docs_indexed_so_far"), std::string::npos) << res.body();
}

TEST_F(HttpApiTest, ndjsonStreamKeepsConnectionAliveAfterFinalLine) {
  net::io_context cioc;
  beast::tcp_stream stream(cioc);
  tcp::resolver resolver(cioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port())));

  http::request<http::string_body> updateReq(http::verb::post, "/collections/main/_update", 11);
  updateReq.set(http::field::host, "127.0.0.1");
  updateReq.set(http::field::content_type, "application/x-ndjson");
  updateReq.body() =
      R"({"id":"ka1","title_w":"keepalive token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";
  updateReq.prepare_payload();
  http::write(stream, updateReq);

  beast::flat_buffer buffer;
  http::response<http::string_body> updateRes;
  http::read(stream, buffer, updateRes);
  ASSERT_EQ(200, updateRes.result_int()) << updateRes.body();
  ASSERT_EQ(1u, splitLines(updateRes.body()).size()) << updateRes.body();

  http::request<http::string_body> healthReq(http::verb::get, "/health", 11);
  healthReq.set(http::field::host, "127.0.0.1");
  http::write(stream, healthReq);

  http::response<http::string_body> healthRes;
  http::read(stream, buffer, healthRes);
  EXPECT_EQ(200, healthRes.result_int()) << healthRes.body();
  EXPECT_NE(healthRes.body().find(R"("status")"), std::string::npos) << healthRes.body();

  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

TEST_F(HttpApiTest, ndjsonUpdateControlDecodesFullRequestAndInlineDeletes) {
  auto seed = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"del-mid","title_w":"deletestream"}],"commit":{}})");
  ASSERT_EQ(200, seed.result_int()) << seed.body();

  std::string body =
      R"({"_update_":{"allow_dups":true,"return_ids":true}})" "\n"
      R"({"id":"dup-full","title_w":"fulldecode one"})" "\n"
      R"({"id":"dup-full","title_w":"fulldecode two"})" "\n"
      R"({"_update_":{"delete_ids":["del-mid"],"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto lines = splitLines(update.body());
  ASSERT_EQ(2u, lines.size()) << update.body();
  EXPECT_EQ((std::vector<std::string>{"dup-full", "dup-full"}), idsInUpdateLine(lines[0]))
      << update.body();

  HttpReq dupReq(port());
  dupReq.collection("main").matchQuery("title_w", "fulldecode").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, dupReq.status()) << dupReq.rawResponse();
  EXPECT_EQ((int64_t)2, dupReq.found()) << dupReq.rawResponse();

  HttpReq delReq(port());
  delReq.collection("main").matchQuery("title_w", "deletestream").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, delReq.status()) << delReq.rawResponse();
  EXPECT_EQ((int64_t)0, delReq.found()) << delReq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonInlineUpdateReturnIdsDefaultFalse) {
  std::string withIds =
      R"({"_update_":{"docs":[{"id":"inline-ret","title_w":"inlinereturn token"}],)"
      R"("return_ids":true,"commit":{}}})" "\n";
  auto returned = httpRequest(port(), http::verb::post, "/collections/main/_update",
                              std::move(withIds), "application/x-ndjson");
  ASSERT_EQ(200, returned.result_int()) << returned.body();
  auto returnedLines = splitLines(returned.body());
  ASSERT_EQ(1u, returnedLines.size()) << returned.body();
  EXPECT_EQ((std::vector<std::string>{"inline-ret"}), idsInUpdateLine(returnedLines[0]))
      << returned.body();

  std::string withoutIds =
      R"({"_update_":{"docs":[{"id":"inline-no-ret","title_w":"inlinereturn token"}],)"
      R"("commit":{}}})" "\n";
  auto omitted = httpRequest(port(), http::verb::post, "/collections/main/_update",
                             std::move(withoutIds), "application/x-ndjson");
  ASSERT_EQ(200, omitted.result_int()) << omitted.body();
  auto omittedLines = splitLines(omitted.body());
  ASSERT_EQ(1u, omittedLines.size()) << omitted.body();
  EXPECT_TRUE(idsInUpdateLine(omittedLines[0]).empty()) << omitted.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "inlinereturn").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)2, hreq.found()) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonDeferredInlineUpdateEnforcesRequestBodyCap) {
  LuxirConfig config;
  config.ingest.max_request_body = 1024;
  config.ingest.max_record = 4096;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  std::string payload(1500, 'z');
  std::string body =
      R"({"id":"defer-pre","title_w":"defercap token"})" "\n"
      R"({"_update_":{"docs":[{"id":"defer-big","title_w":"defercap token","blob_sc":")" +
      payload + R"("}]}})" "\n";

  auto update = httpRequest(localServer.getPort(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  localServer.shutdown();

  EXPECT_EQ(200, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("_update_ inline request exceeds indexing.max-request-body"),
            std::string::npos) << update.body();
}

TEST_F(HttpApiTest, ndjsonGroupConfigDoesNotStickAfterEnd) {
  std::string body =
      R"({"_update_":{"all_or_none":true}})" "\n"
      R"({"id":"nostick-rolled-back","title_w":"nostickyrolled token"})" "\n"
      R"({"id":"nostick-bad","title_w":"nostickyrolled token","no_such_field":"boom"})" "\n"
      "{}\n"
      R"({"id":"nostick-good","title_w":"nostickygood token"})" "\n"
      R"({"id":"nostick-bad2","title_w":"nostickygood token","no_such_field":"boom"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  ASSERT_EQ(2u, splitLines(update.body()).size()) << update.body();

  HttpReq rolledBackReq(port());
  rolledBackReq.collection("main").matchQuery("title_w", "nostickyrolled").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, rolledBackReq.status()) << rolledBackReq.rawResponse();
  EXPECT_EQ((int64_t)0, rolledBackReq.found()) << rolledBackReq.rawResponse();

  HttpReq goodReq(port());
  goodReq.collection("main").matchQuery("title_w", "nostickygood").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, goodReq.status()) << goodReq.rawResponse();
  EXPECT_EQ((int64_t)1, goodReq.found()) << goodReq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonUpdateCommitAppliesToOpenedGroupAtEof) {
  std::string body =
      R"({"_update_":{"request_id":"commit-open","commit":{}}})" "\n"
      R"({"id":"uc1","title_w":"ucommit token"})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  EXPECT_NE(update.body().find(R"("request_id":"commit-open")"), std::string::npos)
      << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "ucommit").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)1, hreq.found()) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonUrlCommitCommitsAtEof) {
  std::string body =
      R"({"id":"urlc1","title_w":"urlcommit token"})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update?commit=true",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "urlcommit").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)1, hreq.found()) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonEmptyUrlCommitCommitsDefaultCollection) {
  LuxirNode node;
  auto writer = node.getCollection("main")->getShard()->getIndexWriter();
  std::uint64_t before = writer->getIndexReader()->commitTime();
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto update = httpRequest(localServer.getPort(), http::verb::post,
                            "/collections/main/_update?commit=true", "",
                            "application/x-ndjson");
  localServer.shutdown();

  ASSERT_EQ(200, update.result_int()) << update.body();
  EXPECT_EQ(1u, splitLines(update.body()).size()) << update.body();
  EXPECT_GT(writer->getIndexReader()->commitTime(), before);
}

TEST_F(HttpApiTest, ndjsonAllOrNoneStreamSuccess) {
  std::string body =
      R"({"_update_":{"all_or_none":true}})" "\n"
      R"({"id":"aon1","title_w":"aonsuccess token"})" "\n"
      R"({"id":"aon2","title_w":"aonsuccess token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "aonsuccess").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)2, hreq.found()) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonAllOrNoneStreamFailureRollsBack) {
  std::string body =
      R"({"_update_":{"all_or_none":true}})" "\n"
      R"({"id":"aon-good","title_w":"aonfail token"})" "\n"
      R"({"id":"aon-bad","title_w":"aonfail token","no_such_field":"boom"})" "\n"
      R"({"id":"aon-never","title_w":"aonfail token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, update.result_int()) << update.body();
  EXPECT_NE(update.body().find(R"("status":"error")"), std::string::npos) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "aonfail").fields({"id"})
      .limit(10).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ((int64_t)0, hreq.found()) << hreq.rawResponse();
}

TEST_F(HttpApiTest, ndjsonAllOrNoneStreamOverCapIs413) {
  LuxirConfig config;
  config.ingest.max_request_body = 1024;
  config.ingest.max_record = 2048;
  LuxirNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  std::string payload(700, 'x');
  std::string body =
      R"({"_update_":{"all_or_none":true}})" "\n"
      R"({"id":"cap-a","title_w":"capatomic","blob_sc":")" + payload + R"("})" "\n"
      R"({"id":"cap-b","title_w":"capatomic","blob_sc":")" + payload + R"("})" "\n";

  auto update = httpRequest(localServer.getPort(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  localServer.shutdown();

  EXPECT_EQ(413, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("all_or_none NDJSON group exceeds indexing.max-request-body"),
            std::string::npos) << update.body();
}

TEST_F(HttpApiTest, ndjsonEndRejectsSubmitTimeConfig) {
  std::string body =
      R"({"id":"bad-end","title_w":"badend token"})" "\n"
      R"({"_end_":{"all_or_none":true}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("_end_ control cannot carry submit-time field 'all_or_none'"),
            std::string::npos) << update.body();
}

TEST_F(HttpApiTest, ndjsonUpdateRejectsUnknownControlField) {
  std::string body =
      R"({"_update_":{"allow_dup":true}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("unsupported _update_ control field 'allow_dup'"),
            std::string::npos) << update.body();
}

TEST_F(HttpApiTest, ndjsonEndRejectsUnknownControlField) {
  std::string body =
      R"({"id":"bad-end-unknown","title_w":"badendunknown token"})" "\n"
      R"({"_end_":{"all_or_non":true}})" "\n";

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
                            std::move(body), "application/x-ndjson");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("unsupported _end_ control field 'all_or_non'"),
            std::string::npos) << update.body();
}

// HTTP results match the in-process engine for the same query, and a doc missing
// a requested field omits that key (rows format: missing is structural, no
// null placeholders).
TEST_F(HttpApiTest, matchQueryParityAndNull) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("b1"), "title_w", std::string("dune novel"),
            "title_s", std::string("Dune"), "year_i", (int64_t)1965),
    flatdoc("id", std::string("b2"), "title_w", std::string("dune messiah"),
            "title_s", std::string("Dune Messiah")),  // no year_i
    flatdoc("id", std::string("b3"), "title_w", std::string("foundation"),
            "title_s", std::string("Foundation"), "year_i", (int64_t)1951),
  }, UpdateMessage::COMMIT);

  // Authoritative result from the in-process engine.
  auto* lreq = LocalReq::create(helper.getSearchEngine());
  lreq->collection("main").topDocs("q").matchQuery("title_w", "dune")
      .fields({"id", "title_s", "year_i"}).withStats();
  lreq->execute();
  auto localIds = idsOf(lreq->getDocs());
  lreq->done();

  // Same query over HTTP.
  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "dune")
      .fields({"id", "title_s", "year_i"}).withStats().execute();

  EXPECT_EQ(200, hreq.status());
  EXPECT_EQ(localIds, idsOf(hreq.getDocs()));
  EXPECT_EQ((int64_t)localIds.size(), hreq.found());
  ASSERT_EQ(2u, localIds.size()) << hreq.rawResponse();

  // b1 has year_i; b2 lacks it -> the key is absent from b2's doc (never null).
  EXPECT_NE(hreq.rawResponse().find(R"("year_i":1965)"), std::string::npos)
      << hreq.rawResponse();
  EXPECT_EQ(hreq.rawResponse().find(R"("year_i":null)"), std::string::npos)
      << hreq.rawResponse();
}

// The root-level shorthand (the body IS one top_docs op, ES-comparable depth)
// returns the same results as the full {"ops": ...} form.
TEST_F(HttpApiTest, rootShorthand) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("s1"), "status_s", std::string("active"),
            "title_w", std::string("alpha")),
    flatdoc("id", std::string("s2"), "status_s", std::string("inactive"),
            "title_w", std::string("beta")),
    flatdoc("id", std::string("s3"), "status_s", std::string("active"),
            "title_w", std::string("gamma")),
  }, UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"match":{"status_s":"active"}},"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();

  HttpReq full(port());
  full.matchQuery("status_s", "active").fields({"id"}).execute();
  ASSERT_EQ(200, full.status());
  EXPECT_EQ(res.body(), full.rawResponse());
  EXPECT_EQ(2u, idsOf(full.getDocs()).size());

  // an unknown root key is rejected with a client-facing error
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"match":{"status_s":"active"}},"limt":10})");
  EXPECT_EQ(400, bad.result_int());
  EXPECT_NE(bad.body().find(R"("error")"), std::string::npos);
}

// ?explain=request echoes the canonical form of the parsed request instead of
// executing it. The echo is itself a valid request body (POST-back equivalence)
// and echoing the echo is a fixpoint.
TEST_F(HttpApiTest, explainRequestEcho) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("e1"), "status_s", std::string("active"),
            "title_w", std::string("alpha")),
    flatdoc("id", std::string("e2"), "status_s", std::string("inactive"),
            "title_w", std::string("beta")),
  }, UpdateMessage::COMMIT);

  const std::string body = R"({"query":{"match":{"status_s":"active"}},"fields":["id"]})";
  auto echo = httpRequest(port(), http::verb::post,
                          "/collections/main/_search?explain=request", body);
  ASSERT_EQ(200, echo.result_int()) << echo.body();
  const std::string canonical = echo.body();
  // sugar expanded to canonical match, shorthand lowered into ops, collection applied
  EXPECT_NE(canonical.find(R"("ops")"), std::string::npos) << canonical;
  EXPECT_NE(canonical.find(R"("top_docs")"), std::string::npos) << canonical;
  EXPECT_NE(canonical.find(R"("field":"status_s")"), std::string::npos) << canonical;
  EXPECT_NE(canonical.find(R"("val":"active")"), std::string::npos) << canonical;
  EXPECT_NE(canonical.find(R"("collection")"), std::string::npos) << canonical;
  // not executed
  EXPECT_EQ(canonical.find(R"("docs")"), std::string::npos) << canonical;
  EXPECT_EQ(canonical.find(R"("found")"), std::string::npos) << canonical;

  // POST-back equivalence: the echo output runs identically to the original body.
  auto direct = httpRequest(port(), http::verb::post, "/collections/main/_search", body);
  auto viaEcho = httpRequest(port(), http::verb::post, "/collections/main/_search", canonical);
  ASSERT_EQ(200, viaEcho.result_int()) << viaEcho.body();
  EXPECT_EQ(direct.body(), viaEcho.body());

  // Fixpoint: echoing the echo is byte-identical.
  auto echo2 = httpRequest(port(), http::verb::post,
                           "/collections/main/_search?explain=request", canonical);
  ASSERT_EQ(200, echo2.result_int());
  EXPECT_EQ(canonical, echo2.body());
}

TEST_F(HttpApiTest, searchGetUrlOnly) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("u1"), "title_w", std::string("dune novel"),
            "title_s", std::string("Dune")),
    flatdoc("id", std::string("u2"), "title_w", std::string("foundation"),
            "title_s", std::string("Foundation")),
  }, UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::get,
      "/collections/main/_search?query=title_w%3Adune&limit=10&fields=id%2Ctitle_s");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("id":"u1")"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("title_s":"Dune")"), std::string::npos) << res.body();
  EXPECT_EQ(res.body().find(R"("id":"u2")"), std::string::npos) << res.body();
}

TEST_F(HttpApiTest, searchGetLimitOnlyMatchesAll) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("all1"), "title_w", std::string("one")),
    flatdoc("id", std::string("all2"), "title_w", std::string("two")),
  }, UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::get,
      "/collections/main/_search?limit=1&fields=id&get_number=true");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(1u, lines.size()) << res.body();
  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, lines[0])) << lines[0];
  ASSERT_NE(nullptr, root["found"].get_if<int64_t>());
  EXPECT_EQ(2, *root["found"].get_if<int64_t>());
  auto* docs = root["docs"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, docs);
  EXPECT_EQ(1u, docs->size());
}

TEST_F(HttpApiTest, searchPostUrlOverlayWins) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("over1"), "title_w", std::string("dune"),
            "title_s", std::string("Dune")),
    flatdoc("id", std::string("over2"), "title_w", std::string("foundation"),
            "title_s", std::string("Foundation")),
  }, UpdateMessage::COMMIT);

  const std::string target =
      "/collections/main/_search?query=title_w%3Adune&limit=10&fields=id%2Ctitle_s";
  auto overlaid = httpRequest(port(), http::verb::post, target,
      R"({"query":"title_w:foundation","limit":0,"fields":["id"]})");
  auto urlOnly = httpRequest(port(), http::verb::get, target);
  ASSERT_EQ(200, overlaid.result_int()) << overlaid.body();
  EXPECT_EQ(urlOnly.body(), overlaid.body());
}

TEST_F(HttpApiTest, searchUrlOverlayPreservesSiblingOps) {
  const std::string body = R"({"ops":{
    "q":{"top_docs":{"query":"title_w:body","fields":["id"]}},
    "cats":{"field_facet":{"field":"cat_s"}}
  }})";
  auto echo = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&query=title_w%3Aurl&fields=title_s", body);
  ASSERT_EQ(200, echo.result_int()) << echo.body();

  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, echo.body())) << echo.body();
  auto* ops = root["ops"].get_if<glz::generic_i64::object_t>();
  ASSERT_NE(nullptr, ops);
  EXPECT_TRUE(ops->contains("cats"));
  auto& td = root["ops"]["q"]["top_docs"];
  EXPECT_EQ("title_w:url", *td["query"]["expr"]["q"].get_if<std::string>());
  auto* fields = td["fields"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, fields);
  ASSERT_EQ(1u, fields->size());
  EXPECT_EQ("title_s", *(*fields)[0].get_if<std::string>());
}

TEST_F(HttpApiTest, searchUrlOverlayEchoPostbackEquivalent) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("ep1"), "title_w", std::string("dune")),
  }, UpdateMessage::COMMIT);

  const std::string target =
      "/collections/main/_search?query=title_w%3Adune&limit=4&fields=id&get_number=true";
  auto direct = httpRequest(port(), http::verb::get, target);
  auto echo = httpRequest(port(), http::verb::get, target + "&explain=request");
  ASSERT_EQ(200, echo.result_int()) << echo.body();
  auto postback = httpRequest(port(), http::verb::post, "/collections/main/_search", echo.body());
  ASSERT_EQ(200, postback.result_int()) << postback.body();
  EXPECT_EQ(direct.body(), postback.body());

  auto echo2 = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request", echo.body());
  ASSERT_EQ(200, echo2.result_int()) << echo2.body();
  EXPECT_EQ(echo.body(), echo2.body());
}

TEST_F(HttpApiTest, searchUrlOverlayScalarGrammars) {
  const std::string target =
      "/collections/main/_search?explain=request"
      "&request_id=url%22%5Cvalue&freshness_ms=17&time_zone=UTC&profile=true&max_parallel=1"
      "&query=title_w%3Adune&limit=bad&limit=7&offset=-2&fields=id%2Ctitle_s"
      "&sort=price_i+desc&sort=score&batch_size=5&document_format=columns"
      "&get_number=true&get_scores=false";
  auto echo = httpRequest(port(), http::verb::get, target);
  ASSERT_EQ(200, echo.result_int()) << echo.body();

  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, echo.body())) << echo.body();
  EXPECT_EQ("url\"\\value", *root["request_id"].get_if<std::string>());
  EXPECT_EQ(17, *root["freshness_ms"].get_if<int64_t>());
  EXPECT_EQ("UTC", *root["time_zone"].get_if<std::string>());
  EXPECT_TRUE(*root["profile"].get_if<bool>());
  EXPECT_EQ(1, *root["max_parallel"].get_if<int64_t>());
  auto& td = root["ops"]["q"]["top_docs"];
  EXPECT_EQ(7, *td["limit"].get_if<int64_t>());
  EXPECT_EQ(-2, *td["offset"].get_if<int64_t>());
  EXPECT_EQ(5, *td["batch_size"].get_if<int64_t>());
  EXPECT_EQ("columns", *td["document_format"].get_if<std::string>());
  EXPECT_TRUE(*td["get_number"].get_if<bool>());
  auto* sorts = td["sorts"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, sorts);
  ASSERT_EQ(2u, sorts->size());
  EXPECT_EQ("price_i", *(*sorts)[0]["expr"].get_if<std::string>());
  EXPECT_EQ("desc", *(*sorts)[0]["dir"].get_if<std::string>());
  EXPECT_EQ("score", *(*sorts)[1]["expr"].get_if<std::string>());
}

TEST_F(HttpApiTest, searchUrlQueryIsJsonStringValue) {
  auto echo = httpRequest(port(), http::verb::get,
      "/collections/main/_search?explain=request&query=title_w%3Adune%22%5Cprobe");
  ASSERT_EQ(200, echo.result_int()) << echo.body();
  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, echo.body())) << echo.body();
  auto& td = root["ops"]["q"]["top_docs"];
  EXPECT_EQ("title_w:dune\"\\probe", *td["query"]["expr"]["q"].get_if<std::string>());
  EXPECT_FALSE(td.contains("limit"));
}

TEST_F(HttpApiTest, searchUrlFieldsGrammar) {
  auto repeated = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&fields=bad%2C%2C&fields=id%2Ctitle_s",
      R"({"fields":["body"]})");
  ASSERT_EQ(200, repeated.result_int()) << repeated.body();
  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, repeated.body())) << repeated.body();
  auto* fields = root["ops"]["q"]["top_docs"]["fields"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, fields);
  ASSERT_EQ(2u, fields->size());
  EXPECT_EQ("id", *(*fields)[0].get_if<std::string>());
  EXPECT_EQ("title_s", *(*fields)[1].get_if<std::string>());

  auto cleared = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&fields=", R"({"fields":["body"]})");
  ASSERT_EQ(200, cleared.result_int()) << cleared.body();
  ASSERT_FALSE(glz::read_json(root, cleared.body())) << cleared.body();
  EXPECT_FALSE(root["ops"]["q"]["top_docs"].contains("fields"));

  for (std::string_view value : {"id%2C", "%2Cid", "id%2C%2Ctitle_s"}) {
    auto bad = httpRequest(port(), http::verb::get,
        "/collections/main/_search?fields=" + std::string(value));
    EXPECT_EQ(400, bad.result_int()) << bad.body();
    EXPECT_NE(bad.body().find("invalid URL parameter 'fields': empty list item"),
              std::string::npos) << bad.body();
  }
}

TEST_F(HttpApiTest, searchUrlSortGrammar) {
  auto echo = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&sort=price_i+desc"
      "&sort=sum%28x_i%2Cy_i%29+asc",
      R"({"sorts":[{"field":"old_i","dir":"asc"}]})");
  ASSERT_EQ(200, echo.result_int()) << echo.body();
  glz::generic_i64 root;
  ASSERT_FALSE(glz::read_json(root, echo.body())) << echo.body();
  auto* sorts = root["ops"]["q"]["top_docs"]["sorts"].get_if<glz::generic_i64::array_t>();
  ASSERT_NE(nullptr, sorts);
  ASSERT_EQ(2u, sorts->size());
  EXPECT_EQ("price_i", *(*sorts)[0]["expr"].get_if<std::string>());
  EXPECT_EQ("desc", *(*sorts)[0]["dir"].get_if<std::string>());
  EXPECT_EQ("sum(x_i,y_i)", *(*sorts)[1]["expr"].get_if<std::string>());

  auto cleared = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&sort=",
      R"({"sorts":[{"field":"old_i"}]})");
  ASSERT_EQ(200, cleared.result_int()) << cleared.body();
  ASSERT_FALSE(glz::read_json(root, cleared.body())) << cleared.body();
  EXPECT_FALSE(root["ops"]["q"]["top_docs"].contains("sorts"));

  auto mixed = httpRequest(port(), http::verb::get,
      "/collections/main/_search?sort=&sort=price_i");
  EXPECT_EQ(400, mixed.result_int()) << mixed.body();
  EXPECT_NE(mixed.body().find("invalid URL parameter 'sort': empty value cannot be combined"),
            std::string::npos) << mixed.body();
}

TEST_F(HttpApiTest, searchUrlKnownValueErrorsNameParam) {
  const std::array badValues{
    std::pair{"freshness_ms", "-1"},
    std::pair{"profile", "TRUE"},
    std::pair{"max_parallel", "1x"},
    std::pair{"limit", "1.0"},
    std::pair{"offset", "9223372036854775808"},
    std::pair{"batch_size", "2147483648"},
    std::pair{"document_format", "row"},
    std::pair{"get_number", "1"},
    std::pair{"get_scores", "falsex"},
  };
  for (const auto& [name, value] : badValues) {
    std::string target = "/collections/main/_search?" + std::string(name) + "=" + value;
    auto bad = httpRequest(port(), http::verb::get, target);
    EXPECT_EQ(400, bad.result_int()) << name << ": " << bad.body();
    EXPECT_NE(bad.body().find("invalid URL parameter '" + std::string(name) + "':"),
              std::string::npos) << bad.body();
  }
}

TEST_F(HttpApiTest, searchUrlSemanticValidationMatchesBody) {
  auto urlZone = httpRequest(port(), http::verb::get,
      "/collections/main/_search?time_zone=Not%2FA%2FZone&limit=0");
  auto bodyZone = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"time_zone":"Not/A/Zone","limit":0})");
  EXPECT_EQ(bodyZone.result_int(), urlZone.result_int());
  EXPECT_EQ(bodyZone.body(), urlZone.body());

  auto urlParallel = httpRequest(port(), http::verb::get,
      "/collections/main/_search?max_parallel=2&limit=0");
  auto bodyParallel = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"max_parallel":2,"limit":0})");
  EXPECT_EQ(bodyParallel.result_int(), urlParallel.result_int());
  EXPECT_EQ(bodyParallel.body(), urlParallel.body());
}

TEST_F(HttpApiTest, searchUrlTopDocsQMaterializationRule) {
  for (std::string_view body : {"{}", R"({"time_zone":"UTC"})"}) {
    auto echo = httpRequest(port(), http::verb::post,
        "/collections/main/_search?explain=request&query=title_w%3Adune", std::string(body));
    ASSERT_EQ(200, echo.result_int()) << echo.body();
    EXPECT_NE(echo.body().find(R"("q":{"top_docs")"), std::string::npos) << echo.body();
  }

  const std::string missingQ = R"({"ops":{"cats":{"field_facet":{"field":"cat_s"}}}})";
  auto bad = httpRequest(port(), http::verb::post,
      "/collections/main/_search?limit=2", missingQ);
  EXPECT_EQ(400, bad.result_int()) << bad.body();
  EXPECT_NE(bad.body().find(
      "URL TopDocs parameters target ops.q, but the request body has no top_docs op named 'q'"),
      std::string::npos) << bad.body();

  auto wrongQ = httpRequest(port(), http::verb::post,
      "/collections/main/_search?fields=id",
      R"({"ops":{"q":{"field_facet":{"field":"cat_s"}}}})");
  EXPECT_EQ(400, wrongQ.result_int()) << wrongQ.body();

  auto lastQTopDocs = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&limit=2",
      R"({"ops":{"q":{"field_facet":{"field":"cat_s"}},"q":{"top_docs":{"limit":1}}}})");
  EXPECT_EQ(200, lastQTopDocs.result_int()) << lastQTopDocs.body();
  auto lastQFacet = httpRequest(port(), http::verb::post,
      "/collections/main/_search?limit=2",
      R"({"ops":{"q":{"top_docs":{"limit":1}},"q":{"field_facet":{"field":"cat_s"}}}})");
  EXPECT_EQ(400, lastQFacet.result_int()) << lastQFacet.body();

  auto requestOnly = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request&time_zone=UTC", missingQ);
  EXPECT_EQ(200, requestOnly.result_int()) << requestOnly.body();
}

TEST_F(HttpApiTest, searchMethodPolicy) {
  auto head = httpRequest(port(), http::verb::head, "/collections/main/_search");
  EXPECT_EQ(405, head.result_int());
  EXPECT_EQ("GET, POST", head[http::field::allow]);

  auto getBody = httpRequest(port(), http::verb::get, "/collections/main/_search", "{}");
  EXPECT_EQ(400, getBody.result_int()) << getBody.body();
  EXPECT_NE(getBody.body().find("GET _search does not accept a request body; use POST"),
            std::string::npos) << getBody.body();

  auto emptyPost = httpRequest(port(), http::verb::post,
      "/collections/main/_search?limit=0");
  EXPECT_EQ(200, emptyPost.result_int()) << emptyPost.body();
}

// URL-parameter policy: unknown parameters are accepted and ignored (the URL is
// an open channel - correlation ids, middleware); recognized keys enforce values.
TEST_F(HttpApiTest, urlParamPolicy) {
  // bad value on a RECOGNIZED key is an author error
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/_search?explain=foo",
                         R"({"limit":1})");
  EXPECT_EQ(400, bad.result_int());
  EXPECT_NE(bad.body().find(R"("error")"), std::string::npos) << bad.body();

  // unknown params (e.g. a correlation id) pass through; the query executes
  auto unknown = httpRequest(port(), http::verb::post,
                             "/collections/main/_search?trace_id=abc-123&_=17",
                             R"({"limit":1})");
  EXPECT_EQ(200, unknown.result_int()) << unknown.body();

  // unknown params compose with explain (last-wins on repeats)
  auto both = httpRequest(port(), http::verb::post,
                          "/collections/main/_search?trace_id=x&explain=request",
                          R"({"limit":1})");
  EXPECT_EQ(200, both.result_int()) << both.body();
  EXPECT_NE(both.body().find(R"("ops")"), std::string::npos) << both.body();

  auto malformed = httpRequest(port(), http::verb::post,
                               "/collections/main/_search?explain=request", "{not json");
  EXPECT_EQ(400, malformed.result_int());
  EXPECT_NE(malformed.body().find(R"("error")"), std::string::npos) << malformed.body();

  // health ignores params entirely
  auto health = httpRequest(port(), http::verb::get, "/health?explain=request&x=1");
  EXPECT_EQ(200, health.result_int());
}

// A string field containing JSON-significant characters round-trips through the
// renderer's escaping and back via the glaze parse in HttpReq.
TEST_F(HttpApiTest, stringEscaping) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("e1"), "title_w", std::string("findme"),
            "title_s", std::string("a\"b\nc\\d")),
  }, UpdateMessage::COMMIT);

  HttpReq hreq(port());
  hreq.matchQuery("title_w", "findme").fields({"id", "title_s"}).execute();
  ASSERT_EQ(200, hreq.status());

  // Escaping actually happened on the wire...
  EXPECT_NE(hreq.rawResponse().find(R"(\")"), std::string::npos) << hreq.rawResponse();
  EXPECT_NE(hreq.rawResponse().find(R"(\n)"), std::string::npos) << hreq.rawResponse();
  // ...and the value survives a full round-trip.
  auto docs = hreq.getDocs();
  ASSERT_EQ(1u, docs.size());
  const auto* v = find(docs[0], "title_s");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(std::string("a\"b\nc\\d"), std::get<std::string>(*v));
}

// A multi-valued field present on one doc and absent on another: the present
// doc renders an array, the absent doc omits the key entirely (rows format).
TEST_F(HttpApiTest, multiValuedMissingOmitsKey) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("m1"), "title_w", std::string("multi"),
            "tags_ss", std::vector<std::string>{"x", "y"}),
    flatdoc("id", std::string("m2"), "title_w", std::string("multi")),  // no tags_ss
  }, UpdateMessage::COMMIT);

  HttpReq hreq(port());
  hreq.matchQuery("title_w", "multi").fields({"id", "tags_ss"}).execute();
  ASSERT_EQ(200, hreq.status());

  EXPECT_NE(hreq.rawResponse().find(R"("tags_ss":[)"), std::string::npos) << hreq.rawResponse();
  EXPECT_EQ(hreq.rawResponse().find(R"("tags_ss":null)"), std::string::npos) << hreq.rawResponse();
}

// document_format parses from JSON ("columns") and overrides the HTTP rows
// default: columnar cells keep null placeholders for missing slots.
TEST_F(HttpApiTest, explicitColumnsFormatOverHttp) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("c1"), "title_w", std::string("col fmt"),
            "year_i", (int64_t)2001),
    flatdoc("id", std::string("c2"), "title_w", std::string("col fmt")),
  }, UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"match":{"title_w":"col"}},"fields":["id","year_i"],"document_format":"columns"})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("year_i":2001)"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("year_i":null)"), std::string::npos) << res.body();
}

// limit > one batch forces the engine to call reply() multiple times, exercising
// the per-connection write queue (multiple NDJSON lines on one connection).
TEST_F(HttpApiTest, streamingMultipleBatches) {
  std::vector<Doc> docs;
  for (int i = 0; i < 250; i++) {
    docs.push_back(flatdoc("id", std::string("d") + std::to_string(i),
                           "title_w", std::string("apple")));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  HttpReq hreq(port());
  hreq.matchQuery("title_w", "apple").fields({"id"})
      .limit(250).batchSize(50).withStats().execute();

  EXPECT_EQ(200, hreq.status());
  EXPECT_EQ((int64_t)250, hreq.found());
  EXPECT_EQ(250u, hreq.ids().size());
  EXPECT_GT(hreq.lineCount(), 1u) << "expected multiple NDJSON batches";
}

// Shut the server down while a client connection is open and a (potentially
// streaming) query is in flight, without the client reading the response.  The
// graceful-drain path must release the request arena and return without hanging
// or use-after-free (validated under ASan).
// ---- /_schema ---------------------------------------------------------------

TEST_F(HttpApiTest, schemaGetDefault) {
  auto res = httpRequest(port(), http::verb::get, "/collections/main/_schema");
  ASSERT_EQ(200, res.result_int()) << res.body();
  // The default schema: reserved fields + suffix templates, pretty-printed.
  EXPECT_NE(res.body().find("\"fields\""), std::string::npos);
  EXPECT_NE(res.body().find("\"templates\""), std::string::npos);
  EXPECT_NE(res.body().find("\"id\""), std::string::npos);
  EXPECT_NE(res.body().find("\"_t\""), std::string::npos);
  EXPECT_NE(res.body().find("\"type\": \"text\""), std::string::npos) << "pretty + lowercase";
  EXPECT_NE(res.body().find('\n'), std::string::npos) << "schema responses are pretty-printed";
}

TEST_F(HttpApiTest, schemaSetGetRoundTrip) {
  const std::string schema = R"({
    "fields": {
      "title": {"type": "text", "stored": true,
                "analyzer": {"tokenizer": "unicode_word", "filters": ["nfkc_cf", "fold"]}},
      "year":  {"type": "int", "index": "range"},
      "vec":   {"type": "vector", "dims": 4, "metric": "cosine"}
    }
  })";
  auto set = httpRequest(port(), http::verb::post, "/collections/main/_schema", schema);
  ASSERT_EQ(200, set.result_int()) << set.body();
  EXPECT_NE(set.body().find("\"title\""), std::string::npos);
  EXPECT_NE(set.body().find("\"metric\": \"cosine\""), std::string::npos);

  auto get = httpRequest(port(), http::verb::get, "/collections/main/_schema");
  ASSERT_EQ(200, get.result_int());
  EXPECT_EQ(set.body(), get.body()) << "write response and GET speak the same shape";

  // Echo doctrine: GET output is a valid write body, and posting it back is a
  // no-op under BOTH modes (set of identical defs is identity).
  auto setBack = httpRequest(port(), http::verb::post, "/collections/main/_schema", get.body());
  ASSERT_EQ(200, setBack.result_int()) << setBack.body();
  EXPECT_EQ(get.body(), setBack.body());
  auto replaceBack = httpRequest(port(), http::verb::post,
                                 "/collections/main/_schema?mode=replace_all", get.body());
  ASSERT_EQ(200, replaceBack.result_int()) << replaceBack.body();
  EXPECT_EQ(get.body(), replaceBack.body());
}

TEST_F(HttpApiTest, schemaSetAndReplaceAllModes) {
  auto seed = httpRequest(port(), http::verb::post, "/collections/main/_schema",
                          R"({"fields": {"title": {"type": "text"}}})");
  ASSERT_EQ(200, seed.result_int()) << seed.body();

  // Default mode=set: sets the named definitions, keeps title.
  auto post = httpRequest(port(), http::verb::post, "/collections/main/_schema",
                          R"({"fields": {"published": {"type": "date", "index": "range"}}})");
  ASSERT_EQ(200, post.result_int()) << post.body();
  EXPECT_NE(post.body().find("\"published\""), std::string::npos);
  EXPECT_NE(post.body().find("\"title\""), std::string::npos);

  // set on an EXISTING name replaces that WHOLE definition (not a property merge).
  auto redefine = httpRequest(port(), http::verb::post, "/collections/main/_schema",
                              R"({"fields": {"published": {"type": "date"}}})");
  ASSERT_EQ(200, redefine.result_int()) << redefine.body();
  EXPECT_EQ(redefine.body().find("\"index\": \"range\""), std::string::npos)
      << "whole-definition set dropped the old index property: " << redefine.body();

  // mode=replace_all: title/published gone, reserved fields materialized.
  auto replace = httpRequest(port(), http::verb::post,
                             "/collections/main/_schema?mode=replace_all",
                             R"({"fields": {"price": "int"}})");
  ASSERT_EQ(200, replace.result_int()) << replace.body();
  EXPECT_EQ(replace.body().find("\"title\""), std::string::npos);
  EXPECT_EQ(replace.body().find("\"published\""), std::string::npos);
  EXPECT_NE(replace.body().find("\"price\""), std::string::npos);
  EXPECT_NE(replace.body().find("\"id\""), std::string::npos);
  EXPECT_NE(replace.body().find("\"_version_\""), std::string::npos);
  // The string shorthand reads as {"type": "int"} and writes canonically.
  EXPECT_NE(replace.body().find("\"type\": \"int\""), std::string::npos);

  // Unknown mode: 400 naming the valid modes.
  auto badMode = httpRequest(port(), http::verb::post,
                             "/collections/main/_schema?mode=merge",
                             R"({"fields": {"x": "int"}})");
  EXPECT_EQ(400, badMode.result_int());
  EXPECT_NE(badMode.body().find("valid: set, replace_all"), std::string::npos) << badMode.body();
}

TEST_F(HttpApiTest, schemaErrorsAreTeaching) {
  // Unknown analyzer component: 400 naming the valid set.
  auto badTok = httpRequest(port(), http::verb::post, "/collections/main/_schema",
      R"({"fields": {"t": {"type": "text", "analyzer": {"tokenizer": "standard"}}}})");
  EXPECT_EQ(400, badTok.result_int());
  EXPECT_NE(badTok.body().find("unknown tokenizer 'standard'"), std::string::npos) << badTok.body();
  EXPECT_NE(badTok.body().find("whitespace"), std::string::npos) << "lists valid tokenizers";

  // Unknown FieldDef key: strict dialect, 400.
  auto badKey = httpRequest(port(), http::verb::post, "/collections/main/_schema",
      R"({"fields": {"x": {"typ": "int"}}})");
  EXPECT_EQ(400, badKey.result_int());

  // Unknown type name: 400 (enum names are lowercase, exact).
  auto badType = httpRequest(port(), http::verb::post, "/collections/main/_schema",
      R"({"fields": {"x": {"type": "INT"}}})");
  EXPECT_EQ(400, badType.result_int());

  // id redefined incompatibly: 400 with a reserved-field message.
  auto badId = httpRequest(port(), http::verb::post, "/collections/main/_schema",
      R"({"fields": {"id": {"type": "string"}}})");
  EXPECT_EQ(400, badId.result_int());
  EXPECT_NE(badId.body().find("reserved field 'id'"), std::string::npos) << badId.body();

  // Bare-integer enum values are rejected at parse over JSON (positioned
  // error); the engine-level range check (SchemaTest.unknownEnumValuesRejected)
  // guards the binary/gRPC path where integers do decode.
  auto badEnum = httpRequest(port(), http::verb::post, "/collections/main/_schema",
      R"({"fields": {"v": {"type": "vector", "metric": 99}}})");
  EXPECT_EQ(400, badEnum.result_int());
  EXPECT_NE(badEnum.body().find("metric"), std::string::npos) << badEnum.body();
}

TEST_F(HttpApiTest, schemaGetMissingCollectionIs404) {
  auto res = httpRequest(port(), http::verb::get, "/collections/never_created/_schema");
  EXPECT_EQ(404, res.result_int());
  EXPECT_NE(res.body().find("\"error\""), std::string::npos);
}

TEST_F(HttpApiTest, schemaMethodNotAllowed) {
  // The verb carries no schema semantics: writes are POST + ?mode=..., and the
  // 405 teaches that spelling.
  for (auto verb : {http::verb::put, http::verb::patch}) {
    auto res = httpRequest(port(), verb, "/collections/main/_schema",
                           R"({"fields": {}})");
    EXPECT_EQ(405, res.result_int());
    EXPECT_EQ("GET, POST", res[http::field::allow]);
    EXPECT_NE(res.body().find("mode=replace_all"), std::string::npos) << res.body();
  }
}

TEST_F(HttpApiTest, schemaDrivesIndexingEndToEnd) {
  // Install a schema over HTTP, index through it, and query through it: the
  // whole loop on one connection surface.
  auto set = httpRequest(port(), http::verb::post, "/collections/main/_schema", R"({
    "fields": {
      "title": {"type": "text", "analyzer": {"tokenizer": "whitespace", "filters": ["lowercase"]}},
      "year":  {"type": "int", "index": "range"}
    }
  })");
  ASSERT_EQ(200, set.result_int()) << set.body();

  auto update = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"1","title":"DUNE rising","year":1965},
                  {"id":"2","title":"other book","year":2001}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();

  // lowercase filter applied at index time -> query for "dune" matches "DUNE".
  auto q = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"match":{"title":"dune"}},"fields":["id"]})");
  ASSERT_EQ(200, q.result_int()) << q.body();
  EXPECT_NE(q.body().find(R"("id":"1")"), std::string::npos) << q.body();
  EXPECT_EQ(q.body().find(R"("id":"2")"), std::string::npos) << q.body();

  // Range index installed via the schema answers a range query.
  auto range = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":"year:[1900 TO 1970]","fields":["id"]})");
  ASSERT_EQ(200, range.result_int()) << range.body();
  EXPECT_NE(range.body().find(R"("id":"1")"), std::string::npos) << range.body();
  EXPECT_EQ(range.body().find(R"("id":"2")"), std::string::npos) << range.body();
}

TEST_F(HttpApiTest, shutdownDuringInflightRequest) {
  std::vector<Doc> docs;
  for (int i = 0; i < 250; i++) {
    docs.push_back(flatdoc("id", std::string("s") + std::to_string(i),
                           "title_w", std::string("banana")));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  net::io_context cioc;
  tcp::socket sock(cioc);
  tcp::resolver resolver(cioc);
  net::connect(sock, resolver.resolve("127.0.0.1", std::to_string(port())));

  http::request<http::string_body> req(http::verb::post, "/collections/main/_search", 11);
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = R"({"query":{"match":{"title_w":"banana"}},"limit":250,"batch_size":50,"fields":["id"]})";
  req.prepare_payload();
  http::write(sock, req);

  // Tear down without reading the response.
  server->shutdown();
  server.reset();
  SUCCEED();  // reaching here means the drain completed without hanging

  beast::error_code ec;
  sock.shutdown(tcp::socket::shutdown_both, ec);
}

TEST_F(HttpApiTest, shutdownDrainsWorkAcrossIoShards) {
  LuxirConfig config;
  config.ingest.stream_batch_docs = 1;
  config.ingest.max_inflight_batches = 8;
  LuxirNode node(config);
  HttpServer localServer(node, 4, 0, 4096);
  localServer.start();

  std::string payload(8 * 1024, 'z');
  std::string seed = R"({"docs":[)";
  for (int i = 0; i < 256; i++) {
    if (i != 0) seed += ',';
    seed += R"({"id":"shutdown-)" + std::to_string(i) +
        R"(","title_w":"shardshutdown","blob_sc":")" + payload + R"("})";
  }
  seed += R"(],"commit":{}})";
  auto update = httpRequest(localServer.getPort(), http::verb::post,
      "/collections/shutdown_shards/_update", std::move(seed));
  ASSERT_EQ(200, update.result_int()) << update.body();

  net::io_context clientIoc;
  tcp::resolver resolver(clientIoc);
  auto endpoint = resolver.resolve("127.0.0.1", std::to_string(localServer.getPort()));
  std::vector<std::unique_ptr<beast::tcp_stream>> connections;
  for (int i = 0; i < 4; i++) {
    auto connection = std::make_unique<beast::tcp_stream>(clientIoc);
    connection->connect(endpoint);
    connections.push_back(std::move(connection));
  }

  for (int i = 0; i < 2; i++) {
    http::request<http::string_body> request{
        http::verb::post, "/collections/shutdown_shards/_search", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    request.keep_alive(true);
    request.body() =
        R"({"query":{"all":true},"max_parallel":0,"limit":256,"batch_size":16,"fields":["id","blob_sc"]})";
    request.prepare_payload();
    http::write(*connections[i], request);
  }

  for (int i = 2; i < 4; i++) {
    std::string body;
    for (int doc = 0; doc < 64; doc++) {
      body += R"({"id":"shutdown-stream-)" + std::to_string(i) + '-' +
          std::to_string(doc) + R"(","title_w":"shardshutdown","blob_sc":")" +
          payload + R"("})" "\n";
    }
    http::request<http::string_body> request{
        http::verb::post, "/collections/shutdown_stream/_update", 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/x-ndjson");
    request.keep_alive(false);
    request.body() = std::move(body);
    request.prepare_payload();
    http::write(*connections[i], request);
  }

  EXPECT_NO_THROW(localServer.shutdown());

  for (auto& connection : connections) {
    beast::error_code ec;
    connection->socket().shutdown(tcp::socket::shutdown_both, ec);
    connection->socket().close(ec);
  }
}

// ?format=docs: every line is a bare document - no envelope, no batching
// visible on the wire regardless of batch_size.
TEST_F(HttpApiTest, docsFormatIsPureDocLines) {
  LuxirTest::clearCollection("http_docs");
  CollectionHelper ch("http_docs");
  for (int i = 0; i < 5; i++) {
    auto commit = i == 4 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
    ch.index(flatdoc("id", "d" + std::to_string(i)), commit);
  }

  auto res = httpRequest(port(), http::verb::post, "/collections/http_docs/_search?format=docs",
      R"({"query":{"all":true},"limit":-1,"batch_size":2,"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();

  auto lines = splitLines(res.body());
  ASSERT_EQ(5u, lines.size()) << res.body();
  std::set<std::string> ids;
  for (auto& line : lines) {
    EXPECT_EQ(std::string::npos, line.find("\"docs\"")) << line;  // no envelope
    EXPECT_EQ(std::string::npos, line.find("\"more\"")) << line;
    glz::generic_i64 doc;
    ASSERT_FALSE(glz::read_json(doc, line)) << line;
    ids.insert(*doc["id"].get_if<std::string>());
  }
  EXPECT_EQ(std::set<std::string>({"d0", "d1", "d2", "d3", "d4"}), ids);
}

// The docs format is also selectable in the request body (full form,
// request-level response_format) for clients that cannot set URL params.
TEST_F(HttpApiTest, docsFormatSelectableInBody) {
  LuxirTest::clearCollection("http_docs_body");
  CollectionHelper ch("http_docs_body");
  ch.index(flatdoc("id", std::string("b1")), UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::post, "/collections/http_docs_body/_search",
      R"({"ops":{"q":{"top_docs":{"query":{"all":true},"fields":["id"]}}},"response_format":"docs"})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_EQ(R"({"id":"b1"})" "\n", res.body());
}

// get_number puts the count in a _header_ meta record on the first line;
// without it the body is pure documents.
TEST_F(HttpApiTest, docsFormatHeaderCarriesFound) {
  LuxirTest::clearCollection("http_docs_hdr");
  CollectionHelper ch("http_docs_hdr");
  ch.index(flatdoc("id", std::string("h1")), UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("h2")), UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::post, "/collections/http_docs_hdr/_search?format=docs",
      R"({"query":{"all":true},"limit":-1,"get_number":true,"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(3u, lines.size()) << res.body();
  EXPECT_EQ(R"({"_header_":{"found":2}})", lines[0]);
  EXPECT_EQ(std::string::npos, res.body().find("_header_", lines[0].size())) << res.body();

  auto pure = httpRequest(port(), http::verb::post, "/collections/http_docs_hdr/_search?format=docs",
      R"({"query":{"all":true},"limit":-1,"fields":["id"]})");
  EXPECT_EQ(std::string::npos, pure.body().find("_header_")) << pure.body();
  EXPECT_EQ(2u, splitLines(pure.body()).size());
}

// The docs format rejects requests whose response would need an envelope.
TEST_F(HttpApiTest, docsFormatValidation) {
  auto facet = httpRequest(port(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"ops":{"cats":{"field_facet":{"field":"http_facet_s"}}}})");
  EXPECT_EQ(400, facet.result_int()) << facet.body();

  auto columns = httpRequest(port(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"query":{"all":true},"document_format":"columns","fields":["id"]})");
  EXPECT_EQ(400, columns.result_int()) << columns.body();
  EXPECT_NE(std::string::npos, columns.body().find("format=docs")) << columns.body();

  auto unknown = httpRequest(port(), http::verb::post, "/collections/main/_search?format=lines",
      R"({"query":{"all":true}})");
  EXPECT_EQ(400, unknown.result_int()) << unknown.body();
}

// An engine error before anything streams is a plain HTTP error, not a 200
// with an error line (the docs format has no in-band error representation).
TEST_F(HttpApiTest, docsFormatErrorBeforeFlushIsHttpError) {
  helper.index(flatdoc("id", std::string("de1")), UpdateMessage::COMMIT);
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"query":{"all":true},"fields":["nosuchfield"]})");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(std::string::npos, res.body().find("nosuchfield")) << res.body();
}

// Object-valued sole-underscore-field records are the reserved meta/control
// namespace in streaming ingest: unknown names are an error, never indexed as
// documents.  Scalar-valued ones stay documents ({"_version_":42} is a
// legitimate one-field export; every real control carries an object).
TEST_F(HttpApiTest, ndjsonUnknownUnderscoreRecordIs400) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
      std::string(R"({"_bogus_":{"x":1}})") + "\n", "application/x-ndjson");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(std::string::npos, res.body().find("_bogus_")) << res.body();

  // Scalar payload: classified as a document, not an unknown control (it may
  // still fail schema-level checks, but never as a control record).
  auto scalar = httpRequest(port(), http::verb::post, "/collections/main/_update",
      std::string(R"({"_version_":42})") + "\n", "application/x-ndjson");
  EXPECT_EQ(std::string::npos, scalar.body().find("unknown control record")) << scalar.body();

  // _header_ itself must carry an object.
  auto malformed = httpRequest(port(), http::verb::post, "/collections/main/_update",
      std::string(R"({"_header_":7})") + "\n", "application/x-ndjson");
  EXPECT_EQ(400, malformed.result_int()) << malformed.body();
  EXPECT_NE(std::string::npos, malformed.body().find("_header_")) << malformed.body();
}

// Meta records must not grow the batch arena unboundedly.  Both rotation
// branches need > ingest.stream_batch_size (1MiB) of meta bytes: a
// headers-only prefix (empty batch -> wholesale replacement) and headers
// behind a pending doc (shared accounting -> threshold submit).
TEST_F(HttpApiTest, ndjsonHeaderRecordsInterleaveWithDocs) {
  LuxirTest::clearCollection("http_hdrs");
  std::string headerLine = R"({"_header_":{"pad":")" + std::string(4096, 'h') + R"("}})" "\n";
  std::string megOfHeaders;
  for (int i = 0; i < 300; i++) megOfHeaders += headerLine;  // ~1.2MiB

  std::string body = megOfHeaders;  // empty-batch branch: batch replacement
  body += R"({"id":"hd0","num_i":0})" "\n";
  body += megOfHeaders;             // pending-doc branch: threshold submit
  body += R"({"id":"hd1","num_i":1})" "\n";
  body += R"({"_end_":{"commit":{}}})" "\n";

  auto res = httpRequest(port(), http::verb::post, "/collections/http_hdrs/_update",
                         std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, res.result_int()) << res.body();

  auto check = httpRequest(port(), http::verb::post, "/collections/http_hdrs/_search?format=docs",
      R"({"query":{"all":true},"limit":-1,"get_number":true,"fields":["id"]})");
  ASSERT_EQ(200, check.result_int()) << check.body();
  auto lines = splitLines(check.body());
  ASSERT_EQ(3u, lines.size()) << check.body();  // header + 2 docs
  EXPECT_EQ(R"({"_header_":{"found":2}})", lines[0]);
}

// format=docs validation covers fusion source sub-ops (silently discarding
// authored ops behind a format flag would be worse than rejecting them).
TEST_F(HttpApiTest, docsFormatRejectsFusionSourceOps) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"ops":{"q":{"fusion":{"sources":{"a":{"query":{"all":true},)"
      R"("ops":{"f":{"field_facet":{"field":"http_facet_s"}}}}},"rrf":{}}}}})");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(std::string::npos, res.body().find("nested ops")) << res.body();

  auto explain = httpRequest(port(), http::verb::post,
      "/collections/main/_search?format=docs&explain=request", R"({"query":{"all":true}})");
  EXPECT_EQ(400, explain.result_int()) << explain.body();

  // Body-selected docs must not slip past explain either.
  auto bodyExplain = httpRequest(port(), http::verb::post,
      "/collections/main/_search?explain=request",
      R"({"ops":{"q":{"top_docs":{"query":{"all":true}}}},"response_format":"docs"})");
  EXPECT_EQ(400, bodyExplain.result_int()) << bodyExplain.body();

  // Duplicate op names: the raw ops view keeps both entries (execution is
  // last-wins), so docs framing rejects the ambiguity.
  auto dup = httpRequest(port(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"ops":{"q":{"top_docs":{"query":{"all":true},"fields":["id"]}},)"
      R"("q":{"top_docs":{"query":{"all":true},"fields":["id"]}}}})");
  EXPECT_EQ(400, dup.result_int()) << dup.body();
  EXPECT_NE(std::string::npos, dup.body().find("duplicate")) << dup.body();
}

// Multiple DocList ops in one docs-format request: outputs interleave in
// runs, each introduced by an op-named _header_ marker; the op's first marker
// carries its found.  Every doc is attributable by tracking the current
// section.
TEST_F(HttpApiTest, docsFormatMultiOpRunMarkers) {
  LuxirTest::clearCollection("http_docs_multi");
  CollectionHelper ch("http_docs_multi");
  std::vector<Doc> docs;
  for (int i = 0; i < 6; i++) docs.push_back(flatdoc("id", "a" + std::to_string(i), "kind_s", std::string("a")));
  for (int i = 0; i < 4; i++) docs.push_back(flatdoc("id", "b" + std::to_string(i), "kind_s", std::string("b")));
  ch.indexAll(docs, UpdateMessage::COMMIT);

  auto res = httpRequest(port(), http::verb::post, "/collections/http_docs_multi/_search?format=docs",
      R"({"ops":{)"
      R"("qa":{"top_docs":{"query":{"match":{"kind_s":"a"}},"limit":-1,"batch_size":2,"get_number":true,"fields":["id"]}},)"
      R"("qb":{"top_docs":{"query":{"match":{"kind_s":"b"}},"limit":-1,"batch_size":2,"get_number":true,"fields":["id"]}})"
      R"(}})");
  ASSERT_EQ(200, res.result_int()) << res.body();

  std::map<std::string, std::set<std::string>> idsByOp;
  std::map<std::string, int64_t> foundByOp;
  std::string current;
  for (auto& line : splitLines(res.body())) {
    glz::generic_i64 root;
    ASSERT_FALSE(glz::read_json(root, line)) << line;
    if (root.contains("_header_")) {
      auto& h = root["_header_"];
      ASSERT_TRUE(h.contains("op")) << line;  // multi-op markers always name their op
      current = *h["op"].get_if<std::string>();
      if (h.contains("found")) foundByOp[current] = *h["found"].get_if<int64_t>();
    } else {
      ASSERT_FALSE(current.empty()) << "doc line before any marker: " << line;
      idsByOp[current].insert(*root["id"].get_if<std::string>());
    }
  }
  EXPECT_EQ(6u, idsByOp["qa"].size()) << res.body();
  EXPECT_EQ(4u, idsByOp["qb"].size()) << res.body();
  EXPECT_EQ(6, foundByOp["qa"]);
  EXPECT_EQ(4, foundByOp["qb"]);
}

// Warnings force a _header_ even without get_number: degraded execution is
// never silent, docs format included.
TEST_F(HttpApiTest, docsFormatWarningsForceHeader) {
  LuxirTest::clearCollection("http_docs_warn");
  CollectionHelper ch("http_docs_warn");
  ch.index(flatdoc("id", std::string("w1"), "tag_s", std::string("v")), UpdateMessage::COMMIT);

  // Quoted-with-slop on a non-TEXT field degrades to an exact match and warns.
  auto res = httpRequest(port(), http::verb::post, "/collections/http_docs_warn/_search?format=docs",
      R"({"query":{"simple_query":{"q":"\"v\"~2","fields":["tag_s"]}},"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(2u, lines.size()) << res.body();
  EXPECT_NE(std::string::npos, lines[0].find(R"({"_header_":{"warnings":)")) << res.body();
  EXPECT_NE(std::string::npos, lines[0].find("phrase_slop_inapplicable")) << res.body();
  EXPECT_EQ(R"({"id":"w1"})", lines[1]);
}

// Zero matches: pure mode returns an empty 200 body; with get_number the body
// is the single header line.
TEST_F(HttpApiTest, docsFormatZeroResults) {
  LuxirTest::clearCollection("http_docs_zero");
  CollectionHelper ch("http_docs_zero");
  ch.index(flatdoc("id", std::string("z1")), UpdateMessage::COMMIT);

  auto pure = httpRequest(port(), http::verb::post, "/collections/http_docs_zero/_search?format=docs",
      R"({"query":{"match":{"id":"nomatch"}},"fields":["id"]})");
  ASSERT_EQ(200, pure.result_int()) << pure.body();
  EXPECT_TRUE(pure.body().empty()) << pure.body();

  auto counted = httpRequest(port(), http::verb::post, "/collections/http_docs_zero/_search?format=docs",
      R"({"query":{"match":{"id":"nomatch"}},"get_number":true,"fields":["id"]})");
  ASSERT_EQ(200, counted.result_int()) << counted.body();
  EXPECT_EQ(R"({"_header_":{"found":0}})" "\n", counted.body());
}

// Keep-alive: two docs-format responses on one connection.
TEST_F(HttpApiTest, docsFormatKeepAliveReuse) {
  LuxirTest::clearCollection("http_docs_ka");
  CollectionHelper ch("http_docs_ka");
  ch.index(flatdoc("id", std::string("k1")), UpdateMessage::COMMIT);

  net::io_context cioc;
  beast::tcp_stream stream(cioc);
  tcp::resolver resolver(cioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port())));

  for (int round = 0; round < 2; round++) {
    http::request<http::string_body> req(http::verb::post,
                                         "/collections/http_docs_ka/_search?format=docs", 11);
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    req.keep_alive(true);
    req.body() = R"({"query":{"all":true},"fields":["id"]})";
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    ASSERT_EQ(200, res.result_int()) << "round " << round << ": " << res.body();
    EXPECT_EQ(R"({"id":"k1"})" "\n", res.body()) << "round " << round;
  }
  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}

// The flagship property: format=docs output (header included) pipes straight
// back into streaming /update - the _header_ record is a recognized no-op.
TEST_F(HttpApiTest, docsFormatRoundTripsIntoIngest) {
  LuxirTest::clearCollection("http_rt_src");
  LuxirTest::clearCollection("http_rt_dst");
  CollectionHelper src("http_rt_src");
  for (int i = 0; i < 7; i++) {
    auto commit = i == 6 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
    src.index(flatdoc("id", "rt" + std::to_string(i), "num_i", (int64_t)i), commit);
  }

  auto exported = httpRequest(port(), http::verb::post, "/collections/http_rt_src/_search?format=docs",
      R"({"query":{"all":true},"limit":-1,"get_number":true,"fields":["id","num_i"]})");
  ASSERT_EQ(200, exported.result_int()) << exported.body();
  ASSERT_EQ(8u, splitLines(exported.body()).size());  // header + 7 docs

  auto import = httpRequest(port(), http::verb::post, "/collections/http_rt_dst/_update?commit=true",
                            std::string(exported.body()), "application/x-ndjson");
  ASSERT_EQ(200, import.result_int()) << import.body();

  // Verify by re-exporting the destination: identical doc lines, same count.
  auto reexported = httpRequest(port(), http::verb::post, "/collections/http_rt_dst/_search?format=docs",
      R"({"query":{"all":true},"limit":-1,"get_number":true,"fields":["id","num_i"]})");
  ASSERT_EQ(200, reexported.result_int()) << reexported.body();
  auto a = splitLines(exported.body());
  auto b = splitLines(reexported.body());
  EXPECT_EQ(std::set<std::string>(a.begin(), a.end()), std::set<std::string>(b.begin(), b.end()));
}

// Flow control: a client that stops reading must not cause unbounded
// server-side buffering.  A private server with a tiny stream buffer forces
// the emitter to pause (observable via streamPauseCount) while the client
// withholds reads; draining the response resumes it and every doc arrives.
TEST_F(HttpApiTest, backpressurePausesEmitter) {
  LuxirTest::clearCollection("http_bp");
  CollectionHelper ch("http_bp");
  std::string pad(400, 'x');
  std::vector<Doc> docs;
  for (int i = 0; i < 2000; i++) {
    docs.push_back(flatdoc("id", "bp" + std::to_string(i), "pad_s", pad));
  }
  ch.indexAll(docs, UpdateMessage::COMMIT);

  HttpServer bpServer(*LuxirTest::luxirNode, 2, 0, /*streamBufferBytes=*/4096);
  bpServer.start();

  // All three dispatch lanes pause and drain: the default (0) parks and
  // resumes on the connection shard; 1 and -1 re-enqueue their resumes onto
  // the arena (HttpSearchRequest::resumeWhenDrained's arena-lane wrapper).
  for (std::string mp : {"0", "1", "-1"}) {
    net::io_context cioc;
    tcp::socket sock(cioc);
    sock.open(tcp::v4());
    // Small receive buffer (set before connect) so the kernel absorbs little
    // and the server's write queue backs up quickly.
    sock.set_option(net::socket_base::receive_buffer_size(8192));
    sock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"),
                               (unsigned short)bpServer.getPort()));

    int64_t pausesBefore = streamPauseCount.load();

    http::request<http::string_body> req(http::verb::post, "/collections/http_bp/_search", 11);
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    req.body() = R"({"query":{"all":true},"limit":-1,"batch_size":100,"max_parallel":)" +
        mp + R"(,"fields":["id","pad_s"]})";
    req.prepare_payload();
    http::write(sock, req);

    // Withhold reads until the connection backs up and the emitter parks.
    bool paused = false;
    for (int i = 0; i < 400 && !paused; i++) {
      paused = streamPauseCount.load() > pausesBefore;
      if (!paused) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_TRUE(paused) << "max_parallel=" << mp;

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(sock, buffer, res);
    EXPECT_EQ(200, res.result_int()) << "max_parallel=" << mp;

    // Every doc arrived once the client drained the stream.
    size_t count = 0;
    std::string_view body = res.body();
    for (size_t pos = 0; (pos = body.find(R"("id":"bp)", pos)) != std::string_view::npos; pos++) count++;
    EXPECT_EQ(2000u, count) << "max_parallel=" << mp;

    beast::error_code ec;
    sock.shutdown(tcp::socket::shutdown_both, ec);
  }
  bpServer.shutdown();
}

// An exception inside the batch emitter (here: an unknown output field) must
// still complete the request: the error is recorded on the final response and
// the stream ends, instead of stranding the completion protocol (which would
// leak the request and never answer the client).
TEST_F(HttpApiTest, emitterExceptionCompletesWithError) {
  helper.index(flatdoc("id", std::string("ee1"), "title_w", std::string("emitex")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", std::string("ee2"), "title_w", std::string("emitex")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", std::string("ee3"), "title_w", std::string("emitex")),
               UpdateMessage::COMMIT);
  // Three docs at batch_size 1 make the throwing batch a NON-final one (fresh
  // response arena), and "id" first puts its loader tasks in flight when the
  // unknown field's schema lookup throws - exercising the emitter's task-group
  // join and batch-arena cleanup, not just the error surface.
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"query":{"all":true},"batch_size":1,"fields":["id","nosuchfield"]})");
  EXPECT_EQ(200, res.result_int());
  EXPECT_NE(std::string::npos, res.body().find(R"("error":)")) << res.body();
  EXPECT_NE(std::string::npos, res.body().find("nosuchfield")) << res.body();
}

// A client that disconnects while the emitter is paused must not strand the
// request: the write failure wakes the parked emitter, whose next reply()
// observes CANCEL and completes the request.  A stranded request would hang
// shutdown() here (it drains in-flight work).
TEST_F(HttpApiTest, disconnectWhilePausedCancelsEmitter) {
  LuxirTest::clearCollection("http_bp2");
  CollectionHelper ch("http_bp2");
  std::string pad(400, 'x');
  std::vector<Doc> docs;
  for (int i = 0; i < 2000; i++) {
    docs.push_back(flatdoc("id", "bp" + std::to_string(i), "pad_s", pad));
  }
  ch.indexAll(docs, UpdateMessage::COMMIT);

  std::optional<HttpServer> bpServer;
  bpServer.emplace(*LuxirTest::luxirNode, 2, 0, /*streamBufferBytes=*/4096);
  bpServer->start();

  net::io_context cioc;
  tcp::socket sock(cioc);
  sock.open(tcp::v4());
  sock.set_option(net::socket_base::receive_buffer_size(8192));
  sock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"),
                             (unsigned short)bpServer->getPort()));

  int64_t pausesBefore = streamPauseCount.load();

  http::request<http::string_body> req(http::verb::post, "/collections/http_bp2/_search", 11);
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = R"({"query":{"all":true},"limit":-1,"batch_size":100,"fields":["id","pad_s"]})";
  req.prepare_payload();
  http::write(sock, req);

  bool paused = false;
  for (int i = 0; i < 400 && !paused; i++) {
    paused = streamPauseCount.load() > pausesBefore;
    if (!paused) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  ASSERT_TRUE(paused);

  // Hard-close without reading: linger(0) sends RST so the server's in-flight
  // write fails promptly instead of waiting out FIN semantics.
  beast::error_code ec;
  sock.set_option(net::socket_base::linger(true, 0), ec);
  sock.close(ec);

  bpServer->shutdown();  // hangs if the paused request was stranded
  bpServer.reset();
  SUCCEED();
}

TEST_F(HttpApiTest, ndjsonCachedWriterFailsCleanlyAfterCollectionDelete) {
  auto created = httpRequest(port(), http::verb::post, "/collections/_create",
                             R"({"name":"admin_stream_delete"})");
  ASSERT_EQ(200, created.result_int()) << created.body();

  net::io_context cioc;
  beast::tcp_stream stream(cioc);
  tcp::resolver resolver(cioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port())));
  stream.expires_after(std::chrono::seconds(10));

  std::string header =
      "POST /collections/admin_stream_delete/_update HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: application/x-ndjson\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  net::write(stream, net::buffer(header));
  writeRawHttpChunk(stream,
      R"({"_update_":{"request_id":"before-delete","return_ids":true}})" "\n"
      R"({"id":"before","title_w":"before delete"})" "\n"
      "{}\n");

  beast::flat_buffer buffer;
  http::response_parser<http::buffer_body> parser;
  beast::error_code ec;
  http::read_header(stream, buffer, parser, ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_EQ(200, parser.get().result_int());

  std::string pending;
  std::string err;
  std::string firstLine;
  ASSERT_TRUE(readNextBodyLine(stream, buffer, parser, pending, firstLine, err)) << err;
  EXPECT_NE(firstLine.find(R"("before")"), std::string::npos) << firstLine;

  auto deleted = httpRequest(port(), http::verb::post, "/collections/_delete",
                             R"({"name":"admin_stream_delete"})");
  ASSERT_EQ(200, deleted.result_int()) << deleted.body();

  writeRawHttpChunk(stream,
      R"({"id":"after","title_w":"after delete"})" "\n"
      "{}\n");

  std::string errorLine;
  ASSERT_TRUE(readNextBodyLine(stream, buffer, parser, pending, errorLine, err)) << err;
  EXPECT_NE(errorLine.find(R"("status":"error")"), std::string::npos) << errorLine;
  EXPECT_NE(errorLine.find("rejected"), std::string::npos) << errorLine;
  EXPECT_NE(errorLine.find("docs_indexed_so_far"), std::string::npos) << errorLine;

  stream.socket().shutdown(tcp::socket::shutdown_both, ec);
}


// ---- error contract ---------------------------------------------------------

TEST_F(HttpApiTest, errorBodyIsStructuredAndEchoesRequestId) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search?request_id=r1",
                         "{not json");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"({"request_id":"r1","error":{"kind":"invalid_request","code":"invalid_json","message":)"),
            std::string::npos) << res.body();
}

TEST_F(HttpApiTest, wrongMethodIs405OnEveryKnownRoute) {
  struct Case { http::verb method; const char* target; const char* allow; };
  const Case cases[] = {
      {http::verb::get, "/collections/main/_update", "POST"},
      {http::verb::delete_, "/health", "GET"},
      {http::verb::put, "/collections/main/_search", "GET, POST"},
      {http::verb::delete_, "/collections/main/_schema", "GET, POST"},
      {http::verb::post, "/collections/main/_stats", "GET"},
      {http::verb::get, "/collections/_delete", "POST"},
  };
  for (const auto& c : cases) {
    auto res = httpRequest(port(), c.method, c.target);
    EXPECT_EQ(405, res.result_int()) << c.target << " " << res.body();
    EXPECT_EQ(c.allow, res[http::field::allow]) << c.target;
    EXPECT_NE(res.body().find(R"("error":{"kind":"invalid_request","code":"method_not_allowed")"),
              std::string::npos) << res.body();
  }
  auto unknown = httpRequest(port(), http::verb::get, "/nope");
  EXPECT_EQ(404, unknown.result_int()) << unknown.body();
  EXPECT_NE(unknown.body().find(R"("error":{"kind":"not_found","code":"not_found")"),
            std::string::npos) << unknown.body();
}

TEST_F(HttpApiTest, collectionResolutionStatusesAreUniform) {
  // A name that cannot denote a collection is the request's fault on every route.
  for (const char* target : {"/collections/Bad-Name/_schema", "/collections/Bad-Name/_stats"}) {
    auto res = httpRequest(port(), http::verb::get, target);
    EXPECT_EQ(400, res.result_int()) << target << " " << res.body();
    EXPECT_NE(res.body().find(R"("error":{"kind":"invalid_request","code":"invalid_collection_name")"),
              std::string::npos) << res.body();
  }
  auto update = httpRequest(port(), http::verb::post, "/collections/Bad-Name/_update",
                            R"({"docs":[{"id":"x"}]})");
  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find(R"("code":"invalid_collection_name")"), std::string::npos)
      << update.body();
  // A search resolves its collection after submission, so the same failure is
  // the in-band error line.
  auto search = httpRequest(port(), http::verb::get, "/collections/Bad-Name/_search");
  ASSERT_EQ(200, search.result_int()) << search.body();
  EXPECT_NE(search.body().find(R"({"error":{"kind":"invalid_request","code":"invalid_collection_name")"),
            std::string::npos) << search.body();

  // A well-formed name nothing answers to is not found.
  auto missing = httpRequest(port(), http::verb::get, "/collections/http_no_such_coll/_schema");
  EXPECT_EQ(404, missing.result_int()) << missing.body();
  EXPECT_NE(missing.body().find(R"("error":{"kind":"not_found","code":"collection_not_found")"),
            std::string::npos) << missing.body();
}

TEST_F(HttpApiTest, searchFailureAfterSubmissionIsAnErrorLine) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"request_id":"s1","max_parallel":5,"ops":{"q":{"top_docs":{"query":"title_w:dune"}}}})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(1u, lines.size()) << res.body();
  EXPECT_NE(lines[0].find(R"({"request_id":"s1","error":{"kind":"invalid_request","code":"invalid_request","message":"max_parallel)"),
            std::string::npos) << res.body();
  EXPECT_EQ(lines[0].find(R"("docs")"), std::string::npos) << res.body();

  // format=docs has no envelope: before any output the same failure is a
  // plain HTTP error with the same body.
  auto docs = httpRequest(port(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"request_id":"s2","max_parallel":5,"ops":{"q":{"top_docs":{"query":"title_w:dune"}}}})");
  EXPECT_EQ(400, docs.result_int()) << docs.body();
  EXPECT_NE(docs.body().find(R"({"request_id":"s2","error":{"kind":"invalid_request")"),
            std::string::npos) << docs.body();
}

TEST_F(HttpApiTest, envelopeEchoesRequestId) {
  helper.index(flatdoc("id", "rid1", "title_w", "requestid token"), UpdateMessage::COMMIT);
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_search?request_id=q9",
                         R"({"query":"title_w:requestid","fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"({"request_id":"q9",)"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("rid1")"), std::string::npos) << res.body();

  // Docs format: the id rides on the first _header_ even without get_number.
  auto docs = httpRequest(port(), http::verb::post,
                          "/collections/main/_search?format=docs&request_id=q10",
                          R"({"query":"title_w:requestid","fields":["id"]})");
  ASSERT_EQ(200, docs.result_int()) << docs.body();
  auto lines = splitLines(docs.body());
  ASSERT_EQ(2u, lines.size()) << docs.body();
  EXPECT_EQ(R"({"_header_":{"request_id":"q10"}})", lines[0]) << docs.body();
}

TEST_F(HttpApiTest, updateErrorsAreStructured) {
  // Request-level: rejected before any document; the message is the reason,
  // not a stack trace.
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"request_id":"u1","docs":[{"id":"u1"}],"drop_unmapped":true})");
  ASSERT_EQ(200, bad.result_int()) << bad.body();
  EXPECT_NE(bad.body().find(R"("request_id":"u1")"), std::string::npos) << bad.body();
  EXPECT_NE(bad.body().find(R"("status":"error")"), std::string::npos) << bad.body();
  EXPECT_NE(bad.body().find(R"("error":{"kind":"invalid_request","code":"invalid_request","message":"drop_unmapped requires a non-empty field_map"})"),
            std::string::npos) << bad.body();
  EXPECT_EQ(bad.body().find("Stack trace"), std::string::npos) << bad.body();

  // Per-document: the failed doc names itself and carries the same error object.
  auto partial = httpRequest(port(), http::verb::post, "/collections/main/_update",
      R"({"docs":[{"id":"ok1","title_w":"fine"},{"id":"bad1","bogus":"x"}]})");
  ASSERT_EQ(200, partial.result_int()) << partial.body();
  EXPECT_NE(partial.body().find(R"("status":"partial")"), std::string::npos) << partial.body();
  EXPECT_NE(partial.body().find(R"("errors":[{"id":"bad1","index":1,"error":{"kind":"invalid_request","code":"unknown_field")"),
            std::string::npos) << partial.body();
  EXPECT_NE(partial.body().find(R"("total_errors":1)"), std::string::npos) << partial.body();
  EXPECT_EQ(partial.body().find("error_message"), std::string::npos) << partial.body();
}

TEST_F(HttpApiTest, ndjsonRequestLevelFailureEndsStreamWithStructuredError) {
  std::string body =
      R"({"_update_":{"request_id":"g1"}})" "\n"
      R"({"id":"ng1","title_w":"ndjson group token"})" "\n"
      R"({"_end_":{"commit":{}}})" "\n"
      R"({"_update_":{"request_id":"g2","drop_unmapped":true}})" "\n"
      R"({"id":"ng2","title_w":"never indexed"})" "\n"
      "{}\n";
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
                         std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(2u, lines.size()) << res.body();
  EXPECT_NE(lines[0].find(R"("request_id":"g1")"), std::string::npos) << res.body();
  EXPECT_NE(lines[0].find(R"("status":"ok")"), std::string::npos) << res.body();
  EXPECT_NE(lines[1].find(R"("request_id":"g2")"), std::string::npos) << res.body();
  EXPECT_NE(lines[1].find(R"("status":"error")"), std::string::npos) << res.body();
  EXPECT_NE(lines[1].find(R"x("error":{"kind":"invalid_request","code":"invalid_request","message":"drop_unmapped requires a non-empty field_map (docs_indexed_so_far=1)"})x"),
            std::string::npos) << res.body();
}


TEST_F(HttpApiTest, deleteInvalidNameIs400) {
  auto res = httpRequest(port(), http::verb::post, "/collections/_delete", R"({"name":"Bad-Name"})");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("error":{"kind":"invalid_request","code":"invalid_collection_name")"),
            std::string::npos) << res.body();
}

TEST_F(HttpApiTest, ndjsonTotalErrorsCountsPastRetention) {
  // The stream keeps the first 100 document errors of a group; total_errors
  // still counts every one.
  std::string body = R"({"_update_":{"request_id":"many"}})" "\n";
  for (int i = 0; i < 101; i++) {
    body += R"({"id":"bad)" + std::to_string(i) + R"(","bogus":"x"})" "\n";
  }
  body += "{}\n";
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_update",
                         std::move(body), "application/x-ndjson");
  ASSERT_EQ(200, res.result_int()) << res.body();
  auto lines = splitLines(res.body());
  ASSERT_EQ(1u, lines.size()) << res.body();
  EXPECT_NE(lines[0].find(R"("status":"error")"), std::string::npos) << res.body();
  EXPECT_NE(lines[0].find(R"("total_errors":101)"), std::string::npos) << res.body();
  EXPECT_NE(lines[0].find(R"("id":"bad99")"), std::string::npos) << res.body();
  EXPECT_EQ(lines[0].find(R"("id":"bad100")"), std::string::npos) << res.body();
  EXPECT_EQ(lines[0].find(R"("error":{"kind":"invalid_request","code":"invalid_request")"),
            std::string::npos) << "no request-level error: " << res.body();
}

// An oversized body is never read, but the route and method are known from
// the headers: a wrong method or an unknown path outranks the size.
TEST_F(HttpApiTest, oversizedBodyStillAnswersRouteAndMethodFirst) {
  auto send = [&](std::string_view requestLine) {
    net::io_context cioc;
    beast::tcp_stream stream(cioc);
    tcp::resolver resolver(cioc);
    stream.connect(resolver.resolve("127.0.0.1", std::to_string(port())));
    stream.expires_after(std::chrono::seconds(10));
    std::string header = std::string(requestLine) +
        " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
        "Content-Length: 99000000\r\nConnection: close\r\n\r\n";
    net::write(stream, net::buffer(header));
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    beast::error_code ec;
    http::read(stream, buffer, res, ec);
    return res;
  };
  auto wrongMethod = send("PUT /collections/main/_update?request_id=big1");
  EXPECT_EQ(405, wrongMethod.result_int()) << wrongMethod.body();
  EXPECT_EQ("POST", wrongMethod[http::field::allow]);
  EXPECT_NE(wrongMethod.body().find(R"({"request_id":"big1","error":{"kind":"invalid_request","code":"method_not_allowed")"),
            std::string::npos) << wrongMethod.body();
  auto unknown = send("POST /nope");
  EXPECT_EQ(404, unknown.result_int()) << unknown.body();
  auto tooLarge = send("POST /collections/main/_update?request_id=big2");
  EXPECT_EQ(413, tooLarge.result_int()) << tooLarge.body();
  EXPECT_NE(tooLarge.body().find(R"({"request_id":"big2","error":{"kind":"resource_exhausted","code":"request_too_large")"),
            std::string::npos) << tooLarge.body();
}

} // namespace luxir::test
