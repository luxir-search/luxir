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

TEST_F(HttpApiTest, updateIndexesAndQueryRoundTrip) {
  auto update = httpRequest(port(), http::verb::post, "/collections/main/update",
      R"({"docs":[{"id":"u1","title_w":"hello world","title_s":"Hello"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  EXPECT_NE(update.body().find(R"("update_version")"), std::string::npos) << update.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "hello").fields({"id"}).execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"u1"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, updateDeleteIds) {
  auto index = httpRequest(port(), http::verb::post, "/collections/main/update",
      R"({"docs":[{"id":"u1","title_w":"delete token"},{"id":"u2","title_w":"delete token"}],"commit":{}})");
  ASSERT_EQ(200, index.result_int()) << index.body();

  auto del = httpRequest(port(), http::verb::post, "/collections/main/update",
      R"({"delete_ids":["u1"],"commit":{}})");
  ASSERT_EQ(200, del.result_int()) << del.body();

  HttpReq hreq(port());
  hreq.collection("main").matchQuery("title_w", "token").fields({"id"}).execute();

  ASSERT_EQ(200, hreq.status()) << hreq.rawResponse();
  EXPECT_EQ(std::set<std::string>({"u2"}), idsOf(hreq.getDocs())) << hreq.rawResponse();
}

TEST_F(HttpApiTest, malformedUpdateJsonIs400) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/update", "{not json");
  EXPECT_EQ(400, res.result_int());
  EXPECT_NE(res.body().find(R"("error")"), std::string::npos) << res.body();
}

TEST_F(HttpApiTest, updateResponseUsesSnakeCase) {
  auto res = httpRequest(port(), http::verb::post, "/collections/main/update",
      R"({"request_id":"req-1","docs":[{"id":"shape1","title_w":"shape"}],"commit":{}})");
  ASSERT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(res.body().find(R"("update_version")"), std::string::npos) << res.body();
  EXPECT_NE(res.body().find(R"("request_id":"req-1")"), std::string::npos) << res.body();
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
                          "/collections/main/query?explain=request", body);
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
  auto direct = httpRequest(port(), http::verb::post, "/collections/main/query", body);
  auto viaEcho = httpRequest(port(), http::verb::post, "/collections/main/query", canonical);
  ASSERT_EQ(200, viaEcho.result_int()) << viaEcho.body();
  EXPECT_EQ(direct.body(), viaEcho.body());

  // Fixpoint: echoing the echo is byte-identical.
  auto echo2 = httpRequest(port(), http::verb::post,
                           "/collections/main/query?explain=request", canonical);
  ASSERT_EQ(200, echo2.result_int());
  EXPECT_EQ(canonical, echo2.body());
}

// URL-parameter policy: unknown parameters are accepted and ignored (the URL is
// an open channel - correlation ids, middleware); recognized keys enforce values.
TEST_F(HttpApiTest, urlParamPolicy) {
  // bad value on a RECOGNIZED key is an author error
  auto bad = httpRequest(port(), http::verb::post, "/collections/main/query?explain=foo",
                         R"({"limit":1})");
  EXPECT_EQ(400, bad.result_int());
  EXPECT_NE(bad.body().find(R"("error")"), std::string::npos) << bad.body();

  // unknown params (e.g. a correlation id) pass through; the query executes
  auto unknown = httpRequest(port(), http::verb::post,
                             "/collections/main/query?trace_id=abc-123&_=17",
                             R"({"limit":1})");
  EXPECT_EQ(200, unknown.result_int()) << unknown.body();

  // unknown params compose with explain (last-wins on repeats)
  auto both = httpRequest(port(), http::verb::post,
                          "/collections/main/query?trace_id=x&explain=request",
                          R"({"limit":1})");
  EXPECT_EQ(200, both.result_int()) << both.body();
  EXPECT_NE(both.body().find(R"("ops")"), std::string::npos) << both.body();

  auto malformed = httpRequest(port(), http::verb::post,
                               "/collections/main/query?explain=request", "{not json");
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
