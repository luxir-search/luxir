// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "test/SchemaBuilder.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/DistinctFacetCounts.h"

#include <map>
#include <random>
#include <set>

using namespace luxir;
using namespace luxir::test;

namespace {
using Part = std::variant<std::monostate, std::string, int64_t>;
using Key = std::vector<Part>;

Part scalar(const api::Val& value) {
  if (auto* text = std::get_if<std::string_view>(&value.kind)) return std::string(*text);
  if (value.isNull()) return {};
  return value.asInt();
}

std::vector<Part> values(const api::Map& doc, std::string_view field) {
  auto* found = doc.fields.find(field);
  if (found == nullptr || (*found)->isNull()) return {};
  const auto& value = **found;
  std::vector<Part> result;
  if (auto* array = std::get_if<api::ArrStr>(&value.kind)) {
    for (auto text : array->v) result.emplace_back(std::string(text));
  } else if (auto* array = std::get_if<api::ArrInt>(&value.kind)) {
    for (auto number : array->v) result.emplace_back(number);
  } else if (auto* array = std::get_if<api::ArrVal>(&value.kind)) {
    for (const auto& v : array->v) result.push_back(scalar(v));
  } else result.push_back(scalar(value));
  return result;
}

// Brute-force oracle: only ordinary exhaustive document rows and test-owned
// tuples/sets. Neither CollapseKey nor the engine's facet counters participate.
struct Oracle {
  std::map<Part, std::set<Key>> buckets;
  std::set<Key> missing;
};

template <typename Accept>
Oracle oracle(const api::DocList& full, std::span<const std::string> fields,
              std::string_view policy, std::string_view facet, Accept accept) {
  Oracle result;
  for (const auto& doc : full.docs) {
    if (!accept(doc)) continue;
    Key key;
    bool absent = false;
    for (const auto& field : fields) {
      auto parts = values(doc, field);
      absent |= parts.empty();
      key.push_back(parts.empty() ? Part{} : parts[0]);
    }
    if (absent && policy == "drop") continue;
    if (absent && policy == "expand") key = {std::string("singleton"), values(doc, "id")[0]};
    auto parts = values(doc, facet);
    if (parts.empty()) result.missing.insert(key);
    for (auto part : parts) result.buckets[part].insert(key);
  }
  return result;
}

std::map<Part, int64_t> counts(const api::FacetResult& result) {
  std::map<Part, int64_t> actual;
  if (auto* column = std::get_if<api::ColStr>(&result.bucket_ids->kind)) {
    for (size_t i = 0; i < result.counts.size(); i++) actual[std::string(column->v[i])] = result.counts[i];
  } else {
    const auto& numbers = std::get<api::ColInt>(result.bucket_ids->kind);
    for (size_t i = 0; i < result.counts.size(); i++) actual[numbers.v[i]] = result.counts[i];
  }
  return actual;
}

void check(const api::FacetResult& actual, const Oracle& expected) {
  std::map<Part, int64_t> wanted;
  for (const auto& [value, groups] : expected.buckets) wanted[value] = (int64_t)groups.size();
  EXPECT_EQ(wanted, counts(actual));
  EXPECT_EQ((int64_t)expected.missing.size(), actual.missing.value_or(-1));
}

std::string spec(std::span<const std::string> fields, std::string_view policy) {
  std::string json = R"json({"field":[)json";
  for (size_t i = 0; i < fields.size(); i++) {
    if (i) json += ',';
    json += '"' + fields[i] + '"';
  }
  return json + R"json(],"missing":")json" + std::string(policy) + R"json("})json";
}
}

class DistinctFacetTest : public LuxirTest {
protected:
  auto run(std::string_view collection, std::string_view body, int32_t parallel = 1,
           bool success = true) {
    auto request = localReq(luxirNode->getSearchEngine());
    std::string json = R"json({"collection":")json" + std::string(collection)
        + R"json(","document_format":"rows","batch_size":10000,)json" + std::string(body) + '}';
    EXPECT_TRUE(api::read_json(request->rawRequest(), json, request->mr)) << json;
    request->execute(parallel);
    EXPECT_EQ(success, request->ok()) << request->errorMsg() << json;
    return request;
  }
};

