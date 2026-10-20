// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ranges>
#include <stdexcept>
#include "luxir/codec/NumColumn.h"
#include "luxir/util/screaming.h"

namespace luxir {

LUXIR_UNALIGNED_START
struct VersionRun {
  uint32_t firstDocId;
  uint64_t version;
} LUXIR_UNALIGNED_END;
static_assert(sizeof(VersionRun) == 12);

struct VersionBlockHeader {
  NumBlockInfo versions;
  uint32_t firstDocId;
  uint32_t step; // Zero means irregular starts, followed by a MonoColumn descriptor.
};
static_assert(sizeof(VersionBlockHeader) == 40);

struct VersionBlockDirectory {
  uint32_t firstDocId;
  uint32_t reserved = 0;
  uint64_t offset;
};

struct VersionRunsHeader {
  uint32_t encoding;
  uint32_t count;
  uint64_t directoryOffset;
  uint64_t bytes;
};

// One physical block view, shared by the inverter and immutable segments.
class VersionBlock {
  const char* data = nullptr;
  uint32_t count_ = 0;

public:
  VersionBlock() = default;
  VersionBlock(const char* data, uint32_t count) : data(data), count_(count) {}
  const VersionBlockHeader& header() const { return *(const VersionBlockHeader*)data; }
  uint32_t size() const { return count_; }
  NumColumn versions() const { return {data, data, count_}; }
  MonoColumn starts() const { return {data, data + sizeof(VersionBlockHeader), count_}; }
  uint32_t start(uint32_t index) const {
    return header().step ? header().firstDocId + index * header().step : (uint32_t)starts().valueAt(index);
  }
  uint64_t version(uint32_t index) const { return (uint64_t)versions().valueAt(index); }

  // Bidirectional gallop keeps nearby probes cheap without restarting backward
  // probes at zero. Dense and affine blocks use arithmetic instead.
  template<class Start>
  static uint32_t find(uint32_t doc, uint32_t hint, uint32_t count, Start&& start) {
    hint = std::min(hint, count - 1);
    if (doc < start(hint)) {
      uint32_t reverse = (uint32_t)screaming::gallopLowerBound(0, (int64_t)hint + 1,
          -(int64_t)doc, [&](int64_t i) { return -(int64_t)start(hint - (uint32_t)i); });
      return hint - reverse;
    }
    return (uint32_t)screaming::gallopLowerBound(hint, count, (uint64_t)doc + 1,
        [&](int64_t i) { return (uint64_t)start((uint32_t)i); }) - 1;
  }

  uint32_t find(uint32_t doc, uint32_t hint) const {
    if (header().step) return std::min((doc - header().firstDocId) / header().step, count_ - 1);
    return find(doc, hint, count_, [&](uint32_t i) { return start(i); });
  }
};

// A bounded cache of an exact affine prefix, derived only from sealed blocks.
// It never includes the mutable trailing boundary.
struct VersionAffinePrefix {
  uint64_t base = 0, step = 0;
  uint32_t count = 0;

  void append(const VersionBlock& block, uint32_t firstRun) {
    if (count != firstRun || block.header().firstDocId != count || block.header().step != 1) return;
    auto fit = block.versions().affineBlock(0);
    if (!fit) return;
    if (!count) {
      base = fit->first;
      step = fit->second;
    } else if (base + count * step != fit->first || step != fit->second) return;
    count += block.size();
  }
  uint64_t birth(uint32_t doc) const { return base + doc * step; }
};

class VersionRuns {
  const char* data = nullptr;
  const VersionBlockDirectory* directory = nullptr;
  uint32_t count_ = 0;
  int32_t maxDoc_ = 0;
  uint64_t bytes_ = 0;
  bool dense = true;
  VersionAffinePrefix affine;

  static void invalid() { throw std::runtime_error("Invalid request-version run coverage or encoding"); }

public:
  static constexpr uint32_t ENCODING = 2;
  // Bound the uncompressed tail to 3 KiB per inverter while amortizing the
  // block descriptors on one-document requests.
  static constexpr uint32_t BLOCK_SIZE = 256;
  VersionRuns() = default;
  VersionRuns(const char* file, size_t fileSize, size_t headerOffset, uint32_t count, int32_t maxDoc);

