// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstring>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/util/MemPool.h"

namespace luxir {

// Appends a request warning unless one with the same code and message is
// already present, since a repeat says nothing new. code must have static
// storage duration; message is copied into pool.
inline void addWarning(std::vector<api::Warning>& warnings, MemPool& pool,
                       std::string_view code, std::string_view message) {
  for (const api::Warning& warning : warnings) {
    if (warning.code == code && warning.message == message) return;
  }
  char* copy = pool.alloc(message.size());
  std::memcpy(copy, message.data(), message.size());
  warnings.push_back({code, std::string_view(copy, message.size())});
}

} // namespace luxir
