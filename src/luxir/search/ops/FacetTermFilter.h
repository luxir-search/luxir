// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>

#include "luxir/query/TermPattern.h"
#include "luxir/search/OrdMapStr.h"

namespace luxir {

// FieldFacet.term_filter with the field's query-time normalization applied.
// The parser builds it (and raises every request error); FacetOrdFilter
// resolves it against the facet's global ords.
struct FacetTermFilterSpec {
  std::optional<std::string> prefix;
  std::optional<TermPattern> regex;
  std::optional<TermPattern> excludeRegex;
  std::optional<std::vector<std::string>> include;
  std::vector<std::string> exclude;
  uint32_t partition = 0;
  uint32_t numPartitions = 0;
};

// Which global ords of a string facet may become ordinary buckets. Counting is
// unaffected; only selection consults it. Two tiers:
// - The ord tier: sorted, disjoint ord intervals, resolved once per request
//   from the prefix, the include and exclude lists, and the literal shapes of
//   both regexes (global ords follow term order, so a prefix is one interval).
// - The term tier: the tests that need a term's bytes (a general regex,
//   a general exclude_regex, the partition hash). It runs only on ords the ord
//   tier keeps, behind the page floor where there is one, and otherwise in
//   ascending ord order so term lookups advance through the dictionary.
class FacetOrdFilter {
public:
  struct Interval {
    int64_t lo;
    int64_t hi;  // exclusive
  };

private:
  std::vector<Interval> intervals;
  std::optional<TermPattern> regex;         // NORMAL patterns only
  std::optional<TermPattern> excludeRegex;  // NORMAL patterns only
  uint32_t partition = 0;
  uint32_t numPartitions = 0;

  static std::vector<Interval> intersect(std::span<const Interval> a, std::span<const Interval> b) {
    std::vector<Interval> out;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      int64_t lo = std::max(a[i].lo, b[j].lo);
      int64_t hi = std::min(a[i].hi, b[j].hi);
      if (lo < hi) out.push_back({lo, hi});
      if (a[i].hi < b[j].hi) i++; else j++;
    }
    return out;
  }

  void keepOnly(std::vector<Interval> with) { intervals = intersect(intervals, with); }

