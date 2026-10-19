// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

#include <mutex>
#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/DistinctFacetCounts.h"


using namespace luxir;
using namespace luxir::test;

// Variant catalog: identical queries/corpus for every reducer. Select with
// LUXIR_DISTINCT_REDUCER=paged|hash (unset selects the measured plan).
// Effort 1: 200K products; effort 5: 1M. Unit mode uses 100 products.
static constexpr int32_t GROUP_VARIANTS = 7;
static std::unique_ptr<CollectionHelper> groupFacetCorpus(bool scattered) {
  constexpr int32_t VARIANTS = 7;
  int32_t products = (int32_t)LuxirTest::scaleTestWork(unit_tests ? 100 : 200'000);
  products = std::max(VARIANTS, products / VARIANTS * VARIANTS);
  int32_t docsPerSegment = products;
  std::string name = "group_facet_catalog_" + std::string(scattered ? "scattered_" : "contiguous_")
      + std::to_string(products);
  auto helper = std::make_unique<CollectionHelper>(name);
  // Several Google Benchmark threads can share this immutable corpus.
  static std::mutex setupMutex;
  {
    std::lock_guard lock(setupMutex);
    std::vector<int32_t> sizes(VARIANTS, docsPerSegment);
    if (!helper->indexMatchesShape(sizes)) {
      helper->clear();
      for (int32_t segment = 0; segment < VARIANTS; segment++) {
        std::vector<Doc> docs;
        docs.reserve((size_t)docsPerSegment);
        for (int32_t local = 0; local < docsPerSegment; local++) {
          int32_t product = scattered ? local : segment * (products / VARIANTS) + local / VARIANTS;
          int32_t variant = scattered ? segment : local % VARIANTS;
          int64_t price = 1000 + (int64_t)(product % 1000) * 100 + variant * 17;
          docs.push_back(flatdoc("id", std::to_string(product * VARIANTS + variant),
              "product_s", "p" + std::to_string(product),
              "product_i", (int64_t)product, "catalog_i", (int64_t)(product % 3),
              "color_s", "c" + std::to_string((product + variant * 3) % 20),
              "size_s", "s" + std::to_string((product / 5 + variant) % 10),
              "brand_s", "b" + std::to_string(product % 4093),
              "price_i", price,
              "tag_ss", vecs("t" + std::to_string(product % 97),
                             "t" + std::to_string((product + variant + 1) % 97))));
        }
        EXPECT_TRUE(helper->indexAll(docs, UpdateMessage::COMMIT).success);
      }
    }
  }
  return helper;
}

enum class CatalogKey { STRING, TUPLE, NUMERIC };
static void groupFacetCase(benchmark::State& state, bool scattered,
                          int32_t shape, bool groups, bool parallel, CatalogKey key) {
  std::optional<SearchOverridesGuard<DistinctFacetStats*>> statsGuard;
  DistinctFacetStats stats;
  if (state.threads() == 1 && groups) {
    statsGuard.emplace(distinctFacetStatsForTests);
    distinctFacetStatsForTests = &stats;
  }
  auto corpus = groupFacetCorpus(scattered);
  auto& helper = *corpus;
  int32_t products = (int32_t)(helper.getIndexWriter()->snapshots.readers.getReader()->maxDoc() / GROUP_VARIANTS);
  constexpr int32_t VARIANTS = GROUP_VARIANTS;
  std::string name = "group_facet_catalog_" + std::string(scattered ? "scattered_" : "contiguous_") + std::to_string(products);
  std::string unit = groups ? "groups" : "docs";
  auto facet = [&](std::string_view field, int32_t limit) {
    return R"({"field_facet":{"field":")" + std::string(field)
        + R"(","limit":)" + std::to_string(limit) + R"(,"count_unit":")" + unit + R"("}})";
  };
  bool narrow = shape == 4 || shape == 5;
  if (narrow) shape -= 4;
  std::string collapse = key == CatalogKey::STRING ? R"("product_s")"
      : key == CatalogKey::NUMERIC ? R"("product_i")" : R"({"field":["product_s","catalog_i"]})";
  std::string json = R"({"collection":")" + name + (narrow ? R"(","query":"tag_ss:t5)" : "")
      + R"(","limit":0,"collapse":)" + collapse + R"(,"ops":{"color":)";
  if (shape == 6) json += R"({"range_facet":{"field":"price_i","start":0,"end":120000,"gap":10000,"count_unit":")" + unit + R"("}})";
  else json += facet(shape == 3 ? "brand_s" : shape == 7 ? "tag_ss" : "color_s", 20);
  if (shape == 1 || shape == 2) json += R"(,"size":)" + facet("size_s", 10)
      + R"(,"brand":)" + facet("brand_s", 20) + R"(,"tag":)" + facet("tag_ss", 20)
      + R"(,"price":{"range_facet":{"field":"price_i","start":0,"end":120000,"gap":10000,"count_unit":")"
      + unit + R"("}})";
  json += '}';
  if (shape == 2) json += R"(,"filter":[{"query":"color_s:c0 OR color_s:c1","except_ops":["color"]},
      {"query":"size_s:s0 OR size_s:s1","except_ops":["size"]}])";
  json += '}';
  auto execute = [&]() {
    auto request = localReq(helper.getSearchEngine());
    EXPECT_TRUE(api::read_json(request->rawRequest(), json, request->mr));
    request->execute(parallel);
    EXPECT_TRUE(request->ok()) << request->errorMsg();
    return request;
  };
  size_t peak = 0;
  size_t paged = 0, hash = 0, run = 0;
  {
    auto warm = execute();
    if (!warm->ok()) { state.SkipWithMessage(warm->errorMsg()); return; }
    peak = warm->memoryTracker.peak();
    paged = stats.paged.load(); hash = stats.hash.load(); run = stats.run.load();
  }
  for (auto _ : state) {
    auto request = execute();
    peak = std::max(peak, request->memoryTracker.peak());
    benchmark::DoNotOptimize(request->docList()->ops.at("color")->facetResult()->counts.data());
  }
  if (state.threads() == 1 && groups) {
    state.counters["paged_instances"] = (double)paged;
    state.counters["hash_instances"] = (double)hash;
    state.counters["run_instances"] = (double)run;
  }
  state.counters["products"] = benchmark::Counter(products, benchmark::Counter::kAvgThreads);
  state.counters["documents"] = benchmark::Counter((double)products * VARIANTS, benchmark::Counter::kAvgThreads);
  state.counters["peak_tracked_bytes"] = benchmark::Counter((double)peak, benchmark::Counter::kAvgThreads);
  state.SetItemsProcessed(state.iterations() * products * VARIANTS);
}

