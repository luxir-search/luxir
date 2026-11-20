// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/KernelInfo.h"

#include <faiss/utils/simd_levels.h>

#ifndef __APPLE__
// OpenBLAS selects its core in a load-time constructor (DYNAMIC_ARCH).
extern "C" char* openblas_get_corename(void);
#endif

namespace luxir {

const KernelInfo& kernelInfo() {
#ifdef __APPLE__
  static const KernelInfo info{faiss::SIMDConfig::get_level_name(), "Accelerate"};
#else
  static const KernelInfo info{faiss::SIMDConfig::get_level_name(), openblas_get_corename()};
#endif
  return info;
}

} // namespace luxir
