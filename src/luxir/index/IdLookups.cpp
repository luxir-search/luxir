// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IdLookups.h"

#include <algorithm>
#include <oneapi/tbb/parallel_for.h>

#include "luxir/reader/SegmentCore.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/IndexReader.h"
#include "luxir/store/AcquireJoin.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/log.h"

namespace luxir {

IdFilterProbe::IdFilterProbe(SegmentCore& core, const DecodedFieldRecord& id, size_t lookups)
    : core(&core), part(id.idFilter()) {
  if (!part) return;
  // Blocks only pay off while they are few next to the filter: past an
  // eighth of it, holding them one by one costs more than the whole part.
  byBlock = !core.holdsFree(part.location.filenum()) && part.bytes > field_parts::WHOLE_READ_BYTES
      && part.bytes > IdFilter::HEADER_BYTES && lookups * IdFilter::BLOCK_BYTES * 8 < part.bytes;
}

bool IdFilterProbe::load() {
  if (!part) return true;
  if (byBlock) {
    filter = IdFilter::parseHeader(core->slice(header(), ReadClass::METADATA), part.bytes);
    if (!filter || filter->filterKind() == IdFilter::Kind::BLOOM) {
      loaded = true;
      return true;
    }
    // Another kind probes elsewhere than one block: read it whole.
    byBlock = false;
    filter.reset();
    if (!core->held(part)) return false;
  }
  filter = IdFilter::parse(core->slice(part));
  loaded = true;
  return true;
}

bool IdFilterProbe::mayContainHash(uint64_t hash) const {
  assert(loaded || !part);
  if (!filter) return true;
  if (!byBlock) return filter->mayContainHash(hash);
  auto block = core->slice(part.subSlice(IdFilter::bloomBlockOffset(hash, part.bytes), IdFilter::BLOCK_BYTES),
                           ReadClass::METADATA);
  return IdFilter::bloomBlockMayContain(block.data(), hash);
}

namespace {

// What one segment acquires in a round: its parts, deduplicated by range.
struct Declared {
  std::vector<SegmentCore::Part> parts;
  bool live = false;

  void add(SegmentCore& core, field_parts::Slice slice, ReadClass readClass) {
    if (slice && slice.bytes && !core.held(slice)) parts.push_back({slice, readClass});
  }

  void unique() {
    auto key = [](const SegmentCore::Part& part) { return std::pair(part.slice.location.raw(), part.slice.bytes); };
    std::ranges::stable_sort(parts, {}, key);
    auto [first, last] = std::ranges::unique(parts, {}, key);
    parts.erase(first, last);
  }

  uint64_t bytes() const noexcept {
    uint64_t total = 0;
    for (const auto& part : parts) total += part.slice.bytes;
    return total;
  }
};

class Rounds {
  std::span<SegmentIdLookups* const> segments;
  std::vector<Declared> declared;

public:
  uint64_t waited = 0;
  // Per segment, the ids its filter keeps.
  std::vector<std::vector<std::string_view>> kept;

  explicit Rounds(std::span<SegmentIdLookups* const> segments)
      : segments(segments), declared(segments.size()), kept(segments.size()) {}

  // Declares each segment's parts with `declare(i, segment, declared)`, in
  // parallel, then issues them as one batch per segment file and waits once.
  template <class Declare>
  void run(Declare&& declare) {
    oneapi::tbb::parallel_for(size_t(0), segments.size(), [&](size_t i) {
      declared[i] = {};
      declare(i, *segments[i], declared[i]);
      declared[i].unique();
    });
    bool issued = false;
    AcquireJoin join;
    try {
      for (size_t i = 0; i < segments.size(); ++i) {
        auto& segment = *segments[i];
        if (!declared[i].parts.empty()) {
          segment.core.acquireParts(declared[i].parts, join);
          issued = true;
        }
        if (declared[i].live && segment.live->acquireAhead(join)) issued = true;
      }
      join.wait();
    } catch (const std::exception& e) {
      // The lookups acquire on demand what the round left unheld.
      LOG_DEBUG("id lookups: a round acquired ahead failed; its parts are acquired on use: {}", e.what());
    }
    if (issued) ++waited;
  }
};

}  // namespace

uint64_t acquireIdLookups(std::span<SegmentIdLookups* const> segments) {
  if (segments.empty()) return 0;
  Rounds rounds(segments);

  // 1. The filters.
  rounds.run([](size_t, SegmentIdLookups& segment, Declared& out) {
    for (auto id : segment.ids) {
      segment.filter.parts(IdFilter::hash(id), [&](auto slice, auto readClass) { out.add(segment.core, slice, readClass); });
      if (!segment.filter.readsByBlock()) break;  // the whole part, once
    }
  });
  std::vector<bool> whole(segments.size());
  for (size_t i = 0; i < segments.size(); ++i) whole[i] = !segments[i]->filter.load();
  if (std::ranges::find(whole, true) != whole.end()) {
    rounds.run([&](size_t i, SegmentIdLookups& segment, Declared& out) {
      if (whole[i]) out.add(segment.core, segment.field.idFilter(), ReadClass::DATA);
    });
    for (size_t i = 0; i < segments.size(); ++i) {
      if (whole[i]) segments[i]->filter.load();
    }
  }
  oneapi::tbb::parallel_for(size_t(0), segments.size(), [&](size_t i) {
    auto& segment = *segments[i];
    for (auto id : segment.ids) {
      if (segment.filter.mayContain(id)) rounds.kept[i].push_back(id);
    }
  });

  // 2. Routing data, and dictionaries read whole.
  rounds.run([&](size_t i, SegmentIdLookups& segment, Declared& out) {
    if (rounds.kept[i].empty()) return;
    out.add(segment.core, TermsEnum::navigationPart(segment.field), ReadClass::METADATA);
    auto dictionary = segment.field.terms()->dictionaryBlocks();
    if (TermsEnum::readsWhole(segment.core, dictionary)) out.add(segment.core, dictionary, ReadClass::DATA);
  });

  // 3. The blocks the ids land in, with what applying a delete reads.
  rounds.run([&](size_t i, SegmentIdLookups& segment, Declared& out) {
    if (rounds.kept[i].empty()) return;
    auto guard = MemPool::threadLocalPoolGuard();
    TermsEnum terms(guard.pool(), segment.core.rootReader(), segment.field);
    terms.readAhead();
    for (auto id : rounds.kept[i]) out.add(segment.core, terms.seekPart(id), ReadClass::DATA);
    // Blocks covering most of the dictionary cost more held one by one.
    auto dictionary = segment.field.terms()->dictionaryBlocks();
    out.unique();
    if (out.bytes() > dictionary.bytes / 2) {
      out.parts.clear();
      out.add(segment.core, dictionary, ReadClass::DATA);
    }
    out.add(segment.core, segment.core.versionRunsPart(), ReadClass::METADATA);
    out.live = segment.live != nullptr;
  });

  // 4. Postings not inlined in their block.
  rounds.run([&](size_t i, SegmentIdLookups& segment, Declared& out) {
    if (rounds.kept[i].empty()) return;
    auto guard = MemPool::threadLocalPoolGuard();
    TermsEnum terms(guard.pool(), segment.core.rootReader(), segment.field);
    terms.readAhead();
    for (auto id : rounds.kept[i]) {
      if (terms.probe(id)) terms.postingsParts([&](field_parts::Slice part) { out.add(segment.core, part, ReadClass::DATA); });
    }
  });
  return rounds.waited;
}

} // namespace luxir
