#pragma once

#include <cstdint>

namespace luxir {

// Write a vint to a raw buffer. Returns pointer past the last byte written.
inline char* writeVInt(char* p, uint32_t val) {
  while (val > 0x7f) {
    *p++ = (char)(val | 0x80);
    val >>= 7;
  }
  *p++ = (char)val;
  return p;
}

// Write a vlong to a raw buffer. Returns pointer past the last byte written.
inline char* writeVLong(char* p, uint64_t val) {
  while (val > 0x7f) {
    *p++ = (char)(val | 0x80);
    val >>= 7;
  }
  *p++ = (char)val;
  return p;
}

// Read a vint from a raw buffer. Advances p past the last byte read.
inline uint32_t readVInt(const char*& p) {
  char b = *p++;
  uint32_t val = b & 0x7f;
  for (int shift = 7; (b & 0x80) != 0; shift += 7) {
    b = *p++;
    val |= (uint32_t)(b & 0x7f) << shift;
  }
  return val;
}

// Read a vlong from a raw buffer. Advances p past the last byte read.
inline uint64_t readVLong(const char*& p) {
  char b = *p++;
  uint64_t val = b & 0x7f;
  for (int shift = 7; (b & 0x80) != 0; shift += 7) {
    b = *p++;
    val |= (uint64_t)(b & 0x7f) << shift;
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

// Max bytes needed to encode a uint32_t as a vint.
constexpr int MAX_VINT_SIZE = 5;

// Max bytes needed to encode a uint64_t as a vlong.
constexpr int MAX_VLONG_SIZE = 10;

} // namespace luxir
