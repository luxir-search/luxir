// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_set.hpp>

namespace luxir {

// Distinct-count sketch over 64-bit hashes, mergeable across segments.
//
// Below precisionThreshold distinct hashes the sketch keeps the hashes
// themselves, so the count is exact up to 64-bit hash collisions. Past the
// threshold it converts to 2^p one-byte HyperLogLog registers, with p chosen
// from the threshold (12 for the default 3000, at most 18). The dense
// estimate uses Ertl's improved raw estimator ("New cardinality estimation
// algorithms for HyperLogLog sketches", 2017), which is unbiased over the
// whole range without the empirical bias tables of HLL++ and has a relative
// standard error of about 1.04 / sqrt(2^p): 1.6% at p = 12.
class HyperLogLog {
public:
  using ExactSet = boost::unordered_flat_set<
      uint64_t, boost::hash<uint64_t>, std::equal_to<uint64_t>,
      std::pmr::polymorphic_allocator<uint64_t>>;
  static constexpr int64_t DEFAULT_PRECISION_THRESHOLD = 3000;
  static constexpr int64_t MAX_PRECISION_THRESHOLD = 40000;
  static constexpr int MIN_PRECISION = 4;
  static constexpr int MAX_PRECISION = 18;

  int64_t threshold;
  int precision;
  ExactSet exact;
  std::pmr::vector<uint8_t> registers; // nonempty once dense

  // Enough registers that the dense estimate at the threshold is about as
  // accurate as a hash table of that many entries at 0.75 load would allow.
  static int precisionForThreshold(int64_t threshold) {
    double entries = std::ceil((double)std::max<int64_t>(threshold, 1) / 0.75);
    int p = (int)std::ceil(std::log2(entries));
    return std::clamp(p, MIN_PRECISION, MAX_PRECISION);
  }

  // A 64-bit finalizer (MurmurHash3 fmix64). It is a bijection, so distinct
  // integers keep distinct hashes and the exact phase stays exact.
  static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
  }

