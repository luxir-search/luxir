// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "test/SchemaBuilder.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/query/QueryPrep.h"
#include <set>
#include <random>

using namespace luxir;
using namespace luxir::test;

namespace {
using Part = std::variant<std::monostate, std::string, int64_t>;
struct Key {
  bool singleton = false;
  std::vector<Part> parts;
  size_t docOrder = 0;
  auto operator<=>(const Key&) const = default;
};
struct Row {
  std::string id;
  float score;
  std::vector<Part> parts;
  bool singleton = false;
  bool operator==(const Row&) const = default;
};

std::string spec(const std::vector<std::string>& fields, std::string_view missing) {
  std::string out = R"({"field":[)";
  for (size_t i = 0; i < fields.size(); i++) {
    if (i) out += ',';
    out += '"' + fields[i] + '"';
  }
  return out + R"(],"missing":")" + std::string(missing) + R"("})";
}

Part part(const luxir::api::Val* value) {
  if (value == nullptr || value->isNull()) return {};
  if (auto* s = std::get_if<std::string_view>(&value->kind)) return std::string(*s);
  return value->asInt();
}

std::vector<Row> rows(LocalReq& request, bool collapsed) {
  std::vector<Row> result;
  for (auto* response : request.responses) {
    const auto& list = *response->proto.ops.at("q")->docList();
    if (collapsed) { EXPECT_EQ(list.groups->keys.size(), (size_t)list.row_count); }
    for (size_t i = 0; i < list.docs.size(); i++) {
      const auto& fields = list.docs[i].fields;
      Row row{std::string(fields.at("id")->asString()),
              fields.find("_score_") ? std::get<float>(fields.at("_score_")->kind) : 0.0f, {}, false};
      if (collapsed) {
        row.singleton = list.groups->keys[i].singleton.has_value();
        for (const auto& component : list.groups->keys[i].component) {
          if (auto* s = std::get_if<std::string_view>(&component.value)) row.parts.emplace_back(std::string(*s));
          else if (auto* n = std::get_if<int64_t>(&component.value)) row.parts.emplace_back(*n);
          else row.parts.emplace_back();
        }
      }
      result.push_back(std::move(row));
    }
  }
  return result;
}

struct Oracle {
  std::vector<Row> groups;
  int64_t eligible = 0;
};

// Exhaustive ordinary ranking, dropping later same-key documents. This
// computes expected heads without consulting collapse admission or pruning.
Oracle oracle(LocalReq& full, const std::vector<std::string>& keyFields,
              std::string_view missing) {
  std::set<Key> seen;
  size_t docOrder = 0;
  Oracle result;
  for (auto* response : full.responses) {
    for (const auto& doc : response->proto.ops.at("q")->docList()->docs) {
      Key key;
      bool absent = false;
      for (const auto& field : keyFields) {
        auto* value = doc.fields.find(field);
        key.parts.push_back(part(value ? &**value : nullptr));
        absent |= std::holds_alternative<std::monostate>(key.parts.back());
      }
      if (!absent || missing != "drop") {
        result.eligible++;
        if (absent && missing == "expand") {
          key.parts.clear(); key.singleton = true; key.docOrder = docOrder;
        }
        if (seen.insert(key).second) {
          auto* score = doc.fields.find("_score_");
          result.groups.push_back(Row{std::string(doc.fields.at("id")->asString()),
              score ? std::get<float>((*score)->kind) : 0.0f, key.parts, key.singleton});
        }
      }
      docOrder++;
    }
  }
  return result;
}

}

class CollapseTest : public LuxirTest {
protected:
  auto run(std::string_view collection, std::string_view query, std::string_view collapse,
           int64_t limit = -1, int64_t offset = 0, int32_t parallel = 1,
           bool count = true, int32_t batch = 13,
           std::string_view sort = {}, bool scores = true) {
    auto req = localReq(luxirNode->getSearchEngine());
    std::string queryJson;
    EXPECT_TRUE(luxir::api::write_json(luxir::api::Val{.kind = query}, queryJson));
    std::string json = R"({"collection":")" + std::string(collection)
        + R"(","query":)" + queryJson
        + R"(,"fields":["id","key_s","key_i","when_dt","sort_i","sort_s"],"document_format":"rows","get_scores":)"
        + (scores ? "true" : "false") + R"(,"count_limit":)"
        + (count ? "-1" : "0") + R"(,"limit":)" + std::to_string(limit)
        + R"(,"offset":)" + std::to_string(offset)
        + R"(,"batch_size":)" + std::to_string(batch);
    if (!sort.empty()) json += R"(,"sort":)" + std::string(sort);
    if (!collapse.empty()) json += R"(,"collapse":)" + std::string(collapse);
    json += '}';
    EXPECT_TRUE(luxir::api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg() << json;
    return req;
  }
};

TEST_F(CollapseTest, bruteForceOracle) {
  std::mt19937 random(937);
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 210; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "body_w",
        i % 4 == 0 ? "alpha beta" : i % 4 == 1 ? "alpha alpha beta" : "beta gamma gamma");
    if (random() % 5 != 0) doc.emplace_back("key_s", "g" + std::to_string(random() % 23));
    if (random() % 4 != 0) doc.emplace_back("key_i", (int64_t)(random() % 7) - 3);
    if (random() % 3 != 0) doc.emplace_back("when_dt", (int64_t)(random() % 4) * 86400000);
    docs.push_back(std::move(doc));
  }
  for (int32_t segments : {1, 3, 7}) {
    std::string collection = "collapse" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int32_t i = 0; i < segments; i++) {
      ASSERT_TRUE(helper.indexAll(std::span<const Doc>(docs).subspan(
          (size_t)(i * 210 / segments), (size_t)(210 / segments)), UpdateMessage::COMMIT).success);
    }
    ASSERT_TRUE(helper.deleteByIds({"d13", "d100"}, UpdateMessage::COMMIT).success);
    for (std::string query : {"*:*", "body_w:alpha", "body_w:alpha OR body_w:beta", "body_w:alpha AND key_i:>=0"}) {
      auto full = run(collection, query, {});
      // The oracle starts with ordinary exhaustive ranked documents. First
      // witness per exact key is its best member, including document ties.
      for (const std::vector<std::string>& fields : {
          std::vector<std::string>{"key_s"}, {"key_i"}, {"when_dt"}, {"id"}, {"key_s", "key_i"}}) {
        for (std::string missing : {"expand", "group", "drop"}) {
          auto expected = oracle(*full, fields, missing);
          const auto& all = expected.groups;
          int64_t eligible = expected.eligible;
          auto collapse = spec(fields, missing);
          for (int32_t parallel : {1, -1}) {
            SCOPED_TRACE(collection + " " + query + " " + collapse + " parallel=" + std::to_string(parallel));
            auto actual = run(collection, query, collapse, -1, 0, parallel);
            EXPECT_EQ(all, rows(*actual, true));
            EXPECT_EQ((int64_t)all.size(), actual->docList()->found);
            EXPECT_EQ(eligible, actual->docList()->found_docs);
            for (bool count : {false, true}) {
              auto page = run(collection, query, collapse, 7, 4, parallel, count);
              auto begin = all.begin() + std::min<size_t>(4, all.size());
              auto end = all.begin() + std::min<size_t>(11, all.size());
              EXPECT_EQ(std::vector<Row>(begin, end), rows(*page, true));
              EXPECT_EQ(count, page->docList()->found.has_value());
              EXPECT_EQ(count, page->docList()->found_docs.has_value());
            }
            auto totals = run(collection, query, collapse, 0, 0, parallel);
            EXPECT_EQ((int64_t)all.size(), totals->docList()->found);
            EXPECT_EQ(eligible, totals->docList()->found_docs);
            EXPECT_EQ(0, totals->docList()->row_count);
          }
          if (fields.size() == 2 && missing == "group") {
            std::vector<Row> pages;
            for (size_t offset = 0; offset < all.size(); offset += 9) {
              auto page = run(collection, query, collapse, 9, (int64_t)offset, -1, false, 3);
              auto current = rows(*page, true);
              pages.insert(pages.end(), current.begin(), current.end());
            }
            EXPECT_EQ(all, pages);
          }
        }
      }
    }
  }
}

TEST_F(CollapseTest, wireAndEmptyPages) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "key_i", INT64_MIN),
      flatdoc("id", "b", "key_i", INT64_MAX), flatdoc("id", "c", "key_s", "x", "key_i", INT64_MIN)},
      UpdateMessage::COMMIT).success);
  auto req = run("main", "*:*", R"({"field":["key_s","key_i"],"missing":"group"})");
  std::vector<std::byte> binary;
  ASSERT_TRUE(luxir::api::encode(req->rawRequest(), binary));
  size_t size = binary.size();
  binary.resize(size + 16);
  auto copied = localReq(luxirNode->getSearchEngine());
  ASSERT_TRUE(luxir::api::decode(copied->rawRequest(), std::span(binary).first(size), copied->mr));
  copied->execute(-1);
  EXPECT_EQ(rows(*req, true), rows(*copied, true));
  const auto& response = req->responses[0]->proto;
  binary.clear();
  ASSERT_TRUE(luxir::api::encode(response, binary));
  size = binary.size();
  binary.resize(size + 16);
  luxir::api::SearchResponse decoded;
  std::pmr::monotonic_buffer_resource arena;
  ASSERT_TRUE(luxir::api::decode(decoded, std::span(binary).first(size), arena));
  EXPECT_EQ(2, decoded.ops.at("q")->docList()->groups->keys.size());
  EXPECT_EQ(INT64_MIN, std::get<int64_t>(decoded.ops.at("q")->docList()->groups->keys[0].component[1].value));
  std::string listJson;
  ASSERT_TRUE(luxir::api::write_json(*req->docList(), listJson));
  luxir::api::DocList jsonList;
  ASSERT_TRUE(luxir::api::read_json(jsonList, listJson, arena)) << listJson;
  EXPECT_EQ(2, jsonList.groups->keys.size());
  EXPECT_EQ(3, jsonList.found_docs);
  EXPECT_EQ(INT64_MIN, std::get<int64_t>(jsonList.groups->keys[0].component[1].value));
  auto empty = run("main", "*:*", R"("key_s")", 10, 500);
  EXPECT_EQ(0, empty->docList()->row_count);
  EXPECT_EQ(500, empty->docList()->offset);
  EXPECT_EQ(2, empty->docList()->found);
  EXPECT_EQ(3, empty->docList()->found_docs);
  std::string json;
  ASSERT_TRUE(luxir::api::write_json(req->rawRequest(), json));
  EXPECT_NE(std::string::npos, json.find(R"("collapse":{"field":["key_s","key_i"],"missing":"group"})"));
}

