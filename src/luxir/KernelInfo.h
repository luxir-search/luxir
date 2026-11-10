// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace luxir {

// Kernels the vector libraries selected for this CPU when the process started.
// Unlike BuildInfo these are runtime choices: one binary reports different
// values on different CPUs, or when FAISS_SIMD_LEVEL forces a level.
struct KernelInfo {
  std::string faissSimd;  // NONE, AVX2, AVX512, AVX512_SPR, ARM_NEON or ARM_SVE
  std::string blasCore;   // OpenBLAS core name, or "Accelerate" on macOS
};

const KernelInfo& kernelInfo();

} // namespace luxir
