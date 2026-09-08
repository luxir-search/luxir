// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <span>

namespace luxir::api {

inline constexpr std::size_t PADDED_PROTO_INPUT_BYTES = 16;

inline std::span<const std::byte> copyToPaddedInput(std::span<const std::byte> data,
                                                    std::pmr::memory_resource& arena) {
  auto* padded = (std::byte*)arena.allocate(data.size() + PADDED_PROTO_INPUT_BYTES, alignof(std::byte));
  if (!data.empty()) {
    std::memcpy(padded, data.data(), data.size());
  }
  std::memset(padded + data.size(), 0, PADDED_PROTO_INPUT_BYTES);
  return {padded, data.size()};
}

} // namespace luxir::api
