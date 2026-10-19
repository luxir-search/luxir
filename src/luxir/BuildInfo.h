// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

namespace luxir {

// The release version and the CPU target this binary was built for.
std::string_view buildVersion();
std::string_view buildCpuTarget();

} // namespace luxir
