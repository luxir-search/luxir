#include <cstdint>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/codec/Codec.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/util/random.h"
#include <gtest/gtest.h>

// Head-to-head for the two integer bit-packing layouts, across bit widths:
//   - LuxirSIMDFor: FastPFOR 4-interleaved-lane layout (the numeric/postings
//     codec). Bulk decode is one shared shift+mask per output vector; single
//     select pays lane arithmetic plus a 1-2 word stitch.
//   - LinearPack: flat contiguous exact-bpv layout (the ord/column codec).
//     Single select is one unaligned u64 load+shift+mask; bulk decode is an
//     AVX2 shuffle+srlv per 4-value group.
// Expectation: flat select wins big and is width-independent; flat block decode
// is at parity except the 11-16 and 26-31 bit bands (load-window boundaries).
// Both access the same 128 values, packed each way from one random source.

namespace luxir {

static constexpr uint32_t N = 128;

// Random values in [0, 2^bits), packed both ways. minval is 0 so a value equals
// its own FOR delta (the production numeric/ord invariant).
struct PackedBlock {
  std::vector<uint32_t> vals;
  std::vector<char> pfor;   // FastPFOR interleaved
  std::vector<char> flat;   // LinearPack contiguous
  uint32_t mask;
  uint8_t bits;

  PackedBlock(uint8_t bits) : vals(N), mask(LinearPack::mask32(bits)), bits(bits) {
    Rng rng(0);
    uint32_t vmask = bits >= 32 ? ~0u : ((1u << bits) - 1u);
    for (uint32_t i = 0; i < N; i++) vals[i] = (uint32_t)rng.rint(vmask) & vmask;

    LuxirSIMDFor codec;
    pfor.assign(LuxirSIMDFor::byteSize(N, bits) + 64, 0);
    uint32_t outSz = pfor.size();
    std::vector<uint32_t> in(vals);
    codec.encodeWithMeta(in.data(), N, pfor.data(), outSz, 0, bits);

    LinearPack::Writer w(flat, bits);
    for (uint32_t v : vals) w.append(v);
    w.finish();
  }
};

static void BM_pfor_block(benchmark::State& state, uint8_t bits) {
  PackedBlock blk(bits);
  LuxirSIMDFor codec;
  uint32_t out[N];
  for (auto _ : state) {
    codec.decodeSingleBlock(blk.pfor.data(), out, N, bits);
    benchmark::DoNotOptimize(out);
  }
  if (unit_tests) {
    for (uint32_t i = 0; i < N; i++) ASSERT_EQ(blk.vals[i], out[i]);
  }
  state.SetItemsProcessed((int64_t)state.iterations() * N);
}

static void BM_flat_block(benchmark::State& state, uint8_t bits) {
  PackedBlock blk(bits);
  uint32_t out[N];
  for (auto _ : state) {
    LinearPack::unpack128(blk.flat.data(), 0, N, bits, blk.mask, out);
    benchmark::DoNotOptimize(out);
  }
  if (unit_tests) {
    for (uint32_t i = 0; i < N; i++) ASSERT_EQ(blk.vals[i], out[i]);
  }
  state.SetItemsProcessed((int64_t)state.iterations() * N);
}

// Pseudo-random probe order so the load address is data-dependent per select.
static void probeOrder(uint32_t* idx) {
  for (uint32_t i = 0; i < N; i++) idx[i] = (uint32_t)((i * 73u + 17u) & 127u);
}

static void BM_pfor_select(benchmark::State& state, uint8_t bits) {
  PackedBlock blk(bits);
  LuxirSIMDFor codec;
  uint32_t idx[N];
  probeOrder(idx);
  uint32_t sink = 0;
  for (auto _ : state) {
    for (uint32_t i = 0; i < N; i++)
      sink += codec.selectWithMeta(blk.pfor.data(), N, idx[i], 0, bits);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed((int64_t)state.iterations() * N);
}

static void BM_flat_select(benchmark::State& state, uint8_t bits) {
  PackedBlock blk(bits);
  uint32_t idx[N];
  probeOrder(idx);
  uint32_t sink = 0;
  for (auto _ : state) {
    for (uint32_t i = 0; i < N; i++)
      sink += LinearPack::select32(blk.flat.data(), idx[i], bits, blk.mask);
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed((int64_t)state.iterations() * N);
}

// Widths span every code path: the fast loadu band (17-24), the narrow-load
// band (11-16), the loadl band (<=10), and the two-part high band (26-31).
#define REG(b)                                                          \
  LUXIR_BENCHMARK_CAPTURE(BM_pfor_block, b/blk, (uint8_t)b);            \
  LUXIR_BENCHMARK_CAPTURE(BM_flat_block, b/blk, (uint8_t)b);           \
  LUXIR_BENCHMARK_CAPTURE(BM_pfor_select, b/sel, (uint8_t)b);          \
  LUXIR_BENCHMARK_CAPTURE(BM_flat_select, b/sel, (uint8_t)b);

REG(4) REG(8) REG(12) REG(16) REG(20) REG(24) REG(31)

}  // namespace luxir
