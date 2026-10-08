// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace luxir {

// Process allocator, version and effective configuration for the startup banner.
std::string allocatorName();

// Returns free allocator memory to the OS where the allocator supports it.
void releaseFreeMemory();

// Bytes allocated by the calling thread; nullopt without jemalloc or a sanitizer.
// In sanitizer builds, first call before starting threads to install the hook.
std::optional<uint64_t> threadAllocatedBytes();

} // namespace luxir
