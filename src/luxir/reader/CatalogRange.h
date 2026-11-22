// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>

namespace luxir {
namespace api { struct CatalogRange; }

// Locates the catalog within the first file. The manifest records this range
// so a reader can open the field tables and directory independently.
struct CatalogRange {
  uint64_t offset = 0;  // Offset from the start of the file.
  uint64_t length = 0;  // Length through the end of the file.
  // XXH3 of the catalog bytes. Every open from a manifest range checks it before
  // decoding, local or ranged. Unset for a catalog found from file 0's trailer.
  std::optional<uint64_t> xxh3;

  bool operator==(const CatalogRange&) const = default;
};

CatalogRange catalogRangeFromManifest(const api::CatalogRange& range);
api::CatalogRange catalogRangeToManifest(const CatalogRange& range);

} // namespace luxir
