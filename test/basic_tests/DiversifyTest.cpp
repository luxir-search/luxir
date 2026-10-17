// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <random>
#include <variant>

#include "luxir/search/Collector.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using Key = std::vector<std::variant<std::monostate, std::string, int64_t>>;
std::vector<std::string> ids(const LocalReq& request, std::string_view name = "q") {
  std::vector<std::string> result;
  for (auto* response : request.responses) {
    for (auto& doc : response->proto.ops.at(name)->docList()->docs) {
      result.emplace_back(doc.fields.at("id")->asString());
    }
  }
  return result;
}
struct Expected {
  std::vector<std::string> rows;
  int64_t groups;
};
Expected capped(const LocalReq& full, const std::vector<std::string>& fields,
                std::string_view policy, int cap) {
  Expected result{{}, 0};
  std::map<Key, int> counts;
  for (auto* response : full.responses) for (const auto& doc : response->proto.ops.at("q")->docList()->docs) {
    Key key;
    bool missing = false;
    for (const auto& field : fields) {
      auto* value = doc.fields.find(field);
      if (!value || (*value)->isNull()) { key.emplace_back(); missing = true; }
      else if (auto* text = std::get_if<std::string_view>(&(*value)->kind)) {
        key.emplace_back(std::string(*text));
      }
      else key.emplace_back((*value)->asInt());
    }
    if (missing && policy == "drop") continue;
    auto id = std::string(doc.fields.at("id")->asString());
    if (missing && policy == "expand") {
      result.groups++;
      result.rows.push_back(id);
    } else {
      auto [found, added] = counts.try_emplace(std::move(key), 0);
      result.groups += added;
      if (found->second++ < cap) result.rows.push_back(id);
    }
  }
  return result;
}
std::string spec(const std::vector<std::string>& fields, std::string_view policy, int cap) {
  std::string result = R"({"field":[)";
  for (const auto& field : fields) result += '"' + field + "\",";
  result.back() = ']';
  return result + R"(,"missing":")" + std::string(policy)
      + R"(","max_per_key":)" + std::to_string(cap) + '}';
}
}

class DiversifyTest : public LuxirTest {
protected:
  auto run(std::string_view collection, std::string_view body, int parallel = 1) {
    auto request = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(body, request->rawRequest(), request->mr);
    request->collection(collection);
    request->execute(parallel);
    EXPECT_TRUE(request->ok()) << request->errorMsg() << body;
    return request;
  }
};

