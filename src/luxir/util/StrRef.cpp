// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/StrRef.h"

#include <xxhash.h>

namespace luxir {

// See the declaration for the persisted-format commitment.
std::string_view PackedTerm::hashTail(std::string_view term, TermBuffer& scratch) noexcept {
  XXH128_canonical_t hash;
  XXH128_canonicalFromHash(&hash, XXH3_128bits(term.data(), term.size()));
  auto prefix = truncate(term, HASH128_PREFIX_LEN);
  memcpy(scratch.data(), prefix.data(), prefix.size());
  unsigned __int128 value = 0;
  for (unsigned char byte : hash.digest) value = (value << 8) | byte;
  static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char* out = scratch.data() + prefix.size() + HASH128_CHARS;
  // 36^25 exceeds 2^128, so 25 digits always suffice; leading zeros keep the
  // width fixed.
  for (uint32_t i = 0; i < HASH128_CHARS; i++) {
    *--out = digits[(uint32_t)(value % 36)];
    value /= 36;
  }
  out = scratch.data() + prefix.size() + HASH128_CHARS;
  return std::string_view(scratch.data(), out - scratch.data());
}

} // namespace luxir
