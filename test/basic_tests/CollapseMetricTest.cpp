// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <random>
#include <set>

#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/search/SearchOverrides.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using Key = std::vector<std::variant<std::monostate, std::string, int64_t>>;
struct Row {
  std::string id;
  int64_t count;
  float score;
  std::optional<double> low;
  int64_t high;
  bool operator==(const Row&) const = default;
};
std::vector<Row> rows(const LocalReq& req) {
  std::vector<Row> result;
  for (auto* response : req.responses) {
    const auto& list = *response->proto.ops.at("q")->docList();
    for (size_t i = 0; i < list.docs.size(); i++) {
      const auto& values = list.groups->ranks[i].v;
      result.push_back({std::string(list.docs[i].fields.at("id")->asString()), list.groups->counts[i],
          list.docs[i].fields.at("_score_")->asFloat(),
          values[0].isNull() ? std::nullopt : std::optional(values[0].asDouble()), values[1].asInt()});
    }
  }
  return result;
}
}

class CollapseMetricTest : public LuxirTest {
protected:
  auto run(std::string_view collection, std::string_view body, int parallel = 1, bool prepared = false) {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(body, req->rawRequest(), req->mr);
    req->collection(collection);
    req->testForcePrepare = prepared;
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg() << body;
    return req;
  }

  static std::vector<Row> oracle(const api::DocList& list, const std::vector<std::string>& fields,
                                 std::string_view policy) {
    struct Group { Row row; size_t best; int64_t headSequence = -1; };
    std::map<Key, Group> groups;
    size_t position = 0;
    for (const auto& doc : list.docs) {
      const auto& f = doc.fields;
      Key key;
      bool missing = false;
      for (const auto& field : fields) {
        auto* value = f.find(field);
        if (!value || (*value)->isNull()) { key.emplace_back(); missing = true; }
        else if (auto* s = std::get_if<std::string_view>(&(*value)->kind)) key.emplace_back(std::string(*s));
        else key.emplace_back((*value)->asInt());
      }
      if (missing && policy == "drop") continue;
      if (missing && policy == "expand") key = {std::string(f.at("id")->asString())};
      auto [where, inserted] = groups.try_emplace(key, Group{{"", 0, 0, {}, INT64_MIN}, position++});
      auto& g = where->second;
      g.row.count++;
      if (auto* price = f.find("price_f")) {
        double value = (*price)->asFloat();
        if (!g.row.low || value < *g.row.low) g.row.low = value;
      }
      g.row.high = std::max(g.row.high, f.at("sort_i")->asInt());
      int64_t sequence = f.at("seq_i")->asInt();
      if (inserted || sequence > g.headSequence) {
        g.headSequence = sequence;
        g.row.id = std::string(f.at("id")->asString());
        g.row.score = f.at("_score_")->asFloat();
      }
    }
    std::vector<Group> ranked;
    for (auto& [key, value] : groups) ranked.push_back(value);
    std::sort(ranked.begin(), ranked.end(), [](const Group& a, const Group& b) {
      if (a.row.low.has_value() != b.row.low.has_value()) return a.row.low.has_value();
      if (a.row.low != b.row.low) return a.row.low < b.row.low;
      if (a.row.high != b.row.high) return a.row.high > b.row.high;
      return a.best < b.best;
    });
    std::vector<Row> result;
    for (const auto& g : ranked) result.push_back(g.row);
    return result;
  }
};

