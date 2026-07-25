#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace solux {

// The terms with the largest docFreq for one field, in global ord space.
//
// Two facet uses:
//  1) When the domain holds every doc, a term's count IS its docFreq, so the
//     top-K buckets by count ARE the first K entries here and nothing needs to
//     be counted.
//  2) For a partial domain, count only the listed terms and stop when the Kth
//     count exceeds unlistedBound, since no unlisted term can reach it.
// Only (1) is wired up.
struct TopTerms {
  struct Entry {
    int64_t ord;
    int64_t df;
  };
  std::vector<Entry> entries;  // df descending, ties by ord ascending
  int64_t unlistedBound = 0;   // every term not in entries has df <= this
};

// How many terms to keep is chosen per field, not fixed:
//
//   M = clamp(terms with df > dfThreshold, nTerms / FLOOR_DIVISOR, MAX_ENTRIES)
//
// The threshold half is the UnInvertedField rule - a term on more than a few
// percent of the docs is a genuinely big term, and there can only ever be
// sumDocFreq / dfThreshold of them, so keeping all of them stays tiny.
//
// The floor half is what use (1) actually rides on, because a uniform
// high-cardinality field has NO term above the threshold and would otherwise
// get an empty list exactly where the whole-domain fast path pays most. Tying
// it to nTerms keeps the list a bounded fraction of the dictionary it lets you
// skip: at most 1/FLOOR_DIVISOR of the terms, so the payoff ratio is never
// worse than FLOOR_DIVISOR to one. A tiny field gets a floor of zero and falls
// back, which costs nothing - walking its whole dictionary was already free.
class TopTermsBuilder {
  // Terms above this are all kept (subject to MAX_ENTRIES). maxDoc / 20 = 5%,
  // the UnInvertedField maxTermDocFreq value. Inherited, not measured: it only
  // decides where use (2)'s certificate starts firing, and no corpus here has
  // the skew to show it.
  static constexpr int64_t DF_THRESHOLD_DIVISOR = 20;
  // The list may never exceed 1/64th of the field's term count.
  static constexpr size_t FLOOR_DIVISOR = 64;

  int64_t dfThreshold;
  size_t maxEntries;
  size_t added = 0;
  size_t aboveThreshold = 0;
  int64_t maxDf = 0;
  std::vector<TopTerms::Entry> entries;
  bool finished = false;

  static bool stronger(const TopTerms::Entry& a,
                       const TopTerms::Entry& b) {
    return a.df != b.df ? a.df > b.df : a.ord < b.ord;
  }

public:
  // Bounds the transient heap, and backstops a pathological terms-per-doc
  // count (above-threshold terms number sumDocFreq / dfThreshold, which is
  // 20 for a single-valued field but scales with values per doc).
  static constexpr size_t MAX_ENTRIES = 4096;

  static int64_t thresholdFor(int64_t maxDoc) {
    return maxDoc / DF_THRESHOLD_DIVISOR;
  }

  explicit TopTermsBuilder(int64_t dfThreshold,
                           size_t maxEntries = MAX_ENTRIES)
      : dfThreshold(dfThreshold), maxEntries(maxEntries) {
    assert(maxEntries > 0);
  }

  void add(int64_t ord, int64_t df) {
    assert(!finished);
    if (df <= 0) return;
    added++;
    if (df > maxDf) maxDf = df;
    if (df > dfThreshold) aboveThreshold++;

    TopTerms::Entry entry{ord, df};
    if (entries.size() < maxEntries) {
      entries.push_back(entry);
      std::push_heap(entries.begin(), entries.end(), stronger);
    } else if (stronger(entry, entries.front())) {
      std::pop_heap(entries.begin(), entries.end(), stronger);
      entries.back() = entry;
      std::push_heap(entries.begin(), entries.end(), stronger);
    }
  }

  TopTerms finish() {
    assert(!finished);
    finished = true;

    size_t floor = std::min(maxEntries, added / FLOOR_DIVISOR);
    size_t keep = std::min(entries.size(),
                           std::max(aboveThreshold, floor));

    std::sort(entries.begin(), entries.end(), stronger);
    entries.resize(keep);

    // Retention is always "the top `keep` by df", so the weakest retained df
    // bounds every term that did not make it - whether it fell below the
    // threshold or was trimmed. Tighter than dfThreshold itself when the floor
    // pulled below-threshold terms in.
    int64_t unlistedBound = keep == 0 ? maxDf
                          : keep >= added ? 0
                          : entries.back().df;
    return {std::move(entries), unlistedBound};
  }
};

}
