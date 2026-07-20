#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "solux/store/OutputStream.h"

namespace solux {

class LinearPack {
public:
  static constexpr uint8_t TAIL_PAD = 7;

  static constexpr uint64_t packedByteSize(uint64_t count, uint8_t bits) {
    assert(bits <= 57);
    return (count * bits + 7) >> 3;
  }

  static constexpr uint64_t byteSize(uint64_t count, uint8_t bits) {
    return packedByteSize(count, bits) + TAIL_PAD;
  }

  static constexpr uint32_t mask32(uint8_t bits) {
    assert(bits <= 32);
    return bits == 0 ? 0 : (uint32_t)((1ull << bits) - 1);
  }

  static constexpr uint64_t mask64(uint8_t bits) {
    assert(bits <= 57);
    return bits == 0 ? 0 : (1ull << bits) - 1;
  }

  class Writer {
    OutputStream* out = nullptr;
    char* target = nullptr;
    std::vector<char>* vectorTarget = nullptr;
    uint64_t pending = 0;
    uint64_t count = 0;
    uint64_t written = 0;
    uint8_t bits = 0;
    uint8_t pendingBits = 0;
    bool finished = false;

    void writeByte(uint8_t value) {
      if (out != nullptr) {
        out->write((char)value);
      } else if (vectorTarget != nullptr) {
        vectorTarget->push_back((char)value);
      } else {
        *target++ = (char)value;
      }
      written++;
    }

  public:
    Writer(OutputStream& out, uint8_t bits) : out(&out), bits(bits) {
      assert(bits <= 57);
    }

    Writer(char* target, uint8_t bits) : target(target), bits(bits) {
      assert(target != nullptr);
      assert(bits <= 57);
    }

    Writer(std::vector<char>& target, uint8_t bits)
        : vectorTarget(&target), bits(bits) {
      assert(bits <= 57);
    }

    void append(uint64_t value) {
      assert(!finished);
      assert(bits == 0 ? value == 0 : value <= mask64(bits));
      if (bits != 0) {
        pending |= value << pendingBits;
        uint8_t totalBits = pendingBits + bits;
        while (totalBits >= 8) {
          writeByte((uint8_t)pending);
          pending >>= 8;
          totalBits -= 8;
        }
        pendingBits = totalBits;
      }
      count++;
    }

    uint64_t finish() {
      assert(!finished);
      if (pendingBits != 0) {
        writeByte((uint8_t)pending);
      }
      for (uint8_t i = 0; i < TAIL_PAD; i++) {
        writeByte(0);
      }
      finished = true;
      assert(written == byteSize(count, bits));
      return written;
    }
  };

  static uint32_t select32(const char* base, uint64_t idx, uint8_t bits,
                           uint32_t mask) {
    assert(bits <= 32);
    if (bits == 0) return 0;
    uint64_t bitPos = (uint64_t)idx * bits;
    uint64_t word;
    memcpy(&word, base + (bitPos >> 3), sizeof(word));
    return (uint32_t)(word >> (bitPos & 7)) & mask;
  }

  static uint64_t select64(const char* base, uint64_t idx, uint8_t bits,
                           uint64_t mask) {
    assert(bits <= 57);
    if (bits == 0) return 0;
    uint64_t bitPos = idx * bits;
    uint64_t word;
    memcpy(&word, base + (bitPos >> 3), sizeof(word));
    return (word >> (bitPos & 7)) & mask;
  }

  static void unpack128(const char* base, uint64_t idx, uint32_t count,
                        uint8_t bits, uint32_t mask, uint32_t* values) {
    assert(count <= 128);
    for (uint32_t i = 0; i < count; i++) {
      values[i] = select32(base, idx + i, bits, mask);
    }
  }

  static void unpack128(const char* base, uint64_t idx, uint32_t count,
                        uint8_t bits, uint64_t mask, uint64_t* values) {
    assert(count <= 128);
    for (uint32_t i = 0; i < count; i++) {
      values[i] = select64(base, idx + i, bits, mask);
    }
  }
};

} // namespace solux