TEST_F(CollapseMetricTest, exhaustiveOracle) {
  std::mt19937 random(736);
  std::vector<Doc> docs;
  for (int i = 0; i < 90; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "seq_i", (int64_t)i,
        "sort_i", (int64_t)(random() % 3), "score_i", (int64_t)(random() % 4),
        "body_w", i % 5 ? "keep" : "other");
    if (i % 7) doc.emplace_back("key_s", "g" + std::to_string(i % 9));
    if (i % 6) doc.emplace_back("key_i", i % 2 ? INT64_MIN + i % 5 : INT64_MAX - i % 5);
    if (i % 9) doc.emplace_back("price_f", (float)(random() % 4) * 0.5f);
    docs.push_back(std::move(doc));
  }
  struct Shape { std::vector<std::string> fields; std::string policy; bool score; };
  std::vector<Shape> shapes;
  for (auto fields : {std::vector<std::string>{"key_s"}, {"key_i"}, {"key_s", "key_i"},
                     {"key_s", "key_i", "seq_i"}})
    for (std::string policy : {"expand", "group", "drop"})
      for (bool score : {false, true}) shapes.push_back({fields, policy, score});
  for (int segments : {1, 3}) {
    std::string collection = "metric" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int s = 0; s < segments; s++) ASSERT_TRUE(helper.indexAll(
        std::span<const Doc>(docs).subspan(s * 90 / segments, 90 / segments),
        UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper.deleteByIds({"d2", "d41"}, UpdateMessage::COMMIT).success);
    for (const auto& shape : shapes) {
      std::string base = R"JSON({"query":"rescore(body_w:keep, score_i)","get_scores":true,
        "count_limit":-1,"document_format":"rows","batch_size":1000,
        "fields":["id","seq_i","sort_i","price_f","key_s","key_i"],"sort":)JSON"
          + std::string(shape.score ? R"("score desc")" : R"("sort_i asc")");
      auto full = run(collection, base + R"(,"limit":-1})");
      auto expected = oracle(*full->docList(), shape.fields, shape.policy);
      std::string collapse = R"(,"collapse":{"field":[)";
      for (const auto& field : shape.fields) {
        if (collapse.back() != '[') collapse += ',';
        collapse += '"' + field + '"';
      }
      collapse += R"(],"missing":")" + shape.policy + R"JSON(","head_sort":"seq_i desc",
        "get_count":true,"sort":["lo asc","hi desc"],
        "ops":{"lo":"min(price_f)","hi":"max(sort_i)"}})JSON";
      for (int parallel : {1, -1}) {
        auto actual = run(collection, base + collapse + R"(,"limit":-1})", parallel);
        ASSERT_OK(actual);
        EXPECT_EQ(expected, rows(*actual));
        EXPECT_EQ((int64_t)expected.size(), actual->docList()->found.value_or(-1));
        int64_t eligible = 0;
        for (const auto& row : expected) eligible += row.count;
        EXPECT_EQ(eligible, actual->docList()->found_docs.value_or(-1));
        auto zero = run(collection, base + collapse + R"(,"limit":0})", parallel);
        EXPECT_EQ((int64_t)expected.size(), zero->docList()->found.value_or(-1));
        EXPECT_EQ(eligible, zero->docList()->found_docs.value_or(-1));
      }
      if (shape.score) continue;
      std::vector<Row> paged;
      for (size_t offset = 0; offset < expected.size() + 5; offset += 5) {
        auto page = run(collection, base + collapse + R"(,"limit":5,"batch_size":2,"offset":)"
            + std::to_string(offset) + '}');
        auto part = rows(*page);
        paged.insert(paged.end(), part.begin(), part.end());
      }
      EXPECT_EQ(expected, paged);
    }
  }
}

TEST_F(CollapseMetricTest, resetPageOpsPreparedSelectionAndValidation) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vector = schema.field("vec");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.dims = 1;
  vector.metric = api::VectorMetric::L2;
  vector.column = true;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "key_s", "x", "price_i", (int64_t)20, "vec", std::vector<float>{0}),
      flatdoc("id", "b", "key_s", "x", "price_i", (int64_t)1, "vec", std::vector<float>{1}),
      flatdoc("id", "c", "key_s", "y", "price_i", (int64_t)10, "vec", std::vector<float>{2})},
      UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto req = run("main", R"JSON({"query":{"knn":{"field":"vec","query":[0],"k":2,
      "unit":"groups","exact":true}},"collapse":{"field":"key_s","sort":"lo asc","ops":{
        "lo":"min(price_i)","all":{"top_docs":{"sort":"id asc","limit":-1,
        "fields":["id"],"document_format":"rows"},"domain":{"query":"*:*"}}}},
      "fields":["id"],"document_format":"rows","count_limit":-1,"get_scores":true})JSON", parallel);
    const auto& list = *req->docList();
    ASSERT_EQ(2u, list.docs.size());
    EXPECT_EQ("c", list.docs[0].fields.at("id")->asString());
    EXPECT_EQ("a", list.docs[1].fields.at("id")->asString());
    EXPECT_EQ(10, list.groups->ranks[0].v[0].asInt());
    EXPECT_EQ(20, list.groups->ranks[1].v[0].asInt());
    const auto& all = std::get<api::ArrVal>(list.groups->ops.at("all")->kind).v;
    EXPECT_EQ(1, all[0].docList()->row_count);
    EXPECT_EQ(2, all[1].docList()->row_count);
  }
  for (auto ops : {R"JSON("lo":"min(price_i)")JSON", R"("bad":{"field_facet":{"field":"key_s"}})",
       R"JSON("bad":{"expr_op":"min(price_i)","domain":{"query":"*:*"}})JSON"}) {
    auto req = localReq(helper.getSearchEngine());
    auto body = R"({"collapse":{"field":"key_s","sort":"bad asc","ops":{)"
        + std::string(ops) + "}}}";
    parseQueryRequest(body, req->rawRequest(), req->mr);
    req->collection("main").execute();
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("collapse.sort")) << req->errorMsg();
  }
}

