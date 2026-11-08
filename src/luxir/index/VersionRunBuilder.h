// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/VersionRuns.h"
#include "luxir/codec/NumBlockCodec.h"

namespace luxir {

// Blocks precede the directory and trailer, permitting bounded streaming merge
// output. Directory offsets are relative to the first block, so relocation
// preserves alignment and never requires rewriting a block.
class VersionRunsOutput {
  OutputStream& out;
  size_t start;
  std::vector<VersionBlockDirectory> directory;

public:
  explicit VersionRunsOutput(OutputStream& out, uint32_t blocks = 0) : out(out) {
    directory.reserve(blocks);
    out.align(8);
    start = out.size();
  }
  void add(std::span<const char> block, uint32_t firstDoc) {
    directory.push_back({firstDoc, 0, out.size() - start});
    out.write(block.data(), block.size());
  }
  seg_location finish(uint32_t count) {
    if (!count) return {};
    uint64_t directoryOffset = out.size() - start;
    out.write(directory.data(), directory.size() * sizeof(VersionBlockDirectory));
    auto location = out.slocation();
    VersionRunsHeader header{VersionRuns::ENCODING, count, directoryOffset,
        out.size() - start + sizeof(VersionRunsHeader)};
    out.write(&header, sizeof(header));
    return location;
  }
};

class VersionRunBuilder {
  std::vector<VersionRun> tail{{0, 0}};
  struct SealedBlock {
    std::unique_ptr<char[]> data;
    uint32_t size;
    uint32_t firstDocId;
  };
  std::vector<SealedBlock> sealed;
  size_t sealedBytes = 0;
  std::function<void(std::span<const char>, uint32_t)> sink;
  VersionRun sealedLast{0, 0};
  uint32_t sealedCount = 0;
  bool sealedDense = true;
  bool tailDense = true;
  VersionAffinePrefix affine;

  static std::vector<char> encode(std::span<const VersionRun> runs) {
    uint32_t step = runs.size() < 2 ? 1 : runs[1].firstDocId - runs[0].firstDocId;
    for (size_t i = 2; i < runs.size(); i++) {
      if (runs[i].firstDocId - runs[i - 1].firstDocId != step) { step = 0; break; }
    }
    VersionBlockHeader header{{}, runs[0].firstDocId, step};
    std::vector<char> block(sizeof(header) + (step ? 0 : sizeof(NumBlockInfo)), 0);
    auto indices = std::views::iota((size_t)0, runs.size());
    auto values = indices | std::views::transform([runs](size_t i) { return (int64_t)runs[i].version; });
    header.versions = NumBlockCodec::append(block, values, true);
    if (!step) {
      auto values = indices | std::views::transform([runs](size_t i) { return (int64_t)runs[i].firstDocId; });
      auto starts = NumBlockCodec::append(block, values, false);
      memcpy(block.data() + sizeof(header), &starts, sizeof(starts));
    }
    memcpy(block.data(), &header, sizeof(header));
    block.resize((block.size() + LinearPack::TAIL_PAD + 7) & ~(size_t)7, 0);
    return block;
  }

  void seal() {
    assert(tail.size() == VersionRuns::BLOCK_SIZE);
    auto block = encode(tail);
    if (sink) {
      sink(block, tail.front().firstDocId);
    } else {
      auto storage = std::make_unique_for_overwrite<char[]>(block.size());
      memcpy(storage.get(), block.data(), block.size());
      // Allocation failures leave the still mutable tail intact.
      sealed.push_back({std::move(storage), (uint32_t)block.size(), tail.front().firstDocId});
      sealedBytes += block.size();
    }
    affine.append(VersionBlock(block.data(), (uint32_t)tail.size()), sealedCount);
    sealedDense &= tailDense && tail.front().firstDocId == sealedCount;
    sealedCount += (uint32_t)tail.size();
    sealedLast = tail.back();
    tail.clear();
    tailDense = true;
  }

public:
  struct Lookup {
    uint32_t block = 0, run = 0;
    uint32_t first = 0, end = 0;
    uint64_t version = 0;
  };
  explicit VersionRunBuilder(std::function<void(std::span<const char>, uint32_t)> sink = {})
      : sink(std::move(sink)) {}