static void BM_GroupFacetCatalog(benchmark::State& state, bool scattered, int32_t shape, bool groups) {
  groupFacetCase(state, scattered, shape, groups, false, CatalogKey::STRING);
}

LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_one_docs, false, 0, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_one_groups, false, 0, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_all_docs, false, 1, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_all_groups, false, 1, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_multiselect_docs, false, 2, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_multiselect_groups, false, 2, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_one_docs, true, 0, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_one_groups, true, 0, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_all_docs, true, 1, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_all_groups, true, 1, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_multiselect_docs, true, 2, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_multiselect_groups, true, 2, true)->Threads(1)->Threads(4);

// A selective query: about 2% of documents and 6% of products.
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_narrow_one_docs, false, 4, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_narrow_one_groups, false, 4, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_narrow_all_docs, false, 5, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_narrow_all_groups, false, 5, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_narrow_one_docs, true, 4, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_narrow_one_groups, true, 4, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_narrow_all_docs, true, 5, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_narrow_all_groups, true, 5, true)->Threads(1)->Threads(4);

// Isolate the high-cardinality crossover on the same catalog.
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_brand_docs, false, 3, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, contiguous_brand_groups, false, 3, true)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_brand_docs, true, 3, false)->Threads(1)->Threads(4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupFacetCatalog, scattered_brand_groups, true, 3, true)->Threads(1)->Threads(4);

// Compare candidates in one process against the same immutable RAM catalog.
// These registrations deliberately have one thread: the forced planning mode
// is a process-wide test override. The ordinary env-selected cases support 4.
static void BM_GroupFacetReducerComparison(benchmark::State& state, bool scattered,
                                            int32_t shape, bool groups, DistinctFacetMode mode) {
  SearchOverridesGuard guard(forcedDistinctFacetMode);
  forcedDistinctFacetMode = mode;
  BM_GroupFacetCatalog(state, scattered, shape, groups);
}

[[maybe_unused]] static const bool groupFacetReducerComparisons = [] {
  const std::array choices{
      std::pair{"auto", DistinctFacetMode::AUTO}, std::pair{"paged", DistinctFacetMode::PAGED},
      std::pair{"hash", DistinctFacetMode::HASH}};
  const std::array shapes{"one", "all", "multiselect", "brand", "narrow_one", "narrow_all", "price", "tag"};
  for (bool scattered : {false, true}) for (int32_t shape = 0; shape < (int32_t)shapes.size(); shape++) {
    std::string name = "Tuning/BM_GroupFacetReducers/" + std::string(scattered ? "scattered_" : "contiguous_")
        + shapes[(size_t)shape];
    benchmark::RegisterBenchmark((name + "_docs").c_str(), BM_GroupFacetReducerComparison,
        scattered, shape, false, DistinctFacetMode::AUTO)->UseRealTime()->Threads(1);
    for (auto [label, mode] : choices) benchmark::RegisterBenchmark(
        (name + "_groups_" + label).c_str(), BM_GroupFacetReducerComparison,
        scattered, shape, true, mode)->UseRealTime()->Threads(1);
  }
  return true;
}();

