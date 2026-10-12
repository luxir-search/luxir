// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

namespace luxir {

// Process-wide dictionary refills after segment-state release.
inline std::atomic<uint64_t> multitermExpansionRefills{0};

} // namespace luxir
