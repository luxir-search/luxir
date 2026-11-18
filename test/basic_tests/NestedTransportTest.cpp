// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <sstream>

#include "test/GrpcLuxirTest.h"
#include "test/GrpcClient.h"
#include "test/HttpReq.h"
#include "test/NestedTestData.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/JsonResponse.h"

namespace luxir::test {
namespace {

class NestedTransportTest : public GrpcLuxirTest {
protected:
  CollectionHelper helper;

  void grpcQueries() {
    auto req = localReq(luxirNode->getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), nested_test::QUERY, req->mr));
    grpc::ClientContext context;
    HppClientReaderWriter<api::SearchRequest, api::SearchResponse> stream(getChannel().get(), rpc::Search, &context);
    ASSERT_TRUE(stream.Write(req->proto)); ASSERT_TRUE(stream.WritesDone());
    Reply<api::SearchResponse> response;
    nested_test::Counts counts;
    while (stream.Read(&response)) nested_test::capture(response.msg, counts);
    ASSERT_TRUE(stream.Finish().ok());
    EXPECT_EQ(nested_test::INITIAL, counts);
    EXPECT_EQ(nested_test::local(*luxirNode), counts);
  }

  void httpQueries(int port) {
    auto response = httpRequest(port, http::verb::post, "/collections/main/_search", std::string(nested_test::QUERY));
    ASSERT_EQ(200, response.result_int()) << response.body();
    std::istringstream lines(response.body());
    nested_test::Counts counts;
    for (std::string line; std::getline(lines, line);) {
      glz::generic_i64 parsed;
      ASSERT_FALSE(glz::read_json(parsed, line)) << line;
      ASSERT_FALSE(parsed.contains("error")) << line;
      auto capture = [&](auto&& self, const glz::generic_i64& value, const std::string& path) -> void {
        if (value.contains("found")) counts[path] = value["found"].get<int64_t>();
        if ((path == "rows" || path == "parts") && value.contains("docs")) {
          for (const auto& row : value["docs"].get_array()) {
            EXPECT_FALSE(row.contains("items.color")); EXPECT_FALSE(row.contains("items.parts.material"));
          }
        }
        if ((path == "notes" || path == "part_notes") && value.contains("docs")) {
          for (const auto& row : value["docs"].get_array()) {
            EXPECT_EQ(path == "notes" ? "red child" : "steel part", row["note_w"].get_string());
          }
        }
        if (value.contains("ops")) for (const auto& [name, child] : value["ops"].get_object()) self(self, child, path + "." + name);
      };
      for (const auto& [name, value] : parsed["ops"].get_object()) capture(capture, value, name);
    }
    EXPECT_EQ(nested_test::INITIAL, counts);
    EXPECT_EQ(nested_test::local(*luxirNode), counts);
    for (auto key : {"to_children", "to_parent"}) {
      auto body = std::string(R"({"ops":{"q":{"domain":{")") + key + R"(":"items"},"top_docs":{}}}})";
      auto bad = httpRequest(port, http::verb::post, "/collections/main/_search", body);
      EXPECT_EQ(400, bad.result_int()) << bad.body();
      EXPECT_NE(std::string::npos, bad.body().find("unknown_key")) << bad.body();
      EXPECT_NE(std::string::npos, bad.body().find(key)) << bad.body();
    }
  }
};

TEST_F(NestedTransportTest, nestedAndAncestorSelectionsTravelThroughBothTransports) {
  nested_test::setSchema(helper.collection());
  CollectionHelper::UpdateBuilder input;
  for (auto doc : nested_test::DOCS) nested_test::add(input, doc);
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto expected = nested_test::retrieval(*luxirNode);
  auto capture = [&](std::string_view json) {
    glz::generic_i64 parsed;
    EXPECT_FALSE(glz::read_json(parsed, json)) << json;
    auto& row = parsed["ops"]["q"]["docs"][0];
    std::vector<std::string> actual{row["id"].get_string() + ':' + row["items"][0]["color"].get_string()
        + ':' + row["items"][0]["parts"][0]["material"].get_string()};
    for (const auto& owner : parsed["ops"]["p"]["docs"].get_array()) actual.push_back(owner["/id"].get_string() + ':' + owner["../color"].get_string());
    std::sort(actual.begin(), actual.end());
    return actual;
  };
  HttpServer server(*luxirNode, 2, 0); server.start();
  auto stop = scope_guard([&] { server.shutdown(); });
  for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
    auto req = localReq(luxirNode->getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), nested_test::RETRIEVAL_QUERY, req->mr));
    for (const auto& [name, entry] : req->rawRequest().ops) {
      const_cast<api::TopDocs&>(std::get<api::TopDocs>(entry->kind)).document_format = format;
    }
    grpc::ClientContext context;
    HppClientReaderWriter<api::SearchRequest, api::SearchResponse> stream(getChannel().get(), rpc::Search, &context);
    ASSERT_TRUE(stream.Write(req->proto)); ASSERT_TRUE(stream.WritesDone());
    Reply<api::SearchResponse> reply;
    ASSERT_TRUE(stream.Read(&reply));
    EXPECT_EQ(expected, capture(renderSearchResponseBody(reply.msg)));
    EXPECT_FALSE(stream.Read(&reply)); ASSERT_TRUE(stream.Finish().ok());
    std::string body;
    ASSERT_TRUE(api::write_json(req->proto, body));
    auto response = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search", body);
    ASSERT_EQ(200, response.result_int()) << response.body();
    EXPECT_EQ(expected, capture(response.body()));
  }
  auto docs = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search?format=docs",
      R"({"ops":{"q":{"top_docs":{"query":"id:b","fields":["items"]}}}})");
  ASSERT_EQ(200, docs.result_int()) << docs.body();
  std::istringstream lines(docs.body());
  int rows = 0;
  for (std::string line; std::getline(lines, line);) {
    glz::generic_i64 parsed;
    ASSERT_FALSE(glz::read_json(parsed, line));
    if (!parsed.contains("items")) continue;
    EXPECT_EQ("steel", parsed["items"][0]["parts"][0]["material"].get_string());
    rows++;
  }
  EXPECT_EQ(1, rows);
}

