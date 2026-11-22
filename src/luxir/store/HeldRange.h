// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <utility>

#include "InputStream.h"

namespace luxir {

struct ByteRange {
  uint64_t offset = 0;
  uint64_t length = 0;
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

} // namespace luxir
