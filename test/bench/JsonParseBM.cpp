// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// JSON document parse cost (the NDJSON ingest decode step): one record into an
// api::Map on a reset arena, as HttpServer::processStreamRecord does.
//
// gcc-release, 28-core x86-64 (shared, loaded), medians of 5:
//   before number-array fast path: vector768 31.3 us, textOnly 0.88 us, VectorCoerce768 851 ns
//   after:                         vector768  8.6 us, textOnly 0.72 us, VectorCoerce768  77 ns

#include <cmath>
#include <memory_resource>
#include <random>
#include <string>

#include <fmt/format.h>

#include "bench/luxir_bench.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/ValCoerce.h"

using namespace luxir;

namespace {

std::string randomWords(std::mt19937_64& rng, int n) {
  std::string out;
  for (int i = 0; i < n; i++) {
    if (i) out += ' ';
    int len = 3 + (int)(rng() % 8);
    for (int j = 0; j < len; j++) out += (char)('a' + rng() % 26);
  }
  return out;
}

// Shaped like a Python json.dumps record: text fields plus, optionally, a
// 768-dim embedding of 5-decimal floats in [-1, 1].
std::string makeDoc(bool withVector) {
  std::mt19937_64 rng(42);
  std::string doc = fmt::format(R"({{"id": "d0", "title_t": "{}", "body_t": "{}", "cat_s": "cat3", )"
                                R"("year_i": 1984, "price_d": 16.135358445921344)",
                                randomWords(rng, 8), randomWords(rng, 160));
  if (withVector) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    doc += R"(, "embedding_v": [)";
    for (int i = 0; i < 768; i++) {
      if (i) doc += ", ";
      doc += fmt::format("{}", std::round(dist(rng) * 1e5) / 1e5);
    }
    doc += "]";
  }
  doc += "}";
  return doc;
}

void BM_JsonParseDoc(benchmark::State& state, bool withVector) {
  std::string doc = makeDoc(withVector);
  std::pmr::monotonic_buffer_resource mr(64 * 1024);
  for (auto _ : state) {
    api::Map map;
    bool ok = api::read_json(map, std::string_view(doc), mr);
    if (!ok) {
      state.SkipWithError("parse failed");
      break;
    }
    benchmark::DoNotOptimize(map);
    mr.release();
  }
  state.SetBytesProcessed((int64_t)state.iterations() * (int64_t)doc.size());
}

// Vector coercion (VectorHandler::index): the parsed 768-dim ArrDouble to floats.
void BM_VectorCoerce768(benchmark::State& state) {
  std::string doc = makeDoc(true);
  std::pmr::monotonic_buffer_resource mr;
  api::Map map;
  if (!api::read_json(map, std::string_view(doc), mr)) {
    state.SkipWithError("parse failed");
    return;
  }
  const api::Val& val = **map.fields.find("embedding_v");
  std::vector<float> floats;
  std::vector<int32_t> lens;
  for (auto _ : state) {
    coerce::toVectors(val, "embedding_v", floats, lens);
    benchmark::DoNotOptimize(floats.data());
  }
}

} // namespace

LUXIR_BENCHMARK(BM_VectorCoerce768);
LUXIR_BENCHMARK_CAPTURE(BM_JsonParseDoc, vector768, true);
LUXIR_BENCHMARK_CAPTURE(BM_JsonParseDoc, textOnly, false);
