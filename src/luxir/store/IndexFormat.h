// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>

namespace luxir {

// The version of everything Luxir writes to an index directory: commit
// manifests, segment files and live docs. Every manifest records it and every
// data file starts with HEADER. Its last three characters are the format version
// in base36 (..., 009, 00A, ..., 00Z, 010).
//
// Bump VERSION when a change requires a different on-disk encoding or
// interpretation. IndexFormatTest also tracks byte changes from writer choices
// among layouts supported by the current version. HEADER is derived from
// VERSION. A collection written by another version fails to load and has to
// be reindexed.
//
// A parameter that a part records about itself, and that readers take from
// there (such as an id filter's kind and size), is not a format change: the
// writer may vary it within one version.
struct IndexFormat {
  static constexpr uint64_t VERSION = 18;
  static_assert(VERSION > 0 && VERSION < 36 * 36 * 36);
  static constexpr auto HEADER_BYTES = [] {
    std::array<char, 8> header{'L', 'U', 'X', 'I', 'R'};
    auto version = VERSION;
    for (int i = 7; i >= 5; --i) {
      header[i] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[version % 36];
      version /= 36;
    }
    return header;
  }();
  static constexpr std::string_view HEADER{HEADER_BYTES.data(), HEADER_BYTES.size()};
};

// A manifest written by another format version.
class IndexFormatError : public std::runtime_error {
public:
  explicit IndexFormatError(uint64_t found)
    : std::runtime_error(std::format(
          "index format version {} is not supported by this build (format version {}); reindex the collection",
          found, IndexFormat::VERSION)) {}
};

} // namespace luxir
