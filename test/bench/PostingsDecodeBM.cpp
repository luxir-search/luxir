#include <cstdint>
#include <vector>

#include "bench/solux_bench.h"
#include "solux/codec/Codec.h"
#include "solux/reader/Postings.h"
#include "solux/util/random.h"
#include <gtest/gtest.h>

// Where does postings doc-decode spend its time? Each block is unpacked (the
// FastPFOR 4-interleaved-lane bit layout, uniform shifts) then prefix-summed to
// undo the doc-gap delta. AVX-512 can widen the unpack, but the D1 inverse-delta
// carries a serial running-count vector-to-vector, so wider registers cannot
// speed it up. This bench measures the split end-to-end over a multi-block list
// so the two phases overlap across blocks (out-of-order), the realistic cost:
//   end2end - SoluxPFORd::decodeBlock (unpack + inverse-delta), the real path
//   unpack  - SoluxSIMDFor unpack of the same-width delta residuals, no delta
// The gap end2end - unpack is the in-situ prefix-sum cost (the width-immune part
// a wider unpack cannot move). Deps/FastPFOR headers are confined to Codec.cpp,
// so we reach the codecs through their public APIs only; the pure-FOR unpack is
// a proxy for the PFor unpack (equal when gaps are uniform, no exceptions).

namespace solux {

static constexpr uint32_t B = Postings::DOCS_BLOCK_SIZE;  // 128
static constexpr uint32_t N_BLOCKS = 256;                 // 32K docs, encoded fits L2

struct PostingsList {
  std::vector<char> encoded;                // SoluxPFORd blocks, contiguous
  std::vector<uint32_t> blockOffset;
  std::vector<char> forEncoded;             // SoluxSIMDFor of the gap residuals
  std::vector<uint32_t> forOffset;
  std::vector<uint8_t> forBits;

  explicit PostingsList(uint8_t gapBits) {
    Rng rng(0);
    uint32_t gapMask = (1u << gapBits) - 1u;
    std::vector<uint32_t> docids(N_BLOCKS * B);
    uint32_t cur = 0;
    for (uint32_t& d : docids) {
      cur += (uint32_t)(rng.rint(gapMask) & gapMask) + 1u;  // strictly increasing
      d = cur;
    }

    SoluxPFORd pfor;
    SoluxSIMDFor forCodec;
    encoded.assign((size_t)N_BLOCKS * B * 5 + 1024, 0);
    forEncoded.assign((size_t)N_BLOCKS * B * 5 + 1024, 0);
    blockOffset.resize(N_BLOCKS);
    forOffset.resize(N_BLOCKS);
    uint32_t eoff = 0, foff = 0, base = 0;
    for (uint32_t b = 0; b < N_BLOCKS; b++) {
      const uint32_t* blkStart = docids.data() + b * B;

      // Gap residuals for the FOR unpack proxy: adjacent delta, first off base.
      std::vector<uint32_t> deltas(blkStart, blkStart + B);
      for (uint32_t i = B - 1; i > 0; i--) deltas[i] -= deltas[i - 1];
      deltas[0] -= base;
      uint8_t bits = 0;
      for (uint32_t v : deltas) bits = std::max(bits, (uint8_t)(32 - __builtin_clz(v | 1)));
      forOffset[b] = foff;
      forBits.push_back(bits);
      uint32_t fsz = forEncoded.size() - foff;
      forCodec.encodeWithMeta(deltas.data(), B, forEncoded.data() + foff, fsz, 0, bits);
      foff += fsz;

      // Real docs codec (does its own delta internally, so feed raw docids).
      std::vector<uint32_t> blk(blkStart, blkStart + B);
      blockOffset[b] = eoff;
      uint32_t esz = encoded.size() - eoff;
      pfor.encodeBlock(blk.data(), B, encoded.data() + eoff, esz, base);
      eoff += esz;
      base = docids[(b + 1) * B - 1];
    }
    encoded.resize(eoff + 64);
    forEncoded.resize(foff + 64);
  }
};

static void BM_postings_end2end(benchmark::State& state, uint8_t gapBits) {
  PostingsList list(gapBits);
  SoluxPFORd pfor;
  uint32_t out[B];
  for (auto _ : state) {
    uint32_t base = 0;
    for (uint32_t b = 0; b < N_BLOCKS; b++) {
      uint32_t outSz = B;
      pfor.decodeBlock(list.encoded.data() + list.blockOffset[b], 1u << 20, out, outSz, base);
      base = out[B - 1];
      benchmark::DoNotOptimize(out);
    }
  }
  state.SetItemsProcessed((int64_t)state.iterations() * N_BLOCKS * B);
}

static void BM_postings_unpack(benchmark::State& state, uint8_t gapBits) {
  PostingsList list(gapBits);
  SoluxSIMDFor forCodec;
  uint32_t out[B];
  for (auto _ : state) {
    for (uint32_t b = 0; b < N_BLOCKS; b++) {
      forCodec.decodeSingleBlock(list.forEncoded.data() + list.forOffset[b], out, B, list.forBits[b]);
      benchmark::DoNotOptimize(out);
    }
  }
  state.SetItemsProcessed((int64_t)state.iterations() * N_BLOCKS * B);
}

#define REG(g)                                                       \
  TUNING_BENCHMARK_CAPTURE(BM_postings_end2end, g/bit, (uint8_t)g)   \
      ->UseRealTime();                                                \
  TUNING_BENCHMARK_CAPTURE(BM_postings_unpack, g/bit, (uint8_t)g)    \
      ->UseRealTime();

REG(9) REG(13) REG(17)

}  // namespace solux
