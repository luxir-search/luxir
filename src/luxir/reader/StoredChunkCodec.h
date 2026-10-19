// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "luxir/schema/FieldType.h"

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace luxir {

// Stored-fields chunk compression.  The codec libraries stay out of headers;
// a ZSTD context is created on the first ZSTD chunk and reused after that.
class StoredChunkCompressor {
  struct FreeContext { void operator()(ZSTD_CCtx_s* ctx) const; };
  std::unique_ptr<ZSTD_CCtx_s, FreeContext> zstd;

public:
  // Compress src into dst, resized to the codec's bound; returns the
  // compressed size.
  size_t compress(const StoredChunkFormat& format, std::string_view src, std::vector<char>& dst);

  // Bytes a ZSTD context needs for chunks up to chunkBytes; 0 for LZ4.
  static int64_t contextBytes(const StoredChunkFormat& format, int64_t chunkBytes);
};

class StoredChunkDecompressor {
  struct FreeContext { void operator()(ZSTD_DCtx_s* ctx) const; };
  std::unique_ptr<ZSTD_DCtx_s, FreeContext> zstd;

public:
  // Decompress src into exactly dstSize bytes at dst; throws on a corrupt chunk.
  void decompress(StoredChunkFormat::Codec codec, const char* src, size_t srcSize,
                  char* dst, size_t dstSize);
};

}  // namespace luxir
