#include "solux/codec/Codec.h"  // SoluxPFOR, SoluxPFORd, SoluxSIMDFor
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include <algorithm>
#include <cstring>
#include <vector>

using namespace solux;

namespace {

constexpr uint32_t BLK = 128;  // PFor codecs always encode one BLOCK_SIZE block.

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

class PForTest : public SoluxTest {};

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
// same base.  Mirrors how PostingsWriter/DocsEnum carry the base across blocks.
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
