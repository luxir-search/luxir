// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "MonoReader.h"
#include "SegmentCore.h"

namespace luxir {

MonoReader::MonoReader(PostingsReader& postingsReader, seg_location loc, int64_t metaOff,
                       int64_t nValues, bool deferred) {
  const char* blocks;
  SegmentCore& core = postingsReader.core();
  InputFile& file = *core.getFile(loc.filenum());
  if (deferred && file.lazy() && !core.residentStream(loc.filenum())) {
    // Payload, then the block descriptors; decoders may load a word past
    // the last one, so take a little slack where the file has it.
    int64_t numBlocks = (nValues + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint64_t bytes = (uint64_t)metaOff + (uint64_t)numBlocks * sizeof(NumBlockInfo) + 16;
    bytes = std::min<uint64_t>(bytes, file.size() - loc.offset());
    blocks = file.rangeStream(loc.offset(), bytes).ptr();
  } else {
    blocks = core.getInputStreamSeek(loc).ptr();
  }
  values = MonoColumn(blocks, blocks + metaOff, nValues);
}

} // namespace luxir
