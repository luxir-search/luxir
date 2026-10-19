// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <random>
#include <set>

#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using Key = std::vector<std::variant<std::monostate, std::string, int64_t>>;
struct Record {
  int64_t sequence;
  std::optional<std::string> text;
  std::optional<int64_t> number;
};
Key key(const Record& row, int shape, std::string_view policy) {
  Key result;
  if (shape != 1) {
    if (row.text) result.emplace_back(*row.text);
    else result.emplace_back();
  }
  if (shape != 0) {
    if (row.number) result.emplace_back(*row.number);
    else result.emplace_back();
  }
  bool missing = std::ranges::any_of(result, [](const auto& v) { return v.index() == 0; });
  if (missing && policy == "drop") return {};
  // A singleton's channel is disjoint from every ordinary tuple.
  if (missing && policy == "expand") return {std::string("singleton"), row.sequence, row.sequence};
  return result;
}
std::string spec(int shape, std::string_view policy) {
  auto field = shape == 0 ? R"("key_s")" : shape == 1 ? R"("key_i")" : R"(["key_s","key_i"])";
  return R"({"field":)" + std::string(field) + R"(,"missing":")" + std::string(policy) + R"("})";
}
std::vector<int64_t> sequence(const LocalReq& req, std::string_view name = "f") {
  std::vector<int64_t> result;
  for (auto* response : req.responses) {
    auto& list = *response->proto.ops.at(name)->docList();
    for (auto& doc : list.docs) result.push_back(doc.fields.at("seq_i")->asInt());
  }
  return result;
}
}

class FusionCollapseTest : public LuxirTest {
protected:
  auto run(std::string_view collection, std::string_view json, int parallel = 1) {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection(collection);
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg() << json;
    return req;
  }
};

TEST_F(FusionCollapseTest, exhaustiveOracle) {
  std::mt19937 random(732);
  std::map<int64_t, Record> records;
  std::vector<Doc> docs;
  for (int64_t i = 0; i < 48; i++) {
    Record row{i, {}, {}};
    auto doc = flatdoc("id", "d" + std::to_string(i), "seq_i", i,
        "rank_i", (int64_t)(random() % 5), "score_i", (int64_t)(random() % 4),
        "body_w", i % 5 ? "keep" : "other");
    if (i % 7) { row.text = "p" + std::to_string(random() % 7); doc.emplace_back("key_s", *row.text); }
    if (i % 6) { row.number = INT64_MAX - random() % 5; doc.emplace_back("key_i", *row.number); }
    records.emplace(i, row);
    docs.push_back(std::move(doc));
  }
  for (int segments : {1, 3}) {
    std::string collection = "fusion_collapse_" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int s = 0; s < segments; s++) ASSERT_TRUE(helper.indexAll(
        std::span<const Doc>(docs).subspan(s * 48 / segments, 48 / segments),
        UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper.deleteByIds({"d2", "d31"}, UpdateMessage::COMMIT).success);
    std::array<std::vector<int64_t>, 2> sources;
    std::array<std::string, 2> sourceBody{
        R"JSON("query":"rescore(body_w:keep,score_i)","sort":"score desc")JSON",
        R"("query":"*:*","sort":"rank_i asc","filter":["seq_i:>5"])"};
    for (size_t s = 0; s < sources.size(); s++) {
      auto full = run(collection, "{" + sourceBody[s]
          + R"(,"limit":-1,"fields":["seq_i"],"document_format":"rows"})");
      sources[s] = sequence(*full, "q");
    }
    for (int shape : {0, 1, 2}) for (std::string policy : {"expand", "group", "drop"}) {
      for (auto [sourceCollapse, fusionCollapse] : {
               std::pair{true, false}, {false, true}, {true, true}}) {
        std::map<int64_t, float> rrf;
        std::string sourceJson;
        for (size_t s = 0; s < sources.size(); s++) {
          std::set<Key> seen;
          int rank = 0;
          for (int64_t id : sources[s]) {
            if (sourceCollapse) {
              auto identity = key(records.at(id), s == 0 ? shape : 1, policy);
              if (identity.empty() || !seen.insert(identity).second) continue;
            }
            rrf[id] += 1.0f / (61 + rank++);
            if (rank == 12) break;
          }
          if (s) sourceJson += ',';
          sourceJson += '"' + std::to_string(s) + R"(":{)" + sourceBody[s] + R"(,"limit":12)";
          if (sourceCollapse) sourceJson += R"(,"collapse":)" + spec(s == 0 ? shape : 1, policy);
          sourceJson += '}';
        }
        std::vector<int64_t> ranked;
        int64_t eligible = 0;
        for (auto [id, score] : rrf) {
          if (fusionCollapse && key(records.at(id), shape, policy).empty()) continue;
          ranked.push_back(id);
          eligible++;
        }
        std::sort(ranked.begin(), ranked.end(), [&](int64_t a, int64_t b) {
          return rrf[a] == rrf[b] ? a < b : rrf[a] > rrf[b];
        });
        if (fusionCollapse) {
          std::set<Key> seen;
          std::erase_if(ranked, [&](int64_t id) {
            return !seen.insert(key(records.at(id), shape, policy)).second;
          });
        }
        std::string body = R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{)" + sourceJson
            + R"JSON(},"get_number":true,"get_scores":true,"fields":["seq_i"],"document_format":"rows",
              "batch_size":2,"ops":{"evidence":"sum(seq_i)"})JSON";
        if (fusionCollapse) body += R"(,"collapse":)" + spec(shape, policy);
        for (int parallel : {1, -1}) {
          std::vector<int64_t> pages;
          for (int64_t offset = 0; offset < std::max<int64_t>(1, (int64_t)ranked.size()); offset += 5) {
            auto req = run(collection, body + R"(,"limit":5,"offset":)" + std::to_string(offset)
                + "}}}}", parallel);
            ASSERT_OK(req);
            auto actual = sequence(*req);
            auto end = ranked.begin() + std::min<int64_t>(offset + 5, (int64_t)ranked.size());
            EXPECT_EQ((std::vector<int64_t>(ranked.begin() + offset, end)), actual);
            pages.insert(pages.end(), actual.begin(), actual.end());
            EXPECT_EQ((int64_t)ranked.size(), req->docList("f")->found.value_or(-1));
            if (fusionCollapse) {
              EXPECT_EQ(eligible, req->docList("f")->found_docs.value_or(-1));
            }
            int64_t total = 0;
            for (auto [id, score] : rrf) total += id;
            auto& final = *req->responses.back()->proto.ops.at("f")->docList();
            EXPECT_EQ(total, final.ops.at("evidence")->asInt());
            for (auto* response : req->responses) {
              const auto& list = *response->proto.ops.at("f")->docList();
              for (size_t i = 0; i < list.docs.size(); i++) {
                auto id = list.docs[i].fields.at("seq_i")->asInt();
                EXPECT_EQ(rrf[id], list.docs[i].fields.at("_score_")->asFloat());
                if (fusionCollapse) {
                  EXPECT_EQ(rrf[id], list.groups->ranks[i].v[0].asFloat());
                }
              }
            }
          }
          EXPECT_EQ(ranked, pages);
          for (int limit : {0, -1}) {
            auto req = run(collection, body + R"(,"limit":)" + std::to_string(limit) + "}}}}", parallel);
            ASSERT_OK(req);
            EXPECT_EQ(limit == 0 ? std::vector<int64_t>{} : ranked, sequence(*req));
            EXPECT_EQ((int64_t)ranked.size(), req->docList("f")->found.value_or(-1));
          }
        }
      }
    }
  }
}