TEST_F(CollapseTest, fieldSortWire) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "sort_i", (int64_t)9),
      flatdoc("id", "b", "key_s", "x", "sort_i", (int64_t)3),
      flatdoc("id", "c", "key_s", "y", "sort_i", (int64_t)3)}, UpdateMessage::COMMIT).success);
  auto req = run("main", "*:*", R"("key_s")", -1, 0, -1, true, 1,
      R"(["sort_i asc","score desc"])");
  for (bool protobuf : {false, true}) {
    auto copy = localReq(luxirNode->getSearchEngine());
    std::vector<std::byte> binary;
    std::string json;
    if (protobuf) {
      ASSERT_TRUE(luxir::api::encode(req->rawRequest(), binary));
      size_t size = binary.size();
      binary.resize(size + 16);
      ASSERT_TRUE(luxir::api::decode(copy->rawRequest(), std::span(binary).first(size), copy->mr));
    } else {
      ASSERT_TRUE(luxir::api::write_json(req->rawRequest(), json));
      ASSERT_TRUE(luxir::api::read_json(copy->rawRequest(), json, copy->mr));
    }
    copy->execute(-1);
    ASSERT_TRUE(copy->ok()) << copy->errorMsg();
    EXPECT_EQ(rows(*req, true), rows(*copy, true));
    EXPECT_EQ(2, copy->docList()->found);
    EXPECT_EQ(3, copy->docList()->found_docs);
    for (auto* response : copy->responses) {
      std::vector<std::byte> binary;
      ASSERT_TRUE(luxir::api::encode(response->proto, binary));
      size_t size = binary.size();
      binary.resize(size + 16);
      luxir::api::SearchResponse decoded;
      std::pmr::monotonic_buffer_resource arena;
      ASSERT_TRUE(luxir::api::decode(decoded, std::span(binary).first(size), arena));
      auto& list = *decoded.ops.at("q")->docList();
      EXPECT_EQ((size_t)list.row_count, list.groups->keys.size());
      EXPECT_EQ(2, list.found);
      EXPECT_EQ(3, list.found_docs);
    }
  }
}

TEST_F(CollapseTest, validationAndAbsentKeys) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  for (std::string collapse : {R"({"field":[]})", R"({"field":["key_s","key_s"]})",
      R"("key_f")", R"("body_w")", R"("keys_ss")"}) {
    auto req = localReq(luxirNode->getSearchEngine());
    ASSERT_TRUE(luxir::api::read_json(req->rawRequest(),
        R"({"collection":"main","limit":0,"collapse":)" + collapse + "}", req->mr));
    req->execute();
    EXPECT_FALSE(req->ok()) << collapse;
  }
  for (std::string missing : {"expand", "group", "drop"}) {
    auto req = run("main", "*:*", spec({"key_s"}, missing));
    EXPECT_EQ(missing == "drop" ? 0 : 1, req->docList()->found);
    EXPECT_EQ(missing == "drop" ? 0 : 1, req->docList()->found_docs);
  }
  CollectionHelper empty("empty_collapse");
  auto req = run("empty_collapse", "*:*", R"("key_s")");
  EXPECT_EQ(0, req->docList()->found);
  EXPECT_EQ(0, req->docList()->found_docs);
}

TEST_F(CollapseTest, valueBindingAndCapabilities) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& field = schema.field("product");
  field.type = luxir::api::FieldDef_::FieldClass::TEXT;
  field.stored = true;
  schema.analyzer(field, "unicode_word", {"nfkc_cf"});
  auto& value = schema.variant(field, "key");
  value.type = luxir::api::FieldDef_::FieldClass::STRING;
  value.column = true;
  schema.normalizer(value, {"nfkc_cf"});
  field.defaults.emplace().value = "key";
  auto& unindexed = schema.field("unindexed");
  unindexed.type = luxir::api::FieldDef_::FieldClass::STRING;
  unindexed.index = luxir::api::FieldDef_::IndexMode::NONE;
  unindexed.column = true;
  auto& uncolumned = schema.field("uncolumned");
  uncolumned.type = luxir::api::FieldDef_::FieldClass::STRING;
  uncolumned.column = false;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "product", "AbC"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "product", "abc"), UpdateMessage::COMMIT).success);
  auto req = run("main", "*:*", R"("product")");
  EXPECT_EQ(1, req->docList()->found);
  ASSERT_EQ(1, req->docList()->groups->keys.size());
  EXPECT_EQ("abc", std::get<std::string_view>(req->docList()->groups->keys[0].component[0].value));
  for (std::string collapse : {R"("unindexed")", R"("uncolumned")",
      R"({"field":["product","product__key"]})"}) {
    auto bad = localReq(luxirNode->getSearchEngine());
    ASSERT_TRUE(luxir::api::read_json(bad->rawRequest(),
        R"({"collection":"main","collapse":)" + collapse + "}", bad->mr));
    bad->execute();
    EXPECT_FALSE(bad->ok());
  }
}

TEST_F(CollapseTest, fieldSortOracle) {
  std::mt19937 random(1841);
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 315; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "body_w",
        i % 3 == 0 ? "alpha alpha beta" : i % 3 == 1 ? "alpha beta beta" : "beta gamma");
    if (random() % 5 != 0) doc.emplace_back("key_s", "g" + std::to_string(random() % 29));
    if (random() % 4 != 0) doc.emplace_back("key_i", (int64_t)(random() % 9) - 4);
    if (random() % 3 != 0) doc.emplace_back("when_dt", (int64_t)(random() % 5) * 86400000);
    if (random() % 4 != 0) doc.emplace_back("sort_i", (int64_t)(random() % 7) - 3);
    if (random() % 5 != 0) doc.emplace_back("sort_s", "s" + std::to_string(random() % 5));
    docs.push_back(std::move(doc));
  }
  struct SortCase {
    std::string json;
    std::vector<std::string> fields;
  };
  for (int32_t segments : {1, 3, 7}) {
    auto savedMode = SortField::setStringSortModeForTests(
        segments == 3 ? StringSortMode::GLOBAL : StringSortMode::SEGMENT);
    auto restoreMode = scope_guard([&]() { SortField::setStringSortModeForTests(savedMode); });
    std::string collection = "collapse_field" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int32_t segment = 0; segment < segments; segment++) {
      ASSERT_TRUE(helper.indexAll(std::span<const Doc>(docs).subspan(
          (size_t)(segment * 315 / segments), (size_t)(315 / segments)), UpdateMessage::COMMIT).success);
    }
    ASSERT_TRUE(helper.deleteByIds({"d12", "d103"}, UpdateMessage::COMMIT).success);
    for (const SortCase& sort : std::vector<SortCase>{
        {R"("sort_i asc")", {"sort_i"}}, {R"("sort_i desc")", {"sort_i"}},
        {R"("sort_s asc")", {"sort_s"}}, {R"("sort_s desc")", {"sort_s"}},
        {R"(["sort_i asc","sort_s desc"])", {"sort_i", "sort_s"}},
        {R"(["sort_s desc","sort_i desc"])", {"sort_s", "sort_i"}},
        {R"(["sort_i asc","score desc"])", {"sort_i", "score"}},
        {R"(["score asc","sort_s asc"])", {"score", "sort_s"}},
        {R"("_docid_ desc")", {"_doc_"}},
        {R"(["score desc","_docid_ asc"])", {"score", "_doc_"}},
        {R"({"expr":"sort_i * 2","dir":"asc"})", {"sort_i"}}}) {
      for (std::string query : {"*:*", "body_w:alpha OR body_w:beta"}) {
        bool scores = std::find(sort.fields.begin(), sort.fields.end(), "score") != sort.fields.end();
        auto full = run(collection, query, {}, -1, 0, 1, true, 13, sort.json, scores);
        for (const std::vector<std::string>& fields : {
            std::vector<std::string>{"key_s"}, {"key_i"}, {"key_s", "key_i", "when_dt"}}) {
          for (std::string missing : {"expand", "group", "drop"}) {
            auto expected = oracle(*full, fields, missing);
            auto collapse = spec(fields, missing);
            for (int32_t parallel : {1, -1}) {
              SCOPED_TRACE(collection + " " + query + " " + sort.json + " " + collapse
                  + " parallel=" + std::to_string(parallel));
              auto actual = run(collection, query, collapse, -1, 0, parallel, true, 11, sort.json, scores);
              EXPECT_EQ(expected.groups, rows(*actual, true));
              EXPECT_EQ((int64_t)expected.groups.size(), actual->docList()->found);
              EXPECT_EQ(expected.eligible, actual->docList()->found_docs);
              for (bool count : {false, true}) {
                auto page = run(collection, query, collapse, 7, 4, parallel, count, 3, sort.json, scores);
                auto begin = expected.groups.begin() + std::min<size_t>(4, expected.groups.size());
                auto end = expected.groups.begin() + std::min<size_t>(11, expected.groups.size());
                EXPECT_EQ(std::vector<Row>(begin, end), rows(*page, true));
                EXPECT_EQ(count, page->docList()->found.has_value());
                EXPECT_EQ(count, page->docList()->found_docs.has_value());
              }
              auto totals = run(collection, query, collapse, 0, 0, parallel, true, 13, sort.json, scores);
              EXPECT_EQ((int64_t)expected.groups.size(), totals->docList()->found);
              EXPECT_EQ(expected.eligible, totals->docList()->found_docs);
            }
            if (fields.size() == 3 && missing == "group") {
              std::vector<Row> pages;
              for (size_t offset = 0; offset < expected.groups.size(); offset += 17) {
                auto page = run(collection, query, collapse, 17, (int64_t)offset, -1, false, 5, sort.json, scores);
                auto current = rows(*page, true);
                pages.insert(pages.end(), current.begin(), current.end());
              }
              EXPECT_EQ(expected.groups, pages);
            }
          }
        }
      }
    }
  }
}

