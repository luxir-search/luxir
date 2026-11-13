// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// FAISS's SIMD RaBitQ popcount kernels (NEON on aarch64, AVX2/AVX-512 on
// x86) score 1-bit codes. Their sums are integers, so each must equal the
// scalar kernel's exactly.

#include <gtest/gtest.h>

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
  std::vector<size_t> bytes = {1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 96, 128, 192, 513};
  for (int i = 0; i < 40; i++) bytes.push_back(1 + rng() % 600);
  // Around 2048 16-byte chunks, where the NEON kernel reduces its lane sums.
  bytes.insert(bytes.end(), {32767, 32768, 32784, 40000});
  for (size_t size : bytes) {
    for (size_t qb = 1; qb <= 8; qb++) {
      // Random bits, then every bit set: the largest per-lane sums.
      for (bool ones : {false, true}) {
        std::vector<uint8_t> query(size * qb), data(size);
        for (auto& b : query) b = ones ? 0xff : (uint8_t)rng();
        for (auto& b : data) b = ones ? 0xff : (uint8_t)rng();
        uint64_t andDot = faiss::rabitq::bitwise_and_dot_product<SIMDLevel::NONE>(query.data(), data.data(), size, qb);
        uint64_t xorDot = faiss::rabitq::bitwise_xor_dot_product<SIMDLevel::NONE>(query.data(), data.data(), size, qb);
        uint64_t bits = faiss::rabitq::popcount<SIMDLevel::NONE>(data.data(), size);
        for (const auto& k : levels) {
          SCOPED_TRACE(faiss::to_string(k.level) + " bytes=" + std::to_string(size) + " qb=" + std::to_string(qb));
          ASSERT_EQ(k.andDot(query.data(), data.data(), size, qb), andDot);
          ASSERT_EQ(k.xorDot(query.data(), data.data(), size, qb), xorDot);
          ASSERT_EQ(k.popcount(data.data(), size), bits);
        }
      }
    }
  }
}
