#include <array>
#include <optional>
#include <set>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/HttpReq.h"
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
    helper.clear();
    server.emplace(*SoluxTest::soluxNode, 2 /*threads*/, 0 /*OS-assigned port*/);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
    helper.clear();
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
  auto res = httpRequest(port(), http::verb::post, "/collections/main/query", "{not json");
  EXPECT_EQ(400, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos);
}

// HTTP results match the in-process engine for the same query, and a doc missing
// a requested field renders that field as JSON null.
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

  // b2 lacks year_i -> rendered as null.
  EXPECT_NE(hreq.rawResponse().find(R"("year_i":null)"), std::string::npos)
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

  auto res = httpRequest(port(), http::verb::post, "/collections/main/query",
      R"({"query":{"match":{"status_s":"active"}},"fields":["id"]})");
  ASSERT_EQ(200, res.result_int()) << res.body();

  HttpReq full(port());
  full.matchQuery("status_s", "active").fields({"id"}).execute();
  ASSERT_EQ(200, full.status());
  EXPECT_EQ(res.body(), full.rawResponse());
  EXPECT_EQ(2u, idsOf(full.getDocs()).size());

  // an unknown root key is rejected with a client-facing error
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/query",
      R"({"query":{"match":{"status_s":"active"}},"limt":10})");
  EXPECT_EQ(400, bad.result_int());
  EXPECT_NE(bad.body().find(R"("error")"), std::string::npos);
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

// A multi-valued field present on one doc and absent on another: the absent slot
// renders as null (not []), and the present one as an array.
TEST_F(HttpApiTest, multiValuedMissingIsNull) {
  helper.indexAll(std::array{
    flatdoc("id", std::string("m1"), "title_w", std::string("multi"),
            "tags_ss", std::vector<std::string>{"x", "y"}),
    flatdoc("id", std::string("m2"), "title_w", std::string("multi")),  // no tags_ss
  }, UpdateMessage::COMMIT);

  HttpReq hreq(port());
  hreq.matchQuery("title_w", "multi").fields({"id", "tags_ss"}).execute();
  ASSERT_EQ(200, hreq.status());

  EXPECT_NE(hreq.rawResponse().find(R"("tags_ss":null)"), std::string::npos) << hreq.rawResponse();
  EXPECT_NE(hreq.rawResponse().find(R"("tags_ss":[)"), std::string::npos) << hreq.rawResponse();
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

  http::request<http::string_body> req(http::verb::post, "/collections/main/query", 11);
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