TEST_F(CollapseTest, fieldSortDrivers) {
  CollectionHelper helper;
  constexpr int32_t nDocs = 8192;
  std::vector<Doc> docs;
  for (int32_t i = 0; i < nDocs; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "body_w", i % 3 == 0 ? "alpha" : "beta",
        "sort_i", (int64_t)((nDocs - i - 1) / 256), "sort_s", "s" + std::to_string(i % 19),
        "key_s", "g" + std::to_string((nDocs - i) % 211));
    if (i % 7 != 0) doc.emplace_back("key_i", (int64_t)(i * 13 % 197));
    docs.push_back(std::move(doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  for (std::string missing : {"group", "drop", "expand"}) {
    auto collapse = spec({"key_i", "key_s"}, missing);
    for (std::string sort : {R"("sort_i asc")", R"("sort_i desc")"}) {
      for (std::string query : {"*:*", "body_w:alpha OR body_w:beta", "body_w:alpha"}) {
        SCOPED_TRACE(missing + " " + sort + " " + query);
        auto full = run("main", query, {}, -1, 0, 1, true, 2000, sort, false);
        auto expected = oracle(*full, {"key_i", "key_s"}, missing);
        expected.groups.resize(17);
        for (int32_t arm = 0; arm < 6; arm++) {
          auto savedBest = forceFieldSortBestFirst;
          auto savedSeed = forceFieldSortSeeding;
          auto savedCap = forceFieldSortWorkCapForTests;
          auto savedBulk = disableFieldSortBulk;
          auto savedPrune = disableFieldSortPruning;
          auto savedBudget = fieldSortSeedBudgetPerMilleForTests;
          auto savedWhole = QueryPrep::disableWholeMembershipPlanForTests;
          auto savedGather = FieldSortCollector::disableKeyGatherForTests;
          auto restore = scope_guard([&]() {
            forceFieldSortBestFirst = savedBest;
            forceFieldSortSeeding = savedSeed;
            forceFieldSortWorkCapForTests = savedCap;
            disableFieldSortBulk = savedBulk;
            disableFieldSortPruning = savedPrune;
            fieldSortSeedBudgetPerMilleForTests = savedBudget;
            QueryPrep::disableWholeMembershipPlanForTests = savedWhole;
            FieldSortCollector::disableKeyGatherForTests = savedGather;
          });
          forceFieldSortBestFirst = arm == 0 || arm == 1;
          forceFieldSortWorkCapForTests = arm == 1 ? 1 : 0;
          forceFieldSortSeeding = arm == 2;
          fieldSortSeedBudgetPerMilleForTests = arm == 2 ? 100 : 0;
          disableFieldSortBulk = arm == 3;
          disableFieldSortPruning = arm == 4;
          FieldSortCollector::disableKeyGatherForTests = arm == 5;
          QueryPrep::disableWholeMembershipPlanForTests = true;
          SkipStatsScope stats;
          auto actual = run("main", query, collapse, 17, 0, 1, false, 13, sort, false);
          EXPECT_EQ(expected.groups, rows(*actual, true)) << "arm=" << arm;
          if (query == "*:*" && arm == 0) { EXPECT_GT(SkipStats::fieldSortBestFirstActivations, 0); }
          if (query == "body_w:alpha OR body_w:beta" && arm == 2) { EXPECT_GT(SkipStats::fieldSortSeededActivations, 0); }
        }
      }
    }
  }
  // Candidate ord intervals close tied bottom terms by document order.
  // Exercise both ordinal comparators, including non-bottom updates.
  for (StringSortMode mode : {StringSortMode::SEGMENT, StringSortMode::GLOBAL}) {
    auto saved = SortField::setStringSortModeForTests(mode);
    auto restore = scope_guard([&]() { SortField::setStringSortModeForTests(saved); });
    for (std::string sort : {R"("sort_s asc")", R"("sort_s desc")"}) {
      auto full = run("main", "*:*", {}, -1, 0, 1, true, 2000, sort, false);
      auto expected = oracle(*full, {"key_s"}, "group");
      expected.groups.resize(17);
      auto actual = run("main", "*:*", spec({"key_s"}, "group"), 17, 0, 1, false, 13, sort, false);
      EXPECT_EQ(expected.groups, rows(*actual, true));
    }
  }
}

TEST_F(CollapseTest, fieldSortCandidatesAndTwoPhase) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int32_t i = 0; i < 12000; i++) {
      int32_t id = segment * 12000 + i;
      uint32_t value = (uint32_t)id * 2654435761u % 200000;
      auto doc = flatdoc("id", "d" + std::to_string(id),
          "body_w", id % 3 == 0 ? "alpha red fox" : "alpha red pad fox",
          "key_s", "g" + std::to_string(id % 211), "sort_i", (int64_t)(value % 31));
      if (id % 13 != 0) doc.emplace_back("sort_s", std::format("s{:06}", value));
      docs.push_back(std::move(doc));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto savedWhole = QueryPrep::disableWholeMembershipPlanForTests;
  auto restoreWhole = scope_guard([&]() { QueryPrep::disableWholeMembershipPlanForTests = savedWhole; });
  QueryPrep::disableWholeMembershipPlanForTests = true;
  for (StringSortMode mode : {StringSortMode::SEGMENT, StringSortMode::GLOBAL}) {
    auto savedMode = SortField::setStringSortModeForTests(mode);
    auto restoreMode = scope_guard([&]() { SortField::setStringSortModeForTests(savedMode); });
    for (std::string query : {"*:*", "body_w:alpha", "body_w:\"red fox\""}) {
      for (std::string field : {"sort_s", "sort_i"}) {
        for (std::string direction : {"asc", "desc"}) {
          std::string sort = '"' + field + ' ' + direction + '"';
          SCOPED_TRACE(query + " " + sort + " mode=" + std::to_string((int)mode));
          auto full = run("main", query, {}, -1, 0, 1, true, 2000, sort, false);
          auto expected = oracle(*full, {"key_s"}, "group");
          expected.groups.resize(9);
          for (bool pull : {false, true}) {
            auto savedBulk = disableFieldSortBulk;
            auto restoreBulk = scope_guard([&]() { disableFieldSortBulk = savedBulk; });
            disableFieldSortBulk = pull;
            SkipStatsScope stats;
            auto actual = run("main", query, R"("key_s")", 9, 0, 1, false, 4, sort, false);
            EXPECT_EQ(expected.groups, rows(*actual, true));
            // Multi-segment global-ord sorts have no inverse ord map;
            // their existing candidate arm is unsupported and falls back.
            if (mode == StringSortMode::SEGMENT && field == "sort_s"
                && query != "body_w:\"red fox\"") {
              EXPECT_GT(SkipStats::fieldSortCandidateActivations, 0);
            }
            if (pull && field == "sort_i" && query == "body_w:\"red fox\"") {
              EXPECT_GT(SkipStats::fieldSortExternalApproxActivations, 0);
              EXPECT_GT(SkipStats::fieldSortExternalApproxBoundRejects, 0);
            }
          }
        }
      }
    }
  }
}

TEST_F(CollapseTest, tiedGroupsUseDocumentOrder) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "z", "key_s", "z", "sort_i", (int64_t)1),
      flatdoc("id", "a", "key_s", "a", "sort_i", (int64_t)1)}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "z-later", "key_s", "z", "sort_i", (int64_t)1),
      flatdoc("id", "b", "key_s", "b", "sort_i", (int64_t)1)}, UpdateMessage::COMMIT).success);
  for (std::string sort : {std::string{}, std::string(R"("sort_i asc")")}) {
    for (int32_t parallel : {1, -1}) {
      auto result = rows(*run("main", "*:*", R"("key_s")", 2, 1, parallel, false, 1, sort), true);
      ASSERT_EQ(2, result.size());
      EXPECT_EQ("a", result[0].id);
      EXPECT_EQ("b", result[1].id);
    }
  }
}

TEST_F(CollapseTest, tiedExpandSingletonUsesDocumentOrder) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "z", "key_s", "z", "sort_i", (int64_t)1),
      flatdoc("id", "missing", "sort_i", (int64_t)1),
      flatdoc("id", "a", "key_s", "a", "sort_i", (int64_t)1)}, UpdateMessage::COMMIT).success);
  for (std::string sort : {std::string{}, std::string(R"("sort_i asc")")}) {
    auto result = rows(*run("main", "*:*", R"("key_s")", -1, 0, -1, false, 1, sort), true);
    ASSERT_EQ(3, result.size());
    EXPECT_EQ("z", result[0].id);
    EXPECT_EQ("missing", result[1].id);
    EXPECT_TRUE(result[1].singleton);
    EXPECT_EQ("a", result[2].id);
  }
}

TEST_F(CollapseTest, explicitKeySortOrdersTiedGroupsByKey) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "z", "key_s", "z"),
      flatdoc("id", "a", "key_s", "a"), flatdoc("id", "b", "key_s", "b")},
      UpdateMessage::COMMIT).success);
  auto result = rows(*run("main", "*:*", R"("key_s")", -1, 0, -1, false, 1,
      R"(["score desc","key_s asc"])"), true);
  ASSERT_EQ(3, result.size());
  EXPECT_EQ("a", result[0].id);
  EXPECT_EQ("b", result[1].id);
  EXPECT_EQ("z", result[2].id);
}

TEST_F(CollapseTest, retainedOrdRemapAcrossDifferentDictionaries) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int32_t i = 0; i < 512; i++) {
      auto doc = flatdoc("id", std::to_string(segment * 512 + i),
          "key_i", (int64_t)(i % 3), "sort_i", (int64_t)i);
      if (i % 5 != 0) doc.emplace_back("key_s", std::string(1, (char)('a' + segment + i % 2)));
      if (segment != 2 && i % 7 != 0) doc.emplace_back("sort_s", std::string(1, (char)('z' - segment - i % 2)));
      docs.push_back(std::move(doc));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds({"1", "514", "1029"}, UpdateMessage::COMMIT).success);
  for (auto fields : {std::vector<std::string>{"key_s"}, {"key_s", "sort_s", "key_i"}}) {
    for (std::string missing : {"expand", "group", "drop"}) {
      for (std::string sort : {std::string{}, std::string(R"("sort_i desc")")}) {
        auto full = run("main", "*:*", {}, -1, 0, 1, true, 2000, sort);
        auto expected = oracle(*full, fields, missing).groups;
        if (expected.size() > 20) expected.resize(20);
        for (int32_t parallel : {1, -1}) {
          // Serial counters certify that encoded remaps ran; parallel workers
          // exercise independent local dictionaries and global merge identities.
          SkipStatsScope stats;
          SkipStats::enabled = parallel == 1;
          auto actual = run("main", "*:*", spec(fields, missing), 20, 0, parallel, false, 2000, sort);
          EXPECT_EQ(expected, rows(*actual, true));
          if (parallel == 1) { EXPECT_GT(SkipStats::collapseRemapProbes, 0); }
        }
      }
    }
  }
}

