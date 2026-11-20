// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "StoredChunkCodec.h"

#include <lz4.h>
#include <new>
#include <stdexcept>
#include <string>
// Advanced dictionary preparation and memory estimates; linked statically.
#define ZSTD_STATIC_LINKING_ONLY
#include <zdict.h>
#include <zstd.h>

namespace luxir {

void StoredChunkCompressor::FreeContext::operator()(ZSTD_CCtx* ctx) const { ZSTD_freeCCtx(ctx); }

void StoredChunkDecompressor::FreeContext::operator()(ZSTD_DCtx* ctx) const { ZSTD_freeDCtx(ctx); }

void StoredCompressionDictionary::FreeDict::operator()(ZSTD_CDict* dict) const { ZSTD_freeCDict(dict); }

void StoredDecompressionDictionary::FreeDict::operator()(ZSTD_DDict* dict) const { ZSTD_freeDDict(dict); }

static void checkZstd(size_t result) {
  if (ZSTD_isError(result)) {
    throw std::runtime_error(std::string("stored fields: ZSTD compression failed: ") + ZSTD_getErrorName(result));
  }
}

StoredCompressionDictionary::StoredCompressionDictionary(
    std::shared_ptr<const StoredDictionary> dictionary, const StoredChunkFormat& format)
    : dictionary(std::move(dictionary)), level(format.zstdLevel), chunkTarget(format.chunkTarget) {
  if (!this->dictionary || bytes().empty() || format.codec != StoredChunkFormat::ZSTD) {
    throw std::invalid_argument("stored fields: a compression dictionary requires ZSTD and nonempty bytes");
  }
  auto params = ZSTD_getCParams(level, chunkTarget, bytes().size());
  cdict.reset(ZSTD_createCDict_advanced(bytes().data(), bytes().size(), ZSTD_dlm_byRef,
                                      ZSTD_dct_auto, params, ZSTD_defaultCMem));
  if (!cdict) throw std::runtime_error("stored fields: invalid ZSTD dictionary");
}

size_t StoredCompressionDictionary::allocatedBytes() const {
  return sizeof(*this) + ZSTD_sizeof_CDict(cdict.get());
}

size_t StoredChunkCompressor::compress(const StoredChunkFormat& format, std::string_view src,
                                       std::vector<char>& dst) {
  if (format.codec == StoredChunkFormat::ZSTD) {
    if (cdict && !cdict->matches(format)) {
      throw std::invalid_argument("stored fields: dictionary compression parameters do not match format");
    }
    if (!zstd) {
      // A compressor serves one writer, whose format and dictionary are fixed.
      zstd.reset(ZSTD_createCCtx());
      if (!zstd) throw std::bad_alloc();
      if (cdict) {
        checkZstd(ZSTD_CCtx_refCDict(zstd.get(), cdict->get()));
      } else {
        checkZstd(ZSTD_CCtx_setParameter(zstd.get(), ZSTD_c_compressionLevel, format.zstdLevel));
      }
      checkZstd(ZSTD_CCtx_setParameter(zstd.get(), ZSTD_c_dictIDFlag, 0));
    }
    dst.resize(ZSTD_compressBound(src.size()));
    size_t n = ZSTD_compress2(zstd.get(), dst.data(), dst.size(), src.data(), src.size());
    checkZstd(n);
    return n;
  }
  if (cdict) throw std::invalid_argument("stored fields: a compression dictionary requires ZSTD");
  int bound = LZ4_compressBound((int)src.size());
  dst.resize((size_t)bound);
  int n = LZ4_compress_default(src.data(), dst.data(), (int)src.size(), bound);
  if (n <= 0) throw std::runtime_error("stored fields: LZ4 compression failed");
  return (size_t)n;
}

int64_t StoredChunkCompressor::contextBytes(const StoredChunkFormat& format, int64_t chunkBytes) {
  if (format.codec != StoredChunkFormat::ZSTD) return 0;
  size_t bytes = ZSTD_estimateCCtxSize_usingCParams(
      ZSTD_getCParams(format.zstdLevel, (unsigned long long)chunkBytes, 0));
  checkZstd(bytes);
  return (int64_t)bytes;
}

std::string StoredChunkCompressor::trainDictionary(std::string_view samples,
                                                   std::span<const size_t> sampleSizes, size_t capacity) {
  std::string dictionary(capacity, '\0');
  size_t n = ZDICT_trainFromBuffer(dictionary.data(), capacity, samples.data(), sampleSizes.data(),
                                   (unsigned)sampleSizes.size());
  if (ZDICT_isError(n)) return {};
  dictionary.resize(n);
  return dictionary;
}

int64_t StoredChunkCompressor::dictionaryTrainingBytes(const StoredChunkFormat& format) {
  if (format.codec != StoredChunkFormat::ZSTD || !format.dictionaryBytes) return 0;
  // Derived from zstd 1.5.7's dictBuilder; recheck when upgrading zstd.
  size_t samples = dictionarySampleBudget(format);
  size_t capacity = std::min((size_t)format.dictionaryBytes, samples / 100);
  // ZDICT_trainFromBuffer uses single-threaded fastCover, d=8, f=20,
  // steps=4. fastcover.c keeps two U32 frequency tables and a U16 table
  // (10 MiB), plus (nbSamples + 1) size_t offsets. cover.c also holds the
  // candidate, largest, spare and previous best dictionaries, alongside our
  // output. Finalization/evaluation use a level-3 CCtx/CDict; 256 KiB covers
  // their 128 KiB entropy buffer, compressed sample and small workspaces.
  size_t trainerContext = ZSTD_estimateCCtxSize(ZSTD_CLEVEL_DEFAULT);
  size_t trainerDictionary = ZSTD_estimateCDictSize(capacity, ZSTD_CLEVEL_DEFAULT);
  size_t preparedDictionary = ZSTD_estimateCDictSize_advanced(capacity,
      ZSTD_getCParams(format.zstdLevel, format.chunkTarget, capacity), ZSTD_dlm_byRef);
  checkZstd(trainerContext);
  checkZstd(trainerDictionary);
  checkZstd(preparedDictionary);
  return (int64_t)(samples + MAX_DICTIONARY_SAMPLES * sizeof(size_t)
      + (MAX_DICTIONARY_SAMPLES + 1) * sizeof(size_t) + 10 * 1024 * 1024
      + 256 * 1024 + 5 * capacity + trainerContext + trainerDictionary + preparedDictionary);
}

StoredDecompressionDictionary::StoredDecompressionDictionary(std::string_view dictionary)
  : ddict(ZSTD_createDDict_byReference(dictionary.data(), dictionary.size())) {
  if (!ddict) throw std::runtime_error("stored fields: invalid ZSTD dictionary");
}

size_t StoredDecompressionDictionary::allocatedBytes() const {
  return ZSTD_sizeof_DDict(ddict.get());
}

void StoredChunkDecompressor::decompress(StoredChunkFormat::Codec codec, const StoredDecompressionDictionary* dictionary,
                                         const char* src, size_t srcSize, char* dst, size_t dstSize) {
  if (codec == StoredChunkFormat::ZSTD) {
    if (!zstd) {
      zstd.reset(ZSTD_createDCtx());
      if (!zstd) throw std::bad_alloc();
    }
    size_t n = dictionary
        ? ZSTD_decompress_usingDDict(zstd.get(), dst, dstSize, src, srcSize, dictionary->get())
        : ZSTD_decompressDCtx(zstd.get(), dst, dstSize, src, srcSize);
    if (ZSTD_isError(n) || n != dstSize) throw std::runtime_error("stored fields: ZSTD decompression failed");
    return;
  }
  int n = LZ4_decompress_safe(src, dst, (int)srcSize, (int)dstSize);
  if (n < 0 || (size_t)n != dstSize) throw std::runtime_error("stored fields: LZ4 decompression failed");
}

}  // namespace luxir
