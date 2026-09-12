// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <span>
#include <string_view>
#include <gtest/gtest.h>
#include "luxir/util/luxir_util.h"

namespace {

// Keep both runtime-length and scalar calls: GCC can specialize the latter
// while leaving xxHash's loads outside the caller's inlining boundary.
[[gnu::noinline]] uint64_t hashBytes(const void* data, size_t size, uint64_t seed) {
  return luxir::Hash::hash(data, size, seed);
}

template<typename T>
uint64_t mixScalar(uint64_t seed, T value) {
  return hashBytes(&value, sizeof(value), seed);
}

[[gnu::noinline]] uint64_t hashScalars(std::string_view field, int64_t lo, int64_t hi,
                                      std::span<const int64_t> values, uint64_t seed) {
  seed = hashBytes(field.data(), field.size(), seed);
  seed = mixScalar(seed, lo);
  seed = mixScalar(seed, hi);
  return mixScalar(seed, values.size());
}

[[gnu::noinline]] uint64_t hashCopiedBytes(std::string_view field, int64_t lo, int64_t hi,
                                         std::span<const int64_t> values, uint64_t seed) {
  seed = hashBytes(field.data(), field.size(), seed);
  unsigned char bytes[sizeof(int64_t)];
  std::memcpy(bytes, &lo, sizeof(lo));
  seed = hashBytes(bytes, sizeof(lo), seed);
  std::memcpy(bytes, &hi, sizeof(hi));
  seed = hashBytes(bytes, sizeof(hi), seed);
  auto size = values.size();
  unsigned char sizeBytes[sizeof(size)];
  std::memcpy(sizeBytes, &size, sizeof(size));
  return hashBytes(sizeBytes, sizeof(size), seed);
}

} // namespace

TEST(HashTest, scalarInputsMatchByteRepresentation) {
  int64_t values[] = {1, 3, 5};
  for (uint64_t seed : {1, 7, 31}) {
    EXPECT_EQ(hashCopiedBytes("field", 12 + seed, 37 + seed, values, seed),
              hashScalars("field", 12 + seed, 37 + seed, values, seed));
  }
}
