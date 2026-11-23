// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "luxir/query/Query.h"
#include "luxir/util/MemPool.h"

namespace luxir {

class AcquireBatches;
class AcquireRound;
class Schema;
class TermsEnum;
namespace api { struct SearchRequest; }

/// The parts a search declares in each segment before it reads them, so that
/// a backend where an acquire is a request can merge nearby ranges and issue
/// them together. The search waits on a few rounds instead of one acquire per
/// part per segment, and each round is one batch per segment file, issued
/// together across segments:
///
///  1. Before parse: the routing data of every term lookup the request's
///     queries make in every segment (term offsets and trie), and the live
///     docs of segments with deletes. A dictionary a lookup reads whole (one
///     at most field_parts::WHOLE_READ_BYTES, or one a multi-term query
///     scans) goes in this round too.
///  2. The term block each lookup lands in.
///  3. The postings of the terms found.
///  4. After planning: what the weights registered (PlannedRead), such as
///     the norms of a scored field in the segments holding its term.
///
/// A round declares only parts the segment does not hold, so a part held
/// already, or declared whole in an earlier round, merges rounds: a small
/// dictionary makes it nav and blocks, then postings. Files with free holds
/// (RAM, mmap) are never declared; a reader whose files all have them makes
/// no rounds at all. A read nothing declared, or one past the byte cap, is
/// acquired on demand when it happens, as without declares.
///
/// The parts are exactly the ranges the readers acquire (TermsEnum::seekPart,
/// TermsEnum::postingsParts, ...), since holds are keyed by range.
class DeclaredReads {
public:
  // A term lookup the request makes in one segment.
  struct Lookup {
    enum class Mode : uint8_t {
      TERM,   // seeks `term`: routing data, its block, then its postings
      SCAN,   // enumerates terms (prefix, fuzzy, pattern): the whole dictionary
      ROUTE,  // seeks terms the walk does not know: the routing data only
    };
    Segment* segment;
    const DecodedFieldRecord* record;
    Mode mode;
    std::string term;
    TermsEnum* terms = nullptr;  // built once the routing data is held
    bool many = false;  // one of many lookups on the field: holds it whole, as TermsEnum does
    bool done = false;
  };

private:
  IndexReader& reader;
  const uint64_t maxBytes;
  uint64_t bytes = 0;    // declared so far, against maxBytes
  uint64_t dropped = 0;  // left to on-demand acquires by the cap
  bool planned = false;
  bool liveDeclared = false;
  int termRounds = 0;
  MemPool pool;
  std::vector<Lookup> lookups;
  PlannedReads registered;

  struct Round;
  void advance(Lookup& lookup, Round& round);
  void declarePlanned(const PlannedRead& read, Round& round);

public:
  // Collects the term lookups that parsing `request` will make, with their
  // terms where the request names them. maxBytes caps what all rounds
  // declare together; zero is unlimited.
  DeclaredReads(IndexReader& reader, const Schema& schema, const api::SearchRequest& request, uint64_t maxBytes);
  ~DeclaredReads();

  /// Whether a search over `reader` declares anything: some segment file does
  /// not hand out free views.
  static bool needed(IndexReader& reader);

  /// Where the weights of the request register their reads while parsing.
  PlannedReads& plannedReads() noexcept { return registered; }

  /// Parsing is done: the next rounds declare what the weights registered.
  void plan() noexcept { planned = true; }

  /// Declares the next round into `round` and keeps its segments alive
  /// there; false when there is nothing left to read ahead at this stage.
  bool next(AcquireRound& round);

  uint64_t declaredBytes() const noexcept { return bytes; }
  uint64_t droppedBytes() const noexcept { return dropped; }
  std::span<const Lookup> termLookups() const noexcept { return lookups; }
};

}  // namespace luxir
