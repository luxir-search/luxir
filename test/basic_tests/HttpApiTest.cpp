#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/HttpReq.h"
#include "test/SchemaBuilder.h"
#include "solux/api/build.h"
#include "solux/reader/Postings.h"
#include "solux/schema/Schema.h"
#include "solux/server/HttpServer.h"

namespace solux::test {

// End-to-end coverage of the Phase 0 HTTP/JSON API (POST /collections/{c}/query
// + GET /health), exercising the async Beast plumbing against the same in-process
// node the rest of the suite uses.  No mocks; RAMDir.
class HttpApiTest : public SoluxTest {
protected:
  std::optional<HttpServer> server;
  CollectionHelper helper{"main"};

  void SetUp() override {
    server.emplace(*SoluxTest::soluxNode, 2 /*threads*/, 0 /*OS-assigned port*/);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
  }

  int port() { return server->getPort(); }

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

TEST_F(HttpApiTest, unknownRouteIs404) {
  auto res = httpRequest(port(), http::verb::get, "/nope");
  EXPECT_EQ(404, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos);
}

TEST_F(HttpApiTest, malformedJsonIs400) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_query", "{not json");
  EXPECT_EQ(400, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos);
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

  auto response = httpRequest(port(), http::verb::post, "/collections/main/_query",
      R"({"ops":{"cats":{"field_facet":{"field":"http_facet_s","limit":-1,"missing":true}}}})");
  ASSERT_EQ(200, response.result_int()) << response.body();
  EXPECT_EQ(
      R"({"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}],"missing":1}}})" "\n",
      response.body());
}

TEST_F(HttpApiTest, multiCollectionRoutingIsIsolated) {
  SoluxTest::clearCollection("http_route_a");
  SoluxTest::clearCollection("http_route_b");

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

  SoluxTest::clearCollection("http_route_a");
  SoluxTest::clearCollection("http_route_b");
}

TEST_F(HttpApiTest, autoCreateCollectionDefaultOn) {
  auto update = httpRequest(port(), http::verb::post, "/collections/http_auto_create_on/_update",
      R"({"docs":[{"id":"auto-on","title_w":"autocreateon token"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  std::shared_ptr<Collection> created;
  EXPECT_NO_THROW(created = SoluxTest::soluxNode->getCollection("http_auto_create_on"));
  ASSERT_NE(nullptr, created);

  HttpReq hreq(port());
  hreq.collection("http_auto_create_on").matchQuery("title_w", "autocreateon")
      .fields({"id"}).withStats().execute();
  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"auto-on"}), idsOf(hreq.getDocs())) << hreq.rawResponse();

  SoluxTest::clearCollection("http_auto_create_on");
}

TEST_F(HttpApiTest, autoCreateCollectionCanBeDisabled) {
  SoluxConfig config;
  config.ingest.auto_create_collection = false;
  SoluxNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto update = httpRequest(localServer.getPort(), http::verb::post,
      "/collections/http_auto_create_off/_update",
      R"({"docs":[{"id":"auto-off","title_w":"autocreateoff token"}],"commit":{}})");
  localServer.shutdown();

  EXPECT_EQ(400, update.result_int()) << update.body();
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
  EXPECT_THROW(SoluxTest::soluxNode->getCollection(autoOnName), CollectionResolutionError);

  SoluxConfig config;
  config.ingest.auto_create_collection = false;
  SoluxNode node(config);
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
  EXPECT_THROW(SoluxTest::soluxNode->getCollection("_reserved"), CollectionResolutionError);
}

TEST_F(HttpApiTest, unsafeCollectionNamesAreRejectedBeforeCreate) {
  auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path base = std::filesystem::temp_directory_path() / ("solux_unsafe_names_" + stamp);
  std::filesystem::path absolute = std::filesystem::temp_directory_path() / ("solux_abs_collection_" + stamp);
  std::filesystem::remove_all(base);
  std::filesystem::remove_all(absolute);

  SoluxConfig config;
  config.store.backend = "fs";
  config.store.data_dir = base.string();
  SoluxNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto expectRejected = [&](std::string target, std::string_view message) {
    auto update = httpRequest(localServer.getPort(), http::verb::post, target,
        R"({"docs":[{"id":"bad-name","title_w":"badname token"}],"commit":{}})");
    EXPECT_EQ(400, update.result_int()) << target << " " << update.body();
    EXPECT_NE(update.body().find(message), std::string::npos) << target << " " << update.body();
  };

  expectRejected("/collections/_reserved/_update", "reserved");
  expectRejected("/collections/unsafe/slash/_update", "single path component");
  expectRejected("/collections/../_update", "reserved");
  expectRejected("/collections/" + absolute.string() + "/_update", "single path component");
  expectRejected("/collections//_update", "empty");

  localServer.shutdown();

  EXPECT_FALSE(std::filesystem::exists(base / "c" / "unsafe"));
  EXPECT_FALSE(std::filesystem::exists(absolute));
  std::filesystem::remove_all(base);
}

