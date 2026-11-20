// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>

namespace luxir::api {

// Shared grammar for JSON and expr field-list sugar: name or name^weight.
// Numeric bounds are validated when building the query, in every wire form.
inline bool splitWeightedField(std::string_view text, std::string_view& field,
                               std::optional<float>& weight) {
  weight.reset();
  size_t caret = text.find('^');
  field = text.substr(0, caret);
  if (field.empty()) return false;
  if (caret == std::string_view::npos) return true;
  std::string_view number = text.substr(caret + 1);
  if (number.empty()) return false;
  float value;
  auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
  if (ec != std::errc() || end != number.data() + number.size()) return false;
  weight = value;
  return true;
}

} // namespace luxir::api
