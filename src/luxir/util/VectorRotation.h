// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace luxir {

// Seeded orthogonal transform over exactly dims coordinates: the FHT-Kac
// rotator of the RaBitQ authors' library, made exactly orthogonal for any
// dims. O(dims log dims) per vector. Parameter-only disk format: changing the
// algorithm or sign stream needs a new kind. SplitMix64 words are consumed low bit first, continuously across
// rounds; a set bit negates the coordinate.
class VectorRotation {
public:
  static constexpr int32_t FHT_KAC = 1;
  static constexpr int ROUNDS = 4;
  const int32_t kind;
  const int32_t dims;
  const uint64_t seed;
private:
  int block;
  std::vector<float> signs;
public:
  VectorRotation(int32_t kind, int32_t dims, uint64_t seed)
      : kind(kind), dims(dims), seed(seed) {
    if (kind != FHT_KAC || dims <= 0) throw std::runtime_error("Invalid vector rotation");
    block = (int)std::bit_floor((uint32_t)dims);
    signs.resize((size_t)dims * ROUNDS);
    uint64_t state = seed, word = 0;
    for (size_t i = 0; i < signs.size(); ++i) {
      if (i % 64 == 0) {
        uint64_t z = (state += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        word = z ^ (z >> 31);
      }
      signs[i] = (word & 1) ? -1.0f : 1.0f;
      word >>= 1;
    }
  }

  void apply(const float* x, float* y) const {
    if (x != y) std::copy_n(x, dims, y);
    const float scale = 1.0f / std::sqrt((float)block);
    const float sqrtHalf = std::sqrt(0.5f);
    for (int r = 0; r < ROUNDS; ++r) {
      const float* s = signs.data() + (size_t)r * dims;
      for (int i = 0; i < dims; ++i) y[i] *= s[i];
      float* b = y + (r % 2 ? dims - block : 0);
      for (int stride = 1; stride < block; stride *= 2) {
        for (int base = 0; base < block; base += 2 * stride) {
          for (int j = 0; j < stride; ++j) {
            float a = b[base + j], c = b[base + j + stride];
            b[base + j] = a + c;
            b[base + j + stride] = a - c;
          }
        }
      }
      for (int i = 0; i < block; ++i) b[i] *= scale;
      if (block < dims) {
        int h = dims / 2;
        for (int i = 0; i < h; ++i) {
          float a = y[i], c = y[i + dims - h];
          y[i] = (a + c) * sqrtHalf;
          y[i + dims - h] = (a - c) * sqrtHalf;
        }
      }
    }
  }

  void applyBatch(int64_t n, const float* x, float* y) const {
    for (int64_t i = 0; i < n; ++i) apply(x + i * dims, y + i * dims);
  }
};

} // namespace luxir
