// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>

#include "bench/luxir_bench.h"
#include "luxir/query/BooleanWeight.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct RangeFilterIndex {
  CollectionHelper helper{"range_filter_bm"};
  std::shared_ptr<IndexReader> reader;
  // LUXIR_RANGE_BM_DOCS scales the fixture for cost calibration.
  int32_t size = std::getenv("LUXIR_RANGE_BM_DOCS") != nullptr
      ? std::atoi(std::getenv("LUXIR_RANGE_BM_DOCS"))
      : luxir::unit_tests ? 8192 : 131072;

  RangeFilterIndex() {
    helper.clear();
    SchemaBuilder schema;
    auto& points = schema.field("points_i");
    points.type = api::FieldDef::FieldClass::INT;
    points.index = api::FieldDef::IndexMode::RANGE;
    schema.set(helper.collection());
    auto writer = helper.getIndexWriter();
    writer->snapshots.readers.filterCache = std::make_shared<FilterCache>(
        FilterCacheConfig{.maxBytes = 0});
    auto& inverter = writer->obtainInverter();
    auto& id = inverter.getIndexHandler("id");
    auto& body = inverter.getIndexHandler("body_w");
    auto& column = inverter.getIndexHandler("column_i");
    auto& indexed = inverter.getIndexHandler("points_i");
    auto& clustered = inverter.getIndexHandler("cluster_i");
  auto& partial = inverter.getIndexHandler("partial_i");
    auto& sort = inverter.getIndexHandler("sort_i");
    for (int32_t doc = 0; doc < size; doc++) {
      std::string text = "pad";
      if (doc % 64 == 0) text += " a";
      if (doc % 64 == 1 || doc % 128 == 0) text += " c";
      if (doc % 3 == 0) text += " b";
      if (doc % 10 < 1) text += " d10";
      if (doc % 10 < 2) text += " d20";
      if (doc % 4 == 0) text += " d25";
      if (doc % 10 < 3) text += " d30";
      if (doc % 10 < 4) text += " d40";
      if (doc % 10 < 5) text += " d50";
      if (doc % 10 < 9) text += " d90";
      if (doc % 500 == 0) text += " d0_2";
      if (doc % 200 == 0) text += " d0_5";
      if (doc % 150 == 0) text += " d0_67";
      if (doc % 64 == 0) text += " d1_56";
      text += " all";
      int64_t value = ((int64_t)doc * 1543) % size;
      inverter.startDoc();
      id.index(inverter, std::to_string(doc));
      body.index(inverter, text);
      column.index(inverter, value);
      indexed.index(inverter, value);
      clustered.index(inverter, (int64_t)doc);
      // The first 45% lies above every range: settled, but short of a zone map.
      // A multiplicative hash keeps the rest crossing every zone at any size.
      partial.index(inverter, (int64_t)doc * 20 < (int64_t)size * 9 ? size + doc
          : (int64_t)(((uint64_t)doc * 2654435761u) % (uint64_t)size));
      sort.index(inverter, ((int64_t)doc * 7919) % size);
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter, true);
    helper.commit();
    reader = std::make_shared<IndexReader>(writer->dir);
  }
};

enum class Consumer { COUNT, MATCH, TOP, MAND_OPT, MAX_SCORE };