  // Subtracts every hole in one merge pass, so a long exclude list costs
  // O(n log n) rather than one rebuild of the intervals per term.
  void removeAll(std::vector<Interval> holes) {
    std::sort(holes.begin(), holes.end(),
              [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
    std::vector<Interval> merged;
    for (const auto& hole : holes) {
      if (!merged.empty() && hole.lo <= merged.back().hi) {
        merged.back().hi = std::max(merged.back().hi, hole.hi);
      } else if (hole.lo < hole.hi) {
        merged.push_back(hole);
      }
    }
    std::vector<Interval> out;
    out.reserve(intervals.size() + merged.size());
    size_t h = 0;
    for (Interval rest : intervals) {
      while (h < merged.size() && merged[h].hi <= rest.lo) h++;
      for (size_t i = h; i < merged.size() && merged[i].lo < rest.hi; i++) {
        if (merged[i].lo > rest.lo) out.push_back({rest.lo, merged[i].lo});
        rest.lo = merged[i].hi;
      }
      if (rest.lo < rest.hi) out.push_back(rest);
    }
    intervals = std::move(out);
  }

  static Interval range(std::pair<int64_t, int64_t> bounds) { return {bounds.first, bounds.second}; }

  static std::optional<Interval> exact(OrdMapStr& lookup, std::string_view term) {
    auto ord = lookup.strToOrd(term);
    if (!ord) return std::nullopt;
    return Interval{*ord, *ord + 1};
  }

public:
  // Null when the filter keeps every ord, so unfiltered selection (and the
  // whole-index top-terms shortcut) stays in force.
  static std::optional<FacetOrdFilter> resolve(FacetTermFilterSpec spec, OrdMapStr& lookup,
                                               int64_t numOrds) {
    if (numOrds == 0) return std::nullopt;
    FacetOrdFilter filter;
    filter.intervals.push_back({0, numOrds});
    if (spec.prefix) filter.keepOnly({range(lookup.prefixRange(*spec.prefix))});
    if (spec.regex) {
      TermPattern& pattern = *spec.regex;
      if (pattern.fittedPrefix) {
        // As the regex query does: every term with the kept prefix.
        filter.keepOnly({range(lookup.prefixRange(*pattern.fittedPrefix))});
      } else {
        switch (pattern.kind) {
          case automaton::ByteDfaKind::NONE: filter.intervals.clear(); break;
          case automaton::ByteDfaKind::ALL: break;
          case automaton::ByteDfaKind::SINGLE: {
            auto term = exact(lookup, pattern.literal);
            filter.keepOnly(term ? std::vector<Interval>{*term} : std::vector<Interval>{});
            break;
          }
          case automaton::ByteDfaKind::PREFIX:
            filter.keepOnly({range(lookup.prefixRange(pattern.literal))});
            break;
          case automaton::ByteDfaKind::NORMAL:
            if (!pattern.literal.empty()) {
              filter.keepOnly({range(lookup.prefixRange(pattern.literal))});
            }
            filter.regex = std::move(pattern);
            break;
        }
      }
    }
    if (spec.include) {
      std::vector<int64_t> ords;
      for (const auto& term : *spec.include) {
        if (auto ord = lookup.strToOrd(term)) ords.push_back(*ord);
      }
      std::sort(ords.begin(), ords.end());
      ords.erase(std::unique(ords.begin(), ords.end()), ords.end());
      std::vector<Interval> units;
      for (int64_t ord : ords) {
        if (!units.empty() && units.back().hi == ord) units.back().hi++;
        else units.push_back({ord, ord + 1});
      }
      filter.keepOnly(std::move(units));
    }
    std::vector<Interval> holes;
    for (const auto& term : spec.exclude) {
      if (auto hole = exact(lookup, term)) holes.push_back(*hole);
    }
    if (spec.excludeRegex) {
      // No fitted over-approximation here: it would drop terms the pattern
      // does not match. A general pattern runs on the indexed bytes instead.
      TermPattern& pattern = *spec.excludeRegex;
      switch (pattern.kind) {
        case automaton::ByteDfaKind::NONE: break;
        case automaton::ByteDfaKind::ALL: filter.intervals.clear(); break;
        case automaton::ByteDfaKind::SINGLE:
          if (auto hole = exact(lookup, pattern.literal)) holes.push_back(*hole);
          break;
        case automaton::ByteDfaKind::PREFIX:
          holes.push_back(range(lookup.prefixRange(pattern.literal)));
          break;
        case automaton::ByteDfaKind::NORMAL: filter.excludeRegex = std::move(pattern); break;
      }
    }
    if (!holes.empty()) filter.removeAll(std::move(holes));
    if (spec.numPartitions > 1) {
      filter.partition = spec.partition;
      filter.numPartitions = spec.numPartitions;
    }
    if (filter.intervals.empty()) {
      filter.regex.reset();
      filter.excludeRegex.reset();
      filter.numPartitions = 0;
    }
    bool everyOrd = filter.intervals.size() == 1 && filter.intervals[0].lo == 0
        && filter.intervals[0].hi == numOrds;
    if (everyOrd && !filter.hasTermTier()) return std::nullopt;
    return filter;
  }

  // Stable across segments, merges and processes, so partitions never move.
  static uint64_t partitionHash(std::string_view term) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char byte : term) {
      hash ^= byte;
      hash *= 0x100000001b3ULL;
    }
    return hash;
  }

  std::span<const Interval> ranges() const { return intervals; }

  int64_t width() const {
    int64_t total = 0;
    for (const auto& interval : intervals) total += interval.hi - interval.lo;
    return total;
  }

  bool hasTermTier() const { return regex || excludeRegex || numPartitions != 0; }