TEST_F(DiversifyTest, exhaustiveOracle) {
  std::mt19937 random(914);
  std::vector<Doc> docs;
  for (int64_t i = 0; i < 60; i++) {
    auto doc = flatdoc("id", std::to_string(i), "rank_i", (int64_t)(random() % 4),
        "body_w", i % 4 ? "alpha alpha beta" : "alpha beta");
    if (i % 7) doc.emplace_back("key_s", "p" + std::to_string(random() % 7));
    if (i % 6) doc.emplace_back("key_i", (int64_t)(INT64_MAX - random() % 4));
    docs.push_back(std::move(doc));
  }
  for (int segments : {1, 3, 7}) {
    std::string collection = "diverse_" + std::to_string(segments);
    CollectionHelper helper(collection);
    for (int s = 0; s < segments; s++) {
      int first = s * 60 / segments, last = (s + 1) * 60 / segments;
      ASSERT_TRUE(helper.indexAll(std::span<const Doc>(docs).subspan(first, last - first),
          UpdateMessage::COMMIT).success);
    }
    ASSERT_TRUE(helper.deleteByIds({"2", "41"}, UpdateMessage::COMMIT).success);
    {
      // Match-all in index order without totals: the constant-score first-K pull.
      auto all = run(collection, R"({"fields":["id","key_s","key_i","rank_i"],)"
          R"("document_format":"rows","limit":-1})");
      auto expected = capped(*all, {"key_s"}, "expand", 2);
      expected.rows.resize(std::min<size_t>(7, expected.rows.size()));
      auto actual = run(collection, R"({"fields":["id"],"document_format":"rows","limit":7,)"
          R"("diversify":)" + spec({"key_s"}, "expand", 2) + "}");
      EXPECT_EQ(expected.rows, ids(*actual));
    }
    for (std::string sort : {"score desc", "rank_i asc", "key_s desc"}) {
      std::string base = R"({"query":"body_w:alpha","fields":["id","key_s","key_i","rank_i"],)"
          + std::string(R"("get_number":true,"get_scores":true,"document_format":"rows","sort":")")
          + sort + '"';
      auto full = run(collection, base + R"(,"limit":-1})");
      for (const std::vector<std::string>& fields : {
          std::vector<std::string>{"key_s"}, {"key_i"}, {"key_s", "key_i"}, {"key_s", "key_i", "rank_i"}}) {
        for (auto policy : {"expand", "group", "drop"}) for (int cap : {1, 2, 5}) {
          auto expected = capped(*full, fields, policy, cap);
          std::string body = base + R"(,"diversify":)" + spec(fields, policy, cap);
          for (int parallel : {1, -1}) {
            SCOPED_TRACE(collection + sort + spec(fields, policy, cap));
            auto actual = run(collection, body + R"(,"limit":-1,"batch_size":9})", parallel);
            ASSERT_TRUE(actual->ok());
            EXPECT_EQ(expected.rows, ids(*actual));
            EXPECT_EQ((int64_t)expected.rows.size(), actual->docList()->found);
            EXPECT_EQ(expected.groups, actual->docList()->found_groups);
            EXPECT_FALSE(actual->docList()->groups);
            auto page = run(collection, body + R"(,"offset":3,"limit":7})", parallel);
            auto first = expected.rows.begin() + std::min<size_t>(3, expected.rows.size());
            auto last = expected.rows.begin() + std::min<size_t>(10, expected.rows.size());
            EXPECT_EQ((std::vector<std::string>{first, last}), ids(*page));
            auto pruningBody = body;
            pruningBody.replace(pruningBody.find("\"get_number\":true"), 17, "\"get_number\":false");
            auto pruned = run(collection, pruningBody + R"(,"offset":3,"limit":7})", parallel);
            EXPECT_EQ((std::vector<std::string>{first, last}), ids(*pruned));
            EXPECT_FALSE(pruned->docList()->found_groups);
            // Without scores the field-sort pruning arms (bound order, seeding,
            // match windows) are eligible too.
            auto bare = pruningBody;
            bare.replace(bare.find("\"get_scores\":true"), 17, "\"get_scores\":false");
            EXPECT_EQ((std::vector<std::string>{first, last}),
                ids(*run(collection, bare + R"(,"offset":3,"limit":7})", parallel)));
            auto counts = run(collection, body + R"(,"limit":0})", parallel);
            EXPECT_EQ((int64_t)expected.rows.size(), counts->docList()->found);
            EXPECT_EQ(expected.groups, counts->docList()->found_groups);
          }
          if (fields.size() == 2 && cap == 2) {
            std::vector<std::string> pages;
            for (size_t offset = 0; offset < expected.rows.size(); offset += 7) {
              auto page = run(collection, body + R"(,"limit":7,"offset":)"
                  + std::to_string(offset) + '}', -1);
              auto rows = ids(*page);
              pages.insert(pages.end(), rows.begin(), rows.end());
            }
            EXPECT_EQ(expected.rows, pages);
          }
        }
      }
    }
  }
}

