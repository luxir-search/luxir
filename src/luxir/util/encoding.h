// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <climits>
#include <cstdint>

namespace luxir {

template <class T>
constexpr T ceilDiv(T value, T divisor) noexcept {
  return value / divisor + (value % divisor != 0);
}

// Alignment is a power of two; callers bound the input before rounding.
constexpr uint64_t alignUp(uint64_t offset, uint64_t alignment) noexcept {
  return (offset + alignment - 1) & ~(alignment - 1);
}

constexpr int VARINT_PAYLOAD_BITS = CHAR_BIT - 1;
constexpr uint8_t VARINT_CONTINUATION = 1u << VARINT_PAYLOAD_BITS;
constexpr uint8_t VARINT_PAYLOAD_MASK = VARINT_CONTINUATION - 1;
constexpr int MAX_VINT_SIZE = ceilDiv((int)sizeof(uint32_t) * CHAR_BIT, VARINT_PAYLOAD_BITS);
constexpr int MAX_VLONG_SIZE = ceilDiv((int)sizeof(uint64_t) * CHAR_BIT, VARINT_PAYLOAD_BITS);

// Write a vint to a raw buffer. Returns pointer past the last byte written.
inline char* writeVInt(char* p, uint32_t val) {
  while (val > VARINT_PAYLOAD_MASK) {
    *p++ = (char)(val | VARINT_CONTINUATION);
    val >>= VARINT_PAYLOAD_BITS;
  }
  *p++ = (char)val;
  return p;
}

// Write a vlong to a raw buffer. Returns pointer past the last byte written.
inline char* writeVLong(char* p, uint64_t val) {
  while (val > VARINT_PAYLOAD_MASK) {
    *p++ = (char)(val | VARINT_CONTINUATION);
    val >>= VARINT_PAYLOAD_BITS;
  }
  *p++ = (char)val;
  return p;
}

// Read a vint from a raw buffer. Advances p past the last byte read.
inline uint32_t readVInt(const char*& p) {
  char b = *p++;
  uint32_t val = b & VARINT_PAYLOAD_MASK;
  for (int shift = VARINT_PAYLOAD_BITS; (b & VARINT_CONTINUATION) != 0; shift += VARINT_PAYLOAD_BITS) {
    b = *p++;
    val |= (uint32_t)(b & VARINT_PAYLOAD_MASK) << shift;
  }
  return val;
}

// Read a vlong from a raw buffer. Advances p past the last byte read.
inline uint64_t readVLong(const char*& p) {
  char b = *p++;
  uint64_t val = b & VARINT_PAYLOAD_MASK;
  for (int shift = VARINT_PAYLOAD_BITS; (b & VARINT_CONTINUATION) != 0; shift += VARINT_PAYLOAD_BITS) {
    b = *p++;
    val |= (uint64_t)(b & VARINT_PAYLOAD_MASK) << shift;
  }
  return val;
}

// Zig-zag encode a signed int64 to unsigned so small magnitudes use few bytes in vlong encoding.
// 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, 2 -> 4, ...
inline uint64_t zigzagEncode(int64_t val) {
  return ((uint64_t)val << 1) ^ (uint64_t)(val >> 63);
}

// Zig-zag decode unsigned back to signed int64.
inline int64_t zigzagDecode(uint64_t val) {
  return (int64_t)((val >> 1) ^ -(val & 1));
}

// Write a zig-zag encoded signed long as a vlong. Returns pointer past the last byte written.
inline char* writeZLong(char* p, int64_t val) {
  return writeVLong(p, zigzagEncode(val));
}

// Read a zig-zag encoded signed long from a vlong. Advances p past the last byte read.
inline int64_t readZLong(const char*& p) {
  return zigzagDecode(readVLong(p));
}

} // namespace luxir
