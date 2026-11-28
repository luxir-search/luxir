// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <future>
#include <sys/resource.h>
#include <map>
#include <memory_resource>
#include <string>
#include <vector>

#include "bench/ScientificText.h"
#include "bench/luxir_bench.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/server/ProtoUpdateMessage.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

// The _source_ store's two costs:
//
//   BM_SourceIngest/{off,on}  - index a batch of documents received as Maps,
//                               with the store off and on. The difference is
//                               the capture cost (serialize, compress, write).
//   BM_SourceFetch/{stored,source} - a top-100 page fetching one stored text
//                               field versus the whole _source: block
//                               decompression and splicing per hit.
//   BM_StoredBytes/<config>   - bytes per document on disk after a force merge
//                               of ~2KB article documents (bytes_per_doc):
//                                 none     no stored text, no source (the index alone)
//                                 lz4      the old default, no source
//                                 zstd     the default stored fields, no source
//                                 source   _source_ on, text served from it
//                                 copy     _source_ on, every text field also stored
//   BM_SourcePatch/{reindex,source_only} - a request of 1000 patches to random
//                               documents of a 200k-document collection, each
//                               committed: an increment of an indexed field
//                               (the document is reindexed) versus a set of a
//                               source-only field (only its record is rewritten).
//                               cpu_us_per_patch counts the whole process, so
//                               the flushes and merges reindexing causes count.

namespace {

std::vector<Doc> makeDocs(int64_t count, int64_t offset) {
  std::vector<Doc> docs;
  docs.reserve((size_t)count);
  for (int64_t i = 0; i < count; i++) {
    int64_t n = offset + i;
    docs.push_back(flatdoc("id", std::to_string(n), "title_t", "document number " + std::to_string(n) + " about things",
                           "body_t", std::string("the quick brown fox jumps over the lazy dog ") + std::to_string(n % 97),
                           "n_i", n % 1000, "tags_ss", std::vector<std::string>{"t" + std::to_string(n % 10), "x"}));
  }
  return docs;
}

void configure(CollectionHelper& helper, bool source) {
  helper.clear();
  if (!source) return;
  SchemaBuilder b;
  b.storedResource(Postings::SOURCE_RESOURCE);
  b.set(helper.collection());
}

void BM_SourceIngest(benchmark::State& state, bool source) {
  CollectionHelper helper;
  configure(helper, source);
  int64_t batch = luxir::unit_tests ? LuxirTest::scaleTestWork(200) : 20'000;
  int64_t offset = 0;
  for (auto _ : state) {
    auto docs = makeDocs(batch, offset);
    offset += batch;
    auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
    if (!result.success) state.SkipWithError("indexing failed");
  }
  state.SetItemsProcessed(state.iterations() * batch);
  state.counters["docs_per_s"] = benchmark::Counter((double)state.iterations() * (double)batch,
                                                    benchmark::Counter::kIsRate);
}

void BM_SourceFetch(benchmark::State& state, bool source) {
  CollectionHelper helper;
  configure(helper, true);
  int64_t count = luxir::unit_tests ? LuxirTest::scaleTestWork(300) : 200'000;
  int64_t batch = 5000;
  for (int64_t offset = 0; offset < count; offset += batch) {
    auto docs = makeDocs(std::min(batch, count - offset), offset);
    if (!helper.indexAll(docs, UpdateMessage::NO_COMMIT).success) {
      state.SkipWithError("indexing failed");
      return;
    }
  }
  helper.commit();
  std::vector<std::string> fields = {source ? "_source" : "title_t"};
  for (auto _ : state) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("body_t", "fox").fields(fields).limit(100);
    req->execute();
    if (!req->ok()) state.SkipWithError(req->errorMsg().c_str());
    benchmark::DoNotOptimize(req->docList());
  }
  state.SetItemsProcessed(state.iterations() * 100);
}

std::vector<Doc> makeArticles(int64_t count) {
  std::vector<Doc> docs;
  docs.reserve((size_t)count);
  for (int64_t i = 0; i < count; i++) {
    docs.push_back(flatdoc("id", "PMC" + std::to_string(1000000 + i), "title_t", scientificText(100, (uint64_t)i * 2 + 1),
                           "abstract_t", scientificText(1500, (uint64_t)i * 2 + 2), "journal_s", "J" + std::to_string(i % 7),
                           "year_i", 1990 + i % 35));
  }
  return docs;
}

enum class StoredConfig { NONE, LZ4, ZSTD, SOURCE, COPY };

void BM_StoredBytes(benchmark::State& state, StoredConfig config) {
  int64_t count = luxir::unit_tests ? LuxirTest::scaleTestWork(300) : 50'000;
  auto docs = makeArticles(count);
  for (auto _ : state) {
    CollectionHelper helper;
    helper.clear();
    SchemaBuilder b;
    if (config == StoredConfig::LZ4) b.storedResource(Postings::STORED_DEFAULT_RESOURCE).codec = api::StoredResourceDef_::Codec::LZ4;
    if (config == StoredConfig::SOURCE || config == StoredConfig::COPY) b.storedResource(Postings::SOURCE_RESOURCE);
    for (auto name : {"title_t", "abstract_t"}) {
      auto& f = b.field(name);
      f.type = api::FieldDef::FieldClass::TEXT;
      if (config == StoredConfig::NONE) f.stored = false;
      if (config == StoredConfig::COPY) f.stored_copy = true;
    }
    b.set(helper.collection());
    for (int64_t offset = 0; offset < count; offset += 5000) {
      std::span<const Doc> batch(docs.data() + offset, (size_t)std::min<int64_t>(5000, count - offset));
      if (!helper.indexAll(batch, UpdateMessage::COMMIT).success) state.SkipWithError("indexing failed");
    }
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    if (!helper.submit(merge).success) state.SkipWithError("merge failed");
    // The committed files only: retired inputs may not be deleted yet. A file
    // two segments share (a referenced source file) counts once.
    std::map<std::string, uint64_t> files;
    for (const auto& file : manifestFiles(*readDurableIndexInfo(helper.getIndexWriter()->dir))) {
      files[std::string(file.name)] = file.size;
    }
    uint64_t bytes = 0;
    for (const auto& [name, size] : files) bytes += size;
    state.counters["bytes_per_doc"] = (double)bytes / (double)count;
    helper.clear();
  }
}

