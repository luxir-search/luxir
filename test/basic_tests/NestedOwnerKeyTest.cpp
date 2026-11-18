// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <glaze/glaze.hpp>
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"

namespace luxir::test {
namespace {

// "owner": "root" keys the rows of a child view by their root document. Each
// chunk and part also carries its root id as a field, so the same request
// keyed by that field is the reference.
class NestedOwnerKeyTest : public LuxirTest, public ::testing::WithParamInterface<bool> {
protected:
  CollectionHelper helper;

  void setup() {
    SchemaBuilder schema;
    using Type = api::FieldDef::FieldClass;
    for (auto name : {"chunks", "chunks.parts"}) {
      auto& path = schema.field(name);
      path.type = Type::NESTED; path.reverse_map = GetParam();
    }
    for (auto name : {"chunks.doc", "chunks.lang", "chunks.parts.doc"}) schema.field(name).type = Type::STRING;
    for (auto name : {"chunks.rank", "chunks.parts.rank"}) schema.field(name).type = Type::INT;
    schema.set(helper.collection());
    helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
    index({root("A", {1, 7, 9}), root("B", {2, 3}), root("X", {0, 0, 0}), R"({"id":"E"})"});
    index({root("C", {4}), R"({"id":"F","chunks":[]})", root("D", {5, 6, 8, 10})});
    ASSERT_TRUE(helper.deleteById("X", UpdateMessage::COMMIT).success);
  }

  // Each chunk holds two parts, ranked rank * 10 and rank * 10 + 1.
  static std::string root(std::string_view id, std::initializer_list<int> ranks) {
    auto doc = "\"doc\":\"" + std::string(id) + "\"";
    std::string json = "{\"id\":\"" + std::string(id) + "\",\"chunks\":[";
    for (int rank : ranks) {
      if (json.back() != '[') json += ',';
      json += "{" + doc + ",\"rank\":" + std::to_string(rank) + ",\"lang\":\"" + (rank % 3 ? "en" : "fr")
          + "\",\"parts\":[{" + doc + ",\"rank\":" + std::to_string(rank * 10) + "},{" + doc + ",\"rank\":"
          + std::to_string(rank * 10 + 1) + "}]}";
    }
    return json + "]}";
  }

  void index(std::initializer_list<std::string> docs) {
    CollectionHelper::UpdateBuilder input;
    for (const auto& json : docs) {
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
    }
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }

  // The response of one operation "q"; `error` receives a failed request's message.
  glz::generic_i64 search(std::string_view op, bool parallel = false, std::string* error = nullptr) {
    auto req = localReq(helper.getSearchEngine());
    auto json = "{\"ops\":{\"q\":" + std::string(op) + "}}";
    EXPECT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    if (parallel) req->execute(true); else req->execute(1);
    if (error) *error = req->ok() ? "" : req->errorMsg();
    else EXPECT_OK(req);
    if (!req->ok()) return {};
    glz::generic_i64 parsed;
    auto body = renderSearchResponseBody(req->responses.back()->proto);
    EXPECT_FALSE(glz::read_json(parsed, body)) << body;
    return parsed["ops"]["q"];
  }

  // Sorted object keys, without the group key values: the owner key is a row
  // number where the reference key is the root id.
  static std::string canonical(const glz::generic_i64& value, bool group = false) {
    if (value.is_object()) {
      std::map<std::string, std::string> fields;
      for (const auto& [key, child] : value.get_object()) {
        if (!group || key != "val") fields[key] = canonical(child, key == "_group_");
      }
      return glz::write_json(fields).value();
    }
    if (value.is_array()) {
      std::vector<std::string> rows;
      for (const auto& child : value.get_array()) rows.push_back(canonical(child));
      return glz::write_json(rows).value();
    }
    return glz::write_json(value).value();
  }

  // A row value per returned row: a field, or a member of its group.
  static std::vector<int64_t> rows(const glz::generic_i64& list, std::string_view field, bool group = false) {
    std::vector<int64_t> out;
    for (const auto& row : list["docs"].get_array()) {
      out.push_back((group ? row["_group_"][field] : row[field]).get<int64_t>());
    }
    return out;
  }

  static std::string list(std::string_view path, std::string_view keys, std::string_view rest) {
    return "{\"domain\":{\"to\":\"" + std::string(path) + "\"},\"top_docs\":{\"document_format\":\"rows\",\"fields\":[\"/id\",\"rank\"]," + std::string(keys)
        + "," + std::string(rest) + "}}";
  }

