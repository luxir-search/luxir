// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/FilterCache.h"
#include "luxir/store/Directory.h"

using namespace luxir;

namespace {

struct KeyShape {
  std::vector<std::string> values;
  std::vector<std::unique_ptr<TermQuery>> terms;
  std::vector<Query*> clauses;
  std::unique_ptr<BooleanQuery> boolean;

  explicit KeyShape(size_t count) {
    values.reserve(count);
    terms.reserve(count);
    clauses.reserve(count);
    for (size_t i = 0; i < count; i++) {
      values.push_back("term_" + std::to_string(i));
      terms.push_back(std::make_unique<TermQuery>("filter_w", values.back()));
      clauses.push_back(terms.back().get());
    }
    if (count > 1) {
      boolean = std::make_unique<BooleanQuery>(
          std::span<Query*>(), std::span<Query*>(clauses),
          std::span<Query*>(), std::span<Query*>());
    }
  }

  Query& query() {
    return boolean == nullptr ? (Query&)*terms[0] : (Query&)*boolean;
  }
};

FilterKey buildKey(Query& query) {
  FilterKeyContext context;
  context.schemaGen = 7;
  FilterKeyBuilder builder;
  FilterKeyScope scope = query.appendFilterKey(builder, context);
  return std::move(builder).finish(scope, context).value();
}

FilterCacheConfig cacheConfig() {
  return {
      .maxBytes = 64ULL * 1024 * 1024,
      .lowWatermarkBytes = 56ULL * 1024 * 1024,
      .maxEntryBytes = 8ULL * 1024 * 1024,
      .minSegmentDocs = 0,
      .admissionHistorySize = 256,
      .admissionThreshold = 1,
      .maxMetadataEntries = 1024,
      .maxMetadataBytes = 16ULL * 1024 * 1024};
}

std::unique_ptr<DocSet> denseDocs(int32_t maxDoc) {
  auto docs = std::make_unique<RAMBitDocSet>(maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc += 2) {
    docs->mutableBits().set(doc);
  }
  return docs;
}

std::shared_ptr<const FilterCache::SegmentValue> populate(
    FilterCache& cache, const FilterKey& key,
    std::span<const FilterCache::SegmentIdentity> segments) {
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);
  auto probe = use->probe(0);
  return use->publishRaw(0, probe, denseDocs(segments[0].maxDoc), 1);
}

void BM_FilterCacheKeyBuild(benchmark::State& state, size_t clauseCount) {
  KeyShape shape(clauseCount);
  FilterKeyContext context;
  context.schemaGen = 7;
  FilterKey sample = buildKey(shape.query());
  state.counters["key_bytes"] = (double)sample.bytes().size();
  for (auto _ : state) {
    FilterKeyBuilder builder;
    FilterKeyScope scope = shape.query().appendFilterKey(builder, context);
    auto key = std::move(builder).finish(scope, context);
    benchmark::DoNotOptimize(key->bytes().data());
    benchmark::DoNotOptimize(key->hash());
  }
  state.SetItemsProcessed((int64_t)state.iterations() * (int64_t)clauseCount);
}

