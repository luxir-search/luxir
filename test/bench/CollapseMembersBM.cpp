// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/reader/FieldReader.h"

using namespace luxir;
using namespace luxir::test;

// Shared catalog rows for member-card and aggregate-order measurements.
static Doc collapseCatalogDoc(int32_t id, int32_t products, int32_t variants) {
  int32_t product = id % products, variant = id / products;
  return flatdoc("id", std::to_string(id), "key_s", "p" + std::to_string(product),
      "shop_s", "s" + std::to_string(product % 17),
      "color_s", "c" + std::to_string(variant % 8),
      "price_i", (int64_t)(100 + variants - variant), "rank_i", (int64_t)product,
      "stock_i", (int64_t)((product + variant) % 3 != 0),
      "eligible_s", (product + variant) % 101 == 0 ? "yes" : "no");
}

// Product variants spread across segments. Every group has one shop component;
// the tuple is correlated and has the same membership as the scalar key.
// Args: page groups, variants/group, segments, selective, tuple.
static void BM_CollapseMembers(benchmark::State& state, int mode) {
  int32_t limit = (int32_t)state.range(0), variants = (int32_t)state.range(1);
  int32_t segments = (int32_t)state.range(2);
  bool selective = state.range(3), tuple = state.range(4);
  int32_t products = unit_tests ? 120 : (int32_t)LuxirTest::scaleTestWork(20'000);
  std::string name = "members_reset_bm_" + std::to_string(products) + "_"
      + std::to_string(variants) + "_" + std::to_string(segments);
  bool numeric = (mode >= 18 && mode <= 21) || mode >= 24;
  bool nullKeys = mode == 22 || mode == 23;
  if (numeric) name += mode >= 24 ? "_wide_points"
      : mode >= 20 ? "_nested_points" : "_points";
  if (nullKeys) name += "_null";
  CollectionHelper helper(name);
  int32_t total = products * variants;
  std::vector<int32_t> sizes;
  for (int32_t s = 0; s < segments; s++) sizes.push_back((s + 1) * total / segments - s * total / segments);
  if (!helper.indexMatchesShape(sizes)) {
    helper.clear();
    if (numeric) {
      SchemaBuilder schema;
      for (auto name : {"rank_i", "price_i"}) {
        auto& field = schema.field(name);
        field.type = api::FieldDef::FieldClass::INT;
        field.index = api::FieldDef::IndexMode::RANGE;
      }
      schema.set(helper.collection());
    }
    for (int32_t s = 0; s < segments; s++) {
      std::vector<Doc> docs;
      for (int32_t id = s * total / segments; id < (s + 1) * total / segments; id++) {
        int32_t product = id % products;
        docs.push_back(collapseCatalogDoc(id, products, variants));
        if (nullKeys && product % 2 == 0) {
          std::erase_if(docs.back(), [](const auto& field) { return field.name == "shop_s"; });
        }
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  SearchOverridesGuard guard(collapseMemberStrategy, collapseMemberStats, facetBucketBlockCounter);
  collapseMemberStrategy = (mode == 1 || mode == 6) ? CollapseMemberStrategy::KEYS
      : (mode == 2 || mode == 7) ? CollapseMemberStrategy::DOMAIN_WALK : CollapseMemberStrategy::AUTO;
  std::string query = selective ? "eligible_s:yes" : "*:*";
  std::string body = R"({"collection":")" + name + R"(","query":")" + query
      + R"(","sort":"rank_i desc","fields":["id"],"limit":)" + std::to_string(limit);
  std::string collapse = tuple ? R"(["key_s","shop_s"])" : R"("key_s")";
  std::string ops = R"JSON({"from_price":"min(price_i)","variants":{"top_docs":{"sort":"price_i asc","limit":3,"fields":["id"]}},"swatches":{"field_facet":{"field":"color_s","limit":-1}}})JSON";
  if (mode == 11 || mode == 12 || mode == 17 || mode == 19 || mode == 23 || mode == 25) {
    ops = R"JSON({"from_price":"min(price_i)","variants":{"top_docs":{"sort":"price_i asc",
        "limit":3,"fields":["id"]}},"swatches":{"field_facet":{"field":"color_s",
        "limit":-1},"domain":{"query":"*:*","filter":["stock_i:>0"]}
        }})JSON";
  }
  if (mode == 13) {
    ops = R"JSON({"from_price":"min(price_i)","variants":{"top_docs":{"sort":"price_i asc",
        "limit":3,"fields":["id"]},"domain":{"query":"*:*","filter":["stock_i:>0"]}
        },"swatches":{"field_facet":{"field":"color_s","limit":-1}
        }})JSON";
  }
  if (mode == 14) {
    ops = R"JSON({"from_price":"min(price_i)","variants":{"top_docs":{"limit":0,
        "ops":{
        "stock":{"top_docs":{"sort":"price_i asc","limit":3,"fields":["id"]}
        ,"domain":{"query":"*:*","filter":["stock_i:>0"]}}}}},"swatches":{
        "field_facet":{"field":"color_s","limit":-1}}})JSON";
  }
  if (mode == 15) {
    ops = R"JSON({"from_price":"min(price_i)","variants":{"top_docs":{"limit":0,
        "ops":{
        "stock":{"top_docs":{"sort":"price_i asc","limit":3,"fields":["id"]}
        }}}},"swatches":{"field_facet":{"field":"color_s","limit":-1}
        }})JSON";
  }
  if (mode == 20 || mode == 21) {
    std::string reset = mode == 21
        ? R"(,"domain":{"query":"*:*","filter":["stock_i:>0"]})" : "";
    ops = R"({"prices":{"range_facet":{"field":"price_i","start":100,"end":210,"gap":10,
      "ops":{"swatches":{"field_facet":{"field":"color_s","limit":-1})"
        + reset + "}}}}}";
  }
  if (mode >= 26) {
    std::string reset = mode == 27
        ? R"(,"domain":{"query":"*:*","filter":["stock_i:>0"]})" : "";
    ops = R"({"prices":{"range_facet":{"field":"price_i","start":101,"end":)"
        + std::to_string(101 + variants / 2) + R"(,"gap":)" + std::to_string(variants / 2)
        + R"(,"ops":{"swatches":{"field_facet":{"field":"color_s","limit":-1})"
        + reset + "}}}}}";
  }
  bool innerHits = mode == 4 || mode == 8;
  auto json = body + R"(,"collapse":{"field":)" + collapse
      + (nullKeys ? R"(,"missing":"group")" : "")
      + (mode == 0 || mode == 10 || innerHits ? "}}" : R"(,"get_count":true,"head_sort":"price_i asc")"
          + (mode >= 5 ? R"(,"ops":)" + ops : "") + "}}");
  if ((mode >= 16 && mode <= 21) || mode >= 24) {
    // The same catalog cards under ordinary string/numeric facet identities.
    // rank_i is the indexed numeric product id, repeated on every variant.
    std::string field = mode == 18 || mode == 19 ? "rank_i" : "key_s";
    json = R"({"collection":")" + name + R"(","query":")" + query
        + R"(","limit":0,"ops":{"products":{"field_facet":{"field":")" + field
        + R"(","limit":)" + std::to_string(limit) + R"(,"ops":)" + ops + "}}}}";
  }
  if (mode == 24 || mode == 25) {
    json = R"({"collection":")" + name + R"(","query":")" + query
        + R"(","limit":0,"ops":{"prices":{"range_facet":{"field":"price_i","start":101,"end":)"
        + std::to_string(101 + variants / 2) + R"(,"gap":)" + std::to_string(variants / 2)
        + R"(,"ops":)" + ops + "}}}}";
  }

  auto execute = [&](const std::string& text) {
    auto req = localReq(helper.getSearchEngine());
    EXPECT_TRUE(api::read_json(req->rawRequest(), text, req->mr)) << text;
    req->execute(mode == 9 || mode == 10 || mode == 12 ? -1 : 1);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  auto run = [&]() {
    auto page = execute(json);
    size_t peak = page->memoryTracker.peak();
    if (innerHits) {
      // Emulated inner_hits: an independent filtered request for each page
      // group, including query preparation, count and top-1 head selection.
      for (auto* response : page->responses) {
        const auto& list = *response->proto.ops.at("q")->docList();
        for (const auto& key : list.groups->keys) {
          auto product = std::get<std::string_view>(key.component[0].value);
          std::string filter = "key_s:" + std::string(product);
          if (tuple) filter += " AND shop_s:" + std::string(std::get<std::string_view>(key.component[1].value));
          auto members = execute(R"({"collection":")" + name + R"(","query":")" + query
              + R"(","filter":[")" + filter
              + R"("],"sort":"price_i asc","limit":1,"count_limit":-1,"fields":["id"])"
              + (mode == 8 ? R"(,"ops":)" + ops : "") + "}");
          peak = std::max(peak, members->memoryTracker.peak());
          int32_t rows = members->docList()->row_count;
          benchmark::DoNotOptimize(rows);
        }
      }
    }
    return peak;
  };
  {
    CollapseMemberStats stats;
    collapseMemberStats = &stats;
    SkipStatsScope skip;
    size_t blocks = 0;
    facetBucketBlockCounter = &blocks;
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    auto key = reader->field(mode >= 20 ? "price_i" : "rank_i");
    if (numeric) for (auto& segment : reader->segments()) {
      auto* info = key.at(segment);
      ASSERT_NE(nullptr, info);
      ASSERT_NE(0, info->pointsMetaOff);
      state.counters["point_segments"] += 1;
    }
    auto* cache = reader->filterCache();
    auto before = cache ? cache->counters() : FilterCache::CounterValues{};
    run(); // First sighting may bypass; second admits the primitive.
    run();
    auto warmed = cache ? cache->counters() : FilterCache::CounterValues{};
    state.counters["warmup_cache_builds"] = (double)(warmed.builds - before.builds);
    state.counters["warmup_owned_builds"] = (double)SkipStats::ownedFilterMaterializations;
    stats.candidates = 0; stats.members = 0;
    stats.keySegments = 0; stats.domainSegments = 0;
    blocks = 0;
    SkipStats::reset();
    auto peak = run();
    auto after = cache ? cache->counters() : FilterCache::CounterValues{};
    state.counters["cache_hits"] = (double)(after.hits - warmed.hits);
    state.counters["cache_misses"] = (double)(after.misses - warmed.misses);
    state.counters["owned_filter_serves"] = (double)SkipStats::ownedFilterServes;
    if (cache) {
      size_t entries;
      for (auto& entry : cache->dump(100, &entries)) if (entry.keyText.find("stock_i") != std::string::npos) {
        state.counters["stock_cached_segments"] += (double)entry.segmentsResident;
        state.counters["stock_cache_hits"] += (double)entry.hits;
      }
    }
    state.counters["bucket_blocks"] = (double)blocks;
    facetBucketBlockCounter = nullptr;
    state.counters["peak_bytes"] = (double)peak;
    state.counters["candidates"] = (double)stats.candidates;
    state.counters["members"] = (double)stats.members;
    state.counters["key_segments"] = (double)stats.keySegments;
    state.counters["domain_segments"] = (double)stats.domainSegments;
    state.counters["doc_blocks"] = (double)SkipStats::docBlocksDecoded;
    collapseMemberStats = nullptr;
  }
  for (auto _ : state) benchmark::DoNotOptimize(run());
}

