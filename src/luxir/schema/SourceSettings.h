// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "luxir/api/luxir_types.hpp"
#include "luxir/source/SourceFormat.h"

namespace luxir {

// A collection's resolved SchemaDef.source: whether documents keep their source
// and how the source files are written and compacted.
struct SourceSettings {
  bool enabled = false;
  source::Codec codec = source::Codec::ZSTD;
  int zstdLevel = 3;
  uint32_t blockBytes = 16384;
  double minLiveRatio = 0.5;
  uint64_t minFileBytes = 4ULL * 1024 * 1024;

  // Applies defaults and validates; throws SchemaError on a bad value.
  static SourceSettings resolve(const api::SourceDef& def);

  source::WriterOptions writerOptions() const {
    source::WriterOptions options;
    options.codec = codec;
    options.zstdLevel = zstdLevel;
    options.blockBytes = blockBytes;
    return options;
  }
};

}  // namespace luxir