TEST_F(DistinctFacetTest, bruteForceFieldAndNestedDomains) {
  std::mt19937 random(1093);
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 210; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "weight_i", (int64_t)i);
    if (i % 7 != 0) doc.emplace_back("key_s", "g" + std::to_string(random() % 17));
    if (i % 5 != 0) doc.emplace_back("key_i", (int64_t)(random() % 4) - 2);
    if (i % 8 != 0) doc.emplace_back("wide_i", i % 2 ? INT64_MIN : INT64_MAX);
    if (i % 3 != 0) doc.emplace_back("when_dt", (int64_t)(random() % 3) * 86400000);
    if (i % 6 != 0) doc.emplace_back("color_s", "c" + std::to_string(random() % 5));
    if (i % 4 != 0) doc.emplace_back("size_i", (int64_t)(random() % 3) - 1);
    if (i % 9 != 0) {
      doc.emplace_back("tag_ss", std::vector<std::string>{"x", "y", "x"});
    }
    docs.push_back(std::move(doc));
  }
  SearchOverridesGuard guard(forcedDistinctFacetMode);
  for (auto mode : {DistinctFacetMode::PAGED, DistinctFacetMode::HASH}) {
    forcedDistinctFacetMode = mode;
    for (int32_t segments : {1, 3, 7}) {
      std::string name = "distinct" + std::to_string(segments) + "_" + std::to_string((int)mode);
      CollectionHelper helper(name);
      for (int32_t segment = 0; segment < segments; segment++) {
        ASSERT_TRUE(helper.indexAll(std::span<const Doc>(docs).subspan(
            (size_t)(segment * 210 / segments), (size_t)(210 / segments)), UpdateMessage::COMMIT).success);
      }
      ASSERT_TRUE(helper.deleteByIds({"d13", "d100"}, UpdateMessage::COMMIT).success);
      auto full = run(name, R"json("limit":-1)json");
      ASSERT_EQ(208, full->docList()->row_count);
      for (const auto& fields : std::vector<std::vector<std::string>>{
          {"key_s"}, {"key_i"}, {"when_dt"}, {"key_s", "key_i"}, {"key_s", "key_i", "when_dt"}, {"key_s", "wide_i"}}) {
        for (std::string policy : {"expand", "group", "drop"}) {
          for (int32_t parallel : {1, -1}) {
            std::string body = R"json("limit":0,"collapse":)json" + spec(fields, policy)
                + R"json(,"ops":{"color":{"field_facet":{"field":"color_s","limit":-1,"missing":true,
                    "ops":{"size":{"field_facet":{"field":"size_i","limit":-1,"missing":true}}}}},
                    "tag":{"field_facet":{"field":"tag_ss","limit":-1,"missing":true}}})json";
            auto request = run(name, body, parallel);
            ASSERT_TRUE(request->ok());
            const auto& color = *request->docList()->ops.at("color")->facetResult();
            auto all = [](const api::Map&) { return true; };
            check(color, oracle(*full->docList(), fields, policy, "color_s", all));
            check(*request->docList()->ops.at("tag")->facetResult(),
                oracle(*full->docList(), fields, policy, "tag_ss", all));
            const auto& labels = std::get<api::ColStr>(color.bucket_ids->kind).v;
            const auto& nested = std::get<api::ArrVal>(color.ops.at("size")->kind).v;
            for (size_t i = 0; i < labels.size(); i++) {
              check(*nested[i].facetResult(), oracle(*full->docList(), fields, policy, "size_i",
                  [&](const api::Map& doc) { return values(doc, "color_s") == std::vector<Part>{std::string(labels[i])}; }));
            }
            // The general measure executes independently of list collapsing.
            auto distinct = run(name, R"json("limit":0,"ops":{"f":{"field_facet":{"field":"color_s","limit":-1,
                "missing":true,"count_distinct":)json" + spec(fields, policy) + "}}}", parallel);
            check(*distinct->docList()->ops.at("f")->facetResult(), oracle(*full->docList(), fields, policy, "color_s", all));
          }
        }
      }
    }
  }
}

TEST_F(DistinctFacetTest, selectedExceptOpsAndDocumentMetrics) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "0", "key_s", "a", "color_s", "red", "size_s", "S", "price_i", 10, "tag_ss", vecs("x", "y")),
      flatdoc("id", "1", "key_s", "a", "color_s", "blue", "size_s", "M", "price_i", 20, "tag_ss", vecs("x")),
      flatdoc("id", "2", "key_s", "b", "color_s", "red", "size_s", "M", "price_i", 30, "tag_ss", vecs("x", "y"))}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "3", "key_s", "b", "color_s", "green", "size_s", "M", "price_i", 40, "tag_ss", vecs("y")),
      flatdoc("id", "4", "key_s", "c", "color_s", "red", "size_s", "M", "price_i", 50, "tag_ss", vecs("x", "y")),
      flatdoc("id", "5", "key_s", "c", "color_s", "red", "size_s", "M", "price_i", 60, "tag_ss", vecs("x")),
      flatdoc("id", "6", "key_s", "c", "size_s", "M", "price_i", 70)}, UpdateMessage::COMMIT).success);
  auto full = run("main", R"json("limit":-1)json");
  std::vector<std::string> fields{"key_s"};
  for (int32_t parallel : {1, -1}) {
    auto request = run("main", R"json("limit":0,"collapse":"key_s",
        "filter":[{"query":"color_s:red","except_ops":["color"]},"size_s:M"],
        "ops":{"color":{"field_facet":{"field":"color_s","limit":-1,"missing":true}},
               "avg":"avg(price_i)"})json", parallel);
    check(*request->docList()->ops.at("color")->facetResult(),
        oracle(*full->docList(), fields, "expand", "color_s",
            [](const api::Map& doc) { return values(doc, "size_s") == std::vector<Part>{std::string("M")}; }));
    EXPECT_DOUBLE_EQ((30.0 + 50.0 + 60.0) / 3, request->docList()->ops.at("avg")->asDouble());

    for (std::string mode : {"any", "all"}) {
      auto selected = run("main", R"json("limit":0,"collapse":"key_s","ops":{
          "color":{"field_facet":{"field":"color_s","limit":0,"mincount":100,"selected":["red","absent"]}},
          "tags":{"field_facet":{"field":"tag_ss","limit":-1,"missing":true,"selected":["x","y"],"selection_mode":")json"
          + mode + R"json("}}})json", parallel);
      const auto& color = *selected->docList()->ops.at("color")->facetResult();
      EXPECT_EQ((std::map<Part, int64_t>{{std::string("red"), 3}, {std::string("absent"), 0}}), counts(color));
      auto expected = oracle(*full->docList(), fields, "expand", "tag_ss", [&](const api::Map& doc) {
        if (values(doc, "color_s") != std::vector<Part>{std::string("red")}) return false;
        auto tags = values(doc, "tag_ss");
        return mode == "any" || (std::ranges::find(tags, Part{std::string("x")}) != tags.end()
            && std::ranges::find(tags, Part{std::string("y")}) != tags.end());
      });
      check(*selected->docList()->ops.at("tags")->facetResult(), expected);
    }
    auto impossible = run("main", R"json("limit":0,"collapse":"key_s","ops":{
        "color":{"field_facet":{"field":"color_s","selected":["red","blue"],"selection_mode":"all"}}})json", parallel);
    EXPECT_EQ((std::map<Part, int64_t>{{std::string("red"), 0}, {std::string("blue"), 0}}),
        counts(*impossible->docList()->ops.at("color")->facetResult()));
  }
}

