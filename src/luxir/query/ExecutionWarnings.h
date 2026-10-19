// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>

namespace luxir {
// Execution may prepare child queries on parallel workers. The request owns
// this sink and deduplicates warnings safely until preparation has drained.
class ExecutionWarnings {
public:
  virtual void warnOnce(std::string_view code, std::string_view message) = 0;
  virtual ~ExecutionWarnings() = default;
};
}
