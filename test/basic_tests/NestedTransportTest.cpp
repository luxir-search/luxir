// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <sstream>

#include "test/GrpcLuxirTest.h"
#include "test/GrpcClient.h"
#include "test/HttpReq.h"
#include "test/NestedTestData.h"
#include "luxir/server/HttpServer.h"

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
