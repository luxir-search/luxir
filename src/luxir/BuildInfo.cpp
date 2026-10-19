// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The only file compiled with the build definitions (CMakeLists.txt), so a
// version bump rebuilds just this file.
#include "BuildInfo.h"

namespace luxir {

std::string_view buildVersion() { return LUXIR_VERSION; }
std::string_view buildCpuTarget() { return LUXIR_CPU_NAME; }

} // namespace luxir
