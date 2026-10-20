// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <assert.h>
#include <cstring>
#include <initializer_list>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>

namespace luxir {

// Fuzzy distance counts edits over code-point units. A well-formed UTF-8
// sequence is one unit holding its code point; every other byte (a stray
// continuation, a bad lead, a sequence cut short) is a unit of its own,
// INVALID_UNIT | byte, which no code point can equal. Decoding is greedy and
// injective, so equal units mean equal bytes.
namespace fuzzy_units {

inline constexpr uint32_t INVALID_UNIT = 0x80000000u;
// No unit equals it: the "previous unit" before the first one.
inline constexpr uint32_t NO_UNIT = 0xffffffffu;

inline bool isLead(uint8_t b) { return b >= 0xc2 && b <= 0xf4; }

inline int sequenceLength(uint8_t lead) { return lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4; }

// Bytes allowed at continuation position `pos` (1-based) after `lead`, per
// the Unicode well-formed sequence table: no overlongs, surrogates, or code
// points past U+10FFFF.
inline void continuationRange(uint8_t lead, int pos, uint8_t& lo, uint8_t& hi) {
  lo = 0x80;
  hi = 0xbf;
  if (pos != 1) return;
  if (lead == 0xe0) lo = 0xa0;
  else if (lead == 0xed) hi = 0x9f;
  else if (lead == 0xf0) lo = 0x90;
  else if (lead == 0xf4) hi = 0x8f;
}

inline uint32_t decodeSequence(const uint8_t* bytes, int len) {
  uint32_t cp = (uint32_t)bytes[0] & (len == 2 ? 0x1fu : len == 3 ? 0x0fu : 0x07u);
  for (int i = 1; i < len; i++) cp = (cp << 6) | ((uint32_t)bytes[i] & 0x3fu);
  return cp;
}

// Length of the well-formed sequence starting at s[i], or 0 when s[i] is a
// unit of its own (ASCII is length 1).
inline int sequenceAt(std::string_view s, size_t i) {
  uint8_t b = (uint8_t)s[i];
  if (b < 0x80) return 1;
  if (!isLead(b)) return 0;
  int len = sequenceLength(b);
  if (i + (size_t)len > s.size()) return 0;
  for (int pos = 1; pos < len; pos++) {
    uint8_t lo, hi;
    continuationRange(b, pos, lo, hi);
    uint8_t c = (uint8_t)s[i + (size_t)pos];
    if (c < lo || c > hi) return 0;
  }
  return len;
}

// Writes the units of `s` to `out` (room for s.size()) and returns how many.
inline int decode(std::string_view s, uint32_t* out) {
  int n = 0;
  size_t i = 0;
  while (i < s.size()) {
    int len = sequenceAt(s, i);
    if (len <= 1) {
      out[n++] = (len == 0 ? INVALID_UNIT : 0) | (uint32_t)(uint8_t)s[i];
      i++;
    } else {
      out[n++] = decodeSequence((const uint8_t*)s.data() + i, len);
      i += (size_t)len;
    }
  }
  return n;
}

inline int count(std::string_view s) {
  int n = 0;
  for (size_t i = 0; i < s.size(); n++) {
    int len = sequenceAt(s, i);
    i += len == 0 ? 1 : (size_t)len;
  }
  return n;
}

// Byte length of the first `units` units of `s` (all of it when shorter).
inline size_t prefixBytes(std::string_view s, int units) {
  size_t i = 0;
  for (int n = 0; n < units && i < s.size(); n++) {
    int len = sequenceAt(s, i);
    i += len == 0 ? 1 : (size_t)len;
  }
  return i;
}

// The bytes of one unit; returns the length.
inline int encode(uint32_t u, uint8_t* out) {
  if ((u & INVALID_UNIT) != 0 || u < 0x80) {
    out[0] = (uint8_t)u;
    return 1;
  }
  if (u < 0x800) {
    out[0] = (uint8_t)(0xc0 | (u >> 6));
    out[1] = (uint8_t)(0x80 | (u & 0x3f));
    return 2;
  }
  if (u < 0x10000) {
    out[0] = (uint8_t)(0xe0 | (u >> 12));
    out[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3f));
    out[2] = (uint8_t)(0x80 | (u & 0x3f));
    return 3;
  }
  out[0] = (uint8_t)(0xf0 | (u >> 18));
  out[1] = (uint8_t)(0x80 | ((u >> 12) & 0x3f));
  out[2] = (uint8_t)(0x80 | ((u >> 6) & 0x3f));
  out[3] = (uint8_t)(0x80 | (u & 0x3f));
  return 4;
}

} // namespace fuzzy_units

// Banded Levenshtein automaton for maxEdits <= 2 over code-point units, with
// optional transpositions (optimal string alignment: an adjacent swap is one
// edit, as in Lucene's LevenshteinAutomata(withTranspositions)). It is driven
// one byte at a time: the bytes of an unfinished UTF-8 sequence wait in the
// state and the distance rows advance only when a unit completes. Bytes still
// waiting when the term ends count as invalid units.
class LevenshteinAutomaton final {
public:
  static constexpr int DEAD = -1;

