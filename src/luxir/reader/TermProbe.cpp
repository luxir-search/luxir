// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TermProbe.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>

#include "FieldRecordCodec.h"
#include "SegmentCore.h"

namespace luxir {

void TermProbe::Round::add(field_parts::Slice slice, ReadClass readClass) {
  if (!slice || slice.bytes == 0) return;
  byFile[slice.location.filenum()].push_back({slice.location.offset(), slice.bytes, readClass});
}

uint64_t TermProbe::Round::bytes() const noexcept {
  uint64_t total = 0;
  for (const auto& [_, ranges] : byFile) {
    for (const auto& range : ranges) total += range.length;
  }
  return total;
}

void TermProbe::Round::fetch(SegmentCore& core, ReadDeadline deadline) {
  struct Pending {
    std::promise<void> done;
    std::atomic<bool> once{false};
  };
  std::vector<std::shared_ptr<Pending>> pending;
  for (auto& [number, ranges] : byFile) {
    std::ranges::sort(ranges, [](const ByteRange& a, const ByteRange& b) { return a.offset < b.offset; });
    size_t kept = 0;
    for (const auto& range : ranges) {
      if (kept && ranges[kept - 1].offset + ranges[kept - 1].length >= range.offset) {
        auto& last = ranges[kept - 1];
        last.length = std::max(last.offset + last.length, range.offset + range.length) - last.offset;
        if (range.readClass == ReadClass::DATA) last.readClass = ReadClass::DATA;
      } else {
        ranges[kept++] = range;
      }
    }
    ranges.resize(kept);
    auto wait = std::make_shared<Pending>();
    pending.push_back(wait);
    auto finish = [wait](std::exception_ptr failure) {
      if (wait->once.exchange(true)) return;
      if (failure && !isFetchNotResident(failure)) wait->done.set_exception(std::move(failure));
      else wait->done.set_value();
    };
    try {
      core.getFile(number)->fetchRanges(ranges, deadline, finish);
    } catch (...) {
      finish(std::current_exception());
    }
  }
  // Every fetch finishes before any error propagates: the ranges' spans and
  // the files must outlive them.
  std::exception_ptr first;
  for (auto& wait : pending) {
    try {
      wait->done.get_future().get();
    } catch (...) {
      if (!first) first = std::current_exception();
    }
  }
  if (first) std::rethrow_exception(first);
}

std::span<const char> TermProbe::hold(field_parts::Slice slice, ReadClass readClass) {
  std::lock_guard lock(holdsMutex);
  auto key = std::pair(slice.location.raw(), slice.bytes);
  auto found = holds.lower_bound(key);
  if (found != holds.end() && found->first == key) return found->second.bytes();
  auto held = core.getFile(slice.location.filenum())->acquire({slice.location.offset(), slice.bytes, readClass});
  return holds.emplace_hint(found, key, std::move(held))->second.bytes();
}

bool TermProbe::saves(SegmentCore& core, const DecodedFieldRecord& field) {
  const auto* terms = field.terms();
  return terms && core.getFile(terms->location.filenum())->lazy();
}

TermProbe::TermProbe(PostingsReader& reader, const DecodedFieldRecord& field, ReadDeadline deadline)
    : core(reader.core()), reader(reader), field(field), deadline(deadline) {
  const auto* terms = field.terms();
  if (!terms) return;
  auto offsets = field.termBlockOffsets();
  auto trie = field_parts::termTrie(offsets, terms->trieBytes);
  Round round;
  round.add({offsets.location, trie.end().offset() - offsets.location.offset()}, ReadClass::METADATA);
  round.fetch(core, deadline);
}

void TermProbe::fetchBlocks(TermsEnum& terms, std::span<const std::string_view> targets) {
  Round round;
  for (auto target : targets) round.add(terms.blockSlice(target), ReadClass::METADATA);
  round.fetch(core, deadline);
}

void TermProbe::fetchPostings(std::span<const field_parts::Slice> postings) {
  Round round;
  for (const auto& slice : postings) round.add(slice, ReadClass::DATA);
  round.fetch(core, deadline);
}

} // namespace luxir