TEST_F(DiversifyTest, pruningOpsScopeAndNestedLists) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 40; i++) docs.push_back(flatdoc("id", std::to_string(i), "key_s", "one",
      "bucket_s", i % 2 ? "odd" : "even", "rank_i", (int64_t)i));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) for (bool count : {false, true}) {
    auto actual = run("main", R"({"sort":"rank_i desc","limit":5,"fields":["id"],
        "document_format":"rows","diversify":{"field":"key_s","max_per_key":2},"get_number":)"
        + std::string(count ? "true" : "false") + R"(,"ops":{
        "all":{"field_facet":{"field":"key_s"}},
        "nested":{"field_facet":{"field":"bucket_s","ops":{
          "rows":{"top_docs":{"sort":"rank_i desc","limit":-1,"fields":["id"],
            "document_format":"rows","get_number":true,
            "diversify":{"field":"key_s","max_per_key":2}}}
        }}}
      }})", parallel);
    EXPECT_EQ((std::vector<std::string>{"39", "38"}), ids(*actual));
    EXPECT_EQ(count, actual->docList()->found.has_value());
    EXPECT_EQ(count, actual->docList()->found_groups.has_value());
    EXPECT_EQ(40, actual->docList()->ops.at("all")->facetResult()->counts[0]);
    const auto& facet = *actual->docList()->ops.at("nested")->facetResult();
    const auto& children = std::get<api::ArrVal>(facet.ops.at("rows")->kind).v;
    ASSERT_EQ(2u, children.size());
    for (auto& child : children) {
      EXPECT_EQ(2, child.docList()->found);
      EXPECT_EQ(1, child.docList()->found_groups);
      EXPECT_EQ(2, child.docList()->row_count);
    }
  }
}

TEST_F(DiversifyTest, fusionSourceAndValidation) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& vector = schema.field("v_v");
  vector.type = api::FieldDef::FieldClass::VECTOR;
  vector.column = true;
  vector.dims = 2;
  vector.metric = api::VectorMetric::L2;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "rank_i", (int64_t)4,
          "v_v", std::vector<float>{0, 0}),
      flatdoc("id", "b", "key_s", "x", "rank_i", (int64_t)3, "v_v", std::vector<float>{0.1f, 0}),
      flatdoc("id", "c", "key_s", "x", "rank_i", (int64_t)2, "v_v", std::vector<float>{0.2f, 0}),
      flatdoc("id", "d", "key_s", "y", "rank_i", (int64_t)1, "v_v", std::vector<float>{1, 0})},
      UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto fused = run("main", R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{"source":{
      "sort":"rank_i desc","limit":3,"diversify":{"field":"key_s","max_per_key":2}}},
      "fields":["id"],"document_format":"rows","get_number":true,"limit":-1}}}})", parallel);
    ASSERT_TRUE(fused->ok());
    EXPECT_EQ((std::vector<std::string>{"a", "b", "d"}), ids(*fused, "f"));
    EXPECT_EQ(3, fused->responses.back()->proto.ops.at("f")->docList()->found);
  }
  for (auto body : {R"({"diversify":{"field":"key_s"}})",
      R"({"diversify":{"field":"key_s","max_per_key":0}})",
      R"({"diversify":{"field":"key_s","max_per_key":-1}})",
      R"({"diversify":{"field":[],"max_per_key":2}})",
      R"({"diversify":{"field":"key_s","max_per_key":2},"collapse":"key_s"})",
      R"({"query":{"knn":{"field":"v_v","query":[0,0],"k":2,"unit":"groups"}},
          "diversify":{"field":"key_s","max_per_key":2}})"}) {
    auto request = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(body, request->rawRequest(), request->mr);
    request->collection("main"); request->execute();
    EXPECT_FALSE(request->ok()) << body;
    if (std::string_view(body).find("knn") != std::string_view::npos) {
      EXPECT_NE(std::string::npos, request->errorMsg().find("requires an enclosing collapse"));
    }
  }
  auto docsKnn = run("main", R"({"fields":["id"],"document_format":"rows","limit":-1,
    "get_number":true,"query":{"knn":{"field":"v_v","query":[0,0],"k":3,"exact":true}},
    "diversify":{"field":"key_s","max_per_key":1}})");
  EXPECT_EQ((std::vector<std::string>{"a"}), ids(*docsKnn));
  EXPECT_EQ(1, docsKnn->docList()->found);
  auto groupKnn = run("main", R"({"collapse":"key_s","limit":0,"ops":{"child":{"top_docs":{
    "fields":["id"],"document_format":"rows","limit":-1,
    "query":{"knn":{"field":"v_v","query":[0,0],"k":2,"unit":"groups","exact":true}},
    "diversify":{"field":"key_s","max_per_key":1}}}}})");
  const auto& child = *groupKnn->docList()->ops.at("child")->docList();
  ASSERT_EQ(2, child.row_count);
  EXPECT_EQ("a", child.docs[0].fields.at("id")->asString());
  EXPECT_EQ("d", child.docs[1].fields.at("id")->asString());
  CollectionHelper empty("empty_diverse");
  auto result = run("empty_diverse", R"({"get_number":true,
      "diversify":{"field":"key_s","max_per_key":2}})");
  EXPECT_EQ(0, result->docList()->found);
  EXPECT_EQ(0, result->docList()->found_groups);
}

