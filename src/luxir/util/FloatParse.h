// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <charconv>
#include <concepts>
#include <glaze/util/fast_float.hpp>

namespace luxir {

// Keep from_chars semantics when libc++ lacks its floating-point overloads.
// Glaze already includes the locale-independent fast_float parser.
template <std::floating_point T>
inline std::from_chars_result fromCharsFloat(
    const char* first, const char* last, T& value) noexcept {
  if constexpr (requires { std::from_chars(first, last, value); }) {
    return std::from_chars(first, last, value);
  } else {
    auto result = glz::fast_float::from_chars(first, last, value);
    return {result.ptr, result.ec};
  }
}

}  // namespace luxir