TEST_F(DistinctFacetTest, inheritedOverridesNearestCollapseAndChildLists) {
  CollectionHelper helper;
  helper.indexAll(std::array{
      flatdoc("id", "0", "key_s", "a", "color_s", "red", "size_s", "S", "price_i", 10),
      flatdoc("id", "1", "key_s", "a", "color_s", "red", "size_s", "M", "price_i", 20),
      flatdoc("id", "2", "key_s", "b", "color_s", "red", "size_s", "M", "price_i", 90)}, UpdateMessage::COMMIT);
  for (int32_t parallel : {1, -1}) {
    auto request = run("main", R"json("limit":0,"collapse":"key_s","ops":{
        "child":{"top_docs":{"limit":1,"count_limit":-1,"ops":{"c":{"field_facet":{"field":"color_s"}}}}},
        "nearest":{"top_docs":{"limit":0,"collapse":"id","ops":{"c":{"field_facet":{"field":"color_s"}}}}},
        "color":{"field_facet":{"field":"color_s","count_unit":"docs","ops":{
            "docs":{"field_facet":{"field":"color_s"}},
            "groups":{"field_facet":{"field":"color_s","count_unit":"groups"}},
            "avg":"avg(price_i)"}}}
        })json", parallel);
    const auto& child = *request->docList()->ops.at("child")->docList();
    EXPECT_EQ(1, child.row_count);
    EXPECT_EQ(3, child.found.value_or(-1));
    EXPECT_TRUE(!child.groups);
    EXPECT_EQ(2, child.ops.at("c")->facetResult()->counts[0]);
    const auto& nearest = *request->docList()->ops.at("nearest")->docList();
    EXPECT_EQ(3, nearest.ops.at("c")->facetResult()->counts[0]);
    const auto& color = *request->docList()->ops.at("color")->facetResult();
    EXPECT_EQ(3, color.counts[0]);
    EXPECT_EQ(3, std::get<api::ArrVal>(color.ops.at("docs")->kind).v[0].facetResult()->counts[0]);
    EXPECT_EQ(2, std::get<api::ArrVal>(color.ops.at("groups")->kind).v[0].facetResult()->counts[0]);
    EXPECT_DOUBLE_EQ(40, std::get<api::ArrVal>(color.ops.at("avg")->kind).v[0].asDouble());
    auto groupedMetric = run("main", R"json("limit":0,"collapse":"key_s","ops":{
        "c":{"field_facet":{"field":"color_s","ops":{"avg":"avg(price_i)",
            "members":{"top_docs":{"limit":1,"count_limit":-1,"ops":{
                "c":{"field_facet":{"field":"color_s"}}}}}}}}})json", parallel);
    const auto& facet = *groupedMetric->docList()->ops.at("c")->facetResult();
    EXPECT_EQ(2, facet.counts[0]);
    EXPECT_DOUBLE_EQ(40, std::get<api::ArrVal>(facet.ops.at("avg")->kind).v[0].asDouble());
    const auto& members = *std::get<api::ArrVal>(facet.ops.at("members")->kind).v[0].docList();
    EXPECT_EQ(1, members.row_count);
    EXPECT_EQ(3, members.found.value_or(-1));
    EXPECT_TRUE(!members.groups);
    EXPECT_EQ(2, members.ops.at("c")->facetResult()->counts[0]);
  }
}

TEST_F(DistinctFacetTest, validationAndMemoryFailure) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "0", "key_s", "a", "color_s", "red", "price_i", 1), UpdateMessage::COMMIT);
  for (std::string_view body : {
      R"json("limit":0,"ops":{"c":{"field_facet":{"field":"color_s","count_unit":"groups"}}})json",
      R"json("limit":0,"ops":{"c":{"field_facet":{"field":"color_s","count_unit":"docs","count_distinct":"key_s"}}})json",
      R"json("limit":0,"ops":{"r":{"range_facet":{"field":"price_i","start":0,"end":10,"gap":5,"count_unit":"groups"}}})json",
      R"json("limit":0,"ops":{"q":{"query_facet":{"buckets":{"all":"*:*"},"count_unit":"docs","count_distinct":"key_s"}}})json",
      R"json("limit":0,"collapse":"key_s","ops":{"q":{"query_facet":{"buckets":{"all":"*:*"},"count_unit":"groups","count_distinct":"id"}}})json",
      R"json("limit":0,"collapse":"key_s","ops":{"c":{"field_facet":{"field":"color_s","sort":[{"expr":"avg","dir":"desc"}],"ops":{"avg":"avg(price_i)"}}}})json",
      R"json("limit":0,"collapse":"key_s","ops":{"c":{"field_facet":{"field":"color_s","count_unit":"groups","count_distinct":"id"}}})json",
      R"json("limit":0,"ops":{"c":{"field_facet":{"field":"color_s","count_distinct":{"field":[]}}}})json"}) {
    auto request = run("main", body, 1, false);
    EXPECT_FALSE(request->errorMsg().empty());
  }
  auto compatible = run("main", R"json("limit":0,"collapse":"key_s","ops":{"c":{"field_facet":{
      "field":"color_s","count_unit":"groups","count_distinct":"key_s"}}})json");
  EXPECT_EQ(1, compatible->docList()->ops.at("c")->facetResult()->counts[0]);
  SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 1;
  for (int32_t parallel : {1, -1}) {
    auto request = run("main", R"json("limit":0,"collapse":"key_s","ops":{"c":{"field_facet":{"field":"color_s"}}})json", parallel, false);
    EXPECT_NE(std::string::npos, request->errorMsg().find("facet distinct state"));
    EXPECT_NE(std::string::npos, request->errorMsg().find("facet 'c'"));
    EXPECT_EQ(0u, request->memoryTracker.bytes());
  }
}