  size_t size() const { return sealedCount + tail.size(); }
  size_t allocatedBytes() const { return sealedBytes + sealed.capacity() * sizeof(SealedBlock) + tail.capacity() * sizeof(VersionRun); }
  size_t flushWorkspaceBytes() const {
    return tail.size() * 40 + 256
        + ((size() + VersionRuns::BLOCK_SIZE - 1) / VersionRuns::BLOCK_SIZE) * sizeof(VersionBlockDirectory);
  }

  void add(uint32_t start, uint64_t version) {
    VersionRun last = tail.empty() ? sealedLast : tail.back();
    if (start < last.firstDocId) throw std::runtime_error("Request-version runs must increase");
    if (start == last.firstDocId) {
      assert(!tail.empty()); // Sealing requires a later, nonempty boundary.
      tail.back().version = version;
      if (size() > 1 && (tail.size() > 1 ? tail[tail.size() - 2].version : sealedLast.version) == version) tail.pop_back();
    } else if (last.version != version) {
      if (tail.size() == VersionRuns::BLOCK_SIZE) seal();
      tail.push_back({start, version});
      if (tail.size() > 1) tailDense &= start == tail.front().firstDocId + tail.size() - 1;
    }
  }

  LUXIR_INLINE uint64_t birth(uint32_t doc, Lookup& lookup) const {
    if (doc < affine.count) return affine.birth(doc);
    return lookupBirth(doc, lookup);
  }

private:
  LUXIR_NOINLINE uint64_t lookupBirth(uint32_t doc, Lookup& lookup) const {
    if (sealedDense && tailDense && (tail.empty() || tail.front().firstDocId == sealedCount)) {
      if (!tail.empty() && doc >= sealedCount) {
        return tail[std::min(doc - sealedCount, (uint32_t)tail.size() - 1)].version;
      }
      doc = std::min(doc, sealedCount - 1);
      VersionBlock block(sealed[doc / VersionRuns::BLOCK_SIZE].data.get(), VersionRuns::BLOCK_SIZE);
      return block.version(doc % VersionRuns::BLOCK_SIZE);
    }
    if (doc >= lookup.first && doc < lookup.end) return lookup.version;
    if (!tail.empty() && doc >= tail.front().firstDocId) {
      uint32_t index = tailDense ? std::min(doc - tail.front().firstDocId, (uint32_t)tail.size() - 1)
          : VersionBlock::find(doc, lookup.run, (uint32_t)tail.size(), [&](uint32_t i) { return tail[i].firstDocId; });
      lookup.run = index;
      lookup.first = tail[index].firstDocId;
      lookup.end = index + 1 < tail.size() ? tail[index + 1].firstDocId : doc + 1;
      return lookup.version = tail[index].version;
    }
    assert(!sink && !sealed.empty());
    lookup.block = sealedDense ? std::min(doc / VersionRuns::BLOCK_SIZE, (uint32_t)sealed.size() - 1)
        : VersionBlock::find(doc, lookup.block, (uint32_t)sealed.size(), [&](uint32_t i) { return sealed[i].firstDocId; });
    VersionBlock block(sealed[lookup.block].data.get(), VersionRuns::BLOCK_SIZE);
    lookup.run = block.find(doc, lookup.run);
    lookup.first = block.start(lookup.run);
    lookup.end = lookup.run + 1 < block.size() ? block.start(lookup.run + 1)
        : lookup.block + 1 < sealed.size() ? sealed[lookup.block + 1].firstDocId
        : !tail.empty() ? tail.front().firstDocId : doc + 1;
    return lookup.version = block.version(lookup.run);
  }

public:
  // The inverter keeps sealed blocks; a merge supplies a sink and retains only
  // this tail. Both paths encode precisely the same bytes.
  uint32_t finish(VersionRunsOutput& output, uint32_t maxDoc) const {
    if (!sink) {
      for (const auto& block : sealed) {
        output.add({block.data.get(), block.size}, block.firstDocId);
      }
    }
    size_t n = tail.size();
    if (n && tail.back().firstDocId == maxDoc) n--;
    if (n) {
      auto block = encode({tail.data(), n});
      output.add(block, tail.front().firstDocId);
    }
    return sealedCount + (uint32_t)n;
  }
};

} // namespace luxir
