// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>

namespace luxir {

// The version of everything Luxir writes to an index directory: commit
// manifests, segment files and live docs. Every manifest records it and every
// data file starts with HEADER, whose digits are the same number.
//
// Bump VERSION (and HEADER) with any change to the bytes Luxir writes.
// IndexFormatTest fails when the bytes of a small reference index change, as a
// reminder. There is no compatibility between versions: a collection written
// by another version fails to load and has to be reindexed.
struct IndexFormat {
  static constexpr uint64_t VERSION = 8;
  static constexpr std::string_view HEADER = "LUXIR008";
};

static_assert(IndexFormat::HEADER.size() == 8 && IndexFormat::HEADER.starts_with("LUXIR")
    && (uint64_t)((IndexFormat::HEADER[5] - '0') * 100 + (IndexFormat::HEADER[6] - '0') * 10
                  + (IndexFormat::HEADER[7] - '0')) == IndexFormat::VERSION,
    "IndexFormat::HEADER digits must match IndexFormat::VERSION");

// A manifest written by another format version.
class IndexFormatError : public std::runtime_error {
public:
  explicit IndexFormatError(uint64_t found)
    : std::runtime_error(std::format(
          "index format version {} is not supported by this build (format version {}); reindex the collection",
          found, IndexFormat::VERSION)) {}
};

} // namespace luxir