  size_t size() const { return count_; }
  size_t byteSize() const { return bytes_; }
  uint32_t numBlocks() const { return (count_ + BLOCK_SIZE - 1) / BLOCK_SIZE; }
  VersionBlock block(uint32_t index) const {
    return {data + directory[index].offset, std::min(BLOCK_SIZE, count_ - index * BLOCK_SIZE)};
  }
  VersionRun run(size_t index) const {
    auto view = block((uint32_t)index / BLOCK_SIZE);
    uint32_t within = (uint32_t)index % BLOCK_SIZE;
    return {view.start(within), view.version(within)};
  }
  auto runs() const {
    return std::views::iota((size_t)0, size())
        | std::views::transform([this](size_t i) { return run(i); });
  }

  class Cursor {
    const VersionRuns& source;
    uint32_t blockIndex = 0;
    uint32_t runIndex = 0;
    uint32_t first = 0, end = 0;
    uint64_t version = 0;
    uint32_t frameBlock = UINT32_MAX;
    int64_t frameStart = -1, frameEnd = -1;
    int64_t values[NumColumn::BULK_SIZE];
    uint32_t decodedFirst = 0, decodedEnd = 0;

  public:
    explicit Cursor(const VersionRuns& source) : source(source) {}
    LUXIR_INLINE uint64_t birth(int32_t docId) {
      if (docId < 0 || docId >= source.maxDoc_) {
        throw std::runtime_error("Document has no request birth version");
      }
      uint32_t doc = (uint32_t)docId;
      if (doc < source.affine.count) return source.affine.birth(doc);
      if (doc >= first && doc < end) return version;
      return lookupBirth(doc);
    }

  private:
    LUXIR_NOINLINE uint64_t lookupBirth(uint32_t doc) {
      if (source.dense && doc < source.count_) {
        if (doc < decodedFirst || doc >= decodedEnd) {
          uint32_t n;
          uint32_t block = doc / BLOCK_SIZE;
          auto view = source.block(block);
          int64_t start = view.versions().decodeFrame(doc % BLOCK_SIZE, values, n);
          decodedFirst = block * BLOCK_SIZE + (uint32_t)start;
          decodedEnd = decodedFirst + n;
          frameBlock = UINT32_MAX;
        }
        return (uint64_t)values[doc - decodedFirst];
      }
      if (source.dense) {
        blockIndex = std::min(doc / BLOCK_SIZE, source.numBlocks() - 1);
      } else {
        blockIndex = VersionBlock::find(doc, blockIndex, source.numBlocks(),
            [&](uint32_t i) { return source.directory[i].firstDocId; });
      }
      auto block = source.block(blockIndex);
      runIndex = block.find(doc, runIndex);
      first = block.start(runIndex);
      end = runIndex + 1 < block.size() ? block.start(runIndex + 1)
          : blockIndex + 1 < source.numBlocks() ? source.directory[blockIndex + 1].firstDocId
          : (uint32_t)source.maxDoc_;
      if (frameBlock != blockIndex || runIndex < frameStart || runIndex >= frameEnd) {
        uint32_t n;
        frameStart = block.versions().decodeFrame(runIndex, values, n);
        frameEnd = frameStart + n;
        frameBlock = blockIndex;
        decodedEnd = 0;
      }
      return version = (uint64_t)values[runIndex - frameStart];
    }
  };

  class Iterator {
    const VersionRuns& source;
    uint32_t index = 0;
    int64_t versions[NumColumn::BULK_SIZE];
    int64_t starts[NumColumn::BULK_SIZE];

