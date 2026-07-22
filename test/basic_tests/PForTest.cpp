#include "solux/codec/Codec.h"  // SoluxPFOR, SoluxPFORd, SoluxSIMDFor
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "FastPFOR/headers/bitpackinghelpers.h"
#include "FastPFOR/headers/simdbitpacking.h"
#include "FastPFOR/headers/usimdbitpacking.h"
#include "FastPFOR/headers/util.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

using namespace solux;

namespace {

constexpr uint32_t BLK = 128;  // PFor codecs always encode one BLOCK_SIZE block.

class ScalarReferenceBitPacker {
  uint16_t sizes[32]{};
  uint32_t data[32][BLK]{};

public:
  void clear() {
    for (uint32_t i = 0; i < 32; i++) sizes[i] = 0;
  }

  void directAppend(uint32_t lane, uint32_t value) {
    data[lane][sizes[lane]++] = value;
  }

  uint32_t* writeLane(uint32_t* out, uint32_t lane) {
    uint32_t j = 0;
    for (; j + 128 <= sizes[lane]; j += 128) {
      FastPForLib::usimdpackwithoutmask(
          &data[lane][j], (__m128i*) out, lane + 1);
      out += 4 * (lane + 1);
    }
    for (; j < sizes[lane]; j += 32) {
      FastPForLib::fastpackwithoutmask(&data[lane][j], out, lane + 1);
      out += lane + 1;
    }
    out -= (j - sizes[lane]) * (lane + 1) / 32;
    return out;
  }
};

// Frozen copy of the scalar selection used before the AVX2 path. Keeping the
// cost walk unchanged makes ties part of the byte-identity oracle.
void getBestBFromDataScalar(const uint32_t* in, uint8_t& bestb,
                            uint8_t& bestcexcept, uint8_t& maxb) {
  constexpr uint32_t overheadofeachexcept = 8;
  uint32_t freqs[33];
  for (uint32_t k = 0; k <= 32; k++) freqs[k] = 0;
  for (uint32_t k = 0; k < BLK; k++) {
    freqs[FastPForLib::gccbits(in[k])]++;
  }
  bestb = 32;
  while (freqs[bestb] == 0) bestb--;
  maxb = bestb;
  uint32_t bestcost = bestb * BLK;
  uint32_t cexcept = 0;
  bestcexcept = (uint8_t) cexcept;
  for (uint32_t b = bestb - 1; b < 32; --b) {
    cexcept += freqs[b + 1];
    uint32_t thiscost = cexcept * overheadofeachexcept
        + cexcept * (maxb - b) + b * BLK + 8;
    if (maxb - b == 1) thiscost -= cexcept;
    if (thiscost < bestcost) {
      bestcost = thiscost;
      bestb = (uint8_t) b;
      bestcexcept = (uint8_t) cexcept;
    }
  }
}

// Full pre-vectorization encoder reference, including the scalar exception
// scan. The bit-packing calls and layout are intentionally the production ones.
void encodeBlockPForScalar(uint32_t* in, char* outc, uint32_t& outSz) {
  uint8_t bestb;
  uint8_t bestcexcept;
  uint8_t maxb = 0;
  getBestBFromDataScalar(in, bestb, bestcexcept, maxb);

  uint8_t* bc = (uint8_t*) outc;
  *bc++ = bestb;
  *bc++ = bestcexcept;

  ScalarReferenceBitPacker bpacker;
  if (bestcexcept > 0) {
    *bc++ = maxb;
    bpacker.clear();
    const uint32_t maxval = 1U << bestb;
    for (uint32_t k = 0; k < BLK; k++) {
      if (in[k] >= maxval) {
        bpacker.directAppend(maxb - bestb - 1, in[k] >> bestb);
        *bc++ = (uint8_t) k;
      }
    }
  }
  while (((char*) bc - outc) % sizeof(uint32_t) != 0) *bc++ = 0;
  uint32_t* out = (uint32_t*) bc;
  FastPForLib::simdpack(in, (__m128i*) out, bestb);
  out += 4 * bestb;
  if (bestcexcept > 0 && (uint32_t) (maxb - bestb) > 1) {
    out = bpacker.writeLane(out, maxb - bestb - 1);
  }
  outSz = (uint32_t) ((char*) out - outc);
}

uint32_t valueWithWidth(uint32_t width, uint32_t lowBits) {
  if (width == 0) return 0;
  if (width == 32) return 0x80000000u | (lowBits & 0x7fffffffu);
  uint32_t highBit = 1U << (width - 1);
  return highBit | (lowBits & (highBit - 1));
}

// Round-trip one 128-value block through a U32Codec and return the decoded
// values. `in` is taken by value because the delta codecs mutate it in place.
std::vector<uint32_t> roundtrip(U32Codec& codec, std::vector<uint32_t> in) {
  std::vector<char> enc(BLK * sizeof(uint32_t) * 2 + 1024);
  uint32_t encSz = enc.size();
  codec.encodeBlock(in.data(), in.size(), enc.data(), encSz);

  // Decode from a freshly-sized buffer (+slack) so memory checkers catch any
  // SIMD over-read past the encoded bytes.
  std::vector<char> buf(encSz + 64, 0);
  memcpy(buf.data(), enc.data(), encSz);
  std::vector<uint32_t> out(BLK, 0xdeadbeef);
  uint32_t outSz = BLK;
  codec.decodeBlock(buf.data(), encSz, out.data(), outSz);
  return out;
}

}  // namespace

