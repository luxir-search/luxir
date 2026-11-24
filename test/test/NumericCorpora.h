// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

#include "luxir/codec/NumColumnFormat.h"
#include "luxir/util/NumericUtils.h"

namespace luxir::test {

// Numeric column corpora in their stored encoding (sortable bits for
// FLOAT/DOUBLE), one per data shape a block kind (NumBlockKind) targets,
// for the codec tests and the column decode benchmarks. Deterministic in
// `seed`.
struct NumericCorpus {
  std::string_view name;
  NumColumnFormat::ValueDomain domain;
  // The kind every block of the corpus should take with the codecs on.
  NumBlockKind kind;
  std::vector<int64_t> values;
};

inline std::vector<NumericCorpus> numericCorpora(size_t n, uint64_t seed = 42) {
  using Domain = NumColumnFormat::ValueDomain;
  std::mt19937_64 gen(seed);
  std::vector<NumericCorpus> corpora;
  auto add = [&](std::string_view name, Domain domain, NumBlockKind kind,
                 auto&& value) {
    NumericCorpus corpus{name, domain, kind, {}};
    corpus.values.reserve(n);
    for (size_t i = 0; i < n; i++) corpus.values.push_back(value(i));
    corpora.push_back(std::move(corpus));
  };

  // Two-decimal prices, 0.01 to 2000.00: FOR sees sortable double bits.
  add("prices", Domain::DOUBLE, NumBlockKind::ALP_DOUBLE, [&](size_t) {
    return doubleToSortableInt64((double)(1 + (int64_t)(gen() % 200'000)) / 100.0);
  });
  // Two-decimal values of both signs, with values ALP cannot scale (-0.0,
  // NaN, infinities, a denormal) and irrational ones as exceptions.
  add("signed_doubles", Domain::DOUBLE, NumBlockKind::ALP_DOUBLE, [&](size_t i) {
    double v = (double)((int64_t)(gen() % 2'000'000) - 1'000'000) / 100.0;
    if (i % 400 == 7) v = std::sqrt((double)i);
    if (i == 100) v = -0.0;
    if (i == 101) v = std::numeric_limits<double>::quiet_NaN();
    if (i == 102) v = std::numeric_limits<double>::infinity();
    if (i == 103) v = -std::numeric_limits<double>::infinity();
    if (i == 104) v = std::numeric_limits<double>::denorm_min();
    return doubleToSortableInt64(v);
  });
  // One-decimal float sensor readings, a few irrational.
  add("float_readings", Domain::FLOAT, NumBlockKind::ALP_FLOAT, [&](size_t i) {
    float v = (float)(int64_t)(gen() % 100'000) / 10.0f;
    if (i % 300 == 11) v = (float)std::sqrt((double)i);
    return (int64_t)floatToSortableInt32(v);
  });
  // Epoch-millisecond timestamps at a steady rate with small jitter: the
  // linear fit leaves a few bits.
  int64_t t = 1'700'000'000'000;
  add("regular_times", Domain::INT, NumBlockKind::FOR, [&](size_t i) {
    return t + (int64_t)i * 1000 + (int64_t)(gen() % 8);
  });
  // Bursts of events milliseconds apart, with gaps of a minute between
  // bursts: the timestamps wander far off any line, their deltas do not.
  t = 1'700'000'000'000;
  add("bursty_times", Domain::INT, NumBlockKind::DELTA, [&](size_t) {
    t += gen() % 40 == 0 ? 60'000 + (int64_t)(gen() % 1000) : (int64_t)(gen() % 16);
    return t;
  });
  // HTTP status codes: 9 bits by FOR, 2 by dictionary.
  add("status_codes", Domain::INT, NumBlockKind::DICT, [&](size_t) {
    constexpr int64_t CODES[] = {200, 301, 404, 503};
    return CODES[gen() % 4];
  });
  // Small counts with a rare huge outlier, which alone sets FOR's width.
  add("outliers", Domain::INT, NumBlockKind::FOR_PATCHED, [&](size_t i) {
    return i % 1000 == 500 ? (int64_t)1 << 40 : (int64_t)(gen() % 1000);
  });
  add("booleans", Domain::INT, NumBlockKind::FOR, [&](size_t) {
    return (int64_t)(gen() % 2);
  });
  // Wide random integers: nothing beats FOR.
  add("random_ints", Domain::INT, NumBlockKind::FOR, [&](size_t) {
    return (int64_t)(gen() % 1'000'000'000);
  });
  return corpora;
}

} // namespace luxir::test