  public:
    explicit Iterator(const VersionRuns& source) : source(source) {}
    bool next(VersionRun& run) {
      if (index == source.size()) return false;
      uint32_t within = index % BLOCK_SIZE;
      uint32_t frame = within % NumColumn::BULK_SIZE;
      auto block = source.block(index / BLOCK_SIZE);
      if (frame == 0) {
        uint32_t n;
        block.versions().decodeFrame(within, versions, n);
        if (!block.header().step) block.starts().decodeFrame(within, starts, n);
      }
      run = {block.header().step ? block.start(within) : (uint32_t)starts[frame], (uint64_t)versions[frame]};
      index++;
      return true;
    }
  };
};

inline VersionRuns::VersionRuns(const char* file, size_t fileSize, size_t headerOffset,
                                uint32_t count, int32_t maxDoc)
    : count_(count), maxDoc_(maxDoc) {
  if (headerOffset > fileSize || fileSize - headerOffset < sizeof(VersionRunsHeader)
      || ((uintptr_t)(file + headerOffset) & 7) || maxDoc <= 0 || !count || count > (uint32_t)maxDoc) invalid();
  const auto& header = *(const VersionRunsHeader*)(file + headerOffset);
  if (header.bytes > headerOffset + sizeof(header) || header.bytes < sizeof(header) || (header.bytes & 7)) invalid();
  data = file + headerOffset + sizeof(header) - header.bytes;
  if (header.encoding != ENCODING || header.count != count || header.bytes > fileSize
      || (header.directoryOffset & 7)
      || header.directoryOffset > header.bytes
      || (uint64_t)numBlocks() * sizeof(VersionBlockDirectory) + sizeof(header) != header.bytes - header.directoryOffset) invalid();
  bytes_ = header.bytes;
  directory = (const VersionBlockDirectory*)(data + header.directoryOffset);
  uint32_t previous = 0;
  for (uint32_t i = 0; i < numBlocks(); i++) {
    const auto& entry = directory[i];
    uint64_t limit = i + 1 < numBlocks() ? directory[i + 1].offset : header.directoryOffset;
    if ((entry.offset & 7) || entry.offset > limit
        || limit > header.directoryOffset || limit - entry.offset < sizeof(VersionBlockHeader)
        || entry.reserved || entry.firstDocId >= (uint32_t)maxDoc
        || (i == 0 ? entry.firstDocId != 0 : entry.firstDocId <= previous)) invalid();
    auto view = block(i);
    const auto& bh = view.header();
    if (entry.firstDocId != bh.firstDocId) invalid();
    size_t metaSize = sizeof(VersionBlockHeader) + (bh.step ? 0 : sizeof(NumBlockInfo));
    auto check = [&](const NumBlockInfo& info, bool monotonic) {
      uint8_t bits = info.bits();
      if (info.kind() != NumBlockKind::FOR
          || (bits > NumColumnFormat::MAX_PACKED_BITS && bits != 64) || !info.gcd
          || (monotonic && info.gcd != 1)) invalid();
      uint64_t payload = info.payloadOffset();
      uint64_t bytes = bits == 64 ? (uint64_t)view.size() * 8
          : LinearPack::packedByteSize(view.size(), bits);
      if (payload < metaSize || payload > limit - entry.offset
          || bytes + LinearPack::TAIL_PAD > limit - entry.offset - payload) invalid();
    };
    check(bh.versions, false);
    if (bh.step) {
      uint64_t last = (uint64_t)bh.firstDocId + (uint64_t)(view.size() - 1) * bh.step;
      if (last >= (uint32_t)maxDoc || (i + 1 < numBlocks() && last >= directory[i + 1].firstDocId)) invalid();
      previous = (uint32_t)last;
    } else {
      if (limit - entry.offset < metaSize) invalid();
      check(*(const NumBlockInfo*)(data + entry.offset + sizeof(VersionBlockHeader)), true);
      MonoColumn col = view.starts();
      MonoColumn::Bulk values(col);
      for (uint32_t j = 0; j < view.size(); j++) {
        int64_t start = values.valueAt(j);
        if (start < 0 || start >= maxDoc || (j == 0 ? start != entry.firstDocId : start <= previous)) invalid();
        previous = (uint32_t)start;
      }
      if (i + 1 < numBlocks() && previous >= directory[i + 1].firstDocId) invalid();
    }
    dense &= bh.step == 1 && entry.firstDocId == i * BLOCK_SIZE;
    affine.append(view, i * BLOCK_SIZE);
  }
}

} // namespace luxir
