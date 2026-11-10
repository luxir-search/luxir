// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <string>

#include "FileIOException.h"

namespace luxir {

// A byte range of a file.
struct ByteRange {
  uint64_t offset = 0;
  uint64_t length = 0;
};

// When a fetch must have finished. The engine sets no search deadline yet, so
// most callers pass kNoReadDeadline.
using ReadDeadline = std::chrono::steady_clock::time_point;
inline constexpr ReadDeadline kNoReadDeadline = ReadDeadline::max();

// Runs exactly once when InputFile::fetchRanges finishes: with null when every
// range is resident, otherwise with the reason it is not.
using FetchDone = std::function<void(std::exception_ptr)>;

// A fetch that had not finished by its deadline.
class FetchTimeoutError : public FileIOException {
public:
  explicit FetchTimeoutError(const std::string& message)
      : FileIOException(ErrorKind::UNAVAILABLE, "fetch_timeout", message) {}
};

// The file did not fetch the ranges ahead of the read (it has no way to). The
// bytes are still readable: the read fetches them on first touch and blocks.
class FetchNotResidentError : public FileIOException {
public:
  FetchNotResidentError() : FileIOException("the file cannot fetch ranges ahead of a read") {}
};

inline bool isFetchNotResident(const std::exception_ptr& error) {
  if (!error) return false;
  try {
    std::rethrow_exception(error);
  } catch (const FetchNotResidentError&) {
    return true;
  } catch (...) {
    return false;
  }
}

// The fetchRanges of a file whose bytes are readable without a fetch (memory,
// a mapping): checks that every range lies in the file and calls done inline.
inline void fetchResident(std::span<const ByteRange> ranges, uint64_t fileSize, const FetchDone& done) {
  std::exception_ptr error;
  for (const auto& range : ranges) {
    if (range.offset > fileSize || range.length > fileSize - range.offset) {
      error = std::make_exception_ptr(FileIOException(
          "range [" + std::to_string(range.offset) + ", +" + std::to_string(range.length)
          + ") is outside a file of " + std::to_string(fileSize) + " bytes"));
      break;
    }
  }
  done(error);
}

}  // namespace luxir
