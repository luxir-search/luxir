// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "luxir/codec/Codec.h"

namespace luxir {

// Headerless FOR payload of uint32 values at a caller-supplied bit width.
// Point leaves use it for docids and for residuals of up to 32 bits.
struct ForCodec {
  static uint64_t byteSize(uint32_t count, uint8_t bits) {
    return LuxirSIMDFor::byteSize(count, bits);
  }

  static uint64_t encodeScratchBytes(uint32_t) { return 0; }

  static void encode(std::span<uint32_t> values, uint8_t bits,
                     std::vector<char>& target) {
    uint64_t bytes = byteSize((uint32_t)values.size(), bits);
    if (bytes > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("ForCodec: encoded leaf exceeds uint32");
    }
    target.resize((size_t)bytes);
    uint32_t encodedSize = (uint32_t)target.size();
    IndexCodec::numericCodec.encodeWithMeta(
        values.data(), (uint32_t)values.size(), target.data(), encodedSize, 0,
        bits);
    if (encodedSize != bytes) {
      throw std::runtime_error("ForCodec: codec byte length mismatch");
    }
  }

  static uint32_t decode(const char* payload, uint32_t count, uint8_t bits,
                         std::span<uint32_t> values) {
    return IndexCodec::numericCodec.decodeWithMeta(payload, values.data(), count, bits);
  }
};

// uint64 values: one FOR stream through 32 bits, then a low 32-bit stream
// followed by a high stream of bits - 32 bits, since LuxirSIMDFor packs
// uint32 only.
struct SplitForCodec {
  static uint32_t lowByteSize(uint32_t count, uint8_t bits) {
    return (uint32_t)ForCodec::byteSize(count, std::min(bits, (uint8_t)32));
  }

  static uint64_t byteSize(uint32_t count, uint8_t bits) {
    return lowByteSize(count, bits)
        + (bits > 32 ? ForCodec::byteSize(count, bits - 32) : 0);
  }

  static uint64_t encodeScratchBytes(uint32_t count) {
    return (uint64_t)count * sizeof(uint32_t) + ForCodec::byteSize(count, 32);
  }

  static void encode(std::span<const uint64_t> values, uint8_t bits,
                     std::vector<char>& target) {
    std::vector<uint32_t> words(values.size());
    for (size_t i = 0; i < values.size(); i++) words[i] = (uint32_t)values[i];
    ForCodec::encode(words, std::min(bits, (uint8_t)32), target);
    if (bits > 32) {
      for (size_t i = 0; i < values.size(); i++) words[i] = (uint32_t)(values[i] >> 32);
      std::vector<char> high;
      ForCodec::encode(words, bits - 32, high);
      target.insert(target.end(), high.begin(), high.end());
    }
  }

  static uint32_t decode(const char* payload, uint32_t count, uint8_t bits,
                         std::span<uint64_t> values) {
    uint32_t lowBytes = lowByteSize(count, bits);
    constexpr uint32_t BLOCK_SIZE = LuxirSIMDFor::BLOCK_SIZE;
    std::array<uint32_t, BLOCK_SIZE> low;
    std::array<uint32_t, BLOCK_SIZE> high;
    uint32_t lowRead = 0, highRead = 0;
    // FOR packs independent BLOCK_SIZE-value blocks and a separately packed
    // tail, so each chunk is a complete decoding unit within either stream.
    for (uint32_t base = 0; base < count; base += BLOCK_SIZE) {
      uint32_t size = std::min(count - base, BLOCK_SIZE);
      uint8_t lowBits = std::min(bits, (uint8_t)32);
      lowRead += ForCodec::decode(payload + lowRead, size, lowBits, low);
      if (bits > 32) {
        highRead += ForCodec::decode(payload + lowBytes + highRead, size, bits - 32, high);
      }
      for (uint32_t i = 0; i < size; i++) {
        values[base + i] = (uint64_t)low[i]
            | (bits > 32 ? (uint64_t)high[i] << 32 : 0);
      }
    }
    return lowRead + highRead;
  }
};

} // namespace luxir
