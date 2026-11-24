// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace luxir::hamming {

/// Counts differing bits in two n-byte rows without reading past either row.
using KernelFn = uint64_t (*)(const uint8_t* a, const uint8_t* b, size_t n);

struct Kernel {
  const char* name;
  KernelFn count;
};

/// Levels available on this CPU, with the word fallback first.
std::span<const Kernel> supported();

/// Selects a kernel once for all rows of this size.
KernelFn best(size_t rowBytes);

} // namespace luxir::hamming
