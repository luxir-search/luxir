// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "luxir/codec/ForCodec.h"

namespace luxir {

// Two int32 coordinates. The geo points format uses dimension 0 for latitude
// and dimension 1 for longitude.
struct BKDInt32Traits {
  using Scalar = int32_t;
  using Unsigned = uint32_t;
  using CoordinateCodec = ForCodec;
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

struct BKDInt64Traits {
  using Scalar = int64_t;
  using Unsigned = uint64_t;
  using CoordinateCodec = SplitForCodec;
  static constexpr uint8_t FORMAT_KIND = 3;

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

  static_assert(sizeof(InnerNode) == 12);
  static_assert(offsetof(InnerNode, splitDim) == 8);
  static_assert(sizeof(LeafHeader) == 40);
  static_assert(offsetof(LeafHeader, docBase) == 8);
  static_assert(offsetof(LeafHeader, min0) == 16);
  static_assert(offsetof(LeafHeader, min1) == 24);
  static_assert(offsetof(LeafHeader, bytes0) == 32);
};

} // namespace luxir