class PForTest : public SoluxTest {
protected:
  void assertScalarEncoding(const std::array<uint32_t, BLK>& block,
                            std::string_view shape) {
    SCOPED_TRACE(shape);
    std::array<uint32_t, BLK> actualInput = block;
    std::array<uint32_t, BLK> scalarInput = block;
    alignas(32) std::array<char, BLK * sizeof(uint32_t) * 2 + 1024> actual{};
    alignas(32) std::array<char, BLK * sizeof(uint32_t) * 2 + 1024> scalar{};
    uint32_t actualSize = (uint32_t) actual.size();
    uint32_t scalarSize = (uint32_t) scalar.size();

    SoluxPFOR codec;
    codec.encodeBlock(actualInput.data(), BLK, actual.data(), actualSize);
    encodeBlockPForScalar(scalarInput.data(), scalar.data(), scalarSize);

    ASSERT_EQ(scalarSize, actualSize);

    // fastpackwithoutmask reads a complete 32-value residual group. For a
    // partial final word, the old encoder therefore leaves its unused high
    // bits dependent on uninitialized padding slots. Preserve that production
    // behavior and normalize only those undefined bits for the byte oracle.
    uint8_t bestb = (uint8_t) scalar[0];
    uint8_t exceptions = (uint8_t) scalar[1];
    if (exceptions > 0) {
      uint8_t maxb = (uint8_t) scalar[2];
      uint32_t residualWidth = (uint32_t) maxb - bestb;
      uint32_t validLastBits = ((uint32_t) exceptions * residualWidth) & 31;
      if (residualWidth > 1 && validLastBits != 0) {
        uint32_t headerBytes = 3 + exceptions;
        uint32_t headerWords = (headerBytes + sizeof(uint32_t) - 1) / sizeof(uint32_t);
        uint32_t* scalarResiduals = (uint32_t*) scalar.data() + headerWords + 4 * bestb;
        const uint32_t* actualResiduals = (const uint32_t*) actual.data()
            + headerWords + 4 * bestb;
        uint32_t residualWords = ((uint32_t) exceptions * residualWidth + 31) / 32;
        uint32_t validMask = (1U << validLastBits) - 1;
        uint32_t last = residualWords - 1;
        scalarResiduals[last] = (scalarResiduals[last] & validMask)
            | (actualResiduals[last] & ~validMask);
      }
    }
    ASSERT_EQ(0, memcmp(scalar.data(), actual.data(), actualSize));
  }
};

// Non-delta PForDelta: round-trips across value distributions that exercise the
// exception path, all-zero (bestb==0), and full 32-bit values.
TEST_F(PForTest, pforRoundTrip) {
  SoluxPFOR fp;

  for (int trial = 0; trial < 300; ++trial) {
    std::vector<uint32_t> data(BLK);
    switch (trial % 6) {
      case 0:  // all zeros (bestb == 0)
        std::fill(data.begin(), data.end(), 0);
        break;
      case 1:  // all equal, nonzero
        std::fill(data.begin(), data.end(), 123456789u);
        break;
      case 2:  // small values, no exceptions
        for (auto& v : data) v = rng() & 0xf;
        break;
      case 3: {  // mostly small with a few large outliers (exceptions)
        for (auto& v : data) v = rng() & 0x7;
        for (int e = 0; e < 5; ++e) data[rng() % BLK] = (rng() & 0xffff) | (1u << 20);
        break;
      }
      case 4:  // full 32-bit range
        for (auto& v : data) v = rng();
        break;
      default:  // moderate range
        for (auto& v : data) v = rng() % 5000;
        break;
    }

    ASSERT_EQ(roundtrip(fp, data), data) << "trial " << trial;
  }
}