TEST_F(NestedTransportTest, rowOperationsAndClauseNamesTravelThroughBothTransports) {
  nested_test::setSchema(helper.collection());
  CollectionHelper::UpdateBuilder input;
  for (auto doc : nested_test::DOCS) nested_test::add(input, doc);
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto expected = nested_test::rowOperations(*luxirNode);
  HttpServer server(*luxirNode, 2, 0); server.start();
  auto stop = scope_guard([&] { server.shutdown(); });
  for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
    auto req = localReq(luxirNode->getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), nested_test::ROW_QUERY, req->mr));
    auto& top = const_cast<api::TopDocs&>(std::get<api::TopDocs>(req->rawRequest().ops.at("q")->kind));
    top.document_format = format;
    for (const auto& [key, row] : top.row_ops) {
      const_cast<api::TopDocs&>(std::get<api::TopDocs>(row->kind)).document_format = format;
    }
    grpc::ClientContext context;
    HppClientReaderWriter<api::SearchRequest, api::SearchResponse> stream(getChannel().get(), rpc::Search, &context);
    ASSERT_TRUE(stream.Write(req->proto)); ASSERT_TRUE(stream.WritesDone());
    Reply<api::SearchResponse> reply;
    ASSERT_TRUE(stream.Read(&reply)); EXPECT_EQ(expected, nested_test::captureRows(reply.msg));
    EXPECT_FALSE(stream.Read(&reply)); ASSERT_TRUE(stream.Finish().ok());
    std::string body; ASSERT_TRUE(api::write_json(req->proto, body));
    auto response = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search", body);
    ASSERT_EQ(200, response.result_int()) << response.body();
    glz::generic_i64 json; ASSERT_FALSE(glz::read_json(json, response.body()));
    EXPECT_EQ("red", json["ops"]["q"]["docs"][0]["matched"]["docs"][0]["color"].get_string());
  }
  auto docs = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search?format=docs", std::string(nested_test::ROW_QUERY));
  ASSERT_EQ(200, docs.result_int()) << docs.body();
  EXPECT_NE(std::string::npos, docs.body().find("\"matched\""));
  auto shorthand = httpRequest(server.getPort(), http::verb::post, "/collections/main/_search",
      R"({"query":"id:a","fields":["id"],"row_ops":{"self":{"top_docs":{"fields":["id"]}}}})");
  ASSERT_EQ(200, shorthand.result_int()) << shorthand.body();
  glz::generic_i64 json; ASSERT_FALSE(glz::read_json(json, shorthand.body()));
  EXPECT_EQ("a", json["docs"][0]["self"]["docs"][0]["id"].get_string());
}

