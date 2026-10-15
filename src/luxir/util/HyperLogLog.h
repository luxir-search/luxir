// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
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
  static constexpr int64_t DEFAULT_PRECISION_THRESHOLD = 3000;
  static constexpr int64_t MAX_PRECISION_THRESHOLD = 40000;
  static constexpr int MIN_PRECISION = 4;
  static constexpr int MAX_PRECISION = 18;

  int64_t threshold;
  int precision;
  boost::unordered_flat_set<uint64_t> exact;
  std::vector<uint8_t> registers; // nonempty once dense

  // Enough registers that the dense estimate at the threshold is about as
  // accurate as a hash table of that many entries at 0.75 load would allow.
  static int precisionForThreshold(int64_t threshold) {
    double entries = std::ceil((double)std::max<int64_t>(threshold, 1) / 0.75);
    int p = (int)std::ceil(std::log2(entries));
    return std::clamp(p, MIN_PRECISION, MAX_PRECISION);
  }

  // Upper bound on resident bytes for a sketch with this threshold.
  static size_t maxBytes(int64_t threshold) {
    // A flat set holds at most threshold + 1 hashes before conversion; its
    // slot array is at most twice that, one control byte per slot.
    size_t exactBytes = (size_t)(threshold + 1) * 2 * (sizeof(uint64_t) + 1);
    size_t denseBytes = (size_t)1 << precisionForThreshold(threshold);
    return std::max(exactBytes, denseBytes);
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

  explicit HyperLogLog(int64_t threshold = DEFAULT_PRECISION_THRESHOLD)
      : threshold(threshold), precision(precisionForThreshold(threshold)) {
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
    std::vector<int64_t> histogram((size_t)q + 2, 0);
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
    boost::unordered_flat_set<uint64_t>().swap(exact);
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
