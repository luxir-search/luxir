// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/schema/FieldType.h"

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;
struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

namespace luxir {

// Immutable owning bytes, shared by prepared dictionaries at different settings.
class StoredDictionary {
  const std::string data;

public:
  explicit StoredDictionary(std::string bytes) : data(std::move(bytes)) {}
  std::string_view bytes() const { return data; }
  size_t allocatedBytes() const { return sizeof(*this) + data.capacity(); }
};

class StoredCompressionDictionary {
  struct FreeDict { void operator()(ZSTD_CDict_s* dict) const; };
  std::shared_ptr<const StoredDictionary> dictionary;
  int level;
  uint32_t chunkTarget;
  std::unique_ptr<ZSTD_CDict_s, FreeDict> cdict;

public:
  StoredCompressionDictionary(std::shared_ptr<const StoredDictionary> dictionary,
                              const StoredChunkFormat& format);
  StoredCompressionDictionary& operator=(StoredCompressionDictionary&&) = delete;
  std::string_view bytes() const { return dictionary->bytes(); }
  bool matches(const StoredChunkFormat& format) const {
    return format.codec == StoredChunkFormat::ZSTD && format.zstdLevel == level
        && format.chunkTarget == chunkTarget;
  }
  const ZSTD_CDict_s* get() const { return cdict.get(); }
  // Excludes the separately shared byte owner.
  size_t allocatedBytes() const;
};

// Stored-fields chunk compression.  The codec libraries stay out of headers;
// a ZSTD context is created on the first ZSTD chunk and reused after that.
class StoredChunkCompressor {
  struct FreeContext { void operator()(ZSTD_CCtx_s* ctx) const; };
  std::shared_ptr<const StoredCompressionDictionary> cdict;
  std::unique_ptr<ZSTD_CCtx_s, FreeContext> zstd;

public:
  static constexpr size_t MAX_DICTIONARY_SAMPLES = 65536;
  static constexpr size_t DICTIONARY_SAMPLE_BYTES = 256;

  explicit StoredChunkCompressor(std::shared_ptr<const StoredCompressionDictionary> dictionary = {})
      : cdict(std::move(dictionary)) {}
  std::string_view dictionary() const { return cdict ? cdict->bytes() : std::string_view{}; }

  // Compress src into dst, resized to the codec's bound; returns the
  // compressed size.
  size_t compress(const StoredChunkFormat& format, std::string_view src, std::vector<char>& dst);

  // Bytes a ZSTD context needs for chunks up to chunkBytes; 0 for LZ4.
  static int64_t contextBytes(const StoredChunkFormat& format, int64_t chunkBytes);

  static size_t dictionarySampleBudget(const StoredChunkFormat& format) {
    return std::min((size_t)format.dictionaryBytes * 100, (size_t)8 * 1024 * 1024);
  }
  // Bootstrap peak, including samples, training workspace and the prepared CDict.
  static int64_t dictionaryTrainingBytes(const StoredChunkFormat& format);

  // Train a ZSTD dictionary of at most capacity bytes from samples, the
  // concatenation of sampleSizes.size() samples.  Empty when training fails,
  // which zstd reports when the samples are too few or too small to help.
  static std::string trainDictionary(std::string_view samples, std::span<const size_t> sampleSizes,
                                     size_t capacity);
};

// A trained ZSTD dictionary prepared for decompression.  Immutable, so one
// instance per segment resource serves every reader and thread.
class StoredDecompressionDictionary {
  struct FreeDict { void operator()(ZSTD_DDict_s* dict) const; };
  std::unique_ptr<ZSTD_DDict_s, FreeDict> ddict;

public:
  // The referenced bytes must outlive this object.
  explicit StoredDecompressionDictionary(std::string_view dictionary);
  const ZSTD_DDict_s* get() const { return ddict.get(); }
  size_t allocatedBytes() const;
};

class StoredChunkDecompressor {
  struct FreeContext { void operator()(ZSTD_DCtx_s* ctx) const; };
  std::unique_ptr<ZSTD_DCtx_s, FreeContext> zstd;

public:
  // Decompress src into exactly dstSize bytes at dst; throws on a corrupt
  // chunk.  dictionary is the resource's, or null when it has none.
  void decompress(StoredChunkFormat::Codec codec, const StoredDecompressionDictionary* dictionary,
                  const char* src, size_t srcSize, char* dst, size_t dstSize);
};

}  // namespace luxir