TEST_F(CollapseTest, constantPullStopsAtDistinctGroups) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 512; i++) {
    auto doc = flatdoc("id", std::to_string(i));
    if (i != 200) doc.emplace_back("key_s", i < 200 ? "z" : "a");
    docs.push_back(std::move(doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  for (auto missing : {"expand", "group", "drop"}) {
    auto full = run("main", "*:*", {}, -1, 0, 1, true, 2000);
    auto expected = oracle(*full, {"key_s"}, missing).groups;
    expected.resize(2);
    for (bool exhaustive : {false, true}) {
      auto saved = disableCollapsePruningForTests;
      auto restore = scope_guard([&]() { disableCollapsePruningForTests = saved; });
      disableCollapsePruningForTests = exhaustive;
      SkipStatsScope stats;
      auto actual = run("main", "*:*", spec({"key_s"}, missing), 2, 0, 1, false);
      EXPECT_EQ(expected, rows(*actual, true));
      // The bounded pull stops once two groups are held and passed.
      if (!exhaustive) {
        EXPECT_EQ(missing == std::string_view("drop") ? 202 : 201, SkipStats::collapseDocsVisited);
      }
    }
  }
}

namespace {
using RankPart = std::variant<std::monostate, int64_t, double, float, std::string>;
RankPart rankPart(const api::Val& value) {
  return std::visit([](const auto& v) -> RankPart {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, std::string_view>) return std::string(v);
    else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, double>
                       || std::is_same_v<T, float>) return v;
    else return {};
  }, value.kind);
}
struct EnrichedRow {
  std::string id;
  float score;
  int64_t count;
  std::vector<RankPart> rank;
  bool operator==(const EnrichedRow&) const = default;
};
Key memberKey(const api::Map& doc, const std::vector<std::string>& fields,
              std::string_view policy) {
  Key key;
  bool missing = false;
  for (const auto& field : fields) {
    auto* value = doc.fields.find(field);
    key.parts.push_back(part(value ? &**value : nullptr));
    missing |= std::holds_alternative<std::monostate>(key.parts.back());
  }
  if (missing && policy == "expand") {
    key.singleton = true;
    key.parts = {std::string(doc.fields.at("id")->asString())};
  }
  return key;
}
std::vector<EnrichedRow> enrichedRows(LocalReq& request) {
  std::vector<EnrichedRow> result;
  for (auto* response : request.responses) {
    const auto& list = *response->proto.ops.at("q")->docList();
    EXPECT_EQ((size_t)list.row_count, list.groups->keys.size());
    EXPECT_EQ((size_t)list.row_count, list.groups->counts.size());
    for (size_t i = 0; i < list.docs.size(); i++) {
      const auto& doc = list.docs[i];
      EnrichedRow row{std::string(doc.fields.at("id")->asString()),
          std::get<float>(doc.fields.at("_score_")->kind), list.groups->counts[i], {}};
      if (!list.groups->ranks.empty()) {
        for (const auto& value : list.groups->ranks[i].v) row.rank.push_back(rankPart(value));
      }
      result.push_back(std::move(row));
    }
  }
  return result;
}
std::vector<EnrichedRow> memberOracle(LocalReq& rankDocs, LocalReq& headDocs,
    const std::vector<std::string>& fields, std::string_view policy, bool fieldSort) {
  std::map<Key, int64_t> counts;
  std::map<Key, const api::Map*> heads;
  for (auto* response : headDocs.responses) for (const auto& doc : response->proto.ops.at("q")->docList()->docs) {
    auto key = memberKey(doc, fields, policy);
    heads.try_emplace(key, &doc);
    counts[key]++;
  }
  std::set<Key> seen;
  std::vector<EnrichedRow> result;
  for (auto* response : rankDocs.responses) for (const auto& doc : response->proto.ops.at("q")->docList()->docs) {
    auto key = memberKey(doc, fields, policy);
    if (policy == "drop" && std::ranges::any_of(key.parts,
        [](const Part& p) { return std::holds_alternative<std::monostate>(p); })) continue;
    if (!seen.insert(key).second) continue;
    const auto& head = *heads.at(key);
    std::string id(head.fields.at("id")->asString());
    EnrichedRow row{id, std::get<float>(head.fields.at("_score_")->kind), counts.at(key), {}};
    if (id != doc.fields.at("id")->asString()) {
      auto* value = doc.fields.find(fieldSort ? "sort_i" : "_score_");
      row.rank.push_back(value ? rankPart(**value) : RankPart{});
    }
    result.push_back(std::move(row));
  }
  return result;
}
}

TEST_F(CollapseTest, pageMemberOracle) {
  std::mt19937 random(7319);
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 90; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "sort_i", (int64_t)(random() % 9),
        "sort_s", "s" + std::to_string(random() % 5), "body_w",
        i % 3 ? "alpha alpha beta" : "alpha beta beta");
    if (i % 7) doc.emplace_back("key_s", "g" + std::to_string(random() % 11));
    if (i % 5) doc.emplace_back("key_i", (int64_t)(random() % 4) - 2);
    docs.push_back(std::move(doc));
  }
  SearchOverridesGuard strategy(collapseMemberStrategy);
  for (int32_t segments : {1, 3}) {
    std::string collection = "members" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int32_t s = 0; s < segments; s++) {
      ASSERT_TRUE(helper.indexAll(std::span<const Doc>(docs).subspan(
          (size_t)s * docs.size() / segments, docs.size() / segments), UpdateMessage::COMMIT).success);
    }
    ASSERT_TRUE(helper.deleteByIds({"d3", "d22"}, UpdateMessage::COMMIT).success);
    for (std::string query : {"body_w:alpha", "body_w:alpha AND sort_i:>=4"}) {
      for (bool fieldSort : {false, true}) {
        std::string sort = fieldSort ? R"("sort_i desc")" : R"("score desc")";
        auto rankDocs = run(collection, query, {}, -1, 0, 1, true, 1000, sort);
        for (std::string head : {R"("sort_i asc")", R"(["sort_s desc","sort_i asc"])",
                                 R"("add(sort_i,1) asc")", R"("score desc")"}) {
          auto headDocs = run(collection, query, {}, -1, 0, 1, true, 1000, head);
          for (const std::vector<std::string>& fields : {
              std::vector<std::string>{"key_s"}, {"key_i"}, {"key_s", "key_i"}, {"key_s", "key_i", "sort_s"}}) {
            for (auto policy : {"expand", "group", "drop"}) {
              auto expected = memberOracle(*rankDocs, *headDocs, fields, policy, fieldSort);
              auto collapse = spec(fields, policy);
              collapse.pop_back(); collapse += R"(,"get_count":true,"head_sort":)" + head + '}';
              for (auto strategy : {CollapseMemberStrategy::KEYS, CollapseMemberStrategy::DOMAIN_WALK}) {
                collapseMemberStrategy = strategy;
                for (int parallel : {1, -1}) {
                  SCOPED_TRACE(collection + query + sort + head + collapse + std::to_string(parallel));
                  auto actual = run(collection, query, collapse, -1, 0, parallel, true, 7, sort);
                  EXPECT_EQ(expected, enrichedRows(*actual));
                  if (!fieldSort && fields == std::vector<std::string>{"key_s"}
                      && policy == std::string_view("group") && strategy == CollapseMemberStrategy::KEYS) {
                    // List totals are off: ranking can prune, but enrichment
                    // must replay later / lower-scoring members of page groups.
                    auto page = run(collection, query, collapse, 2, 1, parallel, false, 1, sort);
                    auto begin = expected.begin() + std::min<size_t>(1, expected.size());
                    auto end = expected.begin() + std::min<size_t>(3, expected.size());
                    EXPECT_EQ(std::vector<EnrichedRow>(begin, end), enrichedRows(*page));
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST_F(CollapseTest, memberPagingWireAndRendering) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "sort_i", (int64_t)9),
      flatdoc("id", "b", "key_s", "x", "sort_i", (int64_t)3),
      flatdoc("id", "c", "sort_i", (int64_t)8),
      flatdoc("id", "d", "key_s", "y", "sort_i", (int64_t)4)}, UpdateMessage::COMMIT).success);
  std::string collapse = R"({"field":"key_s","get_count":true,"head_sort":"sort_i asc"})";
  auto full = run("main", "*:*", collapse, -1, 0, -1, true, 1, R"("sort_i desc")");
  api::Collapse replacement;
  ASSERT_TRUE(api::read_json(replacement, collapse, full->mr));
  ASSERT_TRUE(replacement.get_count);
  ASSERT_TRUE(api::read_json(replacement, R"("key_i")", full->mr));
  EXPECT_EQ("key_i", replacement.field[0]);
  EXPECT_FALSE(replacement.get_count);
  EXPECT_TRUE(replacement.head_sort.empty());
  auto expected = enrichedRows(*full);
  auto jsonRows = renderSearchResponseBody(full->responses[0]->proto, true);
  EXPECT_NE(std::string::npos, jsonRows.find(R"("_group_":{"val":"x","count":2,"rank":[9]})"));
  auto runs = renderDocRuns(full->responses[0]->proto);
  ASSERT_EQ(1u, runs.size());
  EXPECT_NE(std::string::npos, runs[0].body.find(R"("_group_":{"val":"x","count":2,"rank":[9]})"));
  ASSERT_EQ(3u, expected.size());
  EXPECT_EQ("b", expected[0].id);
  EXPECT_EQ(2, expected[0].count);
  EXPECT_EQ((std::vector<RankPart>{(int64_t)9}), expected[0].rank);
  EXPECT_EQ(1, expected[1].count);
  EXPECT_TRUE(expected[1].rank.empty());
  std::vector<EnrichedRow> pages;
  for (size_t offset = 0; offset < expected.size(); offset++) {
    auto page = run("main", "*:*", collapse, 1, (int64_t)offset, -1, true, 1, R"("sort_i desc")");
    auto values = enrichedRows(*page);
    pages.insert(pages.end(), values.begin(), values.end());
  }
  EXPECT_EQ(expected, pages);
  for (bool binary : {false, true}) {
    auto copy = localReq(luxirNode->getSearchEngine());
    std::string json;
    std::vector<std::byte> data;
    if (binary) {
      ASSERT_TRUE(api::encode(full->rawRequest(), data));
      size_t size = data.size(); data.resize(size + 16);
      ASSERT_TRUE(api::decode(copy->rawRequest(), std::span(data).first(size), copy->mr));
    } else {
      ASSERT_TRUE(api::write_json(full->rawRequest(), json));
      EXPECT_NE(std::string::npos, json.find("head_sort"));
      ASSERT_TRUE(api::read_json(copy->rawRequest(), json, copy->mr));
    }
    copy->execute(-1);
    ASSERT_TRUE(copy->ok()) << copy->errorMsg();
    EXPECT_EQ(expected, enrichedRows(*copy));
    for (auto* response : copy->responses) {
      std::vector<std::byte> data;
      ASSERT_TRUE(api::encode(response->proto, data));
      size_t size = data.size(); data.resize(size + 16);
      api::SearchResponse decoded;
      std::pmr::monotonic_buffer_resource arena;
      ASSERT_TRUE(api::decode(decoded, std::span(data).first(size), arena));
      const auto& table = *decoded.ops.at("q")->docList()->groups;
      EXPECT_EQ(table.keys.size(), table.counts.size());
      EXPECT_EQ(table.keys.size(), table.ranks.size());
    }
  }
}

TEST_F(CollapseTest, countOnlyAndAlternateHeadsUseFinalizedFilters) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "key_s", "x", "sort_i", (int64_t)10, "eligible_s", "yes"),
      flatdoc("id", "b", "key_s", "x", "sort_i", (int64_t)0, "eligible_s", "no"),
      flatdoc("id", "c", "key_s", "x", "sort_i", (int64_t)7, "eligible_s", "yes")},
      UpdateMessage::COMMIT).success);
  SearchOverridesGuard guard(disableTopDocsFilterFold, collapseMemberStats);
  CollapseMemberStats stats;
  collapseMemberStats = &stats;
  for (bool passive : {false, true}) for (bool prepared : {false, true}) for (bool routed : {false, true}) {
    disableTopDocsFilterFold = passive;
    auto req = localReq(helper.getSearchEngine());
    std::string json = R"({"collection":"main","sort":"sort_i desc","fields":["id"],"filter":[)";
    json += routed ? R"({"query":"eligible_s:yes","except_ops":["keys"]})" : R"("eligible_s:yes")";
    json += R"(],"collapse":{"field":"key_s","get_count":true,"head_sort":"sort_i asc"})";
    if (routed) json += R"(,"ops":{"keys":{"field_facet":{"field":"key_s","count_unit":"docs"}}})";
    json += '}';
    ASSERT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->testForcePrepare = prepared;
    req->execute(-1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& list = *req->docList();
    EXPECT_EQ(2, list.groups->counts[0]);
    EXPECT_EQ("c", std::get<api::ColStr>(list.columns.at("id").kind).v[0]);
    EXPECT_TRUE(list.groups->ranks.empty()); // get_scores is off
  }
  stats.keySegments = 0; stats.domainSegments = 0;
  auto plain = run("main", "*:*", R"("key_s")");
  auto zero = run("main", "*:*", R"({"field":"key_s","get_count":true,"head_sort":"sort_i asc"})", 0);
  EXPECT_EQ(0, stats.keySegments);
  EXPECT_EQ(0, stats.domainSegments);
  auto counts = run("main", "*:*", R"({"field":"key_s","get_count":true})");
  EXPECT_EQ(3, counts->docList()->groups->counts[0]);
  EXPECT_TRUE(counts->docList()->groups->ranks.empty());
  auto scoreHead = run("main", "rescore(*:*, sub(0,sort_i))",
      R"({"field":"key_s","head_sort":"score desc"})", -1, 0, -1, true, 13,
      R"("sort_i desc")", false);
  EXPECT_EQ("b", rows(*scoreHead, true)[0].id);
  EXPECT_TRUE(scoreHead->docList()->groups->ranks.empty());
  CollectionHelper singletons("singleton_members");
  ASSERT_TRUE(singletons.indexAll({flatdoc("id", "a"), flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  stats.candidates = 0;
  auto single = run("singleton_members", "*:*",
      R"({"field":"key_s","get_count":true,"head_sort":"sort_i asc"})");
  EXPECT_EQ(0, stats.candidates);
  for (auto count : single->docList()->groups->counts) EXPECT_EQ(1, count);
  SearchOverridesGuard budget(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 128;
  auto limited = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(limited->rawRequest(),
      R"({"collection":"main","collapse":{"field":"key_s","get_count":true}})", limited->mr));
  limited->execute();
  EXPECT_FALSE(limited->ok());
  EXPECT_NE(std::string::npos, std::string(limited->errorMsg()).find("collapse member state"));
}

TEST_F(CollapseTest, typedGroupRanks) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "key_s", "x", "sort_i", INT64_MAX, "sort_s", "z", "sort_d", 3.25),
      flatdoc("id", "b", "key_s", "x", "sort_i", INT64_MIN, "sort_s", "a", "sort_d", 0.5)},
      UpdateMessage::COMMIT).success);
  auto key = R"({"field":"key_s","head_sort":"sort_i asc","get_count":true})";
  for (std::string sort : {R"(["sort_s desc","sort_i desc"])", R"("add(sort_d,0) desc")", R"("sort_d desc")"}) {
    auto req = run("main", "*:*", key, -1, 0, -1, true, 10, sort);
    ASSERT_TRUE(req->ok());
    const auto& rank = req->docList()->groups->ranks[0].v;
    ASSERT_FALSE(rank.empty());
    if (rank.size() == 2) {
      EXPECT_EQ("z", rank[0].asString());
      EXPECT_EQ(INT64_MAX, rank[1].asInt());
    } else EXPECT_EQ(3.25, rank[0].asDouble());
  }
}