TEST_F(DistinctFacetTest, countOrderMincountMissingAndEmptyIndex) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 7; i++) {
    auto doc = flatdoc("id", std::to_string(i), "key_s", i < 5 ? "a" : i == 5 ? "b" : "c");
    if (i != 0) doc.emplace_back("color_s", i < 5 ? "red" : "blue");
    docs.push_back(std::move(doc));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  for (int32_t parallel : {1, -1}) {
    auto request = run("main", R"json("limit":0,"collapse":"key_s","ops":{
        "c":{"field_facet":{"field":"color_s","missing":true}},
        "min":{"field_facet":{"field":"color_s","mincount":2}},
        "docs":{"field_facet":{"field":"color_s","missing":true,"count_unit":"docs"}},
        "absent":{"field_facet":{"field":"unseen_s","missing":true}}})json", parallel);
    const auto& grouped = *request->docList()->ops.at("c")->facetResult();
    EXPECT_EQ("blue", std::get<api::ColStr>(grouped.bucket_ids->kind).v[0]);
    EXPECT_EQ((std::map<Part, int64_t>{{std::string("blue"), 2}, {std::string("red"), 1}}), counts(grouped));
    EXPECT_EQ(1, grouped.missing.value_or(-1));
    EXPECT_EQ((std::map<Part, int64_t>{{std::string("blue"), 2}}), counts(*request->docList()->ops.at("min")->facetResult()));
    const auto& documents = *request->docList()->ops.at("docs")->facetResult();
    EXPECT_EQ("red", std::get<api::ColStr>(documents.bucket_ids->kind).v[0]);
    EXPECT_EQ((std::map<Part, int64_t>{{std::string("blue"), 2}, {std::string("red"), 4}}), counts(documents));
    EXPECT_EQ(1, documents.missing.value_or(-1));
    EXPECT_EQ(3, request->docList()->ops.at("absent")->facetResult()->missing.value_or(-1));
    EXPECT_EQ(request->memoryTracker.bytes(), 0u);
  }
  CollectionHelper empty("empty_distinct");
  auto request = run("empty_distinct", R"json("limit":0,"collapse":"key_s","ops":{
      "c":{"field_facet":{"field":"color_s","mincount":0,"missing":true,"selected":"red"}}})json");
  EXPECT_EQ((std::map<Part, int64_t>{{std::string("red"), 0}}), counts(*request->docList()->ops.at("c")->facetResult()));
}

TEST_F(DistinctFacetTest, independentRootMeasureAndWireRoundTrips) {
  CollectionHelper helper;
  helper.indexAll(std::array{
      flatdoc("id", "0", "key_s", "a", "color_s", "red", "size_i", 1),
      flatdoc("id", "1", "key_s", "a", "color_s", "red", "size_i", 1),
      flatdoc("id", "2", "key_s", "b", "color_s", "red", "size_i", 1)}, UpdateMessage::COMMIT);
  auto source = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(source->rawRequest(), R"json({"collection":"main","ops":{
      "f":{"field_facet":{"field":"color_s","count_distinct":"key_s","ops":{
          "sizes":{"field_facet":{"field":"size_i"}}}}},
      "documents":{"field_facet":{"field":"color_s","count_unit":"docs"}},
      "range":{"range_facet":{"field":"size_i","start":0,"end":2,"gap":2,"count_distinct":"key_s"}},
      "query":{"query_facet":{"buckets":{"all":"*:*"},"count_distinct":"key_s"}},
      "range_docs":{"range_facet":{"field":"size_i","start":0,"end":2,"gap":2,"count_unit":"docs"}},
      "query_docs":{"query_facet":{"buckets":{"all":"*:*"},"count_unit":"docs"}},
      "q":{"top_docs":{"limit":0,"collapse":"key_s","ops":{
          "f":{"field_facet":{"field":"color_s","count_unit":"groups"}},
          "r_docs":{"range_facet":{"field":"size_i","start":0,"end":2,"gap":2,"count_unit":"docs"}},
          "q_docs":{"query_facet":{"buckets":{"all":"*:*"},"count_unit":"docs"}}}}}
      }})json", source->mr));
  source->execute(-1);
  ASSERT_TRUE(source->ok()) << source->errorMsg();
  EXPECT_EQ(3, source->responses[0]->proto.ops.at("documents")->facetResult()->counts[0]);
  const auto& root = *source->responses[0]->proto.ops.at("f")->facetResult();
  EXPECT_EQ(2, root.counts[0]);
  EXPECT_EQ(2, std::get<api::ArrVal>(root.ops.at("sizes")->kind).v[0].facetResult()->counts[0]);
  for (bool protobuf : {false, true}) {
    auto copy = localReq(helper.getSearchEngine());
    std::vector<std::byte> bytes;
    std::string json;
    if (protobuf) {
      ASSERT_TRUE(api::encode(source->rawRequest(), bytes));
      size_t size = bytes.size();
      bytes.resize(size + 16);
      ASSERT_TRUE(api::decode(copy->rawRequest(), std::span(bytes).first(size), copy->mr));
    } else {
      ASSERT_TRUE(api::write_json(source->rawRequest(), json));
      ASSERT_TRUE(api::read_json(copy->rawRequest(), json, copy->mr));
    }
    copy->execute(-1);
    ASSERT_TRUE(copy->ok()) << copy->errorMsg();
    EXPECT_EQ(3, copy->responses[0]->proto.ops.at("documents")->facetResult()->counts[0]);
    EXPECT_EQ(2, copy->responses[0]->proto.ops.at("f")->facetResult()->counts[0]);
    for (auto name : {"range", "query"}) EXPECT_EQ(2, copy->responses[0]->proto.ops.at(name)->facetResult()->counts[0]);
    for (auto name : {"range_docs", "query_docs"}) EXPECT_EQ(3, copy->responses[0]->proto.ops.at(name)->facetResult()->counts[0]);
    EXPECT_EQ(2, copy->docList()->ops.at("f")->facetResult()->counts[0]);
    for (auto name : {"r_docs", "q_docs"}) EXPECT_EQ(3, copy->docList()->ops.at(name)->facetResult()->counts[0]);
  }
}

TEST_F(DistinctFacetTest, mergeBudgetFailureUnwindsBothWorkers) {
  RequestMemTracker tracker(1800);
  AtomicMerger<DistinctFacetCounts> merger(
      [&]() { return new DistinctFacetCounts(tracker, "merge", DistinctFacetMode::HASH, {true, 0, 1}, {true, 0, 100}); },
      [](DistinctFacetCounts* data) { delete data; });
  std::unique_ptr<DistinctFacetCounts> a(merger.obtain()), b(merger.obtain());
  a->collect([](auto add, auto) { for (uint32_t i = 0; i < 20; i++) add(0, i); });
  b->collect([](auto add, auto) { for (uint32_t i = 20; i < 40; i++) add(0, i); });
  EXPECT_EQ(1, merger.release(a.release()));
  EXPECT_THROW(merger.release(b.release()), ResourceExhaustedError);
  EXPECT_EQ(0u, tracker.bytes());
  EXPECT_LE(tracker.peak(), tracker.ceiling());
}

