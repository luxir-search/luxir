// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>
#include "luxir/util/luxir_util.h"

namespace luxir::summary {

// Key of a term inside a segment summary's membership structures: XXH3-64 of
// the exact fitted bytes the term dictionary stores, unseeded. Persisted in
// sidecar files, so a change here must bump HASH_VERSION; a reader that sees
// another version treats every filter as unknown.
inline constexpr uint32_t HASH_VERSION = 1;

inline uint64_t termKey(std::string_view term) {
  return XXH3_64bits(term.data(), term.size());
}

} // namespace luxir::summary