// An update request written as JSON, run to completion.
struct JsonRequest {
  std::pmr::monotonic_buffer_resource arena;
  api::UpdateRequest request;
  explicit JsonRequest(std::string_view json) {
    std::string err;
    if (!api::read_json(request, json, arena, &err)) throw std::runtime_error(err);
  }
};

class WaitedUpdate final : public ProtoUpdateMessage {
public:
  std::promise<void> completed;
  using ProtoUpdateMessage::ProtoUpdateMessage;
  void done(IndexWriter&) override { completed.set_value(); }
};

void BM_SourcePatch(benchmark::State& state, bool sourceOnly) {
  CollectionHelper helper;
  helper.clear();
  SchemaBuilder b;
  b.storedResource(Postings::SOURCE_RESOURCE);
  auto& note = b.field("note");
  note.type = api::FieldDef::FieldClass::STRING;
  note.index = api::FieldDef::IndexMode::NONE;
  note.column = false;
  b.set(helper.collection());
  int64_t count = luxir::unit_tests ? LuxirTest::scaleTestWork(300) : 200'000;
  int64_t batch = 5000;
  for (int64_t offset = 0; offset < count; offset += batch) {
    auto docs = makeDocs(std::min(batch, count - offset), offset);
    for (auto& doc : docs) doc.push_back({"note", std::string("initial note")});
    if (!helper.indexAll(docs, UpdateMessage::NO_COMMIT).success) {
      state.SkipWithError("indexing failed");
      return;
    }
  }
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  helper.submit(merge);
  auto iw = helper.getIndexWriter();
  auto sourceOnlyCount = [&] { return iw->metrics->counts().counters[(size_t)MetricCounter::PATCHES_SOURCE_ONLY]; };
  uint64_t sourceOnlyBefore = sourceOnlyCount();
  // CPU of the whole process, background flushes and merges included.
  auto processCpuUs = [] {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return (double)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1e6 + (double)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
  };
  double cpuBefore = processCpuUs();
  int64_t patches = luxir::unit_tests ? 20 : 1000;
  uint64_t next = 1;
  for (auto _ : state) {
    state.PauseTiming();
    std::string json = R"({"patches":[)";
    for (int64_t i = 0; i < patches; i++) {
      next = next * 6364136223846793005ull + 1442695040888963407ull;
      if (i) json += ',';
      json += R"({"id":")" + std::to_string((next >> 33) % (uint64_t)count) + R"(","ops":[)";
      json += sourceOnly ? R"({"op":"set","path":"note","value":"note )" + std::to_string(next % 100000) + R"("})"
                         : std::string(R"({"op":"increment","path":"n_i","value":1})");
      json += "]}";
    }
    json += "]}";
    JsonRequest parsed(json);
    WaitedUpdate update(&parsed.request);
    state.ResumeTiming();
    if (!iw->submitUpdate(&update)) state.SkipWithError("submit failed");
    update.completed.get_future().wait();
    if (update.result.errored()) state.SkipWithError(std::string(update.result.what()));
  }
  state.SetItemsProcessed(state.iterations() * patches);
  // The share of patches written without reindexing.
  state.counters["cpu_us_per_patch"] = (processCpuUs() - cpuBefore) / (double)std::max<int64_t>(1, state.iterations() * patches);
  state.counters["source_only"] = (double)(sourceOnlyCount() - sourceOnlyBefore) /
                                  (double)std::max<int64_t>(1, state.iterations() * patches);
  state.counters["patches_per_s"] = benchmark::Counter((double)state.iterations() * (double)patches,
                                                       benchmark::Counter::kIsRate);
}

}  // namespace

LUXIR_BENCHMARK_CAPTURE(BM_StoredBytes, none, StoredConfig::NONE)->Iterations(1);
LUXIR_BENCHMARK_CAPTURE(BM_StoredBytes, lz4, StoredConfig::LZ4)->Iterations(1);
LUXIR_BENCHMARK_CAPTURE(BM_StoredBytes, zstd, StoredConfig::ZSTD)->Iterations(1);
LUXIR_BENCHMARK_CAPTURE(BM_StoredBytes, source, StoredConfig::SOURCE)->Iterations(1);
LUXIR_BENCHMARK_CAPTURE(BM_StoredBytes, copy, StoredConfig::COPY)->Iterations(1);
LUXIR_BENCHMARK_CAPTURE(BM_SourceIngest, off, false);
LUXIR_BENCHMARK_CAPTURE(BM_SourceIngest, on, true);
LUXIR_BENCHMARK_CAPTURE(BM_SourceFetch, stored, false);
LUXIR_BENCHMARK_CAPTURE(BM_SourceFetch, source, true);
LUXIR_BENCHMARK_CAPTURE(BM_SourcePatch, reindex, false);
LUXIR_BENCHMARK_CAPTURE(BM_SourcePatch, source_only, true);
