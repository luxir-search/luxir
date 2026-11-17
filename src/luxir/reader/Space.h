// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "luxir/store/OutputStream.h"

namespace luxir {

// An ordinal in one reader or writer catalog, never a persisted identity.
enum class SpaceOrdinal : uint32_t { ROOT = 0 };

struct OwnershipDescriptor {
  seg_location ends;
  int64_t endsMetaOff;
  seg_location owners; // Null when the reverse map is absent.
  int64_t ownersMetaOff = 0;
};

struct SpaceDescriptor {
  std::string path;
  int32_t maxDoc = 0;
  seg_location fieldOffsets;
  uint32_t fieldCount = 0;
  std::optional<OwnershipDescriptor> ownership;
};

inline std::string_view spaceParentPath(std::string_view path) {
  auto dot = path.rfind('.');
  return path.substr(0, dot == std::string_view::npos ? 0 : dot);
}

} // namespace luxir