TEST_F(HttpApiTest, corruptCollectionTombstonedAtStartup) {
  auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path base = std::filesystem::temp_directory_path() / ("solux_corrupt_col_" + stamp);
  std::filesystem::remove_all(base);

  SoluxConfig config;
  config.store.backend = "fs";
  config.store.data_dir = base.string();

  {
    SoluxNode node(config);
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
  SoluxNode node(config);
  HttpServer localServer(node, 2, 0);
  localServer.start();

  auto query = httpRequest(localServer.getPort(), http::verb::post, "/collections/good/_query",
      R"({"query":{"match":{"title_w":"good"}},"fields":["id"]})");
  EXPECT_EQ(200, query.result_int()) << query.body();
  EXPECT_NE(query.body().find(R"("g1")"), std::string::npos) << query.body();

  auto badQuery = httpRequest(localServer.getPort(), http::verb::post, "/collections/bad/_query",
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
  auto res = httpRequest(port(), http::verb::post, "/collections/main/_query",
      R"({"query":{"simple_query":{"q":"blade | man","fields":["title_w"]}},"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("s1")"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("s2")"), std::string::npos) << res.body();

  // declared degradations are visible on the wire (clamp-and-declare)
  auto warned = httpRequest(port(), http::verb::post, "/collections/main/_query",
      R"({"query":{"simple_query":{"q":"blade~9","fields":["title_w"]}},"fields":["id"]})");
  ASSERT_EQ(200, warned.result_int()) << warned.body();
  EXPECT_NE(warned.body().find(R"("warnings")"), std::string::npos) << warned.body();
  EXPECT_NE(warned.body().find(R"("fuzzy_clamped")"), std::string::npos) << warned.body();

  // never-fails: garbage user input is still a 200 with results, not an error
  auto garbage = httpRequest(port(), http::verb::post, "/collections/main/_query",
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
      "/collections/main/_query",
      R"({"query":{"geo_distance":{"field":"geo","lat":40.7128,"lon":-74.0060,"radius_meters":1000}},"fields":["id"]})");
  ASSERT_EQ(200, result.result_int()) << result.body();
  EXPECT_NE(result.body().find(R"("ny")"), std::string::npos) << result.body();
  EXPECT_EQ(result.body().find(R"("la")"), std::string::npos) << result.body();
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
  SoluxConfig config;
  config.ingest.max_request_body = 1024;
  config.ingest.max_record = 4096;
  SoluxNode node(config);
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
  EXPECT_NE(update.body().find("_update_ inline request exceeds ingest.max-request-body"),
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
  SoluxNode node;
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

TEST_F(HttpApiTest, ndjsonAllOrNoneStreamOverCapIs400) {
  SoluxConfig config;
  config.ingest.max_request_body = 1024;
  config.ingest.max_record = 2048;
  SoluxNode node(config);
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

  EXPECT_EQ(400, update.result_int()) << update.body();
  EXPECT_NE(update.body().find("all_or_none NDJSON group exceeds ingest.max-request-body"),
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

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_query",
      R"({"query":{"match":{"status_s":"active"}},"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();

  HttpReq full(port());
  full.matchQuery("status_s", "active").fields({"id"}).execute();
  ASSERT_EQ(200, full.status());
  EXPECT_EQ(res.body(), full.rawResponse());
  EXPECT_EQ(2u, idsOf(full.getDocs()).size());

  // an unknown root key is rejected with a client-facing error
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/_query",
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
                          "/collections/main/_query?explain=request", body);
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
  auto direct = httpRequest(port(), http::verb::post, "/collections/main/_query", body);
  auto viaEcho = httpRequest(port(), http::verb::post, "/collections/main/_query", canonical);
  ASSERT_EQ(200, viaEcho.result_int()) << viaEcho.body();
  EXPECT_EQ(direct.body(), viaEcho.body());

  // Fixpoint: echoing the echo is byte-identical.
  auto echo2 = httpRequest(port(), http::verb::post,
                           "/collections/main/_query?explain=request", canonical);
  ASSERT_EQ(200, echo2.result_int());
  EXPECT_EQ(canonical, echo2.body());
}

// URL-parameter policy: unknown parameters are accepted and ignored (the URL is
// an open channel - correlation ids, middleware); recognized keys enforce values.
TEST_F(HttpApiTest, urlParamPolicy) {
  // bad value on a RECOGNIZED key is an author error
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/_query?explain=foo",
                         R"({"limit":1})");
  EXPECT_EQ(400, bad.result_int());
  EXPECT_NE(bad.body().find(R"("error")"), std::string::npos) << bad.body();

  // unknown params (e.g. a correlation id) pass through; the query executes
  auto unknown = httpRequest(port(), http::verb::post,
                             "/collections/main/_query?trace_id=abc-123&_=17",
                             R"({"limit":1})");
  EXPECT_EQ(200, unknown.result_int()) << unknown.body();

  // unknown params compose with explain (last-wins on repeats)
  auto both = httpRequest(port(), http::verb::post,
                          "/collections/main/_query?trace_id=x&explain=request",
                          R"({"limit":1})");
  EXPECT_EQ(200, both.result_int()) << both.body();
  EXPECT_NE(both.body().find(R"("ops")"), std::string::npos) << both.body();

  auto malformed = httpRequest(port(), http::verb::post,
                               "/collections/main/_query?explain=request", "{not json");
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

  auto res = httpRequest(port(), http::verb::post, "/collections/main/_query",
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
  auto q = httpRequest(port(), http::verb::post, "/collections/main/_query",
      R"({"query":{"match":{"title":"dune"}},"fields":["id"]})");
  ASSERT_EQ(200, q.result_int()) << q.body();
  EXPECT_NE(q.body().find(R"("id":"1")"), std::string::npos) << q.body();
  EXPECT_EQ(q.body().find(R"("id":"2")"), std::string::npos) << q.body();

  // Range index installed via the schema answers a range query.
  auto range = httpRequest(port(), http::verb::post, "/collections/main/_query",
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

  http::request<http::string_body> req(http::verb::post, "/collections/main/_query", 11);
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

} // namespace solux::test