TEST_F(PForTest, vectorizedEncoderMatchesScalarBytes) {
  std::array<uint32_t, BLK> block{};
  assertScalarEncoding(block, "all zero");

  for (uint32_t width = 0; width <= 32; width++) {
    for (uint32_t k = 0; k < BLK; k++) {
      block[k] = valueWithWidth(width, k * 0x9e3779b9u);
    }
    SCOPED_TRACE(width);
    assertScalarEncoding(block, "same width");
  }

  for (uint32_t width = 0; width < 32; width++) {
    for (uint32_t k = 0; k < BLK; k++) {
      block[k] = valueWithWidth(width, k);
    }
    block[(width * 17) % BLK] = valueWithWidth(32, width * 0x01010101u);
    SCOPED_TRACE(width);
    assertScalarEncoding(block, "single wide outlier");
  }

  for (uint32_t k = 0; k < BLK; k++) {
    block[k] = valueWithWidth(k % 33, k * 0x85ebca6bu);
  }
  assertScalarEncoding(block, "alternating widths");

  for (uint32_t k = 0; k < BLK; k++) {
    block[k] = valueWithWidth(2, k);
  }
  for (uint32_t k = 0; k < BLK; k += 4) {
    block[k] = valueWithWidth(12, k * 13);
  }
  {
    uint8_t bestb;
    uint8_t exceptions;
    uint8_t maxb;
    getBestBFromDataScalar(block.data(), bestb, exceptions, maxb);
    ASSERT_EQ(2, bestb);
    ASSERT_EQ(32, exceptions);
    ASSERT_EQ(12, maxb);
  }
  assertScalarEncoding(block, "dense exceptions");

  for (uint32_t exponent = 0; exponent < 32; exponent++) {
    uint32_t power = 1U << exponent;
    uint32_t around[3] = {power - 1, power, power + 1};
    for (uint32_t k = 0; k < BLK; k++) block[k] = around[k % 3];
    SCOPED_TRACE(exponent);
    assertScalarEncoding(block, "power boundary");
  }

  constexpr std::array<uint32_t, 8> edges = {
      0, 1, 0x7ffffffeu, 0x7fffffffu,
      0x80000000u, 0x80000001u, 0xfffffffeu, 0xffffffffu};
  for (uint32_t k = 0; k < BLK; k++) block[k] = edges[k % edges.size()];
  assertScalarEncoding(block, "unsigned edges");

  block.fill(0);
  block[BLK - 1] = 1;
  {
    uint8_t bestb;
    uint8_t exceptions;
    uint8_t maxb;
    getBestBFromDataScalar(block.data(), bestb, exceptions, maxb);
    ASSERT_EQ(0, bestb);
    ASSERT_EQ(1, exceptions);
    ASSERT_EQ(1, maxb);
  }
  assertScalarEncoding(block, "width one residual");

  for (uint32_t trial = 0; trial < 5000; trial++) {
    uint32_t baseWidth = (uint32_t) (rng() % 33);
    uint32_t secondWidth = baseWidth + (uint32_t) (rng() % (33 - baseWidth));
    for (uint32_t k = 0; k < BLK; k++) {
      uint32_t width;
      switch (trial % 5) {
        case 0:
          width = (uint32_t) (rng() % 33);
          break;
        case 1:
          width = (rng() % 16 == 0) ? secondWidth : baseWidth;
          break;
        case 2:
          width = (k & 1) == 0 ? baseWidth : secondWidth;
          break;
        case 3:
          block[k] = (uint32_t) rng();
          continue;
        default: {
          uint32_t spread = std::min(32u, baseWidth + 3);
          uint32_t low = baseWidth > 3 ? baseWidth - 3 : 0;
          width = low + (uint32_t) (rng() % (spread - low + 1));
          break;
        }
      }
      block[k] = valueWithWidth(width, (uint32_t) rng());
    }
    SCOPED_TRACE(trial);
    assertScalarEncoding(block, "mixed random widths");
  }
}

// Delta-coded PForDelta (docs codec): monotonic inputs, like document ids.
TEST_F(PForTest, pfordRoundTrip) {
  SoluxPFORd fp;

  for (int trial = 0; trial < 300; ++trial) {
    std::vector<uint32_t> data(BLK);
    uint32_t acc = trial % 3 == 0 ? 0 : rng() % 1000;  // sometimes start at 0
    uint32_t maxGap = (trial % 4) + 1;                 // dense .. sparse gaps
    if (trial % 7 == 0) maxGap = 100000;               // occasional big jumps
    for (auto& v : data) {
      acc += rng() % maxGap;  // gap can be 0 (repeated doc ids allowed)
      v = acc;
    }

    ASSERT_EQ(roundtrip(fp, data), data) << "trial " << trial;
  }
}

// Delta-coded docs with a cross-block base: a block whose first id is coded as a
// delta from the previous block's last id must round-trip when decoded with the
// same base. Mirrors how the postings writer and reader carry it across blocks.
TEST_F(PForTest, pfordBaseCarry) {
  SoluxPFORd fp;

  for (int trial = 0; trial < 200; ++trial) {
    uint32_t base = rng() % 1000000;            // previous block's last id
    uint32_t maxGap = (trial % 4) + 1;          // dense .. sparse gaps
    if (trial % 7 == 0) maxGap = 100000;        // occasional big jumps
    std::vector<uint32_t> data(BLK);
    uint32_t acc = base;
    for (auto& v : data) { acc += 1 + rng() % maxGap; v = acc; }  // strictly increasing, all > base

    std::vector<uint32_t> in = data;            // encodeBlock mutates in place
    std::vector<char> enc(BLK * sizeof(uint32_t) * 2 + 1024);
    uint32_t encSz = enc.size();
    fp.encodeBlock(in.data(), in.size(), enc.data(), encSz, base);

    std::vector<char> buf(encSz + 64, 0);       // exact-sized (+slack) so over-reads are caught
    memcpy(buf.data(), enc.data(), encSz);
    std::vector<uint32_t> out(BLK, 0xdeadbeef);
    uint32_t outSz = BLK;
    fp.decodeBlock(buf.data(), encSz, out.data(), outSz, base);
    ASSERT_EQ(out, data) << "trial " << trial << " base " << base;
  }
}