TEST_F(CollapseMetricTest, failedMetricTiesAndWire) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "z", "price_i", (int64_t)7),
      flatdoc("id", "b", "key_s", "a", "price_i", (int64_t)3),
      flatdoc("id", "c", "key_s", "z", "price_i", (int64_t)3),
      flatdoc("id", "d", "key_s", "null")}, UpdateMessage::COMMIT).success);
  auto req = run("main", R"JSON({"sort":"id asc","get_scores":true,"fields":["id"],
    "document_format":"rows","collapse":{"field":"key_s","sort":["bad desc","lo asc"],
    "ops":{"bad":"min(price_i)/0","lo":"min(price_i)"}}})JSON");
  ASSERT_OK(req);
  const auto& list = *req->docList();
  ASSERT_EQ(3u, list.docs.size());
  // The failed first clause ties. Equal minima then use the best member's
  // document order (a before b), although a's own price is 7.
  EXPECT_EQ("a", list.docs[0].fields.at("id")->asString());
  EXPECT_EQ("b", list.docs[1].fields.at("id")->asString());
  EXPECT_EQ("d", list.docs[2].fields.at("id")->asString());
  for (const auto& rank : list.groups->ranks) EXPECT_TRUE(rank.v[0].isNull());
  EXPECT_EQ(3, list.groups->ranks[0].v[1].asInt());
  EXPECT_TRUE(list.groups->ranks[2].v[1].isNull());
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(req->rawRequest(), bytes));
  auto copy = localReq(helper.getSearchEngine());
  size_t size = bytes.size();
  bytes.resize(size + 16);
  ASSERT_TRUE(api::decode(copy->rawRequest(), std::span(bytes).first(size), copy->mr));
  copy->execute(-1);
  ASSERT_OK(copy);
  const auto& copied = *copy->docList();
  EXPECT_EQ(list.docs[0].fields.at("id")->asString(), copied.docs[0].fields.at("id")->asString());
  EXPECT_EQ(3, copied.groups->ranks[0].v[1].asInt());
  api::Collapse shorthand = *std::get<api::TopDocs>(req->rawRequest().ops.at("q")->kind).collapse;
  ASSERT_TRUE(api::read_json(shorthand, R"("key_s")", req->mr));
  EXPECT_TRUE(shorthand.sort.empty());
}

TEST_F(CollapseMetricTest, memoryFailureEmptyIndexAndDocsRendering) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "price_i", (int64_t)2)},
      UpdateMessage::COMMIT).success);
  std::string body = R"JSON({"collapse":{"field":"key_s","sort":"lo asc",
    "ops":{"lo":"min(price_i)"}},"count_limit":-1,"get_scores":true,
    "fields":["id"],"document_format":"rows"})JSON";
  auto req = run("main", body);
  auto runs = renderDocRuns(req->responses[0]->proto);
  ASSERT_EQ(1u, runs.size());
  EXPECT_NE(std::string::npos, runs[0].body.find(R"("rank":[2])"));
  {
    SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
    forcedRequestMemoryMaxBytes = 1;
    auto limited = localReq(helper.getSearchEngine());
    parseQueryRequest(body, limited->rawRequest(), limited->mr);
    limited->collection("main").execute();
    EXPECT_FALSE(limited->ok());
    EXPECT_NE(std::string::npos, limited->errorMsg().find("request memory breaker"));
  }
  CollectionHelper empty("metric_empty");
  auto zero = run("metric_empty", body);
  EXPECT_EQ(0, zero->docList()->found.value_or(-1));
  EXPECT_EQ(0, zero->docList()->found_docs.value_or(-1));
}

