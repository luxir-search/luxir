// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/reader/Postings.h"
#include "test/CollectionHelper.h"
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

}  // namespace

LUXIR_BENCHMARK_CAPTURE(BM_SourceIngest, off, false);
LUXIR_BENCHMARK_CAPTURE(BM_SourceIngest, on, true);
LUXIR_BENCHMARK_CAPTURE(BM_SourceFetch, stored, false);
LUXIR_BENCHMARK_CAPTURE(BM_SourceFetch, source, true);