  void merge() {
    CollectionHelper::UpdateBuilder input;
    input.commit(true, 1); ASSERT_TRUE(helper.submit(input).success);
    ASSERT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  }
};

TEST_P(NestedOwnerKeyTest, collapseAndDiversifyKeyChildRowsByTheirRoot) {
  setup();
  for (bool merged : {false, true}) {
    if (merged) merge();
    for (std::string path : {"chunks", "chunks.parts"}) {
      // Every shape of list: with and without a count, a cut and a field sort.
      for (std::string rest : {R"("limit":-1,"count_limit":-1,"sort":[{"field":"rank"}])", R"("limit":-1,"count_limit":-1)",
                               R"("limit":2,"sort":[{"field":"rank","dir":"desc"}])", R"("limit":3,"offset":1,"count_limit":-1)",
                               R"("limit":2,"count_limit":0,"sort":[{"field":"rank"}])"}) {
        for (bool parallel : {false, true}) {
          SCOPED_TRACE(::testing::Message() << merged << '/' << path << '/' << rest << '/' << parallel);
          for (auto [owner, field] : std::vector<std::pair<std::string, std::string>>{
              {R"("collapse":{"owner":"root"})", R"("collapse":{"field":"doc"})"},
              {R"("collapse":{"owner":"root","get_count":true})", R"("collapse":{"field":"doc","get_count":true})"},
              {R"("collapse":{"owner":"root","field":"rank"})", R"("collapse":{"field":["rank","doc"]})"},
              {R"("diversify":{"owner":"root","max_per_key":2})", R"("diversify":{"field":"doc","max_per_key":2})"},
              {R"("diversify":{"owner":"root","max_per_key":1})", R"("diversify":{"field":"doc","max_per_key":1})"}}) {
            auto expected = canonical(search(list(path, field, rest), parallel));
            EXPECT_NE(std::string::npos, expected.find("/id")) << expected;
            EXPECT_EQ(expected, canonical(search(list(path, owner, rest), parallel))) << owner;
          }
        }
      }
    }
    // One row per document: its best chunk, and how many chunks it has.
    auto heads = search(list("chunks", R"("collapse":{"owner":"root","get_count":true})",
        R"("limit":-1,"count_limit":-1,"sort":[{"field":"rank"}])"));
    EXPECT_EQ((std::vector<int64_t>{1, 2, 4, 5}), rows(heads, "rank"));
    EXPECT_EQ((std::vector<int64_t>{3, 2, 1, 4}), rows(heads, "count", true));
    EXPECT_EQ(4, heads["found"].get<int64_t>());
    EXPECT_EQ(10, heads["found_docs"].get<int64_t>());
    // The best chunks, at most two per document.
    auto capped = search(list("chunks", R"("diversify":{"owner":"root","max_per_key":2})",
        R"("limit":-1,"count_limit":-1,"sort":[{"field":"rank"}])"));
    EXPECT_EQ((std::vector<int64_t>{1, 2, 3, 4, 5, 6, 7}), rows(capped, "rank"));
    // Two levels down the owner is still the root document, not the chunk.
    auto parts = search(list("chunks.parts", R"("collapse":{"owner":"root","get_count":true})",
        R"("limit":-1,"count_limit":-1,"sort":[{"field":"rank"}])"));
    EXPECT_EQ((std::vector<int64_t>{10, 20, 40, 50}), rows(parts, "rank"));
    EXPECT_EQ((std::vector<int64_t>{6, 4, 2, 8}), rows(parts, "count", true));
    // Distinct documents per facet bucket of their chunks.
    auto facet = [&](std::string_view key) {
      return canonical(search(R"({"domain":{"to":"chunks"},"field_facet":{"field":"lang","count_distinct":)" + std::string(key) + "}}"));
    };
    auto byField = facet(R"({"field":"doc"})");
    EXPECT_NE(std::string::npos, byField.find("fr")) << byField;
    EXPECT_EQ(byField, facet(R"({"owner":"root"})"));
  }
}

TEST_P(NestedOwnerKeyTest, ownerKeysNeedAChildView) {
  setup();
  auto error = [&](std::string_view domain, std::string_view keys) {
    std::string message;
    search("{" + std::string(domain) + "\"top_docs\":{" + std::string(keys) + "}}", false, &message);
    return message;
  };
  const std::string chunks = R"("domain":{"to":"chunks"},)";
  for (auto keys : {R"("collapse":{"owner":"root"})", R"("diversify":{"owner":"root","max_per_key":1})"}) {
    EXPECT_NE(std::string::npos, error("", keys).find("owner keys apply to the rows of a nested path")) << keys;
    EXPECT_EQ("", error(chunks, keys)) << keys;
  }
  for (auto keys : {R"("collapse":{"owner":".."})", R"("diversify":{"owner":"chunks","max_per_key":1})"}) {
    EXPECT_NE(std::string::npos, error(chunks, keys).find("owner must be \"root\"")) << keys;
  }
  for (auto keys : {R"("collapse":{})", R"("diversify":{"max_per_key":1})"}) {
    EXPECT_NE(std::string::npos, error(chunks, keys).find("collapse.field must not be empty")) << keys;
  }
  // An owner's field is still not a key.
  EXPECT_NE("", error(chunks, R"("collapse":{"field":["/id"]})"));
}

INSTANTIATE_TEST_SUITE_P(ReverseMap, NestedOwnerKeyTest, ::testing::Bool());

}  // namespace
}  // namespace luxir::test