  struct State {
    uint32_t last;          // the unit that produced this row, for transpositions
    uint16_t j;             // units consumed
    uint8_t cells[5];       // row j over the band
    uint8_t prevCells[5];   // row j - 1 over its band, for transpositions
    bool prefixMatched;
    uint8_t bestPrefixDist;
    uint8_t pendingLen;     // bytes of an unfinished sequence
    uint8_t pending[3];
  };

private:
  const uint32_t* query;
  int k;
  int n;
  bool prefixMode;
  bool transpositions;

  uint8_t dead() const { return (uint8_t)(k + 1); }

  uint8_t clamp(int v) const {
    return (uint8_t)(v > k + 1 ? k + 1 : v);
  }

  void fillDead(uint8_t* cells) const {
    uint8_t v = dead();
    for (int i = 0; i < 5; i++) cells[i] = v;
  }

  int index(const State& s, int i) const {
    return i - ((int)s.j - k);
  }

  int cell(const State& s, int i) const {
    if (i < 0 || i > n) return k + 1;
    int idx = index(s, i);
    if (idx < 0 || idx > 2 * k) return k + 1;
    return (int)s.cells[idx];
  }

  // Row j - 1 (only meaningful once a unit has been consumed).
  int prevCell(const State& s, int i) const {
    if (s.j == 0 || i < 0 || i > n) return k + 1;
    int idx = i - ((int)s.j - 1 - k);
    if (idx < 0 || idx > 2 * k) return k + 1;
    return (int)s.prevCells[idx];
  }

  void setCell(State& s, int i, int v) const {
    if (i < 0 || i > n) return;
    int idx = index(s, i);
    if (idx < 0 || idx > 2 * k) return;
    s.cells[idx] = clamp(v);
  }

  int minCell(const State& s) const {
    int best = k + 1;
    for (int i = 0; i <= 2 * k; i++) best = std::min(best, (int)s.cells[i]);
    return best;
  }

  // Every unit keeps the state live: no need to know which.
  bool wideOpen(const State& s) const {
    return s.prefixMatched || minCell(s) < k;
  }

  // Advance the distance rows by one complete unit.
  State consume(const State& s, uint32_t c) const {
    assert(s.j < UINT16_MAX);
    State d;
    d.last = c;
    d.j = (uint16_t)(s.j + 1);
    fillDead(d.cells);
    memcpy(d.prevCells, s.cells, sizeof(d.prevCells));
    d.prefixMatched = false;
    d.bestPrefixDist = dead();
    d.pendingLen = 0;

    int lo = std::max(0, (int)d.j - k);
    int hi = std::min(n, (int)d.j + k);
    for (int i = lo; i <= hi; i++) {
      int v;
      if (i == 0) {
        v = (int)d.j;
      } else {
        int sub = cell(s, i - 1) + (c == query[i - 1] ? 0 : 1);
        int ins = cell(s, i) + 1;
        int del = cell(d, i - 1) + 1;
        v = std::min({sub, ins, del});
        if (transpositions && i >= 2 && c == query[i - 2] && s.last == query[i - 1]) {
          v = std::min(v, prevCell(s, i - 2) + 1);
        }
      }
      setCell(d, i, v);
    }

    if (prefixMode) {
      int dist = cell(d, n);
      int best = std::min((int)s.bestPrefixDist, dist);
      d.prefixMatched = s.prefixMatched || dist <= k;
      d.bestPrefixDist = clamp(best);
    }
    return d;
  }

  // A byte that begins a unit (no sequence is waiting).
  State begin(const State& s, uint8_t b) const {
    if (b < 0x80) return consume(s, b);
    if (!fuzzy_units::isLead(b)) return consume(s, fuzzy_units::INVALID_UNIT | b);
    State d = s;
    d.pending[0] = b;
    d.pendingLen = 1;
    return d;
  }

  // Settle the waiting bytes as invalid units, one each.
  State flush(const State& s) const {
    State d = s;
    for (int i = 0; i < (int)s.pendingLen; i++) {
      d = consume(d, fuzzy_units::INVALID_UNIT | s.pending[i]);
    }
    return d;
  }

  // f(unit) for each unit that keeps a narrow (not wideOpen) live state live:
  // a query unit at a cell already at k, or the second half of a swap.
  template <typename F>
  void forEachLiveUnit(const State& s, F&& f) const {
    int lo = std::max(0, (int)s.j - k);
    int hi = std::min(n - 1, (int)s.j + k);
    for (int i = lo; i <= hi; i++) {
      if (cell(s, i) == k) f(query[i]);
    }
    if (!transpositions || s.j == 0) return;
    for (int i = std::max(2, (int)s.j + 1 - k); i <= std::min(n, (int)s.j + 1 + k); i++) {
      if (query[i - 1] == s.last && prevCell(s, i - 2) < k) f(query[i - 2]);
    }
  }