void rangeFilterBench(benchmark::State& state, const char* lead,
                      const char* field, int32_t percent, Consumer consumer) {
  RangeFilterIndex index;
  int64_t hi = (int64_t)index.size * percent / 100 - 1;
  TermQuery a("body_w", lead), b("body_w", lead[0] == 'd' ? "all" : "b");
  TermQuery c("body_w", "c");
  NumericPredicateQuery range(field, 0, hi);
  std::array<Query*, 2> required{&a, &b}, optional{&a, &c};
  BooleanQuery body = consumer == Consumer::MAND_OPT
      ? BooleanQuery(std::span(required).first(1), std::span(optional).last(1), {}, {})
      : consumer == Consumer::MAX_SCORE
          ? BooleanQuery({}, optional, {}, {}, 1)
          : BooleanQuery(required, {}, {}, {});
  std::array<Query*, 1> bodyClause{&body}, filters{&range};
  BooleanQuery query(bodyClause, {}, {}, filters);
  bool scored = consumer != Consumer::COUNT && consumer != Consumer::MATCH;
  auto run = [&](bool pull) {
    MemPool pool;
    Query::Context context(pool, *index.reader);
    auto* weight = query.createWeight(context, scored
        ? Query::NEED_SCORES | Query::ALLOW_PRUNING : 0);
    int64_t result = 0;
    TopDocsCollector collector(10);
    for (auto& segment : index.reader->segments()) {
      auto* supplier = weight->scorerSupplier(pool, segment);
      if (supplier == nullptr) continue;
      auto use = scored ? Query::ExecutionUse::SCORED_WINDOWS
          : consumer == Consumer::MATCH ? Query::ExecutionUse::MATCH_WINDOWS
                                        : Query::ExecutionUse::COUNT_WINDOWS;
      auto plan = supplier->planBulk(use, {});
      auto* bulk = !pull && plan.available == Query::ScorerSupplier::BulkAnswer::YES
          ? supplier->buildBulk(pool, plan) : nullptr;
      if (scored && bulk != nullptr) {
        collectTopKWindowed(segment.ord, bulk, nullptr, collector, nullptr,
                           segment.maxDoc(), false);
      } else if (bulk != nullptr) {
        for (int32_t cursor = 0; cursor < segment.maxDoc();) {
          if (consumer == Consumer::MATCH) {
            ScoreWindow window;
            cursor = bulk->matchNextWindow(window, nullptr, cursor, segment.maxDoc());
            result += window.size;
          } else {
            cursor = bulk->countNextWindow(result, nullptr, nullptr, cursor, segment.maxDoc());
          }
        }
      } else {
        auto* scorer = supplier->resolve(pool, supplier->makePlanContext(
            Query::Demand::fromLeadCost(INT64_MAX)))->build(pool);
        if (scored && scorer != nullptr) {
          collectTopK(segment.ord, scorer, nullptr, nullptr, collector, false);
        } else {
          while (scorer != nullptr && scorer->next() != PostingsReader::END) result++;
        }
      }
    }
    if (scored) {
      for (const auto& hit : collector.sort()) result += hit.doc.docId() + 1;
    }
    return result;
  };
  int64_t expected = run(true);
  for (auto _ : state) {
    int64_t result = run(false);
    benchmark::DoNotOptimize(result);
    if (result != expected) {
      state.SkipWithError("filtered consumer differs from pull");
      break;
    }
  }
  SkipStatsScope stats;
  run(false);
  state.counters["sparse"] = (double)SkipStats::numericPredicateSparseVerifyArms;
  state.counters["zone"] = (double)SkipStats::numericPredicateZoneArms;
  state.counters["scan"] = (double)SkipStats::numericPredicateScanArms;
  state.counters["points"] = (double)(SkipStats::numericPredicatePointsArms
      + SkipStats::numericPredicateComplementArms);
  state.counters["count_windows"] = (double)SkipStats::conjDenseCountWindows;
  state.counters["match_windows"] = (double)SkipStats::conjDenseMatchWindows;
  state.counters["segments"] = (double)index.reader->segments().size();
  state.counters["result"] = (double)expected;
}

void rangeFilterSortBench(benchmark::State& state) {
  RangeFilterIndex index;
  auto run = [&] {
    auto req = localReq(index.helper.getSearchEngine());
    req->collection("range_filter_bm");
    auto& top = req->topDocs("q").limit(10).fields({"id"});
    top.simpleQuery("+a +b", {"body_w"});
    top.filter(qb::range(top.mr(), "column_i", qb::valI64(top.mr(), 0),
                        nullptr, qb::valI64(top.mr(), index.size * 9 / 10), nullptr));
    qb::sort(top, "sort_i", qb::ASC);
    req->execute(false);
    if (!req->ok()) throw std::runtime_error(req->errorMsg());
    uint64_t fingerprint = 0;
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      for (const auto& id : std::get<api::ColStr>(list->columns.at("id").kind).v) {
        fingerprint = fingerprint * 31 + java_string_hashcode(id);
      }
    }
    return fingerprint;
  };
  uint64_t expected = run();
  for (auto _ : state) {
    uint64_t result = run();
    benchmark::DoNotOptimize(result);
    if (result != expected) state.SkipWithError("field-sort result changed");
  }
  SkipStatsScope stats;
  run();
  state.counters["match_bulk"] = (double)SkipStats::fieldSortBulkCollections;
}

[[maybe_unused]] bool registered = [] {
  auto add = [](std::string name, const char* lead, const char* field,
                int32_t percent, Consumer consumer) {
    benchmark::RegisterBenchmark("BM_RangeFilter/" + name,
        [=](benchmark::State& state) {
          rangeFilterBench(state, lead, field, percent, consumer);
        })->UseRealTime();
  };
  for (auto lead : {"d10", "d30", "d40", "d50", "d90"}) {
    add(std::string("dense_") + lead, lead, "column_i", 90, Consumer::COUNT);
  }
  add("cluster_count", "d50", "cluster_i", 75, Consumer::COUNT);
  // Sparse leads against a time-ordered range: zone production is mostly
  // settled runs, so the verify/fill choice moves to sub-percent leads.
  for (auto lead : {"d0_2", "d0_5", "d0_67", "d1_56"}) {
    add(std::string("clustered_") + lead, lead, "cluster_i", 50, Consumer::COUNT);
  }
  for (auto lead : {"d10", "d20", "d25", "d30"}) {
    add(std::string("partial_") + lead, lead, "partial_i", 90, Consumer::COUNT);
  }
  add("match", "a", "column_i", 90, Consumer::MATCH);
  add("mandopt", "a", "column_i", 90, Consumer::MAND_OPT);
  add("maxscore", "a", "column_i", 90, Consumer::MAX_SCORE);
  for (auto field : {"column_i", "points_i"}) {
    for (int32_t percent : {1, 90}) {
      for (auto consumer : {Consumer::COUNT, Consumer::TOP}) {
        add(std::string(field) + "_" + std::to_string(percent)
            + (consumer == Consumer::COUNT ? "_count" : "_top"),
            "a", field, percent, consumer);
      }
    }
  }
  benchmark::RegisterBenchmark("BM_RangeFilter/sort", rangeFilterSortBench)->UseRealTime();
  return true;
}();

} // namespace
