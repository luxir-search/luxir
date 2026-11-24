// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "luxir/reader/FieldParts.h"
#include "luxir/reader/IdFilter.h"
#include "luxir/store/HeldRange.h"

namespace luxir {

class DecodedFieldRecord;
class SegmentCore;
class SegmentLive;

/// How the id lookups of one segment read its id filter, the FILTER part of
/// the root id field. A filter at most field_parts::WHOLE_READ_BYTES, one on
/// a file with free holds, or one the lookups would mostly cover anyway, is
/// read whole. A larger one is read at its header and the one BLOOM block
/// each id hashes to, BLOCK_BYTES each.
class IdFilterProbe {
  SegmentCore* core = nullptr;
  field_parts::Slice part;
  std::optional<IdFilter> filter;  // after load(); header only when read by block
  bool byBlock = false;
  bool loaded = false;

public:
  /// No filter: every id may be present.
  IdFilterProbe() = default;
  /// The filter of `id`, the root id field, for `lookups` ids.
  IdFilterProbe(SegmentCore& core, const DecodedFieldRecord& id, size_t lookups);

  bool readsByBlock() const noexcept { return byBlock; }

  /// The parts a probe of `hash` reads, for acquiring them ahead of load():
  /// the header and its block when read by block, else the whole part.
  template <class Out>
  void parts(uint64_t hash, Out&& out) const {
    if (!part) return;
    if (!byBlock) {
      out(part, ReadClass::METADATA);
      return;
    }
    out(header(), ReadClass::METADATA);
    out(part.subSlice(IdFilter::bloomBlockOffset(hash, part.bytes), IdFilter::BLOCK_BYTES), ReadClass::METADATA);
  }

  /// Reads the filter, or its header when read by block. False when a filter
  /// read by block turns out not to be BLOOM, or its header does not parse:
  /// it is then read whole, and the next load() acquires the whole part
  /// unless it is held by then. A part that does not parse throws
  /// SegmentCorruptError, as SegmentCore::idFilter() does.
  bool load();

  /// After load(): false when the filter proves `hash` absent. Safe to call
  /// from several threads.
  bool mayContainHash(uint64_t hash) const;
  bool mayContain(std::string_view id) const { return mayContainHash(IdFilter::hash(id)); }

private:
  field_parts::Slice header() const noexcept { return part.subSlice(0, IdFilter::HEADER_BYTES); }
};

/// The id lookups a delete application makes in one segment: the ids its
/// bounds keep, its filter, and the live docs generation they read.
struct SegmentIdLookups {
  SegmentCore& core;
  const DecodedFieldRecord& field;  // the root id field, with terms
  IdFilterProbe filter;
  std::vector<std::string_view> ids;  // in dictionary order
  std::shared_ptr<SegmentLive> live;  // the current generation, null without deletes
};

/// Acquires what the id lookups in `segments` read, ahead of them, in rounds
/// that each wait once on one batch per segment file, issued across segments
/// together:
///   1. the filter (whole, or its header and the blocks the ids hash to);
///   2. for the segments with an id the filter keeps: the term routing data,
///      and the dictionary when it is read whole (TermsEnum::readsWhole);
///   3. the term block each kept id lands in, the version runs and the live
///      docs;
///   4. the postings of the ids found, unless inlined in their block (a
///      pulsed term, which an id with one document is). Skipped when none
///      has postings of its own.
/// A filter read by block that is not BLOOM adds a round for its whole part.
/// So a batch costs at most five waits however many ids and segments it
/// looks up. A failed batch fails nothing: the lookups acquire what it left
/// unheld on demand, and fail as they would have. Lookups then read held
/// parts only, through an enum in TermsEnum::readAhead() mode.
/// Returns the rounds it waited on.
uint64_t acquireIdLookups(std::span<SegmentIdLookups* const> segments);

} // namespace luxir