TEST_F(DistinctFacetTest, runsRequireOneSegmentOwnerPerGroup) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int64_t group = segment * 10; group < (segment + 1) * 10; group++) {
      for (int32_t variant = 0; variant < 4; variant++) docs.push_back(flatdoc(
          "id", std::to_string(group * 4 + variant), "key_s", "g" + std::to_string(group),
          "key_i", group, "color_s", variant % 2 ? "blue" : "red", "price_i", variant * 10));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  SearchOverridesGuard guard(distinctFacetRunContractForTests);
  // Assertion by the test: runs are contiguous and groups have disjoint
  // segment owners. Sorting each segment alone would not be sufficient.
  distinctFacetRunContractForTests = true;
  for (auto key : {R"("key_s")", R"("key_i")", R"({"field":["key_s","key_i"]})"}) {
    auto request = run("main", R"("limit":0,"collapse":)" + std::string(key) + R"(,"ops":{
        "c":{"field_facet":{"field":"color_s"}},
        "r":{"range_facet":{"field":"price_i","start":0,"end":40,"gap":20}},
        "q":{"query_facet":{"buckets":{"all":"*:*","red":"color_s:red"}}}})", -1);
    EXPECT_EQ((std::map<Part, int64_t>{{std::string("blue"), 20}, {std::string("red"), 20}}),
        counts(*request->docList()->ops.at("c")->facetResult()));
    EXPECT_EQ(20, request->docList()->ops.at("r")->facetResult()->counts[0]);
    EXPECT_EQ(20, request->docList()->ops.at("q")->facetResult()->counts[1]);
  }
}

TEST_F(DistinctFacetTest, rangeQueryAndNestedOracle) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 80; i++) {
    auto doc = flatdoc("id", std::to_string(i), "color_s", i % 2 ? "red" : "blue");
    if (i % 7) doc.emplace_back("key_s", "g" + std::to_string(i % 9));
    if (i % 5) doc.emplace_back("key_i", (int64_t)(i % 3));
    if (i % 4) doc.emplace_back("price_is", vec((int64_t)(i % 40), (int64_t)(i % 40), (int64_t)5));
    docs.push_back(std::move(doc));
  }
  helper.indexAll(std::span(docs).first(40), UpdateMessage::COMMIT);
  helper.indexAll(std::span(docs).subspan(40), UpdateMessage::COMMIT);
  helper.deleteByIds({"2", "13"}, UpdateMessage::COMMIT);
  auto full = run("main", R"json("limit":-1)json");
  std::vector<std::string> fields{"key_s", "key_i"};
  SearchOverridesGuard guard(forcedDistinctFacetMode);
  for (auto mode : {DistinctFacetMode::PAGED, DistinctFacetMode::HASH}) {
    forcedDistinctFacetMode = mode;
    for (std::string policy : {"expand", "group", "drop"}) for (int32_t parallel : {1, -1}) {
      auto request = run("main", R"json("limit":0,"collapse":)json" + spec(fields, policy) + R"json(,"ops":{
          "r":{"range_facet":{"field":"price_is","start":0,"end":40,"gap":20,"missing":true,"ops":{
              "c":{"field_facet":{"field":"color_s","limit":-1,"missing":true}}}}},
          "q":{"query_facet":{"buckets":{"all":"*:*","red":"color_s:red","none":"color_s:absent"},
              "ops":{"c":{"field_facet":{"field":"color_s","limit":-1,"missing":true}}}}}
          })json", parallel);
      const auto& range = *request->docList()->ops.at("r")->facetResult();
      std::set<Key> missing;
      for (size_t bucket = 0; bucket < 2; bucket++) {
        auto inRange = [&](const api::Map& doc) {
          for (auto value : values(doc, "price_is")) {
            int64_t n = std::get<int64_t>(value);
            if (n >= (int64_t)bucket * 20 && n < (int64_t)(bucket + 1) * 20) return true;
          }
          return false;
        };
        auto expected = oracle(*full->docList(), fields, policy, "color_s", inRange);
        std::set<Key> groups;
        for (const auto& [color, ids] : expected.buckets) { unused(color); groups.insert(ids.begin(), ids.end()); }
        EXPECT_EQ((int64_t)groups.size(), range.counts[bucket]);
        check(*std::get<api::ArrVal>(range.ops.at("c")->kind).v[bucket].facetResult(), expected);
      }
      auto noPrice = oracle(*full->docList(), fields, policy, "color_s",
          [](const api::Map& doc) { return values(doc, "price_is").empty(); });
      for (const auto& [color, ids] : noPrice.buckets) { unused(color); missing.insert(ids.begin(), ids.end()); }
      EXPECT_EQ((int64_t)missing.size(), range.missing.value_or(-1));
      const auto& query = *request->docList()->ops.at("q")->facetResult();
      for (size_t bucket = 0; bucket < 3; bucket++) {
        auto expected = oracle(*full->docList(), fields, policy, "color_s", [&](const api::Map& doc) {
          return bucket == 0 || (bucket == 1 && values(doc, "color_s") == std::vector<Part>{std::string("red")});
        });
        std::set<Key> groups;
        for (const auto& [color, ids] : expected.buckets) { unused(color); groups.insert(ids.begin(), ids.end()); }
        EXPECT_EQ((int64_t)groups.size(), query.counts[bucket]);
        check(*std::get<api::ArrVal>(query.ops.at("c")->kind).v[bucket].facetResult(), expected);
      }
    }
  }
}

