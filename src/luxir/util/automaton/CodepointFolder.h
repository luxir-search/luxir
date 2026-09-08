// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace luxir::automaton {

// Caller-supplied literal folding seam. Compilers invoke this only after
// recognizing syntax, so operators and character-class endpoints stay exact.
struct CodepointFolder {
  virtual int32_t fold(int32_t codepoint, int32_t* out, int32_t maxOut) const = 0;
};

} // namespace luxir::automaton
