// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "InputStream.h"

namespace luxir {

// What a range holds. METADATA is what a reader navigates by before it reads
// data: file headers, the catalog, version runs, term block offsets and tries,
// the stored-field chunk index, and directories and indexes over column and
// point blocks. Everything else is DATA. Whoever keeps a hold decides how long
// it lives; the class is a hint a cache or backend may use for residency and
// scheduling after release. It never changes the bytes returned.
enum class ReadClass : uint8_t { DATA, METADATA };

struct ByteRange {
  uint64_t offset = 0;
  uint64_t length = 0;
  ReadClass readClass = ReadClass::DATA;
};

inline constexpr size_t kReadSlack = 16;
inline constexpr size_t kHoldAlign = 16;

class HeldRange;

class HoldOwner {
public:
  // Thread-safe. A hold can be released on any thread.
  virtual void release(const HeldRange& hold) noexcept = 0;

protected:
  ~HoldOwner() = default;
};

// The next kReadSlack bytes after the range are readable up to EOF, with
// unspecified contents. The data address and file offset agree modulo
// kHoldAlign. A hold must not outlive its InputFile. Keep the hold alive
// while using its views and never take stream() from a temporary hold.
class HeldRange {
  const char* data = nullptr;
  uint64_t length = 0;
  HoldOwner* owner = nullptr;
  uintptr_t token = 0;

public:
  HeldRange() = default;
  HeldRange(const char* data, uint64_t length, HoldOwner* owner = nullptr, uintptr_t token = 0) noexcept
      : data(data), length(length), owner(owner), token(token) {}

  HeldRange(HeldRange&& other) noexcept
      : data(std::exchange(other.data, nullptr)), length(std::exchange(other.length, 0)),
        owner(std::exchange(other.owner, nullptr)), token(std::exchange(other.token, 0)) {}

  HeldRange& operator=(HeldRange&& other) noexcept {
    if (this != &other) {
      if (owner) owner->release(*this);
      data = std::exchange(other.data, nullptr);
      length = std::exchange(other.length, 0);
      owner = std::exchange(other.owner, nullptr);
      token = std::exchange(other.token, 0);
    }
    return *this;
  }

  ~HeldRange() { if (owner) owner->release(*this); }

  const char* getData() const & noexcept { return data; }
  const char* getData() const && = delete;
  uint64_t getLength() const noexcept { return length; }
  uintptr_t getToken() const noexcept { return token; }

  std::span<const char> bytes() const & noexcept { return {data, (size_t)length}; }
  std::span<const char> bytes() const && = delete;
  InputStream stream() const & noexcept { return {data, data + length}; }
  InputStream stream() const && = delete;
};

// Completes a batched acquire exactly once: with one hold per range in the
// order requested and a null error, or with no holds and the error.
using AcquireDone = std::function<void(std::vector<HeldRange>&&, std::exception_ptr)>;

} // namespace luxir