TEST_F(DistinctFacetTest, rangeQueryRoutingSelectedAndDocsOverrides) {
  CollectionHelper helper;
  std::vector<Doc> docs{
      flatdoc("id", "0", "key_s", "a", "color_s", "red", "price_is", vec((int64_t)5, (int64_t)25)),
      flatdoc("id", "1", "key_s", "a", "color_s", "blue", "price_is", vec((int64_t)5)),
      flatdoc("id", "2", "key_s", "b", "color_s", "red", "price_is", vec((int64_t)25)),
      flatdoc("id", "3", "key_s", "b", "color_s", "blue", "price_is", vec((int64_t)5, (int64_t)25)),
      flatdoc("id", "4", "key_s", "c", "color_s", "red"),
      flatdoc("id", "5", "key_s", "c", "color_s", "blue", "price_is", vec((int64_t)25))};
  helper.indexAll(std::span(docs).first(3), UpdateMessage::COMMIT);
  helper.indexAll(std::span(docs).subspan(3), UpdateMessage::COMMIT);
  auto full = run("main", R"("limit":-1)");
  std::vector<std::string> fields{"key_s"};
  auto number = [&](auto predicate, bool groups) {
    if (!groups) {
      int64_t result = 0;
      for (const auto& doc : full->docList()->docs) result += predicate(doc);
      return result;
    }
    auto expected = oracle(*full->docList(), fields, "expand", "color_s", predicate);
    std::set<Key> distinct;
    for (const auto& [color, ids] : expected.buckets) { unused(color); distinct.insert(ids.begin(), ids.end()); }
    return (int64_t)distinct.size();
  };
  auto in = [](const api::Map& doc, int64_t start) {
    for (auto v : values(doc, "price_is")) if (auto n = std::get<int64_t>(v); n >= start && n < start + 20) return true;
    return false;
  };
  auto red = [](const api::Map& doc) { return values(doc, "color_s") == std::vector<Part>{std::string("red")}; };
  for (bool groups : {false, true}) for (int32_t parallel : {1, -1}) {
    std::string unit = groups ? "groups" : "docs";
    auto routed = run("main", R"("limit":0,"collapse":"key_s","filter":[
        {"query":"color_s:red","except_ops":["r"]},
        {"query":"price_is:>=20","except_ops":["q"]}],"ops":{
        "r":{"range_facet":{"field":"price_is","start":0,"end":60,"gap":20,"missing":true,"count_unit":")"
        + unit + R"("}},
        "q":{"query_facet":{"buckets":{"red":"color_s:red","low":"price_is:<20","none":"color_s:absent"},"count_unit":")"
        + unit + R"("}}})", parallel);
    const auto& r = *routed->docList()->ops.at("r")->facetResult();
    const auto& q = *routed->docList()->ops.at("q")->facetResult();
    for (int32_t bucket = 0; bucket < 3; bucket++) EXPECT_EQ(number(
        [&](const api::Map& doc) { return in(doc, 20) && in(doc, bucket * 20); }, groups), r.counts[(size_t)bucket]);
    EXPECT_EQ(0, r.missing.value_or(-1));
    EXPECT_EQ(number(red, groups), q.counts[0]);
    EXPECT_EQ(number([&](const api::Map& doc) { return red(doc) && in(doc, 0); }, groups), q.counts[1]);
    EXPECT_EQ(0, q.counts[2]);

    for (std::string mode : {"any", "all"}) {
      auto request = run("main", R"("limit":0,"collapse":"key_s","ops":{
          "r":{"range_facet":{"field":"price_is","start":0,"end":60,"gap":20,"mincount":100,
              "selected":[0,20],"selection_mode":")" + mode + R"(","count_unit":")" + unit + R"(","ops":{
                  "c":{"field_facet":{"field":"color_s","limit":-1,"missing":true}}}}},
          "q":{"query_facet":{"buckets":{"red":"color_s:red","low":"price_is:<20","none":"color_s:absent"},
              "selected":)" + (mode == "any" ? R"(["red","low","none"])" : R"(["red","low"])")
              + R"(,"selection_mode":")" + mode + R"(","count_unit":")" + unit + R"("}}})", parallel);
      const auto& range = *request->docList()->ops.at("r")->facetResult();
      ASSERT_EQ(2u, range.counts.size()); // Both selected buckets survive mincount.
      const auto& query = *request->docList()->ops.at("q")->facetResult();
      for (size_t bucket = 0; bucket < 2; bucket++) {
        auto domain = [&](const api::Map& doc) {
          bool selected = mode == "any" ? red(doc) || in(doc, 0) : red(doc) && in(doc, 0) && in(doc, 20);
          return selected && in(doc, (int64_t)bucket * 20);
        };
        EXPECT_EQ(number(domain, groups), range.counts[bucket]);
        const auto& child = *std::get<api::ArrVal>(range.ops.at("c")->kind).v[bucket].facetResult();
        if (groups) check(child, oracle(*full->docList(), fields, "expand", "color_s", domain));
        else {
          int64_t actual = 0;
          for (int64_t count : child.counts) actual += count;
          EXPECT_EQ(number(domain, false), actual); // Nested facets inherit docs.
        }
      }
      auto rangeSelected = [&](const api::Map& doc) {
        return mode == "any" ? in(doc, 0) || in(doc, 20) : in(doc, 0) && in(doc, 20);
      };
      auto queryDomain = [&](const api::Map& doc) {
        return rangeSelected(doc) && (mode == "any" || (red(doc) && in(doc, 0)));
      };
      EXPECT_EQ(number([&](const api::Map& doc) { return queryDomain(doc) && red(doc); }, groups), query.counts[0]);
      EXPECT_EQ(number([&](const api::Map& doc) { return queryDomain(doc) && in(doc, 0); }, groups), query.counts[1]);
      EXPECT_EQ(0, query.counts[2]); // Selected empty bucket remains exact.
    }
  }
}

TEST_F(DistinctFacetTest, preparedQueryBucketsCountFinalizedMembers) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vectors = schema.templ("_v");
  vectors.type = api::FieldDef::FieldClass::VECTOR;
  vectors.column = true;
  vectors.metric = api::VectorMetric::L2;
  vectors.dims = 2;
  schema.set(helper.collection());
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int32_t i = segment * 3; i < (segment + 1) * 3; i++) docs.push_back(flatdoc(
        "id", std::to_string(i), "key_s", i < 4 ? "a" : "b", "color_s", i % 2 ? "blue" : "red",
        "embedding_v", std::vector<float>{(float)i, 0}));
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  SearchOverridesGuard guard(forcedDistinctFacetMode);
  for (auto mode : {DistinctFacetMode::PAGED, DistinctFacetMode::HASH}) {
    forcedDistinctFacetMode = mode;
    for (int32_t parallel : {1, -1}) {
      auto request = run("main", R"("limit":0,"collapse":"key_s","ops":{"q":{"query_facet":{
          "buckets":{"near":{"knn":{"field":"embedding_v","query":[0,0],"k":3,"exact":true}},"all":"*:*"},
          "ops":{"c":{"field_facet":{"field":"color_s","limit":-1}}}}}})", parallel);
      const auto& query = *request->docList()->ops.at("q")->facetResult();
      EXPECT_EQ(1, query.counts[0]); // Three finalized neighbors, one group.
      EXPECT_EQ(2, query.counts[1]);
      const auto& nested = *std::get<api::ArrVal>(query.ops.at("c")->kind).v[0].facetResult();
      EXPECT_EQ((std::map<Part, int64_t>{{std::string("red"), 1}, {std::string("blue"), 1}}), counts(nested));
    }
  }
}