TEST_F(FusionCollapseTest, pageOperationsAndReset) {
  CollectionHelper helper;
  for (int i = 0; i < 8; i++) {
    helper.index(flatdoc("id", std::to_string(i), "seq_i", (int64_t)i,
        "key_s", i < 4 ? "a" : "b", "color_s", i % 2 ? "red" : "blue"));
    if (i == 3 || i == 7) helper.commit();
  }
  for (int parallel : {1, -1}) {
    auto req = run("main", R"JSON({"ops":{"f":{"fusion":{"rrf":{},"sources":{
      "a":{"sort":"seq_i asc","limit":2},"b":{"sort":"seq_i desc","limit":2}},
      "collapse":{"field":"key_s","head_sort":"seq_i desc","get_number":true,"ops":{
        "low":"min(seq_i)","members":{"top_docs":{"sort":"seq_i asc","limit":-1,
          "fields":["seq_i"],"document_format":"rows"}},
        "swatches":{"field_facet":{"field":"color_s"}},
        "all":{"top_docs":{"sort":"seq_i asc","limit":-1,"fields":["seq_i"],
          "document_format":"rows"},"domain":{"query":"*:*"}}
      }},"get_scores":true,"get_number":true,"batch_size":1,
      "fields":["seq_i"],"document_format":"rows"}}}})JSON", parallel);
    ASSERT_OK(req);
    EXPECT_EQ((std::vector<int64_t>{1, 7}), sequence(*req));
    size_t group = 0;
    for (auto* response : req->responses) {
      auto& list = *response->proto.ops.at("f")->docList();
      EXPECT_EQ(2, list.found.value_or(-1));
      EXPECT_EQ(4, list.found_docs.value_or(-1));
      ASSERT_EQ(1u, list.groups->counts.size());
      EXPECT_EQ(2, list.groups->counts[0]);
      EXPECT_EQ(1.0f / 61, list.groups->ranks[0].v[0].asFloat());
      EXPECT_EQ(group == 0 ? 1.0f / 62 : 1.0f / 61,
          list.docs[0].fields.at("_score_")->asFloat());
      auto& ops = list.groups->ops;
      EXPECT_EQ(group == 0 ? 0 : 6, std::get<api::ArrVal>(ops.at("low")->kind).v[0].asInt());
      auto& members = *std::get<api::ArrVal>(ops.at("members")->kind).v[0].docList();
      auto& all = *std::get<api::ArrVal>(ops.at("all")->kind).v[0].docList();
      std::vector<int64_t> actualMembers, actualAll;
      for (auto& doc : members.docs) actualMembers.push_back(doc.fields.at("seq_i")->asInt());
      for (auto& doc : all.docs) actualAll.push_back(doc.fields.at("seq_i")->asInt());
      EXPECT_EQ(group == 0 ? (std::vector<int64_t>{0, 1}) : (std::vector<int64_t>{6, 7}), actualMembers);
      EXPECT_EQ(group == 0 ? (std::vector<int64_t>{0, 1, 2, 3})
                          : (std::vector<int64_t>{4, 5, 6, 7}), actualAll);
      for (auto& doc : members.docs) EXPECT_EQ(nullptr, doc.fields.find("_score_"));
      const auto& colors = *std::get<api::ArrVal>(ops.at("swatches")->kind).v[0].facetResult();
      EXPECT_EQ((std::vector<int64_t>{1, 1}),
          (std::vector<int64_t>(colors.counts.begin(), colors.counts.end())));
      group++;
    }
    EXPECT_EQ(2u, group);
  }
}

