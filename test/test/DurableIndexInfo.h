// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/reader/Postings.h"
#include "luxir/store/Directory.h"

namespace luxir::test {

// Owns the arena backing the non-owning IndexInfo view returned by the decoder.
struct DurableIndexInfo {
  std::unique_ptr<std::pmr::monotonic_buffer_resource> arena =
    std::make_unique<std::pmr::monotonic_buffer_resource>();
  luxir::api::IndexInfo info;

  const luxir::api::IndexInfo* operator->() const { return &info; }
  const luxir::api::IndexInfo& operator*() const { return info; }
};

inline DurableIndexInfo readDurableIndexInfo(Directory& dir) {
  DurableIndexInfo result;
  auto file = dir.openFile(Postings::INDEX_INFO_FILE);
  if (file == nullptr) {
    throw std::runtime_error("Missing durable IndexInfo");
  }
  auto input = file->getInputStream();
  std::span<const std::byte> bytes((const std::byte*)input.ptr(), (size_t)input.left());
  auto padded = luxir::api::copyToPaddedInput(bytes, *result.arena);
  if (!luxir::api::decode(result.info, padded, *result.arena)) {
    throw std::runtime_error("Failed to decode durable IndexInfo");
  }
  return result;
}

} // namespace luxir::test
