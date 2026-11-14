// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// FAISS's SIMD RaBitQ popcount kernels (NEON on aarch64, AVX2/AVX-512 on
// x86) score 1-bit codes. Their sums are integers, so each must equal the
// byte-wise scalar counts exactly.

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <random>
#include <vector>

#include <faiss/utils/rabitq_simd.h>
#include <faiss/utils/simd_levels.h>

// FAISS defines these in its per-ISA sources without declaring them.
namespace faiss::rabitq {
#if defined(__aarch64__)
template <> uint64_t bitwise_and_dot_product<SIMDLevel::ARM_NEON>(const uint8_t*, const uint8_t*, size_t, size_t);
template <> uint64_t bitwise_xor_dot_product<SIMDLevel::ARM_NEON>(const uint8_t*, const uint8_t*, size_t, size_t);
template <> uint64_t popcount<SIMDLevel::ARM_NEON>(const uint8_t*, size_t);
#elif defined(__x86_64__)
template <> uint64_t bitwise_and_dot_product<SIMDLevel::AVX2>(const uint8_t*, const uint8_t*, size_t, size_t);
template <> uint64_t bitwise_xor_dot_product<SIMDLevel::AVX2>(const uint8_t*, const uint8_t*, size_t, size_t);
template <> uint64_t popcount<SIMDLevel::AVX2>(const uint8_t*, size_t);
template <> uint64_t bitwise_and_dot_product<SIMDLevel::AVX512>(const uint8_t*, const uint8_t*, size_t, size_t);
template <> uint64_t bitwise_xor_dot_product<SIMDLevel::AVX512>(const uint8_t*, const uint8_t*, size_t, size_t);
template <> uint64_t popcount<SIMDLevel::AVX512>(const uint8_t*, size_t);
#endif
}  // namespace faiss::rabitq

namespace {

using faiss::SIMDLevel;

struct Kernels {
  SIMDLevel level;
  uint64_t (*andDot)(const uint8_t*, const uint8_t*, size_t, size_t);
  uint64_t (*xorDot)(const uint8_t*, const uint8_t*, size_t, size_t);
  uint64_t (*popcount)(const uint8_t*, size_t);
};

template <SIMDLevel SL>
Kernels kernels() {
  return {SL, &faiss::rabitq::bitwise_and_dot_product<SL>, &faiss::rabitq::bitwise_xor_dot_product<SL>,
          &faiss::rabitq::popcount<SL>};
}

std::vector<Kernels> simdKernels() {
  std::vector<Kernels> out;
#if defined(__aarch64__)
  out.push_back(kernels<SIMDLevel::ARM_NEON>());
#elif defined(__x86_64__)
  out.push_back(kernels<SIMDLevel::AVX2>());
  out.push_back(kernels<SIMDLevel::AVX512>());
#endif
  std::erase_if(out, [](const Kernels& k) { return !faiss::SIMDConfig::is_simd_level_available(k.level); });
  return out;
}

}  // namespace

TEST(RaBitQKernelTest, simdPopcountsMatchScalar) {
  auto levels = simdKernels();
  if (levels.empty()) GTEST_SKIP() << "no SIMD RaBitQ kernel for this CPU";
  std::mt19937_64 rng(17);
  std::vector<size_t> bytes = {0, 1, 7, 8, 9, 12, 15, 16, 17, 24, 25, 31, 32, 33, 48, 63, 64, 65, 96, 128, 192, 513};
  for (int i = 0; i < 40; i++) bytes.push_back(1 + rng() % 600);
  // Around one and two 2048-chunk NEON lane-reduction blocks.
  bytes.insert(bytes.end(), {32767, 32768, 32769, 65536, 65537});
  for (size_t size : bytes) {
    for (size_t qb = 1; qb <= 8; qb++) {
      // Random, all ones (maximum AND/popcount), and complementary (maximum XOR).
      for (int pattern = 0; pattern < 3; pattern++) {
        std::vector<uint8_t> query(size * qb + 1), data(size + 1);
        for (auto& b : query) b = pattern == 0 ? (uint8_t)rng() : 0xff;
        for (auto& b : data) b = pattern == 0 ? (uint8_t)rng() : pattern == 1 ? 0xff : 0;
        const uint8_t* q = query.data() + 1;
        const uint8_t* d = data.data() + 1; // Unaligned, with no trailing padding.
        uint64_t andDot = 0, xorDot = 0, bits = 0;
        for (size_t i = 0; i < size; i++) {
          bits += std::popcount((unsigned)d[i]);
          for (size_t j = 0; j < qb; j++) {
            andDot += (uint64_t)std::popcount((unsigned)(q[j * size + i] & d[i])) << j;
            xorDot += (uint64_t)std::popcount((unsigned)(q[j * size + i] ^ d[i])) << j;
          }
        }
        for (const auto& k : levels) {
          SCOPED_TRACE(faiss::to_string(k.level) + " bytes=" + std::to_string(size) + " qb=" + std::to_string(qb)
                       + " pattern=" + std::to_string(pattern));
          ASSERT_EQ(k.andDot(q, d, size, qb), andDot);
          ASSERT_EQ(k.xorDot(q, d, size, qb), xorDot);
          ASSERT_EQ(k.popcount(d, size), bits);
        }
      }
    }
  }
}
