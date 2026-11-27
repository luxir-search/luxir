// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/codec/Codec.h"
#include "luxir/codec/PackedTail.h"
#include "luxir/codec/StreamVByte.h"
#include "luxir/reader/Postings.h"
#include "luxir/util/random.h"
#include <gtest/gtest.h>

// Where does postings doc-decode spend its time? Each block is unpacked (the
// FastPFOR 4-interleaved-lane bit layout, uniform shifts) then prefix-summed to
// undo the doc-gap delta. AVX-512 can widen the unpack, but the D1 inverse-delta
// carries a serial running-count vector-to-vector, so wider registers cannot
// speed it up. This bench measures the split end-to-end over a multi-block list
// so the two phases overlap across blocks (out-of-order), the realistic cost:
//   end2end - LuxirPFORd::decodeBlock (unpack + inverse-delta), the real path
//   unpack  - LuxirSIMDFor unpack of the same-width delta residuals, no delta
// The gap end2end - unpack is the in-situ prefix-sum cost (the width-immune part
// a wider unpack cannot move). Deps/FastPFOR headers are confined to Codec.cpp,
// so we reach the codecs through their public APIs only; the pure-FOR unpack is
// a proxy for the PFor unpack (equal when gaps are uniform, no exceptions).

namespace luxir {

static constexpr uint32_t B = Postings::DOCS_BLOCK_SIZE;  // 128
static constexpr uint32_t N_BLOCKS = 256;                 // 32K docs, encoded fits L2

struct PostingsList {
  std::vector<char> encoded;                // LuxirPFORd blocks, contiguous
  std::vector<uint32_t> blockOffset;
  std::vector<char> forEncoded;             // LuxirSIMDFor of the gap residuals
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

    LuxirPFORd pfor;
    LuxirSIMDFor forCodec;
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
  LuxirPFORd pfor;
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
  LuxirSIMDFor forCodec;
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

// Partial last block (a term's tail, under DOCS_BLOCK_SIZE docs): the
// StreamVByte d1 layout the tails used before vs the bit-packed PackedTail
// layout (gap - 1, scalar unpack) they use now, docs and tfs. Many short
// lists back to back, as a low-df term mix reads them.
static constexpr uint32_t TAIL_DOCS = 100;
static constexpr uint32_t N_TAILS = 512;

struct TailLists {
  std::vector<uint8_t> svb;
  std::vector<uint32_t> svbOffset;
  std::vector<uint8_t> packed;
  std::vector<uint32_t> packedOffset;

  explicit TailLists(uint8_t gapBits) {
    Rng rng(1);
    uint32_t gapMask = (1u << gapBits) - 1u;
    svb.assign((size_t) N_TAILS * TAIL_DOCS * 10 + 64, 0);
    packed.assign((size_t) N_TAILS * PackedTail::MAX_BYTES + 64, 0);
    uint32_t svbOff = 0;
    uint32_t packedOff = 0;
    for (uint32_t t = 0; t < N_TAILS; t++) {
      int32_t docs[TAIL_DOCS];
      int32_t tfs[TAIL_DOCS];
      uint32_t cur = 0;
      for (uint32_t i = 0; i < TAIL_DOCS; i++) {
        cur += (uint32_t) (rng.rint(gapMask) & gapMask) + 1u;
        docs[i] = (int32_t) cur;
        tfs[i] = 1 + (int32_t) (rng.rint(8u) == 0 ? rng.rint(4u) : 0);
      }
      svbOffset.push_back(svbOff);
      uint8_t* keys = svb.data() + svbOff;
      uint32_t kb = svbKeyBytes(TAIL_DOCS);
      uint8_t* end = svb_encode_scalar_d1_init((const uint32_t*) docs, keys, keys + kb,
                                               TAIL_DOCS, 0);
      uint8_t* tfKeys = end;
      end = svb_encode_scalar((const uint32_t*) tfs, tfKeys, tfKeys + kb, TAIL_DOCS);
      svbOff = (uint32_t) (end - svb.data());
      packedOffset.push_back(packedOff);
      packedOff += PackedTail::encode(docs, tfs, TAIL_DOCS, UINT32_MAX,
                                      packed.data() + packedOff);
    }
    svb.resize(svbOff + 64);
    packed.resize(packedOff + 64);
  }
};

static void BM_tail_svb(benchmark::State& state, uint8_t gapBits) {
  TailLists lists(gapBits);
  uint32_t docs[Postings::DOCS_BLOCK_SIZE];
  uint32_t tfs[Postings::DOCS_BLOCK_SIZE];
  for (auto _ : state) {
    for (uint32_t t = 0; t < N_TAILS; t++) {
      uint8_t* p = lists.svb.data() + lists.svbOffset[t];
      uint32_t kb = svbKeyBytes(TAIL_DOCS);
      uint8_t* dataEnd = svb_decode_avx_d1_init(docs, p, p + kb, TAIL_DOCS, 0);
      svb_decode_avx_simple(tfs, dataEnd, dataEnd + kb, TAIL_DOCS);
      benchmark::DoNotOptimize(docs);
      benchmark::DoNotOptimize(tfs);
    }
  }
  state.SetItemsProcessed((int64_t) state.iterations() * N_TAILS * TAIL_DOCS);
}

static void BM_tail_packed(benchmark::State& state, uint8_t gapBits) {
  TailLists lists(gapBits);
  int32_t docs[Postings::DOCS_BLOCK_SIZE];
  int32_t tfs[Postings::DOCS_BLOCK_SIZE];
  for (auto _ : state) {
    for (uint32_t t = 0; t < N_TAILS; t++) {
      const uint8_t* p = lists.packed.data() + lists.packedOffset[t];
      PackedTail::Header header = PackedTail::readHeader(p);
      PackedTail::decodeDocs(p, header, TAIL_DOCS, UINT32_MAX, docs);
      PackedTail::decodeTfs(p, header, TAIL_DOCS, tfs);
      benchmark::DoNotOptimize(docs);
      benchmark::DoNotOptimize(tfs);
    }
  }
  state.SetItemsProcessed((int64_t) state.iterations() * N_TAILS * TAIL_DOCS);
}

#define REG_TAIL(g)                                                  \
  TUNING_BENCHMARK_CAPTURE(BM_tail_svb, g/bit, (uint8_t)g)           \
      ->UseRealTime();                                                \
  TUNING_BENCHMARK_CAPTURE(BM_tail_packed, g/bit, (uint8_t)g)        \
      ->UseRealTime();

REG_TAIL(4) REG_TAIL(9) REG_TAIL(13)

}  // namespace luxir
