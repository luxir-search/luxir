// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace luxir {

/// Sets how flat kNN scans read a lazy (remote) vector file: windows of
/// `windowBytes`, with the windows of the next `aheadBytes` acquired ahead.
/// Process-wide; never changes results.
void setKnnScanWindows(int64_t windowBytes, int64_t aheadBytes);

} // namespace luxir
