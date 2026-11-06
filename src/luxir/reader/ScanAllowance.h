// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace luxir {

// A scan reports visits in slices. charge() records visited more visits and
// returns how many it may make before reporting again; 0 means stop. May throw.
class ScanAllowance {
public:
  virtual ~ScanAllowance() = default;
  virtual int64_t charge(int64_t visited) = 0;
};

} // namespace luxir
