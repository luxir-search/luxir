// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <glaze/glaze.hpp>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "luxir/server/JsonResponse.h"

namespace luxir::test::nested_test {

inline constexpr std::string_view DOCS[]{
    R"({"id":"a","items":[{"color":"red","size":"S","note_w":"red child","parts":{"material":"steel","note_w":"steel part"}},{"color":"blue","size":"L","parts":{"material":"wood"}}]})",
    R"({"id":"b","items":{"color":"red","size":"L","note_w":"red child","parts":{"material":"steel","note_w":"steel part"}}})",
    R"({"id":"c","items":[]})", R"({"id":"d","items":{}})"};
inline constexpr std::string_view QUERY = R"query({"ops":{
  "implicit":{"top_docs":{"query":"items.color:red","count_limit":-1,"limit":0}},
  "group":{"top_docs":{"query":"items:(color:red AND size:L)","count_limit":-1,"limit":0}},
  "explicit":{"top_docs":{"query":{"to_parent":{"path":"items","score":"none","query":{"match":{"field":"color","val":"red"}}}},"count_limit":-1,"limit":0}},
  "exists":{"top_docs":{"query":"items:*","count_limit":-1,"limit":0}},
  "deep":{"top_docs":{"query":"items.parts.material:steel","count_limit":-1,"limit":0}},
  "deep_group":{"top_docs":{"query":"items:(parts:(material:steel))","count_limit":-1,"limit":0}},
  "notes":{"domain":{"to":"items"},"top_docs":{"query":"note_w:red","count_limit":-1,"fields":["note_w"]}},
  "part_notes":{"domain":{"to":"items.parts"},"top_docs":{"query":"note_w:steel","count_limit":-1,"fields":["note_w"]}},
  "rows":{"domain":{"to":"items"},"top_docs":{"count_limit":-1,"fields":["color","size"],"ops":{
    "up":{"domain":{"to":".."},"top_docs":{"count_limit":-1,"limit":0}}}}},
  "parts":{"domain":{"to":"items.parts"},"top_docs":{"count_limit":-1,"fields":["material"],"ops":{
    "up":{"domain":{"to":".."},"top_docs":{"count_limit":-1,"limit":0}},
    "root":{"domain":{"to":"/"},"top_docs":{"count_limit":-1,"limit":0}},
    "two":{"domain":{"to":"../.."},"top_docs":{"count_limit":-1,"limit":0}}}}}
}})query";
using Counts = std::map<std::string, int64_t>;

inline void setSchema(Collection& collection) {
  SchemaBuilder schema;
  using Type = api::FieldDef::FieldClass;
  for (auto name : {"items", "items.parts"}) schema.field(name).type = Type::NESTED;
  for (auto name : {"items.color", "items.size", "items.parts.material"}) schema.field(name).type = Type::STRING;
  schema.set(collection);
}

inline void add(CollectionHelper::UpdateBuilder& input, std::string_view json) {
  input.addRaw([&](auto& doc, auto& arena) { EXPECT_TRUE(api::read_json(doc, json, arena)); });
}

inline void capture(const api::DocList& docs, const std::string& path, Counts& counts) {
  if (docs.found) counts[path] = *docs.found;
  if (!docs.columns.empty() && path == "rows") {
    EXPECT_NE(nullptr, docs.columns.find("color"));
    EXPECT_EQ(nullptr, docs.columns.find("items.color"));
  }
  if (!docs.columns.empty() && path == "parts") {
    EXPECT_NE(nullptr, docs.columns.find("material"));
    EXPECT_EQ(nullptr, docs.columns.find("items.parts.material"));
  }
  if ((path == "notes" || path == "part_notes") && docs.row_count) {
    auto expected = path == "notes" ? "red child" : "steel part";
    auto* column = docs.columns.find("note_w");
    ASSERT_NE(nullptr, column);
    for (auto value : std::get<api::ColStr>(column->kind).v) EXPECT_EQ(expected, value);
  }
  for (const auto& [name, value] : docs.ops) {
    if (auto* child = value->docList()) capture(*child, path + "." + std::string(name), counts);
  }
}