// Intra-request parallelism, plus tuple/numeric keys, on the same catalog.
[[maybe_unused]] static const bool groupFacetExtraCases = [] {
  for (bool scattered : {false, true}) for (auto [label, shape] : {
      std::pair{"one", 0}, std::pair{"all", 1}, std::pair{"brand", 3},
      std::pair{"narrow_one", 4}, std::pair{"narrow_all", 5}}) {
    for (bool groups : {false, true}) {
      std::string name = "BM_GroupFacetCatalog/" + std::string(scattered ? "scattered_" : "contiguous_")
          + label + (groups ? "_groups" : "_docs");
      benchmark::RegisterBenchmark((name + "_parallel").c_str(), groupFacetCase,
          scattered, shape, groups, true, CatalogKey::STRING)->UseRealTime()->Threads(1)->Threads(4);
      for (auto [keyLabel, key] : {std::pair{"tuple", CatalogKey::TUPLE}, std::pair{"numeric", CatalogKey::NUMERIC}}) {
        benchmark::RegisterBenchmark((name + "_" + keyLabel).c_str(), groupFacetCase,
            scattered, shape, groups, false, key)->UseRealTime()->Threads(1)->Threads(4);
      }
    }
  }
  return true;
}();

// Isolated key-read cost. The point arm reproduces the former per-hit read;
// the bulk arm uses the same shared reader as the distinct bindings. Repeated
// walks model one or five facet bindings, with the selective domain unchanged.
static void BM_GroupIdentityRead(benchmark::State& state, bool narrow, int32_t walks, bool bulk) {
  auto helper = groupFacetCorpus(true);
  auto reader = helper->getIndexWriter()->snapshots.readers.getReader();
  CollapsePlan key(*reader, api::Collapse_::MissingPolicy::EXPAND, false);
  key.fields.push_back({"product_s", api::GroupComponent_::Type::STRING, reader->getOrdMap("product_s"),
                        reader->field("product_s")});
  std::vector<std::unique_ptr<DocSet>> domains(reader->segments().size());
  auto tags = reader->field("tag_ss");
  if (narrow) for (size_t segment = 0; segment < domains.size(); segment++) {
    auto pool = MemPool::threadLocalPoolGuard();
    auto& postings = reader->segments()[segment].postingsReader();
    auto* info = tags.at(segment);
    if (!info) { state.SkipWithMessage("tag column missing"); return; }
    TermsEnum terms(pool.pool(), postings, *info);
    if (!terms.seek("t5")) { state.SkipWithMessage("tag term missing"); return; }
    DocsOnlyEnum docs(terms);
    domains[segment] = materializePostingsIntersection(docs, terms.docFreq(), nullptr, postings.maxDoc());
  }
  uint64_t checksum = 0;
  for (auto _ : state) for (int32_t walk = 0; walk < walks; walk++) {
    for (int32_t segment = 0; segment < (int32_t)domains.size(); segment++) {
      auto pool = MemPool::threadLocalPoolGuard();
      int32_t maxDoc = reader->segments()[(size_t)segment].maxDoc();
      DocSet* domain = domains[(size_t)segment].get();
      if (bulk) {
        CollapseKeyReader keys(key);
        keys.withWalk(segment, domain, [&](auto group) {
          forEachDomainDoc(domain, maxDoc, [&](int32_t doc) LUXIR_INLINE {
            auto identity = group(doc);
            if constexpr (std::is_same_v<decltype(identity), int64_t>) checksum += (uint64_t)identity;
            else if constexpr (std::is_same_v<decltype(identity), GroupIdentity>) checksum += (uint64_t)identity.value;
          });
        });
      } else {
        auto& postings = reader->segments()[(size_t)segment].postingsReader();
        auto* info = key.fields[0].segments.at((size_t)segment);
        if (!info) { state.SkipWithMessage("product column missing"); return; }
        OrdColReader column(postings, *info);
        auto mapping = key.fields[0].ords->getSegToGlobal(segment);
        forEachDomainDoc(domain, maxDoc, [&](int32_t doc) LUXIR_INLINE {
          checksum += (uint64_t)mapping.globalOrd((int64_t)column.ordAt(doc) - 1);
        });
      }
    }
    benchmark::DoNotOptimize(checksum);
  }
}
[[maybe_unused]] static const bool groupIdentityMeasurements = [] {
  for (auto [shape, walks, narrow] : {std::tuple{"one", 1, false}, std::tuple{"all", 5, false},
      std::tuple{"brand", 1, false}, std::tuple{"narrow_all", 5, true}}) {
    for (bool bulk : {false, true}) {
      std::string name = "Tuning/BM_GroupIdentityRead/scattered_" + std::string(shape) + (bulk ? "_bulk" : "_point");
      benchmark::RegisterBenchmark(name.c_str(), BM_GroupIdentityRead, narrow, walks, bulk)->UseRealTime()->Threads(1);
    }
  }
  return true;
}();