TEST_F(CollapseTest, multiNumericRanksUseExtremaInBothModes) {
  for (bool array : {false, true}) {
    CollectionHelper helper;
    helper.clear();
    SchemaBuilder b;
    auto& field = b.field("sort_is");
    field.type = api::FieldDef::FieldClass::INT;
    field.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
    b.set(helper.collection());
    helper.index(flatdoc("id", "a", "key_s", "x", "sort_is", vec_i(9, 4)), UpdateMessage::COMMIT);
    helper.index(flatdoc("id", "b", "key_s", "y", "sort_is", vec_i(8, 6)), UpdateMessage::COMMIT);
    // Different displayed heads force rank emission for the winning members.
    helper.index(flatdoc("id", "0a", "key_s", "x", "sort_is", vec_i(5)), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "0b", "key_s", "y", "sort_is", vec_i(7)), UpdateMessage::COMMIT);
    for (auto [sort, first, second] : {
        std::tuple{R"("sort_is asc")", 4, 6}, {R"("sort_is desc")", 9, 8}}) {
      for (int parallel : {0, -1}) {
        auto req = run("main", "*:*", R"({"field":"key_s","get_count":true,"head_sort":"id asc"})",
                       -1, 0, parallel, true, 10, sort);
        ASSERT_TRUE(req->ok()) << req->errorMsg();
        const auto* groups = &*req->docList()->groups;
        ASSERT_EQ(2u, groups->ranks.size());
        ASSERT_EQ(1u, groups->ranks[0].v.size());
        ASSERT_EQ(1u, groups->ranks[1].v.size());
        EXPECT_EQ(first, groups->ranks[0].v[0].asInt());
        EXPECT_EQ(second, groups->ranks[1].v[0].asInt());
      }
    }
  }
}

namespace {
const api::Val& groupOp(const api::GroupTable& groups, std::string_view name, size_t row) {
  return std::get<api::ArrVal>(groups.ops.at(name)->kind).v[row];
}
struct GroupCard {
  std::string id;
  int64_t count;
  std::vector<std::string> variants;
  std::vector<std::pair<std::string, float>> relevant;
  double low, high, average;
  std::map<std::string, int64_t> swatches;
  bool operator==(const GroupCard&) const = default;
};
std::vector<GroupCard> groupCards(LocalReq& request) {
  std::vector<GroupCard> cards;
  for (auto* response : request.responses) {
    const auto& list = *response->proto.ops.at("q")->docList();
    const auto& groups = *list.groups;
    for (auto& [name, column] : groups.ops) {
      EXPECT_EQ((size_t)list.row_count, std::get<api::ArrVal>(column->kind).v.size()) << name;
    }
    for (size_t row = 0; row < list.docs.size(); row++) {
      GroupCard card;
      card.id = std::string(list.docs[row].fields.at("id")->asString());
      card.count = groups.counts[row];
      auto& variants = *groupOp(groups, "variants", row).docList();
      for (auto& doc : variants.docs) {
        card.variants.emplace_back(doc.fields.at("id")->asString());
        EXPECT_EQ(nullptr, doc.fields.find("_score_"));
      }
      auto& relevant = *groupOp(groups, "relevant", row).docList();
      for (auto& doc : relevant.docs) card.relevant.emplace_back(
          doc.fields.at("id")->asString(), std::get<float>(doc.fields.at("_score_")->kind));
      card.low = (double)groupOp(groups, "low", row).asInt();
      card.high = (double)groupOp(groups, "high", row).asInt();
      card.average = groupOp(groups, "average", row).asDouble();
      auto& swatches = *groupOp(groups, "swatches", row).facetResult();
      auto values = std::get<api::ColStr>(swatches.bucket_ids->kind).v;
      for (size_t i = 0; i < values.size(); i++) card.swatches[std::string(values[i])] = swatches.counts[i];
      cards.push_back(std::move(card));
    }
  }
  return cards;
}
}