TEST_F(DistinctFacetTest, compactAllocationFailuresAreAccounted) {
  SearchOverridesGuard guard(distinctFacetRunContractForTests);
  distinctFacetRunContractForTests = true;
  for (auto mode : {DistinctFacetMode::PAGED, DistinctFacetMode::RUN}) {
    RequestMemTracker tracker(200);
    EXPECT_THROW({
      DistinctFacetCounts data(tracker, "compact", mode, {true, 0, mode == DistinctFacetMode::RUN ? 20ULL : 2ULL}, {true, 0, 10000});
      data.collect([](auto add, auto) { for (uint32_t group = 0; group < 64; group++) add(0, group); });
    }, ResourceExhaustedError);
    EXPECT_EQ(0u, tracker.bytes());
    EXPECT_LE(tracker.peak(), tracker.ceiling());
  }
}

TEST_F(DistinctFacetTest, wideNumericBucketIdentitiesPreserveValuesAndPins) {
  CollectionHelper helper;
  helper.indexAll(std::array{
      flatdoc("id", "0", "key_s", "same", "key_i", INT64_MIN, "value_i", INT64_MIN),
      flatdoc("id", "1", "key_s", "same", "key_i", INT64_MIN, "value_i", INT64_MIN),
      flatdoc("id", "2", "key_s", "same", "key_i", INT64_MAX, "value_i", INT64_MAX)}, UpdateMessage::COMMIT);
  SearchOverridesGuard guard(forcedDistinctFacetMode);
  for (auto key : {std::string(R"("key_i")"), std::string(R"({"field":["key_s","key_i"]})")}) {
  for (auto mode : {DistinctFacetMode::PAGED, DistinctFacetMode::HASH}) {
    forcedDistinctFacetMode = mode;
    auto request = run("main", R"("limit":0,"collapse":)" + key + R"(,"ops":{
        "v":{"field_facet":{"field":"value_i","limit":-1,"selected":0}}})", -1);
    const auto& result = *request->docList()->ops.at("v")->facetResult();
    EXPECT_EQ((std::map<Part, int64_t>{{INT64_MIN, 1}, {INT64_MAX, 1}, {(int64_t)0, 0}}), counts(result));
    const auto& ids = std::get<api::ColInt>(result.bucket_ids->kind).v;
    ASSERT_EQ(3u, ids.size());
    EXPECT_EQ(INT64_MIN, ids[0]);
    EXPECT_EQ(INT64_MAX, ids[1]);
  }
  }
}


TEST_F(DistinctFacetTest, fullWidthPairsAndTaggedMissingCannotCollide) {
  RequestMemTracker tracker(100000);
  DistinctFacetCounts a(tracker, "wide", DistinctFacetMode::HASH, {}, {});
  DistinctFacetCounts b(tracker, "wide", DistinctFacetMode::HASH, {}, {});
  auto fill = [](auto add, auto missing) {
    add(INT64_MIN, INT64_MIN);
    add(INT64_MIN, INT64_MAX);
    add(INT64_MIN, (int64_t)0);
    add(0, (int64_t)1 << 40);
    add(256, (int64_t)0);
    add(INT64_MAX, (int64_t)1 << 48);
    add((int64_t)1 << 40, (int64_t)1 << 50);
    add(INT64_MIN, GroupIdentity{GroupIdentity::NULL_KEY});
    add(INT64_MIN, GroupIdentity{GroupIdentity::SINGLETON, 0});
    add(INT64_MIN, GroupIdentity{GroupIdentity::SINGLETON, 0});
    missing(INT64_MIN);
    missing(INT64_MAX);
    missing(GroupIdentity{GroupIdentity::NULL_KEY});
  };
  a.collect(fill);
  // b repeats only non-singleton identities; their global union is exact.
  b.collect([](auto add, auto missing) { add(INT64_MIN, INT64_MIN); missing(INT64_MIN); });
  DistinctFacetCounts::merge(&a, &b);
  a.finish();
  EXPECT_EQ(5, a.count(INT64_MIN));
  EXPECT_EQ(1, a.count(0));
  EXPECT_EQ(1, a.count(256));
  EXPECT_EQ(1, a.count(INT64_MAX));
  EXPECT_EQ(1, a.count((int64_t)1 << 40));
  EXPECT_EQ(3, a.missing());
  EXPECT_EQ(DistinctFacetMode::HASH, DistinctFacetCounts::select({true, 0, 1ULL << 40}, {true, 0, 1ULL << 40}, false));
}

TEST_F(DistinctFacetTest, sparsePagesAndExpandDoNotWidenDimensions) {
  RequestMemTracker tracker(200000);
  {
    DistinctFacetCounts a(tracker, "narrow", DistinctFacetMode::PAGED, {true, 0, 20}, {true, 0, 1000000});
    DistinctFacetCounts b(tracker, "narrow", DistinctFacetMode::PAGED, {true, 0, 20}, {true, 0, 1000000});
    a.collect([](auto add, auto) { add(3, 17); add(3, GroupIdentity{GroupIdentity::SINGLETON, 0}); });
    b.collect([](auto add, auto) { add(3, 17); add(3, 18); });
    DistinctFacetCounts::merge(&a, &b);
    a.finish();
    EXPECT_EQ(3, a.count(3));
    EXPECT_LT(tracker.peak(), 100000u);
  }
  EXPECT_EQ(0u, tracker.bytes());
}