  int nextLiveSettled(const State& s, int b0) const {
    if (b0 > 255 || !canMatch(s)) return DEAD;
    if (wideOpen(s)) return b0;
    int answer = DEAD;
    forEachLiveUnit(s, [&](uint32_t u) {
      uint8_t bytes[4];
      fuzzy_units::encode(u, bytes);
      int b = (int)bytes[0];
      if (b >= b0 && (answer == DEAD || b < answer)) answer = b;
    });
    return answer;
  }

  // The next byte either continues the waiting sequence toward a live unit,
  // or breaks it (the waiting bytes settle as invalid units) and begins a new
  // unit from there.
  int nextLivePending(const State& s, int b0) const {
    uint8_t lo, hi;
    fuzzy_units::continuationRange(s.pending[0], (int)s.pendingLen, lo, hi);
    int answer = DEAD;
    if (b0 <= (int)hi) {
      if (wideOpen(s)) {
        answer = std::max(b0, (int)lo);
      } else {
        forEachLiveUnit(s, [&](uint32_t u) {
          uint8_t bytes[4];
          int len = fuzzy_units::encode(u, bytes);
          if (len <= (int)s.pendingLen || memcmp(bytes, s.pending, s.pendingLen) != 0) return;
          int b = (int)bytes[s.pendingLen];
          if (b >= b0 && (answer == DEAD || b < answer)) answer = b;
        });
      }
    }
    State settled = flush(s);
    int other = nextLiveSettled(settled, b0);
    if (other != DEAD && other >= (int)lo && other <= (int)hi) {
      other = nextLiveSettled(settled, (int)hi + 1);
    }
    if (other != DEAD && (answer == DEAD || other < answer)) answer = other;
    return answer;
  }

public:
  // `query` holds the n units of the fuzzy term and must outlive the
  // automaton and its copies.
  LevenshteinAutomaton(std::span<const uint32_t> query, int maxEdits,
                       bool prefixMode = false, bool transpositions = true)
    : query(query.data()), k(maxEdits), n((int)query.size()), prefixMode(prefixMode),
      transpositions(transpositions) {
    assert(k >= 0 && k <= 2);
  }

  State start() const {
    State s;
    s.last = fuzzy_units::NO_UNIT;
    s.j = 0;
    fillDead(s.cells);
    fillDead(s.prevCells);
    s.prefixMatched = false;
    s.bestPrefixDist = dead();
    s.pendingLen = 0;
    memset(s.pending, 0, sizeof(s.pending));
    for (int i = 0; i <= n && i <= k; i++) setCell(s, i, i);
    if (prefixMode) {
      int dist = cell(s, n);
      s.prefixMatched = dist <= k;
      s.bestPrefixDist = (uint8_t)dist;
    }
    return s;
  }

  State step(const State& s, uint8_t b) const {
    if (s.pendingLen == 0) [[likely]] {
      if (b < 0x80) [[likely]] return consume(s, b);
      return begin(s, b);
    }
    uint8_t lead = s.pending[0];
    uint8_t lo, hi;
    fuzzy_units::continuationRange(lead, (int)s.pendingLen, lo, hi);
    if (b >= lo && b <= hi) {
      if ((int)s.pendingLen + 1 == fuzzy_units::sequenceLength(lead)) {
        uint8_t bytes[4];
        memcpy(bytes, s.pending, s.pendingLen);
        bytes[s.pendingLen] = b;
        return consume(s, fuzzy_units::decodeSequence(bytes, (int)s.pendingLen + 1));
      }
      State d = s;
      d.pending[d.pendingLen++] = b;
      return d;
    }
    return begin(flush(s), b);
  }

  // Waiting bytes do not move the rows, so this is a bound for them: the rows
  // can still reach a match whatever unit the bytes become.
  bool canMatch(const State& s) const {
    return s.prefixMatched || minCell(s) <= k;
  }

  bool isMatch(const State& s) const {
    if (s.pendingLen != 0) [[unlikely]] return isMatch(flush(s));
    return prefixMode ? s.prefixMatched : cell(s, n) <= k;
  }

  int matchDistance(const State& s) const {
    if (s.pendingLen != 0) [[unlikely]] return matchDistance(flush(s));
    return prefixMode ? (int)s.bestPrefixDist : cell(s, n);
  }

  // Units in the bytes stepped so far, waiting bytes counted one each.
  int units(const State& s) const {
    return (int)s.j + (int)s.pendingLen;
  }

  // The smallest byte >= b0 from which some continuation can still match, or
  // DEAD. It may answer lower than that (never higher): a seek only skips
  // what it proves dead.
  int nextLiveByte(const State& s, int b0) const {
    if (b0 < 0) b0 = 0;
    if (b0 > 255 || !canMatch(s)) return DEAD;
    if (s.pendingLen == 0) [[likely]] return nextLiveSettled(s, b0);
    return nextLivePending(s, b0);
  }

};

} // namespace luxir
