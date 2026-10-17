// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "FieldComparator.h"

namespace luxir {
size_t StrColComparator::retainedSlotBytes(int32_t slot) const {
  if ((size_t)slot >= values.size()) return 0;
  const auto& value = values[(size_t)slot];
  return value.capacity() > std::string{}.capacity() ? value.capacity() + 1 : 0;
}
}  // namespace luxir
