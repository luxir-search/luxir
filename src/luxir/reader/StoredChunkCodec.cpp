// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "StoredChunkCodec.h"

#include <lz4.h>
#include <new>
#include <stdexcept>
#include <string>
// For ZSTD_estimateCCtxSize_usingCParams (stable since 1.4; linked statically).
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace luxir {

void StoredChunkCompressor::FreeContext::operator()(ZSTD_CCtx* ctx) const { ZSTD_freeCCtx(ctx); }

void StoredChunkDecompressor::FreeContext::operator()(ZSTD_DCtx* ctx) const { ZSTD_freeDCtx(ctx); }

size_t StoredChunkCompressor::compress(const StoredChunkFormat& format, std::string_view src,
                                       std::vector<char>& dst) {
  if (format.codec == StoredChunkFormat::ZSTD) {
    if (!zstd) {
      zstd.reset(ZSTD_createCCtx());
      if (!zstd) throw std::bad_alloc();
    }
    dst.resize(ZSTD_compressBound(src.size()));
    size_t n = ZSTD_compressCCtx(zstd.get(), dst.data(), dst.size(), src.data(), src.size(),
                                 format.zstdLevel);
    if (ZSTD_isError(n)) {
      throw std::runtime_error(std::string("stored fields: ZSTD compression failed: ") + ZSTD_getErrorName(n));
    }
    return n;
  }
  int bound = LZ4_compressBound((int)src.size());
  dst.resize((size_t)bound);
  int n = LZ4_compress_default(src.data(), dst.data(), (int)src.size(), bound);
  if (n <= 0) throw std::runtime_error("stored fields: LZ4 compression failed");
  return (size_t)n;
}

int64_t StoredChunkCompressor::contextBytes(const StoredChunkFormat& format, int64_t chunkBytes) {
  if (format.codec != StoredChunkFormat::ZSTD) return 0;
  return (int64_t)ZSTD_estimateCCtxSize_usingCParams(
      ZSTD_getCParams(format.zstdLevel, (unsigned long long)chunkBytes, 0));
}

void StoredChunkDecompressor::decompress(StoredChunkFormat::Codec codec, const char* src, size_t srcSize,
                                         char* dst, size_t dstSize) {
  if (codec == StoredChunkFormat::ZSTD) {
    if (!zstd) {
      zstd.reset(ZSTD_createDCtx());
      if (!zstd) throw std::bad_alloc();
    }
    size_t n = ZSTD_decompressDCtx(zstd.get(), dst, dstSize, src, srcSize);
    if (ZSTD_isError(n) || n != dstSize) throw std::runtime_error("stored fields: ZSTD decompression failed");
    return;
  }
  int n = LZ4_decompress_safe(src, dst, (int)srcSize, (int)dstSize);
  if (n < 0 || (size_t)n != dstSize) throw std::runtime_error("stored fields: LZ4 decompression failed");
}

}  // namespace luxir