TEST_F(NestedTransportTest, unaryAndStreamingProtobufCarryNestedValues) {
  for (bool streaming : {false, true}) {
    SCOPED_TRACE(streaming);
    helper.clear(); nested_test::setSchema(helper.collection());
    CollectionHelper::UpdateBuilder first, last;
    first.collection("main"); last.collection("main").commit();
    for (auto* input : {&first, &last}) {
      input->fieldMap({{"id","id"},{"external","items"},{"color","wrong"}}).dropUnmapped();
    }
    for (size_t i = 0; i < std::size(nested_test::DOCS); i++) {
      std::string doc(nested_test::DOCS[i]); doc.replace(doc.find("\"items\""), 7, "\"external\"");
      nested_test::add(streaming && i < 2 ? first : last, doc);
    }
    if (streaming) {
      grpc::ClientContext context;
      HppClientReaderWriter<api::UpdateRequest, api::UpdateResponse> stream(getChannel().get(), rpc::UpdateStream, &context);
      ASSERT_TRUE(stream.Write(first.finish()));
      Reply<api::UpdateResponse> response;
      ASSERT_TRUE(stream.Read(&response)); EXPECT_TRUE(response.msg.errors.empty());
      ASSERT_TRUE(stream.Write(last.finish())); ASSERT_TRUE(stream.WritesDone());
      ASSERT_TRUE(stream.Read(&response)); EXPECT_TRUE(response.msg.errors.empty());
      EXPECT_FALSE(stream.Read(&response)); ASSERT_TRUE(stream.Finish().ok());
    } else {
      grpc::ClientContext context; Reply<api::UpdateResponse> response;
      ASSERT_TRUE(hppUnaryCall(getChannel().get(), rpc::Update, &context, last.finish(), &response).ok());
      EXPECT_TRUE(response.msg.errors.empty());
    }
    grpcQueries();
    CollectionHelper::UpdateBuilder invalid;
    invalid.collection("main").overwrite().commit();
    nested_test::add(invalid, R"({"id":"a","items":[{"color":"discard"},{"parts":[1]}]})");
    grpc::ClientContext context; Reply<api::UpdateResponse> response;
    if (streaming) {
      HppClientReaderWriter<api::UpdateRequest, api::UpdateResponse> stream(getChannel().get(), rpc::UpdateStream, &context);
      ASSERT_TRUE(stream.Write(invalid.finish())); ASSERT_TRUE(stream.WritesDone());
      ASSERT_TRUE(stream.Read(&response)); ASSERT_EQ(1u, response.msg.errors.size());
      EXPECT_FALSE(stream.Read(&response)); ASSERT_TRUE(stream.Finish().ok());
    } else {
      ASSERT_TRUE(hppUnaryCall(getChannel().get(), rpc::Update, &context, invalid.finish(), &response).ok());
      ASSERT_EQ(1u, response.msg.errors.size());
    }
    grpcQueries();
  }
}

TEST_F(NestedTransportTest, bufferedAndNdjsonUseRootMappingAndKeepChildKeys) {
  HttpServer server(*luxirNode, 2, 0); server.start();
  auto stop = scope_guard([&] { server.shutdown(); });
  constexpr auto config = R"("field_map":{"id":"id","external":"items","color":"wrong"},"drop_unmapped":true)";
  for (bool ndjson : {false, true}) {
    SCOPED_TRACE(ndjson);
    helper.clear(); nested_test::setSchema(helper.collection());
    std::string body = ndjson ? std::string("{\"_update_\":{") + config + "}}\n" : std::string("{") + config + ",\"docs\":[";
    for (size_t i = 0; i < std::size(nested_test::DOCS); i++) {
      std::string doc(nested_test::DOCS[i]);
      doc.replace(doc.find("\"items\""), 7, "\"external\"");
      if (!ndjson && i) body += ',';
      body += doc; if (ndjson) body += '\n';
    }
    body += ndjson ? "{\"_end_\":{\"commit\":{}}}\n" : "],\"commit\":{}}";
    auto response = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update", body,
        ndjson ? "application/x-ndjson" : "application/json");
    ASSERT_EQ(200, response.result_int()) << response.body();
    httpQueries(server.getPort());
    auto invalid = httpRequest(server.getPort(), http::verb::post, "/collections/main/_update",
        ndjson ? "{\"id\":\"a\",\"items\":[{},1]}\n{\"_end_\":{\"commit\":{}}}\n"
               : "{\"docs\":[{\"id\":\"a\",\"items\":{\"unknown\":1}}],\"commit\":{}}",
        ndjson ? "application/x-ndjson" : "application/json");
    EXPECT_NE(std::string::npos, invalid.body().find("errors")) << invalid.body();
    httpQueries(server.getPort());
  }
}

} // namespace
} // namespace luxir::test