  explicit HyperLogLog(int64_t threshold = DEFAULT_PRECISION_THRESHOLD,
                       std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : threshold(threshold), precision(precisionForThreshold(threshold)),
        exact(resource), registers(resource) {
    assert(threshold >= 0 && threshold <= MAX_PRECISION_THRESHOLD);
  }

  bool dense() const { return !registers.empty(); }

  void add(uint64_t hash) {
    if (dense()) {
      addDense(hash);
      return;
    }
    exact.insert(hash);
    if ((int64_t)exact.size() > threshold) toDense();
  }

  void merge(const HyperLogLog& other) {
    assert(precision == other.precision);
    if (other.dense()) {
      if (!dense()) toDense();
      for (size_t i = 0; i < registers.size(); i++) {
        registers[i] = std::max(registers[i], other.registers[i]);
      }
      return;
    }
    for (uint64_t hash : other.exact) add(hash);
  }

  int64_t estimate() const {
    if (!dense()) return (int64_t)exact.size();
    const int q = 64 - precision;
    const double m = (double)registers.size();
    std::array<int64_t, 65> histogram{};
    for (uint8_t r : registers) histogram[r]++;
    double z = m * tau(1.0 - (double)histogram[(size_t)q + 1] / m);
    for (int k = q; k >= 1; k--) {
      z = 0.5 * (z + (double)histogram[(size_t)k]);
    }
    z += m * sigma((double)histogram[0] / m);
    // alpha_inf = 1 / (2 ln 2)
    constexpr double ALPHA_INF = 0.721347520444481703680;
    double estimate = ALPHA_INF * m * m / z;
    return (int64_t)std::llround(estimate);
  }

  // Serialized state (cardinality_state), little-endian, no padding:
  //   u8 version (1), u8 kind (2: exact hashes, 3: registers),
  //   u8 precision p, u32 threshold, then
  //   kind 2: u32 n, n x u64 hash in ascending order;
  //   kind 3: 2^p x u8 register.
  // Two states merge only when their thresholds (hence precisions) match.
  static constexpr uint8_t STATE_VERSION = 1;
  static constexpr uint8_t STATE_KIND_EXACT = 2;
  static constexpr uint8_t STATE_KIND_DENSE = 3;
  static constexpr size_t STATE_HEADER_BYTES = 7;

  // Bytes is a contiguous byte container such as std::string.
  template <class Bytes>
  void writeState(Bytes& out) const {
    static_assert(std::endian::native == std::endian::little);
    size_t start = out.size();
    size_t body = dense() ? registers.size() : sizeof(uint32_t) + exact.size() * sizeof(uint64_t);
    out.resize(start + STATE_HEADER_BYTES + body);
    char* p = out.data() + start;
    *p++ = (char)STATE_VERSION;
    *p++ = (char)(dense() ? STATE_KIND_DENSE : STATE_KIND_EXACT);
    *p++ = (char)precision;
    uint32_t limit = (uint32_t)threshold;
    std::memcpy(p, &limit, sizeof limit);
    p += sizeof limit;
    if (dense()) {
      std::memcpy(p, registers.data(), registers.size());
      return;
    }
    uint32_t n = (uint32_t)exact.size();
    std::memcpy(p, &n, sizeof n);
    p += sizeof n;
    // Ascending order keeps the bytes independent of the set's iteration order.
    std::pmr::vector<uint64_t> hashes(exact.begin(), exact.end(), exact.get_allocator().resource());
    std::sort(hashes.begin(), hashes.end());
    if (n != 0) std::memcpy(p, hashes.data(), n * sizeof(uint64_t));
  }

  // Parses writeState output. Returns nullopt on a malformed state.
  static std::optional<HyperLogLog> readState(
      std::string_view bytes,
      std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    static_assert(std::endian::native == std::endian::little);
    if (bytes.size() < STATE_HEADER_BYTES) return std::nullopt;
    const char* p = bytes.data();
    uint8_t kind = (uint8_t)p[1];
    int p0 = (uint8_t)p[2];
    uint32_t limit;
    std::memcpy(&limit, p + 3, sizeof limit);
    if ((uint8_t)p[0] != STATE_VERSION || (kind != STATE_KIND_EXACT && kind != STATE_KIND_DENSE)
        || limit > MAX_PRECISION_THRESHOLD || p0 != precisionForThreshold(limit)) {
      return std::nullopt;
    }
    HyperLogLog sketch(limit, resource);
    p += STATE_HEADER_BYTES;
    size_t rest = bytes.size() - STATE_HEADER_BYTES;
    if (kind == STATE_KIND_DENSE) {
      if (rest != (size_t)1 << sketch.precision) return std::nullopt;
      sketch.registers.assign((const uint8_t*)p, (const uint8_t*)p + rest);
      for (uint8_t rank : sketch.registers) {
        if (rank > 64 - sketch.precision + 1) return std::nullopt;
      }
      // An all-zero register file would read as empty dense state.
      if (std::ranges::all_of(sketch.registers, [](uint8_t rank) { return rank == 0; })) {
        return std::nullopt;
      }
      return sketch;
    }
    uint32_t n;
    if (rest < sizeof n) return std::nullopt;
    std::memcpy(&n, p, sizeof n);
    p += sizeof n;
    if (rest != sizeof n + (size_t)n * sizeof(uint64_t) || n > limit) return std::nullopt;
    for (uint32_t i = 0; i < n; i++) {
      uint64_t hash;
      std::memcpy(&hash, p, sizeof hash);
      p += sizeof hash;
      sketch.exact.insert(hash);
    }
    if (sketch.exact.size() != n) return std::nullopt;
    return sketch;
  }

private:
  void addDense(uint64_t hash) {
    const int q = 64 - precision;
    size_t index = (size_t)(hash >> q);
    uint64_t rest = hash << precision;
    // Leading zeros of the remaining q bits plus one, capped at q + 1.
    int rank = rest == 0 ? q + 1 : std::countl_zero(rest) + 1;
    rank = std::min(rank, q + 1);
    if (registers[index] < rank) registers[index] = (uint8_t)rank;
  }

  void toDense() {
    registers.assign((size_t)1 << precision, 0);
    for (uint64_t hash : exact) addDense(hash);
    ExactSet(exact.get_allocator()).swap(exact);
  }

  static double sigma(double x) {
    if (x == 1.0) return std::numeric_limits<double>::infinity();
    double y = 1.0;
    double z = x;
    double previous;
    do {
      x *= x;
      previous = z;
      z += x * y;
      y += y;
    } while (z != previous);
    return z;
  }

  static double tau(double x) {
    if (x == 0.0 || x == 1.0) return 0.0;
    double y = 1.0;
    double z = 1.0 - x;
    double previous;
    do {
      x = std::sqrt(x);
      previous = z;
      y *= 0.5;
      z -= (1.0 - x) * (1.0 - x) * y;
    } while (z != previous);
    return z / 3.0;
  }
};

} // namespace luxir
