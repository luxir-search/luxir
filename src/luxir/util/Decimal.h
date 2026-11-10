// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace luxir {

struct DecimalScale {
  double factor;
  double reciprocal;

  explicit constexpr DecimalScale(int scale) : factor(1), reciprocal(1) {
    for (int i = 0; i < scale; i++) factor *= 10;
    reciprocal = 1 / factor;
  }

  template <typename T> double operator()(T raw) const {
    double a = (double)raw;
#ifdef __FMA__
    double q = a * reciprocal;
    double e = std::fma(-q, factor, a);
    return std::fma(e, reciprocal, q);
#else
    return a / factor;
#endif
  }
};

inline constexpr auto DECIMAL_SCALES = [] {
  return []<size_t... I>(std::index_sequence<I...>) {
    return std::array{DecimalScale((int)I)...};
  }(std::make_index_sequence<19>{});
}();

} // namespace luxir
