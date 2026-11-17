// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "luxir/codec/Codec.h"

namespace luxir {

// The FOR payload shared by docids and int32 coordinate residuals.
struct BKDForCodec {
  static uint64_t byteSize(uint32_t count, uint8_t bits) {
    return LuxirSIMDFor::byteSize(count, bits);
  }

  static void encode(std::span<uint32_t> values, uint8_t bits,
                     std::vector<char>& target) {
    uint64_t bytes = byteSize((uint32_t)values.size(), bits);
    if (bytes > std::numeric_limits<uint32_t>::max()) {
      throw std::overflow_error("BKDWriter: encoded leaf exceeds uint32");
    }
    target.resize((size_t)bytes);
    uint32_t encodedSize = (uint32_t)target.size();
    IndexCodec::numericCodec.encodeWithMeta(
        values.data(), (uint32_t)values.size(), target.data(), encodedSize, 0,
        bits);
    if (encodedSize != bytes) {
      throw std::runtime_error("BKDWriter: codec byte length mismatch");
    }
  }

  static uint32_t decode(const char* payload, uint32_t count, uint8_t bits,
                         std::span<uint32_t> values) {
    return IndexCodec::numericCodec.decodeWithMeta(payload, values.data(), count, bits);
  }
};

// Two int32 coordinates. The geo points format uses dimension 0 for latitude
// and dimension 1 for longitude.
struct BKDInt32Traits {
  using Scalar = int32_t;
  using Unsigned = uint32_t;
  using CoordinateCodec = BKDForCodec;
  static constexpr uint8_t FORMAT_KIND = 2;

  // Logical nodes are 1-indexed in heap order; entry nodeId - 1 holds nodeId.
  struct __attribute__((packed)) InnerNode {
    Scalar splitValue;
    uint8_t splitDim;
    uint8_t reserved8;
    uint16_t reserved16;
  };

  struct __attribute__((packed)) LeafHeader {
    uint16_t count;
    uint8_t docCodec;
    uint8_t docBits;
    uint8_t bits0;
    uint8_t bits1;
    uint16_t reserved;
    uint32_t docBase;
    uint32_t docBytes;
    Scalar min0;
    Scalar min1;
    uint32_t bytes0;
    uint32_t reserved2;
  };

  static_assert(sizeof(InnerNode) == 8);
  static_assert(offsetof(InnerNode, splitDim) == 4);
  static_assert(sizeof(LeafHeader) == 32);
  static_assert(offsetof(LeafHeader, docBase) == 8);
  static_assert(offsetof(LeafHeader, min0) == 16);
};

} // namespace luxir
