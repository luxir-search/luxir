// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "FieldParts.h"

namespace luxir {

// Keeps doc ID window and block arithmetic away from the INT32_MAX sentinel.
inline constexpr int32_t HARD_MAX_ROW_COUNT = INT32_MAX - (1 << 20);

// An ordinal in one reader or writer catalog, never a persisted identity.
enum class SpaceOrdinal : uint32_t { ROOT = 0 };

// Immediate child ownership in parent-major order.
struct OwnershipDescriptor {
  field_parts::Slice ends;  // End child ordinal per parent, null for the root.
  field_parts::Slice owners;  // Parent ordinal per child, if a reverse map is written.
};

// A segment's root documents or the rows belonging to one nested path.
struct RowSpace {
  std::string_view path;  // Empty for the root. The catalog or writer owns the bytes.
  int32_t maxDoc;  // The exclusive end of the space's doc IDs.
  OwnershipDescriptor ownership;  // Parent-to-child mapping for a nested path.
};

inline bool validChildSpacePath(std::string_view path) {
  return !path.empty() && path.front() != '.' && path.back() != '.' && path.find("..") == std::string_view::npos;
}

inline bool validSpaceDocCount(int32_t maxDoc) {
  return maxDoc >= 0 && maxDoc < HARD_MAX_ROW_COUNT;
}

inline std::string_view spaceParentPath(std::string_view path) {
  auto dot = path.rfind('.');
  return path.substr(0, dot == std::string_view::npos ? 0 : dot);
}

} // namespace luxir