TEST_F(FusionCollapseTest, fusionOpsFacetsCountGroups) {
  CollectionHelper helper;
  for (int i = 0; i < 8; i++) {
    helper.index(flatdoc("id", std::to_string(i), "seq_i", (int64_t)i, "key_s", i < 4 ? "a" : "b"));
  }
  helper.commit();
  // Evidence is {0, 1} from source a and {7, 6} from source b: two docs per key.
  auto req = run("main", R"JSON({"ops":{"f":{"fusion":{"rrf":{},"sources":{
    "a":{"sort":"seq_i asc","limit":2},"b":{"sort":"seq_i desc","limit":2}},
    "collapse":"key_s","ops":{"groups":{"field_facet":{"field":"key_s"}},
      "docs":{"field_facet":{"field":"key_s","count_unit":"docs"}}}}}}})JSON");
  ASSERT_OK(req);
  auto& ops = req->responses.back()->proto.ops.at("f")->docList()->ops;
  auto counts = [&](std::string_view name) {
    const auto& facet = *ops.at(name)->facetResult();
    return std::vector<int64_t>(facet.counts.begin(), facet.counts.end());
  };
  EXPECT_EQ((std::vector<int64_t>{1, 1}), counts("groups"));
  EXPECT_EQ((std::vector<int64_t>{2, 2}), counts("docs"));
}

TEST_F(FusionCollapseTest, sourceKnnAndValidation) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vector = schema.templ("_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());
  for (int i = 0; i < 6; i++) helper.index(flatdoc("id", std::to_string(i), "seq_i", (int64_t)i,
      "key_s", i < 4 ? "a" : "b", "embedding_v", std::vector<float>{(float)i}));
  helper.commit();
  for (int parallel : {1, -1}) {
    auto req = run("main", R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{"s":{
      "query":{"knn":{"field":"embedding_v","query":[0],"k":2,"exact":true,"unit":"groups"}},
      "collapse":"key_s","limit":2}},"limit":-1,"get_number":true,
      "fields":["seq_i"],"document_format":"rows"}}}})", parallel);
    ASSERT_OK(req);
    EXPECT_EQ((std::vector<int64_t>{0, 4}), sequence(*req));
    EXPECT_EQ(2, req->docList("f")->found.value_or(-1));
  }
  for (auto option : {R"("head_sort":"seq_i asc")", R"("sort":"m asc")",
                      R"("get_number":true)", R"JSON("ops":{"m":"min(seq_i)"})JSON"}) {
    auto req = localReq(helper.getSearchEngine());
    parseQueryRequest(R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{"s":{
      "collapse":{"field":"key_s",)" + std::string(option) + "}}}}}}}", req->rawRequest(), req->mr);
    req->execute();
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("collapse supports only field and missing"));
  }
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"JSON({"ops":{"f":{"fusion":{"rrf":{},"sources":{"s":{}},
    "collapse":{"field":"key_s","sort":"m asc","ops":{"m":"min(seq_i)"}}}}}})JSON",
      req->rawRequest(), req->mr);
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("fusion.collapse.sort"));
}