void BM_FilterCacheBeginUseHot(benchmark::State& state, size_t clauseCount) {
  KeyShape shape(clauseCount);
  FilterKey key = buildKey(shape.query());
  FilterCache cache(cacheConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  auto held = populate(cache, key, segments);
  benchmark::DoNotOptimize(held);

  for (auto _ : state) {
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(key);
    benchmark::DoNotOptimize(use);
  }
  state.SetItemsProcessed((int64_t)state.iterations());
}

void BM_FilterCacheProbePin(benchmark::State& state) {
  FilterCache cache(cacheConfig());
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  cache.onReaderPublished(1, segments);
  FilterKey key("probe-pin");
  auto held = populate(cache, key, segments);
  FilterCache::UseRegistry request(cache, 1, segments);
  auto* use = request.get(key);

  for (auto _ : state) {
    auto probe = use->probe(0);
    benchmark::DoNotOptimize(probe.docSet());
  }
  benchmark::DoNotOptimize(held);
  state.SetItemsProcessed((int64_t)state.iterations());
}

struct EffectiveFixture {
  static constexpr int32_t MAX_DOC = 2048;

  RAMDir dir;
  CommitSnapshotRegistry snapshots{dir};
  IndexWriter writer;
  std::shared_ptr<IndexReader> withoutDeletes;
  std::shared_ptr<IndexReader> withDeletes;

  EffectiveFixture() : writer(snapshots) {
    auto& inverter = writer.obtainInverter(1);
    auto& id = inverter.getIndexHandler("id");
    for (int32_t doc = 0; doc < MAX_DOC; doc++) {
      inverter.startDoc();
      id.index(inverter, std::to_string(doc));
      inverter.finishDoc();
    }
    writer.releaseInverter(inverter);
    writer.commit();
    withoutDeletes = writer.snapshots.readers.getReader(0);

    auto& deletes = writer.obtainInverter(2);
    for (int32_t doc = 1; doc < MAX_DOC; doc += 2) {
      deletes.deleteId(std::to_string(doc), 2);
    }
    writer.releaseInverter(deletes);
    writer.commit();
    withDeletes = writer.snapshots.readers.getReader(0);
  }
};

EffectiveFixture& effectiveFixture() {
  static EffectiveFixture fixture;
  return fixture;
}

void BM_FilterCacheEffectiveDocSet(benchmark::State& state, bool deletes) {
  auto& fixture = effectiveFixture();
  auto reader = deletes ? fixture.withDeletes : fixture.withoutDeletes;
  if (reader->segments().size() != 1
      || (reader->segments()[0].liveDocs() != nullptr) != deletes) {
    state.SkipWithError("unexpected effectiveDocSet benchmark segment shape");
    return;
  }

  FilterCache cache(cacheConfig());
  cache.onReaderPublished(*reader);
  FilterKey key("effective");
  std::array segments{FilterCache::SegmentIdentity{
      reader->segments()[0].segInfo.seg_id, reader->segments()[0].maxDoc()}};
  auto held = populate(cache, key, segments);
  benchmark::DoNotOptimize(held);

  for (auto _ : state) {
    FilterCache::UseRegistry request(cache, *reader);
    auto* use = request.get(key);
    auto probe = use->probe(0);
    benchmark::DoNotOptimize(probe.docSet());
    DocSet* result;
    {
      BenchTimer timer(state);
      result = use->effectiveDocSet(0, *reader);
      benchmark::DoNotOptimize(result);
    }
  }
  state.SetItemsProcessed((int64_t)state.iterations());
}

void BM_FilterCachePublishDense(benchmark::State& state) {
  std::array segments{FilterCache::SegmentIdentity{1, 4096}};
  for (auto _ : state) {
    FilterCache cache(cacheConfig());
    cache.onReaderPublished(1, segments);
    FilterCache::UseRegistry request(cache, 1, segments);
    auto* use = request.get(FilterKey("dense-publish"));
    auto probe = use->probe(0);
    auto docs = denseDocs(4096);
    std::shared_ptr<const FilterCache::SegmentValue> value;
    {
      BenchTimer timer(state);
      value = use->publishRaw(0, probe, std::move(docs), 1);
      benchmark::DoNotOptimize(value);
    }
  }
  state.SetItemsProcessed((int64_t)state.iterations());
}

} // namespace

LUXIR_BENCHMARK_CAPTURE(BM_FilterCacheKeyBuild, term, 1);
LUXIR_BENCHMARK_CAPTURE(BM_FilterCacheKeyBuild, boolean_100, 100);
LUXIR_BENCHMARK_CAPTURE(BM_FilterCacheKeyBuild, boolean_10k, 10'000);

LUXIR_BENCHMARK_CAPTURE(BM_FilterCacheBeginUseHot, term, 1);
LUXIR_BENCHMARK_CAPTURE(BM_FilterCacheBeginUseHot, boolean_100, 100);
LUXIR_BENCHMARK_CAPTURE(BM_FilterCacheBeginUseHot, boolean_10k, 10'000);

LUXIR_BENCHMARK(BM_FilterCacheProbePin);

BENCHMARK_CAPTURE(BM_FilterCacheEffectiveDocSet, raw_serve, false)
    ->UseManualTime()->Iterations(100);
BENCHMARK_CAPTURE(BM_FilterCacheEffectiveDocSet, live_docs_and, true)
    ->UseManualTime()->Iterations(100);
BENCHMARK(BM_FilterCachePublishDense)->UseManualTime()->Iterations(100);
