// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace luxir {

// On-disk constants of the NORMS_PACKED layout, shared by NormsWriter and
// NormsReader. A packed region is
//   [bits][addressing][tableLen][table or min][codes]
// bits is 1, 2 or 4, so a code never straddles a byte. addressing says
// whether codes are indexed by docid (one per doc in the segment) or by
// presence rank (one per doc with the field). tableLen 0 is frame of
// reference: one min byte follows and code c decodes to min + c. Otherwise
// tableLen distinct norm bytes follow and code c decodes to table[c]. Codes
// are packed little-endian within each byte, lowest index in the low bits.
// Wider codes would save less than half of the raw bytes, so fields with more
// than 16 distinct norm bytes keep the raw FLAT or SPARSE layout.
struct NormsLayout {
  static constexpr uint32_t MAX_PACKED_BITS = 4;
  static constexpr uint32_t MAX_PACKED_CODES = 1u << MAX_PACKED_BITS;
  static constexpr uint8_t BY_DOC = 0;
  static constexpr uint8_t BY_RANK = 1;

  static constexpr int64_t packedBytes(int64_t count, uint32_t bits) {
    return (count * (int64_t) bits + 7) / 8;
  }

  // Smallest packed width (1, 2 or 4) that holds numCodes codes, or 0 when
  // numCodes needs more than MAX_PACKED_BITS.
  static constexpr uint32_t packedBits(uint32_t numCodes) {
    if (numCodes <= 2) return 1;
    if (numCodes <= 4) return 2;
    if (numCodes <= MAX_PACKED_CODES) return 4;
    return 0;
  }
};

} // namespace luxir
