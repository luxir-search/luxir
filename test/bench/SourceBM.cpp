// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <string>
#include <vector>

#include "bench/ScientificText.h"
#include "bench/luxir_bench.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
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