TEST_F(CollapseTest, groupOpsMemberOracle) {
  std::mt19937 random(2219);
  std::vector<Doc> docs;
  for (int i = 0; i < 72; i++) {
    auto doc = flatdoc("id", "d" + std::to_string(i), "sort_i", (int64_t)(random() % 9),
        "sort_s", "s" + std::to_string(random() % 3), "body_w",
        i % 3 ? "alpha alpha beta" : "alpha beta beta");
    if (i % 7) doc.emplace_back("key_s", "g" + std::to_string(random() % 7));
    if (i % 5) doc.emplace_back("key_i", (int64_t)(random() % 3) - 1);
    docs.push_back(std::move(doc));
  }
  SearchOverridesGuard strategy(collapseMemberStrategy, forcedFacetBucketBlockBuckets);
  for (int segments : {1, 3}) {
    std::string collection = "cards" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int s = 0; s < segments; s++) ASSERT_TRUE(helper.indexAll(
        std::span<const Doc>(docs).subspan(s * docs.size() / segments, docs.size() / segments),
        UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper.deleteByIds({"d3", "d22"}, UpdateMessage::COMMIT).success);
    for (std::string query : {"body_w:alpha", "body_w:alpha AND sort_i:>=4"}) {
      auto cheap = run(collection, query, {}, -1, 0, 1, true, 1000, R"(["sort_i asc","id asc"])");
      auto relevant = run(collection, query, {}, -1, 0, 1, true, 1000, R"("score desc")");
      for (std::string sort : {R"("score desc")", R"("sort_i desc")"}) {
        auto ranks = run(collection, query, {}, -1, 0, 1, true, 1000, sort);
        for (auto fields : {std::vector<std::string>{"key_s"}, {"key_i"}, {"key_s", "key_i"}}) {
          for (std::string policy : {"expand", "group", "drop"}) {
            std::map<Key, GroupCard> byKey;
            for (auto* response : cheap->responses) for (auto& doc : response->proto.ops.at("q")->docList()->docs) {
              auto key = memberKey(doc, fields, policy);
              auto& card = byKey[key];
              double value = (double)doc.fields.at("sort_i")->asInt();
              if (card.count++ == 0) card.low = card.high = card.average = value;
              else { card.low = std::min(card.low, value); card.high = std::max(card.high, value); card.average += value; }
              if (card.variants.size() < 3) card.variants.emplace_back(doc.fields.at("id")->asString());
              card.swatches[std::string(doc.fields.at("sort_s")->asString())]++;
            }
            for (auto& [key, card] : byKey) card.average /= (double)card.count;
            for (auto* response : relevant->responses) for (auto& doc : response->proto.ops.at("q")->docList()->docs) {
              auto& card = byKey.at(memberKey(doc, fields, policy));
              if (card.relevant.size() < 3) card.relevant.emplace_back(
                  doc.fields.at("id")->asString(), std::get<float>(doc.fields.at("_score_")->kind));
            }
            std::vector<GroupCard> expected;
            std::set<Key> seen;
            for (auto* response : ranks->responses) for (auto& doc : response->proto.ops.at("q")->docList()->docs) {
              auto key = memberKey(doc, fields, policy);
              if (policy == "drop" && std::ranges::any_of(key.parts,
                  [](const Part& part) { return std::holds_alternative<std::monostate>(part); })) continue;
              if (!seen.insert(key).second) continue;
              auto card = byKey.at(key);
              card.id = card.variants.front();
              expected.push_back(std::move(card));
            }
            auto collapse = spec(fields, policy);
            collapse.pop_back();
            collapse += R"JSON(,"get_count":true,"head_sort":["sort_i asc","id asc"],"ops":{"low":"min(sort_i)","high":"max(sort_i)","average":"avg(sort_i)","variants":{"top_docs":{"sort":["sort_i asc","id asc"],"fields":["id"],"document_format":"rows","limit":3}},"relevant":{"top_docs":{"query":")JSON" + query
                + R"JSON(","get_scores":true,"fields":["id"],"document_format":"rows","limit":3}},"swatches":{"field_facet":{"field":"sort_s","limit":-1}}}})JSON";
            for (auto forced : {CollapseMemberStrategy::KEYS, CollapseMemberStrategy::DOMAIN_WALK}) {
              collapseMemberStrategy = forced;
              forcedFacetBucketBlockBuckets = forced == CollapseMemberStrategy::DOMAIN_WALK ? 2 : 0;
              for (int parallel : {1, -1}) {
                SCOPED_TRACE(collection + query + sort + collapse + std::to_string(parallel));
                auto actual = run(collection, query, collapse, -1, 0, parallel, true, 7, sort);
                ASSERT_TRUE(actual->ok()) << actual->errorMsg();
                EXPECT_EQ(expected, groupCards(*actual));
                if (policy == "group" && fields.size() == 2) {
                  std::vector<GroupCard> pages;
                  for (size_t offset = 0; offset < expected.size(); offset += 5) {
                    auto page = run(collection, query, collapse, 5, (int64_t)offset, parallel, false, 1, sort);
                    auto cards = groupCards(*page);
                    pages.insert(pages.end(), cards.begin(), cards.end());
                  }
                  EXPECT_EQ(expected, pages);
                }
              }
            }
          }
        }
      }
    }
  }
}

TEST_F(CollapseTest, groupOpsNestedAndCompleteStreamingBlocks) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "key_i", (int64_t)1, "sort_i", (int64_t)9, "sort_s", "red"),
      flatdoc("id", "b", "key_s", "x", "key_i", (int64_t)1, "sort_i", (int64_t)3, "sort_s", "red"),
      flatdoc("id", "c", "sort_i", (int64_t)8, "sort_s", "blue"),
      flatdoc("id", "d", "key_s", "y", "sort_i", (int64_t)4, "sort_s", "red")}, UpdateMessage::COMMIT).success);
  std::string collapse = R"JSON({"field":"key_s","get_count":true,"ops":{
    "variants":{"top_docs":{"sort":"sort_i asc","fields":["id"],"document_format":"rows","limit":3}},
    "swatches":{"field_facet":{"field":"sort_s","ops":{"low":"min(sort_i)"}}},
    "prices":{"range_facet":{"field":"sort_i","start":0,"end":10,"gap":5,"ops":{"low":"min(sort_i)"}}},
    "offers":{"query_facet":{"buckets":{"all":"*:*"},"ops":{"low":"min(sort_i)"}}},
    "distinct":{"field_facet":{"field":"sort_s","count_distinct":{"field":"key_i"}}},
    "grouped":{"top_docs":{"collapse":{"field":"key_i","missing":"group"},"fields":["id"],"limit":3}}
  }})JSON";
  auto actual = run("main", "*:*", collapse, -1, 0, -1, true, 1, R"("sort_i desc")");
  ASSERT_TRUE(actual->ok()) << actual->errorMsg();
  ASSERT_EQ(3u, actual->responses.size());
  const auto& groups = *actual->responses[0]->proto.ops.at("q")->docList()->groups;
  const auto& swatches = *groupOp(groups, "swatches", 0).facetResult();
  EXPECT_EQ((std::vector<int64_t>{2}), std::vector(swatches.counts.begin(), swatches.counts.end()));
  EXPECT_EQ(3, std::get<api::ArrVal>(swatches.ops.at("low")->kind).v[0].asInt());
  const auto& offers = *groupOp(groups, "offers", 0).facetResult();
  EXPECT_EQ(2, offers.counts[0]);
  const auto& prices = *groupOp(groups, "prices", 0).facetResult();
  EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector(prices.counts.begin(), prices.counts.end()));
  const auto& distinct = *groupOp(groups, "distinct", 0).facetResult();
  EXPECT_EQ(1, distinct.counts[0]);
  EXPECT_EQ(1, groupOp(groups, "grouped", 0).docList()->row_count);
  auto body = renderSearchResponseBody(actual->responses[0]->proto, true);
  EXPECT_NE(std::string::npos, body.find(R"("_group_":{"val":"x","count":2,)"));
  EXPECT_NE(std::string::npos, body.find(R"("variants":{"docs":[{"id":"b"},{"id":"a"}])"));
  auto runs = renderDocRuns(actual->responses[0]->proto);
  ASSERT_EQ(1u, runs.size());
  EXPECT_NE(std::string::npos, runs[0].body.find(R"("swatches":{"buckets":[{"val":"red","count":2,)"));
  for (auto* response : actual->responses) {
    std::vector<std::byte> bytes;
    ASSERT_TRUE(api::encode(response->proto, bytes));
    size_t size = bytes.size(); bytes.resize(size + 16);
    api::SearchResponse decoded;
    std::pmr::monotonic_buffer_resource mr;
    ASSERT_TRUE(api::decode(decoded, std::span(bytes).first(size), mr));
    EXPECT_EQ(renderSearchResponseBody(response->proto, true), renderSearchResponseBody(decoded, true));
  }
  std::string canonical;
  ASSERT_TRUE(api::write_json(actual->rawRequest(), canonical));
  auto copy = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(copy->rawRequest(), canonical, copy->mr));
  copy->testForcePrepare = true;
  copy->execute(-1);
  ASSERT_TRUE(copy->ok()) << copy->errorMsg();
  ASSERT_EQ(actual->responses.size(), copy->responses.size());
  for (size_t i = 0; i < actual->responses.size(); i++) {
    EXPECT_EQ(renderSearchResponseBody(actual->responses[i]->proto, true),
        renderSearchResponseBody(copy->responses[i]->proto, true));
  }
  api::Collapse replaced = *std::get<api::TopDocs>(actual->rawRequest().ops.at("q")->kind).collapse;
  ASSERT_FALSE(replaced.ops.empty());
  ASSERT_TRUE(api::read_json(replaced, R"("key_i")", actual->mr));
  EXPECT_TRUE(replaced.ops.empty());
}

TEST_F(CollapseTest, bucketAndGroupOpNamesAndScope) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "sort_i", (int64_t)1)}, UpdateMessage::COMMIT).success);
  auto reject = [&](std::string json, std::string_view message) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute();
    EXPECT_FALSE(req->ok()) << json;
    EXPECT_NE(std::string::npos, req->errorMsg().find(message)) << req->errorMsg();
  };
  for (std::string name : {"val", "count", "rank", "singleton", "missing"}) {
    for (std::string parent : {R"("collapse":{"field":"key_s","ops":)",
        R"("ops":{"f":{"field_facet":{"field":"key_s","ops":)"}) {
      reject(R"JSON({"collection":"main",)JSON" + parent + "{\"" + name
          + R"JSON(":"min(sort_i)"})JSON" + (parent.starts_with("\"collapse") ? "}}" : "}}}}"), "reserved");
    }
  }
  reject(R"({"collection":"main","collapse":{"field":"key_s","ops":{"d":{"top_docs":{},
    "domain":{"apply_parent_filters":true}}}}})", "requires domain.query");
  reject(R"JSON({"collection":"main","ops":{"f":{"field_facet":{"field":"key_s","count_distinct":{"field":"key_s","ops":{"m":"count()"}}}}}})JSON", "only field and missing");
}