  bool containsOrd(int64_t ord) const {
    auto it = std::upper_bound(intervals.begin(), intervals.end(), ord,
                               [](int64_t key, const Interval& interval) { return key < interval.hi; });
    return it != intervals.end() && it->lo <= ord;
  }

  // The term tier, for a term whose ord passed the ord tier.
  bool acceptsTerm(std::string_view term) const {
    if (regex) {
      // The ord tier already holds every candidate to the literal prefix.
      automaton::ByteDfaView dfa = regex->dfa.view();
      auto state = regex->afterLiteral;
      for (size_t i = regex->literal.size(); i < term.size(); i++) {
        state = dfa.step(state, (uint8_t)term[i]);
        if (state == automaton::ByteDfaView::DEAD) return false;
      }
      if (!dfa.isMatch(state)) return false;
    }
    if (excludeRegex && excludeRegex->dfa.matches(term)) return false;
    if (numPartitions != 0 && partitionHash(term) % numPartitions != partition) return false;
    return true;
  }

  // Verdicts for a selection site that tests every candidate, with no page
  // floor to spare it the term tier: the tier runs once over the ord-tier
  // survivors among the site's keys, in ascending ord order.
  class Selection {
    const FacetOrdFilter* filter;
    boost::unordered_flat_set<int64_t> accepted;
    friend class FacetOrdFilter;

  public:
    explicit Selection(const FacetOrdFilter* filter) : filter(filter) {}
    bool operator()(int64_t ord) const {
      return filter->hasTermTier() ? accepted.contains(ord) : filter->containsOrd(ord);
    }
  };

  // forEachKey(visit) calls visit(ord) for each candidate key.
  template <typename ForEachKey>
  Selection select(ForEachKey&& forEachKey, OrdMapStr& labels) const {
    Selection selection(this);
    if (!hasTermTier()) return selection;
    std::vector<int64_t> survivors;
    forEachKey([&](int64_t ord) {
      if (containsOrd(ord)) survivors.push_back(ord);
    });
    std::sort(survivors.begin(), survivors.end());
    survivors.erase(std::unique(survivors.begin(), survivors.end()), survivors.end());
    selection.accepted.reserve(survivors.size());
    for (int64_t ord : survivors) {
      if (acceptsTerm(labels.ordToStr(ord))) selection.accepted.insert(ord);
    }
    return selection;
  }

  // Term access for one selection site: the term tier by ord, and the
  // accepted ords in ascending order for zero-count padding.
  class Terms {
    const FacetOrdFilter& filter;
    OrdMapStr* labels;  // null without a term tier
    std::vector<int64_t> accepted;
    size_t interval = 0;
    int64_t next = 0;

  public:
    Terms(const FacetOrdFilter& filter, OrdMapStr* labels) : filter(filter), labels(labels) {
      if (!filter.intervals.empty()) next = filter.intervals[0].lo;
    }

    bool acceptsTerm(int64_t ord) { return filter.acceptsTerm(labels->ordToStr(ord)); }

    // Calls visit(ord) on the accepted ords in ascending order until it
    // returns false.
    template <typename Visit>
    void forEachAccepted(Visit&& visit) {
      for (const auto& range : filter.intervals) {
        for (int64_t ord = range.lo; ord < range.hi; ord++) {
          if (filter.hasTermTier() && !acceptsTerm(ord)) continue;
          if (!visit(ord)) return;
        }
      }
    }

    // The i-th accepted ord in ascending order, or nullopt past the last.
    // Produced ords are kept, so the rows that share one Terms test each
    // padded term once.
    std::optional<int64_t> acceptedAt(size_t i) {
      while (accepted.size() <= i) {
        if (interval >= filter.intervals.size()) return std::nullopt;
        int64_t ord = next++;
        if (next >= filter.intervals[interval].hi && ++interval < filter.intervals.size()) {
          next = filter.intervals[interval].lo;
        }
        if (!filter.hasTermTier() || acceptsTerm(ord)) accepted.push_back(ord);
      }
      return accepted[i];
    }
  };
};

}  // namespace luxir
