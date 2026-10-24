// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

namespace luxir {

struct BuildInfo {
  std::string_view version;
  std::string_view sourceCommit;
  std::string_view cpuTarget;
};

// Metadata compiled into the binary; never reads Git or files at runtime.
const BuildInfo& buildInfo();

} // namespace luxir