#define MEMBER_GRID(name, mode) \
  TUNING_BENCHMARK_CAPTURE(BM_CollapseMembers, name, mode)->UseRealTime() \
      ->ArgsProduct({{10, 100, 1000}, {1, 8, 100}, {1, 7}, {0, 1}, {0, 1}})
MEMBER_GRID(plain, 0);
MEMBER_GRID(keys, 1);
MEMBER_GRID(domain, 2);
MEMBER_GRID(auto, 3);
MEMBER_GRID(inner_hits, 4);
#undef MEMBER_GRID

// Catalog cards: counts/head, price metric, three variants and color swatches.
#define CARD_GRID(name, mode) \
  TUNING_BENCHMARK_CAPTURE(BM_CollapseMembers, name, mode)->UseRealTime() \
      ->ArgsProduct({{10, 100, 1000}, {8, 100}, {7}, {0, 1}, {0, 1}})
CARD_GRID(card, 5);
CARD_GRID(card_keys, 6);
CARD_GRID(card_domain, 7);
CARD_GRID(card_inner_hits, 8);
CARD_GRID(card_parallel, 9);
CARD_GRID(plain_parallel, 10);
CARD_GRID(card_reset, 11);
CARD_GRID(card_reset_parallel, 12);
CARD_GRID(card_top_reset, 13);
CARD_GRID(card_nested_top_reset, 14);
CARD_GRID(card_nested_top, 15);
#undef CARD_GRID

