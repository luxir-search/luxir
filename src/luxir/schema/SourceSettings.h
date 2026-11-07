// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "luxir/api/luxir_types.hpp"
#include "luxir/source/SourceFilter.h"
#include "luxir/source/SourceFormat.h"

namespace luxir {

// The resolved "_source_" stored resource: whether documents keep their source,
// which of their paths it keeps, and how the source files are written and
// compacted. A schema without the entry resolves to the defaults with enabled
// false.
struct SourceSettings {
  bool enabled = false;
  source::Codec codec = source::Codec::ZSTD;
  int zstdLevel = 3;
  uint32_t blockBytes = 16384;
  uint32_t maxDocsPerBlock = 0;  // 0 = bytes alone seal a block
  double minLiveRatio = 0.5;
  uint64_t minFileBytes = 4ULL * 1024 * 1024;
  uint32_t maxFiles = 32;
  source::SourceFilter filter;  // empty keeps whole documents; an update request may override it

  // Applies defaults and validates the "_source_" entry; throws SchemaError on
  // a bad value.
  static SourceSettings resolve(const api::StoredResourceDef& def);

  source::WriterOptions writerOptions() const {
    source::WriterOptions options;
    options.codec = codec;
    options.zstdLevel = zstdLevel;
    options.blockBytes = blockBytes;
    options.maxDocsPerBlock = maxDocsPerBlock;
    return options;
  }
};

}  // namespace luxir