inline void capture(const api::SearchResponse& response, Counts& counts) {
  EXPECT_FALSE(response.error.has_value());
  for (const auto& [name, value] : response.ops) {
    if (auto* docs = value->docList()) capture(*docs, std::string(name), counts);
  }
}

inline Counts local(LuxirNode& node, std::shared_ptr<IndexReader> reader = {}) {
  auto req = localReq(node.getSearchEngine());
  EXPECT_TRUE(api::read_json(req->rawRequest(), QUERY, req->mr));
  if (reader) req->execute(std::move(reader), false); else req->execute(1);
  EXPECT_OK(req);
  Counts counts;
  for (const auto* response : req->responses) capture(response->proto, counts);
  return counts;
}

inline constexpr std::string_view ROW_QUERY = R"JSON({"ops":{"q":{"top_docs":{
  "query":{"name":"m","expr":"items:(color:red)"},"fields":["id"],"sort":[{"field":"id"}],
  "row_ops":{"matched":{"domain":{"from_match":"m"},"top_docs":{"fields":["color","size"],"count_limit":-1}},
    "deep":{"domain":{"to":"items.parts","filter":["material:steel"]},"top_docs":{"fields":["material"]}}}
}}}})JSON";

inline std::vector<std::string> captureRows(const api::SearchResponse& response) {
  glz::generic_i64 json;
  EXPECT_FALSE(glz::read_json(json, renderSearchResponseBody(response)));
  std::vector<std::string> values;
  for (const auto& row : json["ops"]["q"]["docs"].get_array()) {
    values.push_back(row["id"].get_string() + ':'
        + row["matched"]["docs"][0]["color"].get_string() + ':'
        + row["deep"]["docs"][0]["material"].get_string());
  }
  return values;
}

inline std::vector<std::string> rowOperations(LuxirNode& node) {
  auto req = localReq(node.getSearchEngine());
  EXPECT_TRUE(api::read_json(req->rawRequest(), ROW_QUERY, req->mr));
  req->execute(1); EXPECT_OK(req);
  return captureRows(req->responses.back()->proto);
}

inline constexpr std::string_view RETRIEVAL_QUERY = R"({"ops":{
  "q":{"top_docs":{"query":"id:b","fields":["id","items"],"document_format":"rows"}},
  "p":{"domain":{"to":"items.parts"},"top_docs":{"query":"material:steel","fields":["/id","../color"],"document_format":"rows"}}
}})";

inline std::vector<std::string> captureRetrieval(const api::SearchResponse& response) {
  std::vector<std::string> values;
  EXPECT_FALSE(response.error);
  if (response.error) return values;
  const auto& row = response.ops.at("q")->docList()->docs[0];
  const auto& item = std::get<api::Map>(std::get<api::ArrVal>(row.fields.at("items")->kind).v[0].kind);
  const auto& part = std::get<api::Map>(std::get<api::ArrVal>(item.fields.at("parts")->kind).v[0].kind);
  values.push_back(std::string(row.fields.at("id")->asString()) + ':'
      + std::string(item.fields.at("color")->asString()) + ':' + std::string(part.fields.at("material")->asString()));
  for (const auto& owner : response.ops.at("p")->docList()->docs) {
    values.push_back(std::string(owner.fields.at("/id")->asString()) + ':' + std::string(owner.fields.at("../color")->asString()));
  }
  std::sort(values.begin(), values.end());
  return values;
}

inline std::vector<std::string> retrieval(LuxirNode& node, std::shared_ptr<IndexReader> reader = {}) {
  auto req = localReq(node.getSearchEngine());
  EXPECT_TRUE(api::read_json(req->rawRequest(), RETRIEVAL_QUERY, req->mr));
  if (reader) req->execute(std::move(reader), false); else req->execute(1);
  EXPECT_OK(req);
  return captureRetrieval(req->responses.back()->proto);
}

inline const Counts INITIAL{{"notes",2},{"part_notes",2},{"implicit",2},{"group",1},{"explicit",2},{"exists",3},
    {"deep",2},{"deep_group",2},{"rows",4},{"rows.up",3},{"parts",3},
    {"parts.up",3},{"parts.root",2},{"parts.two",2}};

} // namespace luxir::test::nested_test