TEST_F(DiversifyTest, orderedAdmissionAcrossPartitionsAndLargeCaps) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "one")}, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan plan(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  struct Entry { int rank, doc, key; size_t slot = 0; };
  auto better = [](const Entry& a, const Entry& b) {
    return a.rank != b.rank ? a.rank > b.rank : a.doc < b.doc;
  };
  std::mt19937 rng(741);
  for (int cap : {1, 2, 10, 100}) for (size_t depth : {0, 7, 300}) {
    std::vector<Entry> rows;
    for (int i = 0; i < 900; i++) rows.push_back({(int)(rng() % 9), i, (int)(rng() % 4)});
    auto expected = rows;
    std::sort(expected.begin(), expected.end(), better);
    std::array<int, 4> counts{};
    std::erase_if(expected, [&](const Entry& row) { return counts[(size_t)row.key]++ >= cap; });
    expected.resize(std::min(expected.size(), depth));
    auto select = [&](const std::vector<Entry>& input) {
      GroupAdmission<false, true> admission(plan, (size_t)cap);
      ExpandingPQ<Entry, decltype(better), true> heap(depth, better);
      for (const auto& row : input) {
        CollapseKey key; key.components.push_back({true, row.key});
        admission.admit(key, heap, row, [&](const Entry& old) { return better(row, old); },
                        [](size_t, bool) {});
      }
      std::vector<Entry> result(heap.span().begin(), heap.span().end());
      std::sort(result.begin(), result.end(), better);
      return result;
    };
    std::shuffle(rows.begin(), rows.end(), rng);
    auto verify = [&](const auto& actual) {
      ASSERT_EQ(expected.size(), actual.size());
      for (size_t i = 0; i < actual.size(); i++) EXPECT_EQ(expected[i].doc, actual[i].doc);
    };
    verify(select(rows));
    std::array<std::vector<Entry>, 5> parts;
    for (auto row : rows) parts[rng() % parts.size()].push_back(row);
    for (auto& part : parts) part = select(part);
    std::shuffle(parts.begin(), parts.end(), rng);
    std::vector<Entry> merged;
    for (auto& part : parts) {
      merged.insert(merged.end(), part.begin(), part.end());
      merged = select(merged);
    }
    verify(merged);
  }
}

