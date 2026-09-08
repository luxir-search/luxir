// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <vector>
#include <cstdint>
#include "luxir/util/random.h"
#include "luxir/util/luxir_util.h"
#include "luxir/store/InputStream.h"
#include "bench/luxir_bench.h"
#include <gtest/gtest.h>

// StreamVByte (FastPFOR deps/FastPFOR/src/streamvbyte.c), C linkage. The _d1 variants
// are differential (delta + prefix-sum), matching a delta-coded docid postings tail.
// Separate key (control) + data streams; the AVX decoder may over-read data with
// 16-byte loads, so the data buffer is padded. This benchmark checks whether the
// format beats scalar vint before changing the postings tail.
extern "C" {
  uint8_t* svb_encode_scalar_d1_init(const uint32_t* in, uint8_t* keyPtr, uint8_t* dataPtr,
                                     uint32_t count, uint32_t prev);
  uint8_t* svb_decode_scalar_d1_init(uint32_t* out, const uint8_t* keyPtr, uint8_t* dataPtr,
                                     uint32_t count, uint32_t prev);
  uint8_t* svb_decode_avx_d1_init(uint32_t* out, uint8_t* keyPtr, uint8_t* dataPtr,
                                  uint64_t count, uint32_t prev);
}

namespace luxir {

// n sorted docids; gaps uniform in [1, 2*avgGap] so byte-lengths vary (vint mispredicts).
static std::vector<uint32_t> genDocs(Rng& rng, uint32_t n, uint32_t avgGap) {
  std::vector<uint32_t> v;
  v.reserve(n);
  uint32_t d = 0;
  for (uint32_t i = 0; i < n; i++) { d += 1 + rng.rint(2 * avgGap); v.push_back(d); }
  return v;
}

static void writeVint(std::vector<uint8_t>& out, uint32_t v) {
  while (v >= 0x80) { out.push_back(uint8_t((v & 0x7f) | 0x80)); v >>= 7; }
  out.push_back(uint8_t(v));
}

// Baseline: LEB128 delta plus prefix accumulation, matching postings-tail decode.
static void BM_Tail_vint(benchmark::State& state, uint32_t n, uint32_t avgGap) {
  Rng rng(0);
  auto docs = genDocs(rng, n, avgGap);
  std::vector<uint8_t> enc;
  uint32_t prev = 0;
  for (auto d : docs) { writeVint(enc, d - prev); prev = d; }
  size_t encBytes = enc.size();
  enc.resize(enc.size() + 16);
  std::vector<uint32_t> out(n);
  for (auto _ : state) {
    const char* pos = (const char*) enc.data();
    const char* end = (const char*) enc.data() + enc.size();
    uint32_t docid = 0;
    for (uint32_t i = 0; i < n; i++) { docid += InputStream::readVint(pos, end); out[i] = docid; }
    benchmark::DoNotOptimize(out.data());
  }
  if (unit_tests) { std::vector<uint32_t> chk(out.begin(), out.begin() + n); ASSERT_EQ(chk, docs); }
  state.counters["bytes"] = (double) encBytes;
}

static void BM_Tail_svb(benchmark::State& state, uint32_t n, uint32_t avgGap, bool avx) {
  Rng rng(0);
  auto docs = genDocs(rng, n, avgGap);
  std::vector<uint8_t> keys((n + 3) / 4 + 16, 0);
  std::vector<uint8_t> data((size_t) n * 4 + 32, 0);  // up to 4B/val + SIMD over-read pad
  uint8_t* dataEnd = svb_encode_scalar_d1_init(docs.data(), keys.data(), data.data(), n, 0);
  size_t encBytes = (n + 3) / 4 + (size_t)(dataEnd - data.data());
  std::vector<uint32_t> out(n + 8);  // avx writes in groups; pad the output
  for (auto _ : state) {
    if (avx) svb_decode_avx_d1_init(out.data(), keys.data(), data.data(), n, 0);
    else     svb_decode_scalar_d1_init(out.data(), keys.data(), data.data(), n, 0);
    benchmark::DoNotOptimize(out.data());
  }
  if (unit_tests) { std::vector<uint32_t> chk(out.begin(), out.begin() + n); ASSERT_EQ(chk, docs); }
  state.counters["bytes"] = (double) encBytes;
}

// n=8 (small rare-term tail) and n=120 (near-full tail); dense 1-byte gaps vs sparse
// multi-byte, variable-length gaps (where the vint continuation branch mispredicts).
BENCHMARK_CAPTURE(BM_Tail_vint, n8_dense,    8,   2);
BENCHMARK_CAPTURE(BM_Tail_svb,  n8_dense,    8,   2,      true);
BENCHMARK_CAPTURE(BM_Tail_vint, n8_sparse,   8,   100000);
BENCHMARK_CAPTURE(BM_Tail_svb,  n8_sparse,   8,   100000, true);
BENCHMARK_CAPTURE(BM_Tail_vint, n120_dense,  120, 2);
BENCHMARK_CAPTURE(BM_Tail_svb,  n120_dense,  120, 2,      true);
BENCHMARK_CAPTURE(BM_Tail_vint, n120_sparse, 120, 100000);
BENCHMARK_CAPTURE(BM_Tail_svb,  n120_sparse_scalar, 120, 100000, false);
BENCHMARK_CAPTURE(BM_Tail_svb,  n120_sparse, 120, 100000, true);

}  // namespace luxir