#define FACET_CARD_GRID(name, mode) \
  TUNING_BENCHMARK_CAPTURE(BM_CollapseMembers, name, mode)->UseRealTime() \
      ->ArgsProduct({{10}, {8, 100}, {7}, {0, 1}, {0}})
FACET_CARD_GRID(field_card, 16);
FACET_CARD_GRID(field_reset, 17);
FACET_CARD_GRID(numeric_card, 18);
FACET_CARD_GRID(numeric_reset, 19);
FACET_CARD_GRID(nested_numeric_card, 20);
FACET_CARD_GRID(nested_numeric_reset, 21);
FACET_CARD_GRID(wide_range_card, 24);
FACET_CARD_GRID(wide_range_reset, 25);
FACET_CARD_GRID(nested_wide_range_card, 26);
FACET_CARD_GRID(nested_wide_range_reset, 27);
TUNING_BENCHMARK_CAPTURE(BM_CollapseMembers, null_card, 22)->UseRealTime()
    ->ArgsProduct({{10}, {8, 100}, {7}, {0, 1}, {1}});
TUNING_BENCHMARK_CAPTURE(BM_CollapseMembers, null_reset, 23)->UseRealTime()
    ->ArgsProduct({{10}, {8, 100}, {7}, {0, 1}, {1}});