TEST_F(DistinctFacetTest, jointParentKeepsUsefulDistinctChildBlock) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int64_t i = 0; i < 10000; i++) docs.push_back(flatdoc("id", std::to_string(i),
      "parent_i", i % 20, "key_i", i, "price_i", i % 128));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  SearchOverridesGuard guard(facetBucketBlockCounter);
  size_t blocks = 0;
  facetBucketBlockCounter = &blocks;
  auto request = run("main", R"("limit":0,"ops":{"p":{"field_facet":{"field":"parent_i","limit":-1,
      "ops":{"r":{"range_facet":{"field":"price_i","start":0,"end":128,"gap":1,
          "count_distinct":"key_i"}}}}}})");
  EXPECT_EQ(1u, blocks); // One joint parent column pass serves all 20 buckets.
  const auto& parent = *request->docList()->ops.at("p")->facetResult();
  const auto& children = std::get<api::ArrVal>(parent.ops.at("r")->kind).v;
  for (size_t i = 0; i < children.size(); i++) {
    int64_t sum = 0;
    for (int64_t count : children[i].facetResult()->counts) sum += count;
    EXPECT_EQ(parent.counts[i], sum);
  }
}

TEST_F(DistinctFacetTest, wideTupleWorkerIdsAreRemappedBeforeUnion) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "0", "key_s", "a", "key_i", INT64_MIN),
      flatdoc("id", "1", "key_s", "b", "key_i", INT64_MAX)}, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan key(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  key.fields.push_back({"key_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("key_s"),
                        reader->field("key_s")});
  key.fields.push_back({"key_i", api::GroupComponent_::Type::INT, {}, reader->field("key_i")});
  FacetGroupPlan plan(key);
  CollapseKey first, second;
  first.components = {{true, 0}, {true, INT64_MIN}};
  second.components = {{true, 1}, {true, INT64_MAX}};
  RequestMemTracker tracker(100000);
  DistinctFacetCounts a(tracker, "tuple worker", DistinctFacetMode::HASH, {}, {});
  DistinctFacetCounts b(tracker, "tuple worker", DistinctFacetMode::HASH, {}, {});
  a.collect([&](auto add, auto missing) {
    auto one = a.identity(plan, &first), two = a.identity(plan, &second);
    add(10, one); add(20, two); missing(one);
  });
  b.collect([&](auto add, auto missing) {
    auto two = b.identity(plan, &second), one = b.identity(plan, &first);
    add(10, two); add(20, one); missing(two);
  });
  auto* merged = DistinctFacetCounts::merge(&a, &b);
  merged->finish();
  EXPECT_EQ(2, merged->count(10));
  EXPECT_EQ(2, merged->count(20));
  EXPECT_EQ(2, merged->missing());
}

TEST_F(DistinctFacetTest, scalarNumericDimensionsPreserveExtremaAndMissingChannels) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int64_t i = 0; i < 32; i++) {
      auto doc = flatdoc("id", std::to_string(segment * 32 + i), "color_s", i % 2 ? "red" : "blue");
      if (i % 7 != 0) {
        doc.emplace_back("near_min_i", INT64_MIN + i % 5);
        doc.emplace_back("near_max_i", INT64_MAX - i % 5);
        doc.emplace_back("cross_i", i % 5 - 2);
        doc.emplace_back("when_dt", (i % 5) * 1000);
        doc.emplace_back("wide_i", i % 2 ? INT64_MIN : INT64_MAX);
      }
      docs.push_back(std::move(doc));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto full = run("main", R"("limit":-1)");
  SearchOverridesGuard modeGuard(forcedDistinctFacetMode);
  SearchOverridesGuard statsGuard(distinctFacetStatsForTests);
  forcedDistinctFacetMode = DistinctFacetMode::AUTO;
  for (auto field : {"near_min_i", "near_max_i", "cross_i", "when_dt", "wide_i"}) {
    for (auto policy : {"expand", "group", "drop"}) {
      DistinctFacetStats stats;
      distinctFacetStatsForTests = &stats;
      auto request = run("main", R"("limit":0,"ops":{"c":{"field_facet":{"field":"color_s","limit":-1,
          "missing":true,"count_distinct":)" + spec(std::vector<std::string>{field}, policy) + "}}}");
      check(*request->docList()->ops.at("c")->facetResult(),
          oracle(*full->docList(), std::vector<std::string>{field}, policy, "color_s", [](const api::Map&) { return true; }));
      if (field == std::string_view("wide_i")) { EXPECT_GT(stats.hash.load(), 0); }
      else { EXPECT_GT(stats.paged.load(), 0); }
    }
  }
}

TEST_F(DistinctFacetTest, sparseNumericWalksWithTuplesAndDeletes) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int64_t i = segment * 4096; i < (segment + 1) * 4096; i++) {
      auto doc = flatdoc("id", std::to_string(i), "marker_s", i % 257 == 0 ? "hit" : "other");
      if (i % 5 != 0) doc.emplace_back("key_i", INT64_MIN + i % 19);
      if (i % 7 != 0) doc.emplace_back("when_dt", (i % 3) * 1000);
      if (i % 3 != 0) doc.emplace_back("color_s", i % 2 ? "red" : "blue");
      if (i % 11 != 0) doc.emplace_back("size_i", i % 4);
      docs.push_back(std::move(doc));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds({"257", "5397"}, UpdateMessage::COMMIT).success);
  auto full = run("main", R"("query":"marker_s:hit","limit":-1)");
  for (const auto& fields : std::vector<std::vector<std::string>>{{"key_i"}, {"when_dt"}, {"key_i", "when_dt"}}) {
    for (auto policy : {"expand", "group", "drop"}) for (int32_t parallel : {1, -1}) {
      auto request = run("main", R"("query":"marker_s:hit","limit":0,"collapse":)" + spec(fields, policy)
          + R"(,"ops":{"c":{"field_facet":{"field":"color_s","limit":-1,"missing":true}},
              "s":{"field_facet":{"field":"size_i","limit":-1,"missing":true}}})", parallel);
      for (auto [name, field] : {std::pair{"c", "color_s"}, std::pair{"s", "size_i"}}) {
        check(*request->docList()->ops.at(name)->facetResult(),
            oracle(*full->docList(), fields, policy, field, [](const api::Map&) { return true; }));
      }
    }
  }
}
