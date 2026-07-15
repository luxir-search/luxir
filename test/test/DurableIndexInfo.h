#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "solux/api/padded_input.h"
#include "solux/api/solux_types.hpp"
#include "solux/reader/Postings.h"
#include "solux/store/Directory.h"

namespace solux::test {

// Owns the arena backing the non-owning IndexInfo view returned by the decoder.
struct DurableIndexInfo {
  std::unique_ptr<std::pmr::monotonic_buffer_resource> arena =
    std::make_unique<std::pmr::monotonic_buffer_resource>();
  solux::api::IndexInfo info;

  const solux::api::IndexInfo* operator->() const { return &info; }
  const solux::api::IndexInfo& operator*() const { return info; }
};

inline DurableIndexInfo readDurableIndexInfo(Directory& dir) {
  DurableIndexInfo result;
  auto file = dir.openFile(Postings::INDEX_INFO_FILE);
  if (file == nullptr) {
    throw std::runtime_error("Missing durable IndexInfo");
  }
  auto input = file->getInputStream();
  std::span<const std::byte> bytes((const std::byte*)input.ptr(), (size_t)input.left());
  auto padded = solux::api::copyToPaddedInput(bytes, *result.arena);
  if (!solux::api::decode(result.info, padded, *result.arena)) {
    throw std::runtime_error("Failed to decode durable IndexInfo");
  }
  return result;
}

} // namespace solux::test
