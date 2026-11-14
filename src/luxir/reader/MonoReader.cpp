// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "MonoReader.h"
#include "SegmentCore.h"

namespace luxir {

MonoReader::MonoReader(PostingsReader& postingsReader, seg_location loc, int64_t metaOff,
                       int64_t nValues) {
  InputStream columnIS = postingsReader.core().getInputStreamSeek(loc);
  const char* blocks = columnIS.ptr();
  values = MonoColumn(blocks, blocks + metaOff, nValues);
}

} // namespace luxir