TEST_F(DiversifyTest, wireRenderingAndTotalsBudget) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 80; i++) docs.push_back(flatdoc("id", std::to_string(i),
      "key_s", "p" + std::to_string(i / 4)));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto full = run("main", R"({"fields":["id"],"document_format":"rows","limit":5,
      "batch_size":2,"get_number":true,"diversify":{"field":"key_s","max_per_key":2}})");
  auto expected = ids(*full);
  EXPECT_EQ(40, full->docList()->found);
  EXPECT_EQ(20, full->docList()->found_groups);
  auto body = renderSearchResponseBody(full->responses[0]->proto, true);
  EXPECT_NE(std::string::npos, body.find(R"("found_groups":20)"));
  auto runs = renderDocRuns(full->responses[0]->proto);
  ASSERT_EQ(1u, runs.size());
  DocLinesState framing;
  std::string header;
  ASSERT_TRUE(frameDocRun(runs[0], framing, {}, header));
  EXPECT_NE(std::string::npos, header.find(R"("found_groups":20)"));
  for (bool binary : {false, true}) {
    auto copy = localReq(luxirNode->getSearchEngine());
    std::vector<std::byte> data;
    std::string canonical;
    if (binary) {
      ASSERT_TRUE(api::encode(full->rawRequest(), data));
      size_t size = data.size(); data.resize(size + 16);
      ASSERT_TRUE(api::decode(copy->rawRequest(), std::span(data).first(size), copy->mr));
    } else {
      ASSERT_TRUE(api::write_json(full->rawRequest(), canonical));
      ASSERT_TRUE(api::read_json(copy->rawRequest(), canonical, copy->mr)) << canonical;
    }
    copy->execute(-1);
    ASSERT_TRUE(copy->ok()) << copy->errorMsg();
    EXPECT_EQ(expected, ids(*copy));
    for (auto* response : copy->responses) {
      std::vector<std::byte> data;
      ASSERT_TRUE(api::encode(response->proto, data));
      size_t size = data.size(); data.resize(size + 16);
      api::SearchResponse decoded;
      std::pmr::monotonic_buffer_resource arena;
      ASSERT_TRUE(api::decode(decoded, std::span(data).first(size), arena));
      EXPECT_EQ(20, decoded.ops.at("q")->docList()->found_groups);
    }
  }
  SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
  forcedRequestMemoryMaxBytes = 1;
  for (int parallel : {1, -1}) {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(R"({"collection":"main","get_number":true,"limit":0,
        "diversify":{"field":"key_s","max_per_key":2}})", req->rawRequest(), req->mr);
    req->execute(parallel);
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("diversify"));
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
}

TEST_F(DiversifyTest, routedAndResetDomains) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "key_s", "x", "rank_i", (int64_t)1),
      flatdoc("id", "b", "key_s", "x", "rank_i", (int64_t)2),
      flatdoc("id", "c", "key_s", "y", "rank_i", (int64_t)3)}, UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto req = run("main", R"({"query":"id:a","limit":0,"ops":{
      "buckets":{"field_facet":{"field":"key_s","ops":{
        "reset":{"top_docs":{"sort":"rank_i desc","limit":-1,"fields":["id"],
          "document_format":"rows","get_number":true,
          "diversify":{"field":"key_s","max_per_key":2}},"domain":{"query":"*:*"}}
      }}},
      "routed":{"top_docs":{"limit":-1,"sort":"rank_i desc","fields":["id"],
        "document_format":"rows","get_number":true,
        "filter":[{"query":"rank_i:>1","except_ops":["all"]}],
        "diversify":{"field":"key_s","max_per_key":2},
        "ops":{"all":{"top_docs":{"limit":0,"get_number":true}}}
      },"domain":{"query":"*:*"}}
    }})", parallel);
    ASSERT_TRUE(req->ok());
    const auto& facet = *req->docList()->ops.at("buckets")->facetResult();
    auto& reset = *std::get<api::ArrVal>(facet.ops.at("reset")->kind).v[0].docList();
    ASSERT_EQ(2, reset.row_count);
    EXPECT_EQ("b", reset.docs[0].fields.at("id")->asString());
    EXPECT_EQ("a", reset.docs[1].fields.at("id")->asString());
    EXPECT_EQ(1, reset.found_groups);
    auto& routed = *req->docList()->ops.at("routed")->docList();
    EXPECT_EQ(2, routed.found);
    EXPECT_EQ(2, routed.found_groups);
    EXPECT_EQ(3, routed.ops.at("all")->docList()->found);
  }
}
