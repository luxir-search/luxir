// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <set>

#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class CollapseNestingTest : public LuxirTest {
protected:
  struct Record {
    std::string id, key, owner, category;
    int64_t number, stock;
    bool matches;
  };

  void indexRows() {
    CollectionHelper helper;
    for (int segment = 0; segment < 2; segment++) {
      std::vector<Doc> docs;
      for (int i = segment * 4; i < (segment + 1) * 4; i++) {
        docs.push_back(flatdoc("id", std::string(1, (char)('a' + i)),
            "key_s", i < 2 || i == 6 ? "p" : i < 4 || i == 7 ? "q" : "r",
            "owner_s", i == 5 ? "B" : "A", "cat_s", i < 3 || i == 6 ? "x" : "y",
            "n_i", (int64_t)(i + 1), "stock_i", (int64_t)(i != 0 && i != 5),
            "body_w", i < 6 ? "keep" : "other"));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
    ASSERT_TRUE(helper.indexAll({flatdoc("id", "deleted", "key_s", "gone",
        "cat_s", "x", "n_i", (int64_t)2, "body_w", "keep")}, UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper.deleteByIds({"deleted"}, UpdateMessage::COMMIT).success);
  }

  auto execute(std::string_view json, int parallel = 1, bool prepared = false) {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("main");
    req->testForcePrepare = prepared;
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg() << json;
    return req;
  }

  std::vector<Record> records() {
    auto req = execute(R"({"query":"*:*","limit":-1,"sort":"id asc","document_format":"rows",
        "fields":["id","key_s","owner_s","cat_s","n_i","stock_i","body_w"]})");
    std::vector<Record> result;
    for (auto& doc : req->docList()->docs) {
      const auto& f = doc.fields;
      result.push_back({std::string(f.at("id")->asString()), std::string(f.at("key_s")->asString()),
          std::string(f.at("owner_s")->asString()), std::string(f.at("cat_s")->asString()),
          f.at("n_i")->asInt(), f.at("stock_i")->asInt(), f.at("body_w")->asString() == "keep"});
    }
    return result;
  }

  static std::vector<std::string> ids(const api::DocList& list) {
    std::vector<std::string> result;
    for (auto& doc : list.docs) result.emplace_back(doc.fields.at("id")->asString());
    return result;
  }

  static std::vector<std::string> heads(const std::vector<const Record*>& rows) {
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (auto* row : rows) if (seen.insert(row->key).second) result.push_back(row->id);
    return result;
  }

  static std::map<std::string, int64_t> counts(const api::FacetResult& facet) {
    std::map<std::string, int64_t> result;
    auto labels = std::get<api::ColStr>(facet.bucket_ids->kind).v;
    for (size_t i = 0; i < labels.size(); i++) result[std::string(labels[i])] = facet.counts[i];
    return result;
  }

  // Brute-force source windows and RRF, independent of Fusion's collectors.
  static auto fusionOracle(const std::vector<Record>& rows) {
    std::vector<const Record*> matches, evidence;
    for (auto& row : rows) if (row.matches) matches.push_back(&row);
    std::map<const Record*, float> scores;
    for (size_t i = 0; i < 3; i++) scores[matches[i]] += 1.0f / (61 + i);
    for (size_t i = 0; i < 2; i++) scores[matches[matches.size() - 1 - i]] += 1.0f / (61 + i);
    for (auto* row : matches) if (scores.contains(row)) evidence.push_back(row);
    auto ranking = evidence;
    std::sort(ranking.begin(), ranking.end(), [&](auto* a, auto* b) {
      return scores[a] == scores[b] ? a->id < b->id : scores[a] > scores[b];
    });
    return std::pair{evidence, ranking};
  }
};

TEST_F(CollapseNestingTest, collapsedListsUnderFacetBuckets) {
  indexRows();
  auto all = records();
  SearchOverridesGuard guard(forcedFacetBucketBlockBuckets);
  forcedFacetBucketBlockBuckets = 1;
  std::vector<std::string> facets{
      R"("field_facet":{"field":"cat_s","limit":-1,)",
      R"("range_facet":{"field":"n_i","start":0,"end":10,"gap":5,)",
      R"("query_facet":{"buckets":{"x":"cat_s:x","y":"cat_s:y"},)"};
  for (size_t kind = 0; kind < facets.size(); kind++) {
    auto json = R"({"query":"body_w:keep","collapse":"owner_s","limit":0,"ops":{"f":{)"
        + facets[kind] + R"("ops":{"lists":{"top_docs":{"collapse":"key_s","sort":"id asc",
          "count_limit":-1,"limit":1,"batch_size":1,"fields":["id"],"document_format":"rows",
          "ops":{
            "default":{"field_facet":{"field":"cat_s"}},
            "groups":{"field_facet":{"field":"cat_s","count_unit":"groups"}},
            "docs":{"field_facet":{"field":"cat_s","count_unit":"docs"}},
            "nested":{"top_docs":{"collapse":"id","limit":0,"ops":{
              "counts":{"field_facet":{"field":"cat_s"}}}}},
            "after":{"field_facet":{"field":"cat_s"}},
            "reset":{"top_docs":{"sort":"id asc","limit":-1,"count_limit":-1,
              "fields":["id"],"document_format":"rows"},
              "domain":{"query":"*:*","filter":["stock_i:>0"]}}
          }}}}}}}})";
    for (int parallel : {1, -1}) for (bool prepared : {false, true}) {
      SCOPED_TRACE(std::to_string(kind) + "/" + std::to_string(parallel) + "/"
          + std::to_string(prepared));
      auto req = execute(json, parallel, prepared);
      ASSERT_OK(req);
      ASSERT_EQ(1u, req->responses.size());
      const auto& facet = *req->docList()->ops.at("f")->facetResult();
      const auto& lists = std::get<api::ArrVal>(facet.ops.at("lists")->kind).v;
      ASSERT_EQ(2u, lists.size());
      for (size_t bucket = 0; bucket < lists.size(); bucket++) {
        auto inBucket = [&](const Record& r) {
          if (kind == 1) {
            auto bounds = std::get<api::ArrArrInt>(facet.bucket_ids->kind).v[bucket].v;
            return r.number >= bounds[0] && r.number < bounds[1];
          }
          return r.category == std::get<api::ColStr>(facet.bucket_ids->kind).v[bucket];
        };
        std::vector<const Record*> members;
        std::vector<std::string> resetIds;
        std::set<std::string> owners;
        std::map<std::string, std::set<std::string>> groups;
        std::map<std::string, int64_t> documents, groupCounts;
        for (auto& row : all) if (inBucket(row)) {
          if (row.stock) resetIds.push_back(row.id);
          if (!row.matches) continue;
          members.push_back(&row);
          owners.insert(row.owner);
          groups[row.category].insert(row.key);
          documents[row.category]++;
        }
        for (auto& [category, keys] : groups) groupCounts[category] = (int64_t)keys.size();
        auto expected = heads(members);
        const auto& list = *lists[bucket].docList();
        EXPECT_EQ((int64_t)owners.size(), facet.counts[bucket]);
        EXPECT_EQ((int64_t)expected.size(), list.found.value_or(-1));
        EXPECT_EQ((int64_t)members.size(), list.found_docs.value_or(-1));
        expected.resize(std::min<size_t>(1, expected.size()));
        EXPECT_EQ(expected, ids(list));
        EXPECT_FALSE(list.more);
        for (auto name : {"default", "groups", "after"})
          EXPECT_EQ(groupCounts, counts(*list.ops.at(name)->facetResult()));
        EXPECT_EQ(documents, counts(*list.ops.at("docs")->facetResult()));
        auto& nested = *list.ops.at("nested")->docList();
        EXPECT_EQ(documents, counts(*nested.ops.at("counts")->facetResult()));
        auto& reset = *list.ops.at("reset")->docList();
        EXPECT_EQ(resetIds, ids(reset));
        EXPECT_EQ((int64_t)resetIds.size(), reset.found.value_or(-1));
      }
    }
  }
}

