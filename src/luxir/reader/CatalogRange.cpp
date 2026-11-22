// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CatalogRange.h"

#include "luxir/api/luxir_index.hpp"

namespace luxir {

CatalogRange catalogRangeFromManifest(const api::CatalogRange& range) {
  return {range.offset, range.length, range.xxh3};
}

api::CatalogRange catalogRangeToManifest(const CatalogRange& range) {
  if (!range.xxh3) throw std::logic_error("cannot publish a catalog range without a hash");
  return {range.offset, range.length, *range.xxh3};
}

} // namespace luxir