TEST_F(CollapseTest, resetGroupDomainOracle) {
  std::vector<Doc> docs;
  for (int i = 0; i < 72; i++) {
    auto doc = flatdoc("id", "r" + std::to_string(i), "stock_i", (int64_t)(i % 4), "available_i",
                       (int64_t)(i % 3 != 0));
    if (i % 7)
      doc.emplace_back("key_s", "g" + std::to_string(i % 5));
    if (i % 5)
      doc.emplace_back("key_i", (int64_t)(i % 3));
    if (i % 6)
      doc.emplace_back("color_s", i % 2 ? "red" : "blue");
    docs.push_back(std::move(doc));
  }
  size_t recomputations = 0;
  SearchOverridesGuard strategy(collapseMemberStrategy, forcedFacetBucketBlockBuckets,
                                structuralPrefixRecomputations);
  structuralPrefixRecomputations = &recomputations;
  forcedFacetBucketBlockBuckets = 2;
  for (int segments : {1, 3}) {
    std::string collection = "reset" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int s = 0; s < segments; s++)
      ASSERT_TRUE(helper
                      .indexAll(std::span<const Doc>(docs).subspan(s * docs.size() / segments,
                                                                   docs.size() / segments),
                                UpdateMessage::COMMIT)
                      .success);
    ASSERT_TRUE(helper.deleteByIds({"r3", "r39"}, UpdateMessage::COMMIT).success);
    auto execute = [&](std::string tail, int parallel) {
      auto req = localReq(helper.getSearchEngine());
      auto json = R"({"collection":")" + collection + R"(","sort":"id asc","limit":-1,
        "fields":["id","key_s","key_i","color_s","stock_i","available_i"],"document_format":"rows",
        )" + tail +
                  '}';
      EXPECT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
      req->execute(parallel);
      EXPECT_TRUE(req->ok()) << req->errorMsg() << json;
      return req;
    };
    auto all = execute(R"("query":"*:*")", 1);
    auto ranked = execute(R"("query":"color_s:red","filter":["available_i:1"])", 1);
    struct Shape {
      std::vector<std::string> fields;
      std::string policy;
      bool parentFilters;
    };
    std::vector<Shape> shapes;
    for (auto fields : {std::vector<std::string>{"key_s"}, {"key_i"}, {"key_s", "key_i"}}) {
      for (std::string policy : {"expand", "group", "drop"}) {
        for (bool parentFilters : {false, true})
          shapes.push_back({fields, policy, parentFilters});
      }
    }
    for (const auto& [fields, policy, parentFilters] : shapes) {
      std::vector<Key> keys;
      std::map<Key, int64_t> counts;
      for (auto* response : ranked->responses)
        for (auto& doc : response->proto.ops.at("q")->docList()->docs) {
          auto key = memberKey(doc, fields, policy);
          if (policy == "drop" && std::ranges::any_of(key.parts, [](const Part &value) {
                return std::holds_alternative<std::monostate>(value);
              }))
            continue;
          if (counts[key]++ == 0)
            keys.push_back(key);
        }
      auto domain = R"("domain":{"query":"*:*","filter":["stock_i:>0"],"apply_parent_filters":)" +
                    std::string(parentFilters ? "true}" : "false}");
      auto collapse = spec(fields, policy);
      collapse.pop_back();
      collapse += R"(,"get_count":true,"ops":{"members":{"top_docs":{"limit":-1,
        "sort":"id asc","fields":["id"],"document_format":"rows"},)" +
                  domain + R"(},"swatches":{"field_facet":{"field":"color_s","limit":-1,
        "missing":true},)" +
                  domain + "}}}";
      struct Expected {
        std::vector<std::string> ids;
        std::map<std::string, int64_t> colors;
        int64_t missing = 0;
      };
      std::map<Key, Expected> expectedGroups;
      for (auto* response : all->responses) {
        for (const auto& doc : response->proto.ops.at("q")->docList()->docs) {
          if (doc.fields.at("stock_i")->asInt() == 0 ||
              (parentFilters && doc.fields.at("available_i")->asInt() == 0))
            continue;
          auto& expected = expectedGroups[memberKey(doc, fields, policy)];
          expected.ids.emplace_back(doc.fields.at("id")->asString());
          if (auto* color = doc.fields.find("color_s")) {
            expected.colors[std::string((*color)->asString())]++;
          } else
            expected.missing++;
        }
      }
      for (auto [source, parallel] : {std::pair{CollapseMemberStrategy::KEYS, 1},
                                      {CollapseMemberStrategy::KEYS, -1},
                                      {CollapseMemberStrategy::DOMAIN_WALK, 1},
                                      {CollapseMemberStrategy::DOMAIN_WALK, -1}}) {
        collapseMemberStrategy = source;
        auto actual = execute(R"("query":"color_s:red","filter":["available_i:1"],
        "collapse":)" + collapse,
                              parallel);
        ASSERT_TRUE(actual->ok()) << actual->errorMsg();
        size_t row = 0;
        for (auto* response : actual->responses) {
          const auto& list = *response->proto.ops.at("q")->docList();
          for (size_t i = 0; i < list.groups->keys.size(); i++, row++) {
            ASSERT_LT(row, keys.size());
            const auto& expected = expectedGroups[keys[row]];
            const auto& members = *groupOp(*list.groups, "members", i).docList();
            std::vector<std::string> ids;
            for (auto& doc : members.docs)
              ids.emplace_back(doc.fields.at("id")->asString());
            EXPECT_EQ(expected.ids, ids) << collapse;
            EXPECT_EQ(counts.at(keys[row]), list.groups->counts[i]);
            const auto& facet = *groupOp(*list.groups, "swatches", i).facetResult();
            std::map<std::string, int64_t> actualColors;
            auto labels = std::get<api::ColStr>(facet.bucket_ids->kind).v;
            for (size_t j = 0; j < labels.size(); j++) {
              actualColors[std::string(labels[j])] = facet.counts[j];
            }
            EXPECT_EQ(expected.colors, actualColors);
            EXPECT_EQ(expected.missing, facet.missing.value_or(-1));
          }
        }
        EXPECT_EQ(keys.size(), row);
      }
    }
  }
  EXPECT_EQ(0u, recomputations);
}

TEST_F(CollapseTest, resetKeepsEveryEnclosingBucketIdentity) {
  CollectionHelper helper;
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int i = segment * 24; i < (segment + 1) * 24; i++) docs.push_back(flatdoc(
        "id", "n" + std::to_string(i), "zone_s", "z" + std::to_string(i % 3),
        "number_i", (int64_t)(i % 5), "key_i", (int64_t)(i % 4), "stock_i", (int64_t)(i % 4),
        "color_s", i % 7 == 0 ? "red" : "blue"));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds({"n4", "n35"}, UpdateMessage::COMMIT).success);
  auto execute = [&](std::string body, int parallel, bool prepared) {
    auto req = localReq(helper.getSearchEngine());
    EXPECT_TRUE(api::read_json(req->rawRequest(), body, req->mr)) << body;
    req->testForcePrepare = prepared;
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  auto all = execute(R"({"collection":"main","query":"*:*","limit":-1,"sort":"id asc",
        "document_format":"rows",
        "fields":["id","zone_s","number_i","key_i","stock_i"]})", 1, false);
  std::string request = R"({"collection":"main","query":"color_s:red","limit":0,
        "ops":{"zones":{
        "field_facet":{"field":"zone_s","limit":-1,"ops":{
    "stock":{"query_facet":{"buckets":{"positive":"stock_i:>0"},"ops":{
      "ranges":{"range_facet":{"field":"number_i","start":0,"end":6,
        "gap":2,"ops":{
        "keys":{"field_facet":{"field":"key_i","limit":-1,"ops":{
          "rows":{"top_docs":{"sort":"id asc","limit":-1,"document_format":"rows",
        "fields":["id"]},"domain":{"query":"*:*","filter":["stock_i:>=2"]}
        }
        }}}
      }}}
    }}}
  }}}}})";
  auto child = [](const api::FacetResult& facet, std::string_view name, size_t row) -> const api::Val& {
    return std::get<api::ArrVal>(facet.ops.at(name)->kind).v[row];
  };
  for (int parallel : {1, -1}) for (bool prepared : {false, true}) {
    auto actual = execute(request, parallel, prepared);
    ASSERT_TRUE(actual->ok()) << actual->errorMsg();
    auto& zones = *actual->docList()->ops.at("zones")->facetResult();
    auto labels = std::get<api::ColStr>(zones.bucket_ids->kind).v;
    for (size_t z = 0; z < labels.size(); z++) {
      auto& stock = *child(zones, "stock", z).facetResult();
      auto& ranges = *child(stock, "ranges", 0).facetResult();
      auto bounds = std::get<api::ArrArrInt>(ranges.bucket_ids->kind).v;
      for (size_t r = 0; r < ranges.counts.size(); r++) {
        auto& keys = *child(ranges, "keys", r).facetResult();
        auto values = std::get<api::ColInt>(keys.bucket_ids->kind).v;
        for (size_t k = 0; k < values.size(); k++) {
          std::vector<std::string> expected, found;
          for (auto* response : all->responses)
              for (auto& doc : response->proto.ops.at("q")->docList()->docs) {
            int64_t number = doc.fields.at("number_i")->asInt();
            if (doc.fields.at("zone_s")->asString() == labels[z] && number >= bounds[r].v[0]
                && number < bounds[r].v[1] && doc.fields.at("key_i")->asInt() == values[k]
                && doc.fields.at("stock_i")->asInt() >= 2) {
              expected.emplace_back(doc.fields.at("id")->asString());
            }
          }
          for (auto& doc : child(keys, "rows", k).docList()->docs) {
            found.emplace_back(doc.fields.at("id")->asString());
          }
          EXPECT_EQ(expected, found);
        }
      }
    }
  }
}

TEST_F(CollapseTest, topDocsResetInsideBucketRetainsStructuralScope) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "zone_s", "x", "color_s", "red"),
      flatdoc("id", "b", "zone_s", "x", "color_s", "blue"),
      flatdoc("id", "c", "zone_s", "y", "color_s", "blue")}, UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"collection":"main","query":"color_s:red",
        "limit":0,"ops":{
      "zones":{"field_facet":{"field":"zone_s","ops":{"browse":{"top_docs":{
        "query":"color_s:red","limit":0,"ops":{
        "colors":{"field_facet":{"field":"color_s","limit":-1},"domain":{
        "query":"*:*"}}
      }}}}},"domain":{"query":"color_s:red"}}}})", req->mr));
    req->execute(parallel);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    auto& zones = *req->docList()->ops.at("zones")->facetResult();
    auto& list = *std::get<api::ArrVal>(zones.ops.at("browse")->kind).v[0].docList();
    auto& colors = *list.ops.at("colors")->facetResult();
    EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector(colors.counts.begin(), colors.counts.end()));
  }
}

