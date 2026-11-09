// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/search/SearchOverrides.h"
#include <set>

using namespace luxir;
using namespace luxir::test;

namespace {
struct Row { std::string parent, child; };
std::vector<Row> rows(const LocalReq& req) {
  std::vector<Row> out;
  for (auto* response : req.responses) {
    auto* q = response->proto.ops.find("q");
    if (!q) continue;
    const auto& list = *(*q)->docList();
    for (size_t i = 0; i < list.docs.size(); ++i) {
      auto* hits = list.matches[i].fields.at("chunk")->docList();
      out.push_back({std::string(list.docs[i].fields.at("id")->asString()),
          std::string(hits->docs[0].fields.at("id")->asString())});
    }
  }
  return out;
}

void BM_ParentChild(benchmark::State& state, bool exact, bool lexical, bool allProbes) {
  int parents = unit_tests ? 48 : 4000;
  int fanout = (int)state.range(0);
  std::string name = "parent_child_bench_" + std::to_string(fanout);
  CollectionHelper helper(name);
  std::vector<int32_t> shape(2, parents / 2 * (fanout + 1));
  if (!helper.indexMatchesShape(shape)) {
    helper.clear();
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef schema;
    std::string definition = R"({"relations":{"chunks":{"fields":{
      "color":{"type":"string","column":true},
      "price":{"type":"int","index":"range","column":true},
      "vec":{"type":"vector","dims":4,"metric":"l2","column":true,
        "ann":{"codec":"pq","nlist":4,"pq":{"m":2,"bits":)"
        + std::to_string(unit_tests ? 1 : 4) + "}}}}}}}";
    ASSERT_TRUE(api::read_json(schema, definition, mr));
    helper.collection().setSchema(Schema::fromProto(schema, Schema::createDefaultSchema().get()));
    for (int segment = 0; segment < 2; ++segment) {
      std::string json = R"({"families":[)";
      for (int p = segment * parents / 2; p < (segment + 1) * parents / 2; ++p) {
        if (p != segment * parents / 2) json += ',';
        json += R"({"parent":{"id":")" + std::to_string(p) + R"(","eligible_s":")"
            + (p % 3 ? "yes" : "no") + R"("},"children":{"chunks":[)";
        for (int c = 0; c < fanout; ++c) {
          if (c) json += ',';
          // One dense near family competes with many distinct parents.
          double x = p == 1 ? 0 : p * 0.01 + c * 0.000001;
          json += R"({"id":")" + std::to_string(c) + R"(","color":")" + (c % 2 ? "blue" : "red")
              + R"(","price":)" + std::to_string(p + c)
              + R"(,"vec":[)" + std::to_string(x) + ",0,0,0]}";
        }
        json += "]}}";
      }
      json += R"(],"commit":{}})";
      std::pmr::monotonic_buffer_resource updateArena;
      api::UpdateRequest update;
      ASSERT_TRUE(api::read_json(update, json, updateArena));
      auto result = helper.submit(update);
      ASSERT_TRUE(result.success) << result.error_message;
    }
    IvfTestGuard ivf(unit_tests ? 8 : 64);
    helper.commit({"vec.luxir_rel_6_chunks_vec"});
  }
  auto execute = [&](bool forceExact) {
    auto req = localReq(helper.getSearchEngine());
    std::string query = lexical ? R"({"range":{"field":"price","lt":100}})"
        : R"({"knn":{"field":"vec","query":[0,0,0,0],"k":10,"exact":)"
          + std::string(forceExact ? "true" : "false")
          + (allProbes ? R"(,"ivf":{"nprobe":1000000})" : "") + "}}";
    std::string json = R"({"collection":")" + name + R"(","document_format":"rows","fields":["id"],"limit":10,"count_limit":-1,
      "filter":["eligible_s:yes"],"query":{"has_child":{"relation":"chunks","query":)" + query
        + R"(,"filter":[{"match":{"field":"color","val":"red"}}],"inner_hits":{"name":"chunk","limit":1,"fields":["id"]}}}})";
    EXPECT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(1);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  auto expected = rows(*execute(true));
  KnnQuery::Stats stats;
  SearchOverridesGuard guard(KnnQuery::statsForTests);
  KnnQuery::statsForTests = &stats;
  auto actual = rows(*execute(exact));
  KnnQuery::statsForTests = nullptr;
  std::set<std::string> expectedParents, expectedWitnesses;
  for (const auto& hit : expected) {
    expectedParents.insert(hit.parent);
    expectedWitnesses.insert(hit.parent + ":" + hit.child);
  }
  double recall = 0, witnesses = 0;
  for (const auto& hit : actual) {
    recall += expectedParents.contains(hit.parent);
    witnesses += expectedWitnesses.contains(hit.parent + ":" + hit.child);
  }
  if (!lexical && !exact && stats.lists.load() == 0) {
    state.SkipWithError("parent-child ANN benchmark did not traverse an IVF index"); return;
  }
  state.counters["parent_recall"] = recall / expected.size();
  state.counters["witness_recall"] = witnesses / expected.size();
  state.counters["underfill"] = expected.size() - actual.size();
  state.counters["children"] = parents * fanout;
  state.counters["lists"] = stats.lists.load();
  state.counters["vectors"] = stats.vectors.load();
  state.counters["rss_mb"] = currentRSSKB() / 1024.0;
  if (exact || lexical) { EXPECT_EQ(1.0, recall / expected.size()); }
  for (auto _ : state) {
    auto req = execute(exact);
    benchmark::DoNotOptimize(req->docList());
  }
}
}
LUXIR_BENCHMARK_CAPTURE(BM_ParentChild, exact, true, false, false)->Arg(4)->Arg(64);
LUXIR_BENCHMARK_CAPTURE(BM_ParentChild, ann_adaptive, false, false, false)->Arg(4)->Arg(64);
LUXIR_BENCHMARK_CAPTURE(BM_ParentChild, ann_full, false, false, true)->Arg(4)->Arg(64);
LUXIR_BENCHMARK_CAPTURE(BM_ParentChild, correlated_filter, true, true, false)->Arg(4)->Arg(64);

namespace {
void BM_ParentChildUpdate(benchmark::State& state, bool replaceFamily) {
  int fanout = (int)state.range(0);
  CollectionHelper helper("parent_child_update_" + std::to_string(fanout) + (replaceFamily ? "_family" : "_child"));
  helper.clear();
  std::pmr::monotonic_buffer_resource schemaArena;
  api::SchemaDef schema;
  ASSERT_TRUE(api::read_json(schema, R"({"relations":{"variants":{"fields":{"price":{"type":"int","column":true,"index":"range"}}}}})", schemaArena));
  helper.collection().setSchema(Schema::fromProto(schema, Schema::createDefaultSchema().get()));
  std::string family = R"({"families":[{"parent":{"id":"p"},"children":{"variants":[)";
  for (int c = 0; c < fanout; ++c) {
    if (c) family += ',';
    family += R"({"id":")" + std::to_string(c) + R"(","price":)" + std::to_string(c) + '}';
  }
  family += R"(]}}],"commit":{}})";
  auto submit = [&](std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::UpdateRequest request;
    if (!api::read_json(request, json, arena)) throw std::runtime_error("invalid benchmark update");
    return helper.submit(request);
  };
  auto initial = submit(family);
  ASSERT_TRUE(initial.success);
  std::string child = R"({"child_updates":[{"parent_id":"p","relation":"variants","family":")"
      + initial.families.at("p") + R"(","docs":[{"id":"0","price":99}]}],"commit":{}})";
  const auto& request = replaceFamily ? family : child;
  for (auto _ : state) {
    auto result = submit(request);
    if (!result.success) { state.SkipWithError(result.error_message); break; }
    benchmark::DoNotOptimize(result.updateVersion);
  }
  state.counters["records_per_update"] = replaceFamily ? fanout + 1 : 1;
  state.counters["request_bytes"] = (double)request.size();
}
}
LUXIR_BENCHMARK_CAPTURE(BM_ParentChildUpdate, child, false)->Arg(4)->Arg(64);
LUXIR_BENCHMARK_CAPTURE(BM_ParentChildUpdate, family, true)->Arg(4)->Arg(64);