#undef FACET_CARD_GRID


// Small default-suite coverage; the complete measurement matrix is explicit.
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_keys, 1)->Args({10, 8, 3, 0, 1});
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_domain, 2)->Args({10, 8, 3, 1, 1});

LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_card, 5)->Args({10, 8, 3, 0, 1});

LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_reset, 11)->Args({10, 8, 3, 1, 1});
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_top_reset, 13)->Args({10, 8, 3, 1, 1});
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_nested_top_reset, 14)->Args({10, 8, 3, 1, 1});
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_nested_numeric_reset, 21)->Args({10, 8, 3, 1, 0});
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_null_reset, 23)->Args({10, 8, 3, 1, 1});

LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_wide_range_reset, 25)->Args({10, 8, 3, 0, 0});
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMembers, page_nested_wide_range_reset, 27)->Args({10, 8, 3, 1, 0});

// Dense live-doc bitsets after deletes. The query and filter frames execute
// the same primitive, so RESET must not insert a second live-doc intersection.
static void BM_RootDomainReset(benchmark::State& state, bool reset) {
  int32_t docs = unit_tests ? 120 : (int32_t)LuxirTest::scaleTestWork((int)state.range(0));
  CollectionHelper helper("root_domain_reset_" + std::to_string(docs));
  if (!helper.indexMatchesShape(std::vector<int32_t>{docs})) {
    helper.clear();
    std::vector<Doc> batch;
    for (int32_t doc = 0; doc < docs; doc++) batch.push_back(flatdoc(
        "id", std::to_string(doc), "stock_i", (int64_t)(doc % 3 != 0)));
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
    std::vector<std::string> deleted;
    for (int32_t doc = 0; doc < docs; doc += 5) deleted.push_back(std::to_string(doc));
    ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);
  }
  auto json = R"({"collection":")" + std::string("root_domain_reset_") + std::to_string(docs) + R"JSON(",
        "ops":{"n":{"top_docs":{"limit":0,"count_limit":-1},"domain":)JSON"
      + (reset ? R"({"query":"stock_i:>0"})" : R"({"filter":["stock_i:>0"]})") + "}}}";
  auto run = [&]() {
    auto req = localReq(helper.getSearchEngine());
    EXPECT_TRUE(api::read_json(req->rawRequest(), json, req->mr));
    req->execute(1);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    benchmark::DoNotOptimize(req->memoryTracker.peak());
    return req;
  };
  run(); run();
  {
    SkipStatsScope skip;
    auto req = run();
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    EXPECT_LT(reader->liveDocs(), reader->maxDoc());
    state.counters["max_doc"] = (double)reader->maxDoc();
    state.counters["live_docs"] = (double)reader->liveDocs();
    state.counters["peak_bytes"] = (double)req->memoryTracker.peak();
    state.counters["doc_blocks"] = (double)SkipStats::docBlocksDecoded;
    state.counters["owned_filter_serves"] = (double)SkipStats::ownedFilterServes;
  }
  for (auto _ : state) benchmark::DoNotOptimize(run());
}
TUNING_BENCHMARK_CAPTURE(BM_RootDomainReset, query, true)->UseRealTime()->Arg(160'000)->Arg(2'000'000);
TUNING_BENCHMARK_CAPTURE(BM_RootDomainReset, filter, false)->UseRealTime()->Arg(160'000)->Arg(2'000'000);
LUXIR_BENCHMARK_CAPTURE(BM_RootDomainReset, deletes, true)->Arg(120);

// Same scattered seven-segment catalog as the member-card cases. The facet
// supports one sort expression; the two-clause equivalent is an exact integer
// encoding, since product rank is in [0, products) and price is integral.
static void BM_CollapseMetricOrder(benchmark::State& state, bool facet, bool twoClauses) {
  int32_t variants = (int32_t)state.range(0), segments = 7;
  int32_t products = unit_tests ? 120 : (int32_t)LuxirTest::scaleTestWork(20'000);
  std::string name = "members_reset_bm_" + std::to_string(products) + "_"
      + std::to_string(variants) + "_" + std::to_string(segments);
  CollectionHelper helper(name);
  int32_t total = products * variants;
  std::vector<int32_t> sizes;
  for (int32_t s = 0; s < segments; s++) sizes.push_back((s + 1) * total / segments - s * total / segments);
  if (!helper.indexMatchesShape(sizes)) {
    helper.clear();
    for (int32_t s = 0; s < segments; s++) {
      std::vector<Doc> docs;
      for (int32_t id = s * total / segments; id < (s + 1) * total / segments; id++)
        docs.push_back(collapseCatalogDoc(id, products, variants));
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  std::string body = R"({"collection":")" + name + R"(","fields":[],"sort":"_docid_ asc",)";
  if (facet) {
    std::string metric = twoClauses
        ? "min(price_i)*" + std::to_string(products + 1) + "-max(rank_i)" : "min(price_i)";
    body += R"JSON("limit":0,"ops":{"products":{"field_facet":{"field":"key_s",
      "limit":10,"sort":["order asc"],"ops":{"order":")JSON" + metric + R"("}}}}})";
  } else {
    body += R"JSON("limit":10,"count_limit":-1,"collapse":{"field":"key_s","sort":)JSON"
        + std::string(twoClauses ? R"(["lo asc","hi desc"])" : R"(["lo asc"])")
        + R"JSON(,"ops":{"lo":"min(price_i)")JSON"
        + (twoClauses ? R"JSON(,"hi":"max(rank_i)")JSON" : "") + "}}}";
  }
  auto execute = [&] {
    auto req = localReq(helper.getSearchEngine());
    if (!api::read_json(req->rawRequest(), body, req->mr)) throw std::runtime_error(body);
    req->execute(false);
    if (!req->ok()) throw std::runtime_error(req->errorMsg());
    return req;
  };
  {
    auto req = execute();
    state.counters["peak_tracked"] = (double)req->memoryTracker.peak();
    state.counters["groups"] = facet ? products : req->docList()->found.value_or(-1);
  }
  for (auto _ : state) {
    auto req = execute();
    benchmark::DoNotOptimize(req->responses.size());
  }
  state.SetItemsProcessed(state.iterations() * total);
}
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMetricOrder, groups_min, false, false)->Arg(8)->Arg(100);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMetricOrder, facet_min, true, false)->Arg(8)->Arg(100);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMetricOrder, groups_two, false, true)->Arg(8)->Arg(100);
LUXIR_BENCHMARK_CAPTURE(BM_CollapseMetricOrder, facet_two, true, true)->Arg(8)->Arg(100);

