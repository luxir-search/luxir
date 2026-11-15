// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// JSON-driven helpers for nested-document tests: schemas, documents and search
// requests are written in the HTTP JSON dialect, and responses are rendered
// back to JSON for assertions.

#include <deque>
#include <map>
#include <string>

#include "luxir/api/luxir_types.hpp"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

class NestedFixture : public LuxirTest {
public:
  std::deque<std::string> retained;  // JSON texts viewed by parsed requests

  void TearDown() override {
    CollectionHelper("main").clear();
    LuxirTest::TearDown();
  }

  CollectionHelper helper() { return CollectionHelper("main"); }

  void schema(std::string json) {
    auto& text = retained.emplace_back(std::move(json));
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    std::string err;
    auto ok = api::read_json(def, text, mr, &err);
    ASSERT_TRUE(ok) << text << ": " << err;
    helper().collection().updateSchema(def, api::SchemaRequest_::Mode::REPLACE_ALL);
  }

  // Each element of `docs` is one JSON document object.
  IndexResult index(std::initializer_list<std::string> docs, bool commit = true, bool overwrite = false) {
    CollectionHelper h = helper();
    CollectionHelper::UpdateBuilder b;
    for (const auto& doc : docs) {
      auto& text = retained.emplace_back(doc);
      b.addRaw([&](api::Map& map, std::pmr::memory_resource& mr) {
        auto ok = api::read_json(map, text, mr);
        EXPECT_TRUE(ok) << text;
      });
    }
    if (commit) b.commit();
    b.overwrite(overwrite);
    return h.submit(b);
  }

  // Runs a JSON SearchRequest and returns the final response as JSON.
  std::string search(std::string json) {
    auto& text = retained.emplace_back(std::move(json));
    auto req = localReq(helper().getSearchEngine());
    std::string err;
    auto ok = api::read_json(req->rawRequest(), text, req->mr, &err);
    EXPECT_TRUE(ok) << text << ": " << err;
    if (req->rawRequest().collection.empty()) req->collection("main");
    req->execute();
    std::string out;
    EXPECT_FALSE(req->responses.empty());
    if (req->responses.empty()) return out;
    if (!api::write_json(req->responses.back()->proto, out)) return "<unrenderable>";
    return out;
  }

  // ids of op `op` in a final response rendered as JSON rows.
  std::vector<std::string> ids(std::string json, std::string_view op = "q") {
    auto& text = retained.emplace_back(std::move(json));
    auto req = localReq(helper().getSearchEngine());
    std::string err;
    auto ok = api::read_json(req->rawRequest(), text, req->mr, &err);
    EXPECT_TRUE(ok) << text << ": " << err;
    if (req->rawRequest().collection.empty()) req->collection("main");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> result;
    for (const auto& doc : req->getDocs(op)) {
      if (auto* value = find(doc, "id")) result.push_back(std::get<std::string>(*value));
    }
    return result;
  }

  // id -> _score_ of op `op` (the request must ask for rows with get_scores).
  std::map<std::string, double> scores(std::string json, std::string_view op = "q") {
    auto& text = retained.emplace_back(std::move(json));
    auto req = localReq(helper().getSearchEngine());
    std::string err;
    auto ok = api::read_json(req->rawRequest(), text, req->mr, &err);
    EXPECT_TRUE(ok) << text << ": " << err;
    if (req->rawRequest().collection.empty()) req->collection("main");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::map<std::string, double> result;
    for (auto* response : req->responses) {
      const auto* list = response->proto.ops.find(op);
      if (!list || !(*list)->docList()) continue;
      for (const auto& doc : (*list)->docList()->docs) {
        auto* id = doc.fields.find("id");
        auto* score = doc.fields.find("_score_");
        if (!id || !score) continue;
        const auto& kind = (*score)->kind;
        double value = std::holds_alternative<float>(kind) ? std::get<float>(kind)
                     : std::holds_alternative<double>(kind) ? std::get<double>(kind) : -1;
        result[std::string((*id)->asString())] = value;
      }
    }
    return result;
  }

  std::string error(std::string json) {
    auto& text = retained.emplace_back(std::move(json));
    auto req = localReq(helper().getSearchEngine());
    std::string err;
    auto ok = api::read_json(req->rawRequest(), text, req->mr, &err);
    EXPECT_TRUE(ok) << text << ": " << err;
    if (req->rawRequest().collection.empty()) req->collection("main");
    req->execute();
    return req->ok() ? std::string() : req->errorMsg();
  }
};

} // namespace luxir::test
