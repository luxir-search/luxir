#pragma once

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "solux/reader/FieldReader.h"
#include "solux/search/Similarity.h"

namespace solux {

// Immutable mmap view of one segment-field block-bounds sidecar.
class BlockBounds {
  std::shared_ptr<InputFile> file;
  const char* base = nullptr;
  uint32_t admittedTerms = 0;
  uint64_t tableOffset = 0;
  float envelope = 0.0f;

  static uint32_t readU32(const char* p);
  static uint64_t readU64(const char* p);
  static float readF32(const char* p);

public:
  static constexpr std::string_view MAGIC = "SLXBB001";
  static constexpr uint32_t FORMAT_REVISION = 1;
  static constexpr uint32_t FIXED_HEADER_SIZE = 80;
  static constexpr uint32_t TABLE_ENTRY_SIZE = 16;
  static constexpr uint32_t TERM_HEADER_SIZE = 16;
  static constexpr uint32_t RECORD_SIZE = 8;

  class TermView {
    const char* record = nullptr;

    friend class BlockBounds;
    explicit TermView(const char* record) : record(record) {}

  public:
    TermView() = default;
    explicit operator bool() const { return record != nullptr; }
    int32_t groupCount() const;
    int32_t blockCount() const;
    float denominatorScale() const;
    int32_t groupContaining(int32_t doc) const;
    int32_t blockContaining(int32_t doc) const;
    int32_t groupLastDoc(int32_t group) const;
    int32_t blockLastDoc(int32_t block) const;
    float groupDenominator(int32_t group) const;
    float blockDenominator(int32_t block) const;
  };

  BlockBounds(std::shared_ptr<InputFile> file, const char* base,
              uint32_t admittedTerms, uint64_t tableOffset, float envelope)
      : file(std::move(file)), base(base), admittedTerms(admittedTerms),
        tableOffset(tableOffset), envelope(envelope) {}

  TermView find(int32_t termOrd) const;
  float envelopeAvgdl() const { return envelope; }
  int64_t mappedBytes() const { return file ? file->size() : 0; }

  static std::string fileName(uint64_t segId, std::string_view field);
  static uint64_t checksum(std::span<const char> bytes);
  static std::shared_ptr<BlockBounds> open(
      Directory& dir, uint64_t segId, const SegFieldInfo& fieldInfo,
      int32_t maxDoc, float globalAvgdl, std::string* failure = nullptr,
      bool expectSynced = true, std::string_view fileNameOverride = {});

  static std::atomic<uint64_t> validationFailures;
};

} // namespace solux
