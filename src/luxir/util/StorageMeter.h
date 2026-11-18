// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstddef>
#include <utility>

namespace luxir {

// Accounting for storage that grows on demand. charge() runs before the
// allocation it pays for and may throw to refuse it; release() returns bytes
// charged earlier.
class StorageMeter {
public:
  virtual void charge(size_t bytes) = 0;
  virtual void release(size_t bytes) noexcept = 0;

protected:
  ~StorageMeter() = default;
};

// The bytes one container has charged to an optional meter, returned when it
// is destroyed. Without a meter every call is a no-op.
class MeteredBytes {
  StorageMeter* meter = nullptr;
  size_t bytes = 0;

public:
  MeteredBytes() = default;
  MeteredBytes(const MeteredBytes&) = delete;
  MeteredBytes& operator=(const MeteredBytes&) = delete;
  MeteredBytes(MeteredBytes&& other) noexcept
      : meter(other.meter), bytes(std::exchange(other.bytes, 0)) {}
  MeteredBytes& operator=(MeteredBytes&& other) noexcept {
    if (this != &other) {
      reset();
      meter = other.meter;
      bytes = std::exchange(other.bytes, 0);
    }
    return *this;
  }
  ~MeteredBytes() { reset(); }

  void setMeter(StorageMeter* newMeter) {
    assert(bytes == 0);
    meter = newMeter;
  }

  // Charges `more` bytes before the caller allocates them.
  void grow(size_t more) {
    if (meter == nullptr || more == 0) return;
    meter->charge(more);
    bytes += more;
  }

  void reset() noexcept {
    if (meter != nullptr && bytes != 0) meter->release(bytes);
    bytes = 0;
  }

  size_t charged() const { return bytes; }
};

} // namespace luxir