TEST_F(FusionCollapseTest, linearNormalizesCollapsedSourceHeads) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vector = schema.templ("_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.column = true;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());
  for (int i = 0; i < 6; i++) helper.index(flatdoc("id", std::to_string(i), "seq_i", (int64_t)i,
      "key_s", i < 4 ? "a" : "b", "embedding_v", std::vector<float>{(float)i}));
  helper.commit();
  // Min-max runs over the source's group heads, not its uncollapsed matches:
  // the second head is the source's worst row, so it normalizes to 0.
  auto req = run("main", R"({"ops":{"f":{"fusion":{"linear":{},"sources":{"s":{
    "query":{"knn":{"field":"embedding_v","query":[0],"k":6,"exact":true}},
    "collapse":"key_s","limit":2}},"limit":-1,"get_scores":true,
    "fields":["seq_i"],"document_format":"rows"}}}})");
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<int64_t>{0, 4}), sequence(*req));
  auto& docs = req->docList("f")->docs;
  ASSERT_EQ(2u, docs.size());
  EXPECT_FLOAT_EQ(1.0f, std::get<float>(docs[0].fields.at("_score_")->kind));
  EXPECT_FLOAT_EQ(0.0f, std::get<float>(docs[1].fields.at("_score_")->kind));
}

TEST_F(FusionCollapseTest, structuralScopesAndParallelOrdinaryChildren) {
  CollectionHelper helper;
  for (int i = 0; i < 30; i++) {
    helper.index(flatdoc("id", std::to_string(i), "seq_i", (int64_t)i,
        "key_s", "p" + std::to_string(i % 3), "cat_s", i % 2 ? "red" : "blue",
        "body_w", "prefix" + std::to_string(i % 7)));
    if (i % 10 == 9) helper.commit();
  }
  std::string fusion = R"JSON({"fusion":{"rrf":{},"sources":{"s":{
    "sort":"seq_i asc","limit":12}},"filter":["seq_i:<10"],
    "collapse":{"field":"key_s","get_number":true,"ops":{
      "all":{"top_docs":{"sort":"seq_i asc","limit":-1,"fields":["seq_i"],
        "document_format":"rows"},"domain":{"query":"*:*"}},
      "filtered":{"top_docs":{"sort":"seq_i asc","limit":-1,"fields":["seq_i"],
        "document_format":"rows"},"domain":{"query":"*:*","apply_parent_filters":true}}
    }},"limit":-1,"get_number":true,"fields":["seq_i"],"document_format":"rows",
    "ops":{"ordinary":{"top_docs":{"query":"body_w:prefix*","limit":-1,
      "fields":["seq_i"],"document_format":"rows"}}}}})JSON";
  for (int parallel : {1, -1}) for (int placement : {0, 1, 2}) {
    std::string body = placement == 0 ? R"({"ops":{"f":)" + fusion + "}}"
        : placement == 1 ? R"({"limit":0,"ops":{"colors":{"field_facet":{"field":"cat_s",
            "ops":{"f":)" + fusion + "}}}}}"
        : R"({"collapse":{"field":"cat_s","ops":{"f":)" + fusion + R"(}},"limit":-1})";
    auto req = run("main", body, parallel);
    ASSERT_OK(req);
    auto verify = [&](const api::DocList& list, int color) {
      auto& ordinary = *list.ops.at("ordinary")->docList();
      std::vector<int64_t> actual, expected;
      for (auto& doc : ordinary.docs) actual.push_back(doc.fields.at("seq_i")->asInt());
      for (int i = 0; i < 10; i++) if (color < 0 || i % 2 == color) expected.push_back(i);
      std::sort(actual.begin(), actual.end());
      EXPECT_EQ(expected, actual);
      auto& table = *list.groups;
      for (size_t slot = 0; slot < table.keys.size(); slot++) {
        int64_t group = list.docs[slot].fields.at("seq_i")->asInt() % 3;
        for (auto name : {"all", "filtered"}) {
          const auto& child = *std::get<api::ArrVal>(table.ops.at(name)->kind).v[slot].docList();
          actual.clear(); expected.clear();
          for (auto& doc : child.docs) actual.push_back(doc.fields.at("seq_i")->asInt());
          for (int i = 0; i < (std::string_view(name) == "all" ? 30 : 10); i++) {
            if (i % 3 == group && (color < 0 || i % 2 == color)) expected.push_back(i);
          }
          EXPECT_EQ(expected, actual);
        }
      }
    };
    if (placement == 0) verify(*req->docList("f"), -1);
    else {
      auto value = placement == 1 ? req->docList()->ops.at("colors")->facetResult()->ops.at("f")
                                 : req->docList()->groups->ops.at("f");
      const auto& lists = std::get<api::ArrVal>(value->kind).v;
      ASSERT_EQ(2u, lists.size());
      for (size_t i = 0; i < lists.size(); i++) {
        // Both parent kinds use document-order ties: blue first, then red.
        verify(*lists[i].docList(), (int)i);
      }
    }
  }
}