TEST_F(CollapseTest, bucketResetKeepsRoutedOwningFilters) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "brand_s", "x", "color_s", "red", "stock_i", (int64_t)1),
      flatdoc("id", "b", "brand_s", "x", "color_s", "blue", "stock_i", (int64_t)0),
      flatdoc("id", "c", "brand_s", "y", "color_s", "blue", "stock_i", (int64_t)1)},
      UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), R"JSON({"collection":"main",
        "query":"color_s:red","limit":0,
      "filter":[{"query":"stock_i:1","except_ops":["brands"]}],
      "ops":{"brands":{"field_facet":{"field":"brand_s","ops":{
        "colors":{"field_facet":{"field":"color_s","limit":-1},"domain":{
        "query":"*:*","apply_parent_filters":true}},
        "red_only":{"expr_op":"sum(stock_i)","domain":{"filter":["color_s:red"]}
        }
      }}}}})JSON", req->mr));
    req->execute(parallel);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    auto& brands = *req->docList()->ops.at("brands")->facetResult();
    auto& colors = *std::get<api::ArrVal>(brands.ops.at("colors")->kind).v[0].facetResult();
    EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector(colors.counts.begin(), colors.counts.end()));
    EXPECT_EQ(1, std::get<api::ArrVal>(brands.ops.at("red_only")->kind).v[0].asInt());

    auto inner = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(inner->rawRequest(), R"JSON({"collection":"main",
      "query":"color_s:red","filter":["stock_i:1"],"limit":0,"ops":{
      "brands":{"field_facet":{"field":"brand_s","ops":{
        "list":{"top_docs":{"limit":0,"ops":{
          "colors":{"field_facet":{"field":"color_s","limit":-1},
            "domain":{"query":"*:*","apply_parent_filters":true}}
        }}}
      }}}}})JSON", inner->mr));
    inner->execute(parallel);
    ASSERT_TRUE(inner->ok()) << inner->errorMsg();
    const auto& outer = *inner->docList()->ops.at("brands")->facetResult();
    const auto& list = *std::get<api::ArrVal>(outer.ops.at("list")->kind).v[0].docList();
    EXPECT_EQ(2u, list.ops.at("colors")->facetResult()->counts.size());
  }
}

TEST_F(CollapseTest, smallBucketDomainsUseActualMemoryBudget) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 20'000; i++) docs.push_back(flatdoc("id", "d" + std::to_string(i),
      "key_s", "p" + std::to_string(i / 2), "color_s", i % 2 ? "red" : "blue", "stock_i", (int64_t)1));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 8192;
  for (bool reset : {false, true}) {
    auto req = localReq(helper.getSearchEngine());
    auto json = std::string(R"({"collection":"main","query":"id:d0","limit":1,
        "collapse":{"field":"key_s","ops":{
      "colors":{"field_facet":{"field":"color_s","limit":-1},"domain":{)")
        + (reset ? R"("query":"*:*",)" : "") + R"("filter":["stock_i:>0"]}}}}
        })";
    ASSERT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& colors = *groupOp(*req->docList()->groups, "colors", 0).facetResult();
    EXPECT_EQ(reset ? 2u : 1u, colors.counts.size());
    for (auto count : colors.counts) EXPECT_EQ(1, count);
    EXPECT_LT(req->memoryTracker.peak(), 8192u);
  }
}

TEST_F(CollapseTest, nestedTopDocsResetUsesPageMemoryBudget) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 20'000; i++) docs.push_back(flatdoc(
      "id", "d" + std::to_string(i), "key_s", "p" + std::to_string(i / 2),
      "stock_i", (int64_t)(i % 2 == 0)));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 8192;
  for (int parallel : {1, -1}) for (bool prepared : {false, true})
      for (bool parentFilters : {false, true}) {
    auto req = localReq(helper.getSearchEngine());
    auto json = std::string(R"({"collection":"main","query":"id:d0","limit":1,
      "collapse":{"field":"key_s","ops":{"browse":{"top_docs":{"query":"id:d0",
        "filter":["stock_i:>0"],"limit":0,"ops":{
        "variants":{"top_docs":{"sort":"id asc","limit":3,"fields":["id"],
        "document_format":"rows"},
          "domain":{"query":"*:*","filter":["stock_i:>=0"],"apply_parent_filters":)")
        + (parentFilters ? "true" : "false") + "}}}}}}}}";
    ASSERT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->testForcePrepare = prepared;
    req->execute(parallel);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& browse = *groupOp(*req->docList()->groups, "browse", 0).docList();
    const auto& variants = *browse.ops.at("variants")->docList();
    ASSERT_EQ(parentFilters ? 1 : 2, variants.row_count);
    EXPECT_EQ("d0", variants.docs[0].fields.at("id")->asString());
    if (!parentFilters) {
      EXPECT_EQ("d1", variants.docs[1].fields.at("id")->asString());
    }
    EXPECT_LT(req->memoryTracker.peak(), 8192u);
  }
}

TEST_F(CollapseTest, bucketFilterAdmissionProbesOncePerSegment) {
  CollectionHelper helper;
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 24; i++) docs.push_back(flatdoc("id", std::to_string(segment * 24 + i),
        "key_s", "p" + std::to_string(i), "color_s", "red", "stock_i", (int64_t)1));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto* cache = reader->filterCache();
  ASSERT_NE(nullptr, cache);
  for (int iteration = 0; iteration < 3; iteration++) {
    auto before = cache->counters();
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"collection":"main","query":"*:*",
        "limit":24,
      "collapse":{"field":"key_s","ops":{
        "colors":{"field_facet":{"field":"color_s"},"domain":{"query":"*:*","filter":["stock_i:>0"]}
        }
      }}})", req->mr));
    req->execute(1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_LE(cache->counters().misses - before.misses, 3u);
    EXPECT_EQ(24u, req->docList()->groups->keys.size());
  }
}

TEST_F(CollapseTest, matchAllBucketResetWithoutDeletes) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "color_s", "red"),
      flatdoc("id", "b", "color_s", "blue")}, UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"collection":"main",
      "query":"color_s:red","limit":0,"ops":{"offers":{"query_facet":{
        "buckets":{"all":"*:*"},"ops":{"colors":{
          "field_facet":{"field":"color_s","limit":-1},"domain":{"query":"*:*"}
        }
        }}}}})", req->mr));
    req->execute(parallel);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& offers = *req->docList()->ops.at("offers")->facetResult();
    ASSERT_EQ(1u, offers.counts.size());
    EXPECT_EQ(1, offers.counts[0]);
    const auto& colors = *std::get<api::ArrVal>(offers.ops.at("colors")->kind).v[0].facetResult();
    EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector(colors.counts.begin(), colors.counts.end()));
  }
}

TEST_F(CollapseTest, rootResetAndInheritReuseTheirSharedPrimitive) {
  CollectionHelper helper("root_reuse");
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "stock_i", (int64_t)1),
      flatdoc("id", "b", "stock_i", (int64_t)1),
      flatdoc("id", "c", "stock_i", (int64_t)0)}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
  SkipStatsScope skip;
  auto req = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"collection":"root_reuse",
        "ops":{
    "reset":{"top_docs":{"limit":0,"count_limit":-1},
      "domain":{"query":"*:*","filter":["stock_i:>0"]}},
    "inherit":{"top_docs":{"limit":0,"count_limit":-1},
      "domain":{"filter":["stock_i:>0"]}}
  }})", req->mr));
  req->execute(1);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  for (std::string_view name : {"reset", "inherit"}) {
    EXPECT_EQ(1, req->responses[0]->proto.ops.at(name)->docList()->found);
  }
  // Adoption serves once; an unnecessary second materialization would serve
  // that owned raw set again. Cached admission produces neither counter.
  EXPECT_EQ(SkipStats::ownedFilterMaterializations, SkipStats::ownedFilterServes);
}

TEST_F(CollapseTest, fusionChildrenRetainStructuralPrefixes) {
  CollectionHelper helper;
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int group = 0; group < 2; group++) {
      docs.push_back(flatdoc("id", std::format("{}-{}", segment, group),
          "key_s", std::to_string(group), "color_s", segment == 0 ? "red" : "blue"));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  for (int parallel : {1, -1}) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"collection":"main","query":"color_s:red",
      "sort":"id asc","limit":2,"collapse":{"field":"key_s","ops":{
        "f":{"fusion":{"sources":{"s":{"sort":"id asc","limit":3}},"rrf":{},"limit":1,
          "ops":{"q":{"top_docs":{"limit":0,"ops":{
            "colors":{"field_facet":{"field":"color_s","limit":-1},
              "domain":{"query":"*:*"}}}}}}}}}}})", req->mr));
    req->execute(parallel);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& groups = *req->docList()->groups;
    for (size_t i = 0; i < groups.keys.size(); i++) {
      const auto& fused = *groupOp(groups, "f", i).docList();
      const auto& child = *fused.ops.at("q")->docList();
      const auto& colors = *child.ops.at("colors")->facetResult();
      EXPECT_EQ((std::vector<int64_t>{2, 1}),
          std::vector<int64_t>(colors.counts.begin(), colors.counts.end()));
    }
  }
}

TEST_F(CollapseTest, multiColumnStringRanksSelectExtremaInBothModes) {
  for (bool array : {false, true}) {
    CollectionHelper helper;
    helper.clear();
    SchemaBuilder schema;
    auto& field = schema.field("value");
    field.type = api::FieldDef::FieldClass::STRING;
    field.index = api::FieldDef::IndexMode::NONE;
    field.column = true;
    field.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
    schema.set(helper.collection());
    helper.index(flatdoc("id", "a", "key_s", "x", "value", vecs("m", "z", "a")), UpdateMessage::COMMIT);
    helper.index(flatdoc("id", "b", "key_s", "y", "value", vecs("n", "b", "y")), UpdateMessage::COMMIT);
    helper.index(flatdoc("id", "0a", "key_s", "x", "value", vecs("c")), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "0b", "key_s", "y", "value", vecs("d")), UpdateMessage::COMMIT);
    for (int merge = 0; merge < 2; merge++) {
      for (auto [sort, first, second] : {
          std::tuple{R"("value asc")", "a", "b"}, {R"("value desc")", "z", "y"}}) {
        for (int parallel : {0, -1}) {
          auto req = run("main", "*:*", R"({"field":"key_s","head_sort":"id asc"})",
              -1, 0, parallel, true, 10, sort);
          ASSERT_OK(req);
          const auto& ranks = req->docList()->groups->ranks;
          ASSERT_EQ(2u, ranks.size());
          EXPECT_EQ(first, std::get<std::string_view>(ranks[0].v[0].kind));
          EXPECT_EQ(second, std::get<std::string_view>(ranks[1].v[0].kind));
        }
      }
      if (!merge) {
        CollectionHelper::UpdateBuilder update;
        update.commit(true, 1);
        ASSERT_TRUE(helper.submit(update).success);
      }
    }
  }
}