TEST_F(CollapseMetricTest, metricListsUnderBucketsAndFusion) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "price_i", (int64_t)7),
      flatdoc("id", "b", "key_s", "y", "price_i", (int64_t)3),
      flatdoc("id", "c", "key_s", "x", "price_i", (int64_t)1)}, UpdateMessage::COMMIT).success);
  std::string child = R"JSON({"top_docs":{"collapse":{"field":"key_s","sort":"lo asc",
    "ops":{"lo":"min(price_i)"}},"sort":"id asc","count_limit":-1,
    "get_scores":true,"fields":["id"],"document_format":"rows"}})JSON";
  for (bool fusion : {false, true}) {
    auto body = fusion ? R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{"all":{"limit":-1}},
        "ops":{"m":)" + child + "}}}}}"
        : R"({"limit":0,"ops":{"f":{"query_facet":{"buckets":{"all":"*:*"},
        "ops":{"m":)" + child + "}}}}}";
    auto req = run("main", body, -1, !fusion);
    ASSERT_OK(req);
    const auto* root = &*req->responses.back()->proto.ops.at(fusion ? "f" : "q");
    const auto& list = fusion ? *root->docList()->ops.at("m")->docList()
        : *std::get<api::ArrVal>(root->docList()->ops.at("f")->facetResult()->ops.at("m")->kind)
            .v[0].docList();
    EXPECT_EQ(2, list.found.value_or(-1));
    EXPECT_EQ(3, list.found_docs.value_or(-1));
    ASSERT_EQ(2u, list.docs.size());
    EXPECT_EQ("a", list.docs[0].fields.at("id")->asString());
    EXPECT_EQ("b", list.docs[1].fields.at("id")->asString());
    EXPECT_EQ(1, list.groups->ranks[0].v[0].asInt());
  }
}

TEST_F(CollapseMetricTest, stringBestMembersAndWeightedMetricMerge) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& raw = schema.field("raw");
  raw.type = api::FieldDef::FieldClass::STRING;
  raw.column = true;
  raw.index = api::FieldDef::IndexMode::NONE;
  schema.set(helper.collection());
  auto add = [&](const char* id, const char* key, int price, std::string value) {
    ASSERT_TRUE(helper.index(flatdoc("id", id, "key_s", key, "price_i", (int64_t)price,
        "sort_s", value, "raw", value), UpdateMessage::COMMIT).success);
  };
  add("a", "x", 1, std::string(5000, 'z'));
  add("b", "y", 8, "b");
  add("c", "x", 3, "a");
  add("d", "x", 9, std::string(2000, 'm'));
  add("e", "y", 4, std::string(6000, 'c'));
  for (auto field : {"sort_s", "raw"}) for (int parallel : {1, -1}) {
    auto req = run("main", R"JSON({"collapse":{"field":"key_s","sort":"average asc",
      "get_count":true,"ops":{"average":"avg(price_i)","sum":"sum(price_i)"}},
      "sort":")JSON" + std::string(field) + R"( asc","get_scores":true,
      "fields":["id"],"document_format":"rows"})", parallel);
    ASSERT_OK(req);
    const auto& list = *req->docList();
    ASSERT_EQ(2u, list.docs.size());
    EXPECT_EQ("c", list.docs[0].fields.at("id")->asString());
    EXPECT_EQ("b", list.docs[1].fields.at("id")->asString());
    EXPECT_DOUBLE_EQ(13.0 / 3, list.groups->ranks[0].v[0].asDouble());
    EXPECT_DOUBLE_EQ(6, list.groups->ranks[1].v[0].asDouble());
    EXPECT_EQ(3, list.groups->counts[0]);
    if (std::string_view(field) == "raw") {
      EXPECT_GT(req->memoryTracker.peak(), 5000u);
    }
  }
}

TEST_F(CollapseMetricTest, routedOrdinaryOpsKeepTheirOwnDomains) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "key_s", "x", "keep_s", "yes", "price_i", (int64_t)10),
      flatdoc("id", "b", "key_s", "x", "keep_s", "no", "price_i", (int64_t)1),
      flatdoc("id", "c", "key_s", "y", "keep_s", "yes", "price_i", (int64_t)20),
      flatdoc("id", "d", "key_s", "y", "keep_s", "no", "price_i", (int64_t)2),
      flatdoc("id", "e", "key_s", "z", "keep_s", "no", "price_i", (int64_t)3)},
      UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto req = run("main", R"JSON({"collapse":{"field":"key_s","sort":"lo asc",
      "ops":{"lo":"min(price_i)"}},"filter":[{"query":"keep_s:yes","except_ops":["all"]}],
      "sort":"id asc","get_scores":true,"count_limit":-1,"fields":["id"],"document_format":"rows",
      "ops":{"all":{"field_facet":{"field":"key_s","count_unit":"docs","limit":-1}}}})JSON",
      parallel);
    ASSERT_OK(req);
    const auto& list = *req->docList();
    EXPECT_EQ(2, list.found.value_or(-1));
    EXPECT_EQ(2, list.found_docs.value_or(-1));
    EXPECT_EQ(10, list.groups->ranks[0].v[0].asInt());
    EXPECT_EQ(20, list.groups->ranks[1].v[0].asInt());
    auto counts = list.ops.at("all")->facetResult()->counts;
    EXPECT_EQ((std::vector<int64_t>{2, 2, 1}), std::vector(counts.begin(), counts.end()));
  }
}
