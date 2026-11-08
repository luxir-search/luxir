// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/search/Collector.h"

using namespace luxir;
using namespace luxir::test;

// Scattered groups across five segments. Same-limit document, group and
// exhaustive group cases compare ranking and traversal work.
static void BM_CollapseTopDocs(benchmark::State& state, int32_t mode) {
  int32_t nDocs = unit_tests ? (int32_t)LuxirTest::scaleTestWork(500) : 1'000'000;
  int32_t nGroups = unit_tests ? 23 : 50'000;
  std::vector<int32_t> sizes(5, nDocs / 5);
  nDocs = sizes[0] * 5;
  CollectionHelper helper("collapse_bm");
  if (!helper.indexMatchesShape(sizes)) {
    helper.clear();
    for (int32_t segment = 0; segment < 5; segment++) {
      std::vector<Doc> docs;
      docs.reserve((size_t)sizes[(size_t)segment]);
      for (int32_t doc = 0; doc < sizes[(size_t)segment]; doc++) {
        int32_t id = segment * sizes[0] + doc;
        // Impact bounds can prune the lower-score bands. Every segment
        // contains many groups and repeated members, in scattered key order.
        std::string body;
        int32_t freq = doc < sizes[0] / 10 ? 8 : doc < sizes[0] / 2 ? 3 : 1;
        for (int32_t word = 0; word < 16; word++) body += word < freq ? "alpha " : "beta ";
        docs.push_back(flatdoc("id", std::to_string(id), "body_w", body,
            "key_s", "g" + std::to_string(id % nGroups), "sort_i", (int64_t)(id * 37 % nDocs), "seq_i", (int64_t)id + 1, "sort_s", "s" + std::to_string(id * 17 % 101)));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }

  bool savedPruning = disableCollapsePruningForTests;
  auto restore = scope_guard([&]() { disableCollapsePruningForTests = savedPruning; });
  if (mode >= 13) {
    disableCollapsePruningForTests = true;
    mode = mode == 13 ? 2 : mode == 14 ? 6 : 7;
  }
  bool churn = mode == 4 || mode == 5;
  std::string query = churn ? "rescore(*:*, seq_i)" : "body_w:alpha";
  bool unscored = mode == 6 || mode == 7 || mode >= 9;
  std::string fields = unscored ? R"([])" : R"(["_score_"])";
  std::string json = R"({"collection":"collapse_bm","query":")" + query
      + R"(","fields":)" + fields + R"(,"limit":)" + std::to_string((mode == 3 || mode == 9 || mode == 10) ? -1 : churn ? 10 : 100);
  if (mode == 1 || mode == 6 || mode == 9 || mode == 11) json += R"(,"sort":"sort_i asc")";
  if (mode == 7 || mode == 10 || mode == 12) json += R"(,"sort":"sort_s desc")";
  if (mode == 8) json += R"(,"sort":["sort_i desc","score desc"])";
  if (mode == 2 || mode == 4 || (mode >= 6 && mode <= 8)) json += R"(,"collapse":"key_s")";
  if (mode == 5) json += R"(,"collapse":{"field":["key_s","sort_i"]})";
  json += '}';
  // Warm the reader/OrdMap and record decode work outside the timing loop.
  {
    SkipStatsScope stats;
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    if (!luxir::api::read_json(req->rawRequest(), json, req->mr)) {
      state.SkipWithMessage("collapse not implemented in this baseline");
      return;
    }
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    state.counters["remap_probes"] = (double)SkipStats::collapseRemapProbes;
    state.counters["gathered_docs"] = (double)SkipStats::fieldSortDocsGathered;
    state.counters["doc_blocks"] = (double)SkipStats::docBlocksDecoded;
    state.counters["tfreq_blocks"] = (double)SkipStats::tfreqBlocksDecoded;
  }
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    ASSERT_TRUE(luxir::api::read_json(req->rawRequest(), json, req->mr));
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    int64_t rows = 0;
    for (auto* response : req->responses) rows += response->proto.ops.at("q")->docList()->row_count;
    benchmark::DoNotOptimize(rows);
    ASSERT_GT(rows, 0);
  }
  state.SetItemsProcessed(state.iterations() * nDocs);
}

LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, docs_score, 0);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, docs_field, 1);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_score, 2);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, exhaustive_docs, 3);

LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, churn_score, 4);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, churn_tuple, 5);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_field_numeric, 6);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_field_string, 7);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_field_score_secondary, 8);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, exhaustive_field_numeric, 9);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, exhaustive_field_string, 10);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, docs_field_unscored, 11);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, docs_field_string, 12);

// Same limit and group admission as the pruned cases; only traversal is exhaustive.
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_score_no_pruning, 13);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_field_numeric_no_pruning, 14);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseTopDocs, collapse_field_string_no_pruning, 15);

// Isolate retained-group remapping on overlapping, incomplete dictionaries.
// Cost zero pins the direct-translation baseline via the production cost rule;
// the other cases supply the segment's actual document count. Both visit the
// same documents and perform the same heap updates, with no pruning.
static void BM_CollapseRetainedRemap(benchmark::State& state, bool remap, bool tuple) {
  int32_t size = unit_tests ? 100 : 200'000;
  CollectionHelper helper("collapse_remap_bm");
  if (!helper.indexMatchesShape(std::vector<int32_t>(5, size))) {
    helper.clear();
    for (int32_t segment = 0; segment < 5; segment++) {
      std::vector<Doc> docs;
      for (int32_t doc = 0; doc < size; doc++) {
        docs.push_back(flatdoc("id", std::to_string(segment * size + doc),
            "key_s", "g" + std::to_string(segment + doc % 8), "key_i", (int64_t)(doc % 2)));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan plan(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  plan.fields.push_back({"key_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("key_s"),
                         reader->field("key_s")});
  if (tuple) plan.fields.push_back({"key_i", api::GroupComponent_::Type::INT, {}, reader->field("key_i")});
  auto execute = [&]() {
    GroupScoreCollector collector(10, &plan);
    for (int32_t segment = 0; segment < 5; segment++) {
      collector.setSegment(segment, remap ? size : 0);
      for (int32_t doc = 0; doc < size; doc++) {
        collector.collect(segment, doc, (float)(segment * size + doc));
      }
    }
    benchmark::DoNotOptimize(collector.pq.top().score);
  };
  {
    SkipStatsScope stats;
    execute();
    state.counters["remap_probes"] = (double)SkipStats::collapseRemapProbes;
  }
  for (auto _ : state) execute();
  state.SetItemsProcessed(state.iterations() * size * 5);
}
LUXIR_BENCHMARK_CAPTURE(BM_CollapseRetainedRemap, direct_score, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseRetainedRemap, remap_score, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseRetainedRemap, direct_tuple, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseRetainedRemap, remap_tuple, true, true);

// Isolate per-key worst-member maintenance from key decoding and scorer work.
// Improving candidates exercise a full cap repeatedly, including large caps.
template <bool Ordered>
static void BM_CappedAdmission(benchmark::State& state) {
  CollectionHelper helper("cap_admission");
  if (!helper.indexMatchesShape(std::vector<int32_t>{1})) {
    helper.clear();
    ASSERT_TRUE(helper.indexAll({flatdoc("id", "one")}, UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan plan(*reader, api::Collapse_::MissingPolicy::GROUP, false);
  struct Item { int rank; size_t slot = 0; };
  auto better = [](const Item& a, const Item& b) { return a.rank > b.rank; };
  int cap = (int)state.range(0);
  std::array<CollapseKey, 20> keys;
  for (size_t i = 0; i < keys.size(); i++) keys[i].components.push_back({true, (int64_t)i});
  for (auto _ : state) {
    GroupAdmission<false, Ordered> admission(plan, (size_t)cap);
    ExpandingPQ<Item, decltype(better), true> heap((size_t)cap * keys.size(), better);
    for (int i = 0; i < 20'000; i++) {
      Item candidate{i};
      admission.admit(keys[(size_t)i % keys.size()], heap, candidate,
          [&](const Item& old) { return better(candidate, old); }, [](size_t, bool) {});
    }
    benchmark::DoNotOptimize(heap.size());
  }
  state.SetItemsProcessed(state.iterations() * 20'000);
}
BENCHMARK_TEMPLATE(BM_CappedAdmission, false)->Name("Tuning/BM_CappedAdmission/linear")
    ->Arg(1)->Arg(2)->Arg(10)->Arg(100);
BENCHMARK_TEMPLATE(BM_CappedAdmission, true)->Name("Tuning/BM_CappedAdmission/ordered")
    ->Arg(1)->Arg(2)->Arg(10)->Arg(100);
