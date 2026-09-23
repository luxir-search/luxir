// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <string>

namespace luxir {

inline std::string newUuid() {
  std::random_device random;
  std::array<uint8_t, 16> bytes;
  for (auto& byte : bytes) byte = (uint8_t)random();
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  constexpr char hex[] = "0123456789abcdef";
  std::string uuid;
  uuid.reserve(36);
  for (size_t i = 0; i < bytes.size(); i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) uuid += '-';
    uuid += hex[bytes[i] >> 4];
    uuid += hex[bytes[i] & 15];
  }
  return uuid;
}

}