TEST_F(CollapseNestingTest, collapsedListUnderFusionUsesAllEvidence) {
  indexRows();
  auto all = records();
  auto [evidence, ranking] = fusionOracle(all);
  for (int parallel : {1, -1}) {
    auto req = execute(R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{
      "forward":{"query":"body_w:keep","sort":"id asc","limit":3},
      "reverse":{"query":"body_w:keep","sort":"id desc","limit":2}},
      "limit":1,"offset":1,"count_limit":-1,"fields":["id"],"document_format":"rows",
      "ops":{"products":{"top_docs":{"collapse":"key_s","sort":"id asc","limit":-1,
        "count_limit":-1,"batch_size":1,"fields":["id"],"document_format":"rows"}}}
    }}}})", parallel);
    ASSERT_OK(req);
    std::vector<std::string> fusedIds, childIds;
    int64_t found = -1, groups = -1, documents = -1;
    for (auto* response : req->responses) {
      const auto& fusion = *response->proto.ops.at("f")->docList();
      for (auto& id : ids(fusion)) fusedIds.push_back(id);
      if (fusion.found) found = *fusion.found;
      if (auto* op = fusion.ops.find("products")) {
        const auto& child = *(**op).docList();
        for (auto& id : ids(child)) childIds.push_back(id);
        if (child.found) groups = *child.found;
        if (child.found_docs) documents = *child.found_docs;
        if (child.groups) {
          EXPECT_EQ(child.row_count, (int64_t)child.groups->keys.size());
        }
      }
    }
    EXPECT_EQ((std::vector<std::string>{ranking[1]->id}), fusedIds);
    EXPECT_EQ((int64_t)evidence.size(), found);
    EXPECT_EQ(heads(evidence), childIds);
    EXPECT_EQ((int64_t)heads(evidence).size(), groups);
    EXPECT_EQ((int64_t)evidence.size(), documents);
  }
}

TEST_F(CollapseNestingTest, fusionUnderCollapsedListRanksMatchingDocuments) {
  indexRows();
  auto all = records();
  auto [evidence, ranking] = fusionOracle(all);
  std::vector<const Record*> matches;
  for (auto& row : all) if (row.matches) matches.push_back(&row);
  std::vector<std::string> expected;
  for (auto* row : ranking) expected.push_back(row->id);
  for (int parallel : {1, -1}) {
    auto req = execute(R"({"query":"body_w:keep","collapse":"key_s","sort":"id asc","limit":1,
      "count_limit":-1,"fields":["id"],"document_format":"rows","ops":{"f":{"fusion":{
        "rrf":{},"sources":{"forward":{"sort":"id asc","limit":3},
          "reverse":{"sort":"id desc","limit":2}},"limit":-1,"count_limit":-1,
        "fields":["id"],"document_format":"rows"
      }}}})", parallel);
    ASSERT_OK(req);
    EXPECT_EQ((std::vector<std::string>{heads(matches).front()}), ids(*req->docList()));
    EXPECT_EQ((int64_t)heads(matches).size(), req->docList()->found.value_or(-1));
    const auto& fusion = *req->docList()->ops.at("f")->docList();
    EXPECT_EQ(expected, ids(fusion));
    EXPECT_EQ((int64_t)evidence.size(), fusion.found.value_or(-1));
  }
}