// Document RRF over two catalog windows, optionally collapsed before paging.
static void BM_FusionCollapse(benchmark::State& state, bool collapsed) {
  int32_t variants = (int32_t)state.range(0), segments = 7;
  int32_t products = unit_tests ? 120 : (int32_t)LuxirTest::scaleTestWork(20'000);
  std::string name = "members_reset_bm_" + std::to_string(products) + "_"
      + std::to_string(variants) + "_" + std::to_string(segments);
  CollectionHelper helper(name);
  int32_t total = products * variants;
  std::vector<int32_t> sizes;
  for (int32_t s = 0; s < segments; s++) {
    sizes.push_back((s + 1) * total / segments - s * total / segments);
  }
  if (!helper.indexMatchesShape(sizes)) {
    helper.clear();
    for (int32_t s = 0; s < segments; s++) {
      std::vector<Doc> docs;
      for (int32_t id = s * total / segments; id < (s + 1) * total / segments; id++) {
        docs.push_back(collapseCatalogDoc(id, products, variants));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  std::string json = R"({"collection":")" + name + R"(","ops":{"f":{"fusion":{
    "rrf":{},"sources":{"price":{"sort":"price_i asc","limit":1000},
    "rank":{"sort":"rank_i asc","limit":1000}},"limit":10,
    "count_limit":-1,"fields":["id"])";
  if (collapsed) json += R"(,"collapse":"key_s")";
  json += "}}}}";
  auto execute = [&] {
    auto req = localReq(helper.getSearchEngine());
    if (!api::read_json(req->rawRequest(), json, req->mr)) throw std::runtime_error(json);
    req->execute(false);
    if (!req->ok()) throw std::runtime_error(req->errorMsg());
    return req;
  };
  {
    auto req = execute();
    state.counters["peak_tracked"] = (double)req->memoryTracker.peak();
    state.counters["found"] = req->docList("f")->found.value_or(-1);
    state.counters["found_docs"] = req->docList("f")->found_docs.value_or(
        req->docList("f")->found.value_or(-1));
  }
  for (auto _ : state) {
    auto req = execute();
    benchmark::DoNotOptimize(req->responses.size());
  }
}
LUXIR_BENCHMARK_CAPTURE(BM_FusionCollapse, docs, false)->Arg(8)->Arg(100);
LUXIR_BENCHMARK_CAPTURE(BM_FusionCollapse, groups, true)->Arg(8)->Arg(100);

// Document caps on the same scattered catalog. Args: page, cap (0 = plain),
// exact totals. The diagnostic run records work outside the timed loop.
static void BM_Diversify(benchmark::State& state) {
  int32_t page = (int32_t)state.range(0), cap = (int32_t)state.range(1);
  bool totals = state.range(2);
  int32_t variants = 100, segments = 7;
  int32_t products = unit_tests ? 120 : (int32_t)LuxirTest::scaleTestWork(20'000);
  std::string name = "members_reset_bm_" + std::to_string(products) + "_100_7";
  CollectionHelper helper(name);
  int32_t total = products * variants;
  std::vector<int32_t> sizes;
  for (int32_t s = 0; s < segments; s++)
    sizes.push_back((s + 1) * total / segments - s * total / segments);
  if (!helper.indexMatchesShape(sizes)) {
    helper.clear();
    for (int32_t s = 0; s < segments; s++) {
      std::vector<Doc> docs;
      for (int32_t id = s * total / segments; id < (s + 1) * total / segments; id++)
        docs.push_back(collapseCatalogDoc(id, products, variants));
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
  std::string body = R"({"collection":")" + name
      + R"(","sort":"rank_i desc","fields":["id"],"limit":)" + std::to_string(page)
      + R"(,"count_limit":)" + (totals ? "-1" : "0");
  if (cap) body += R"(,"diversify":{"field":"key_s","max_per_key":)" + std::to_string(cap) + '}';
  body += '}';
  auto execute = [&] {
    auto req = localReq(helper.getSearchEngine());
    if (!api::read_json(req->rawRequest(), body, req->mr)) throw std::runtime_error(body);
    req->execute(false);
    if (!req->ok()) throw std::runtime_error(req->errorMsg());
    return req;
  };
  {
    SearchOverridesGuard guard(SkipStats::enabled);
    SkipStats::enabled = true;
    SkipStats::reset();
    auto req = execute();
    state.counters["peak_tracked"] = (double)req->memoryTracker.peak();
    state.counters["visited"] = (double)SkipStats::fieldSortDocsGathered;
    state.counters["decoded"] = (double)SkipStats::docBlocksDecoded;
    state.counters["found"] = (double)req->docList()->found.value_or(-1);
  }
  for (auto _ : state) {
    auto req = execute();
    benchmark::DoNotOptimize(req->responses.size());
  }
  state.SetItemsProcessed(state.iterations() * total);
}
LUXIR_BENCHMARK(BM_Diversify)->ArgsProduct({{10, 100}, {0, 1, 2, 10, 100}, {0, 1}});
