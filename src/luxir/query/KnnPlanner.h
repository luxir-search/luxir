// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>
#include "VectorEngine.h"

namespace luxir {

// Pure metadata planner. Costs are exact-score-dimension equivalents, not
// predicted microseconds. Features have the same units as actualWork below.
struct KnnPlanner {
  enum class Routing { AUTO, FORCE_IVF, FORCE_EXACT };
  enum class Codec { FLAT, PQ, RABITQ };
  enum class Selector { AUTO, RESOLVE, FOLD }; // Round one only; deepen always folds.
  // Cumulative search variants, also used by the planner benchmark.
  enum class Search { FORWARD, SEEDED, REVERSE, IMPROVE };
  static constexpr Search DEFAULT_SEARCH = Search::SEEDED;
  enum Unit { EXACT_DIM, TERMINAL_DIM, DOMAIN_DOC, RANK_VISIT, FLAT_PROBE, SPARSE_MAP, MULTI_MAP,
              ENTRY_NONE, ENTRY_BITMAP, RESOLVE_IDENTITY, RESOLVE_SELECT, RESOLVE_MONO,
              BIT_PROBE, ARRAY_STEP, PQ_SUBCODE, PQ_BIT, ONE_BIT_DIM, EXTRA_BIT_DIM,
              PQ_LIST_SETUP, RABITQ_LIST_DIM, COARSE_DIM, ROUND, FOLD_DOC, FOLD_WORD, NUM_UNITS };
  static constexpr std::array<std::string_view, NUM_UNITS> unitNames{
    "exact_dims", "terminal_dims", "domain_docs", "rank_visits", "flat_probes", "sparse_maps", "multi_maps",
    "entry_none", "entry_bitmap", "resolve_identity", "resolve_select", "resolve_mono",
    "bit_probes", "array_steps", "pq_subcodes", "pq_bits", "one_bit_dims", "extra_bit_dims",
    "pq_list_setups", "rabitq_list_dims", "coarse_dims", "ivf_slot_rounds", "fold_docs", "fold_words"};
  using Work = std::array<double, NUM_UNITS>;
  struct Costs {
    // Fitted nanoseconds divided by the exact-walk dimension cost. Retain
    // relative seeds for rank walks, sparse/multi mapping and rank bitmaps:
    // dense filtered calibration does not identify those costs. Identity
    // resolve rides in the probe cost; PQ bits are collinear with subcodes.
    static constexpr double NS_PER_DIM = 0.135;
    Work units{1, 1.43 / NS_PER_DIM, 22.7 / NS_PER_DIM, 1, 4, 16, 24,
               1, 2.11 / NS_PER_DIM, 0, 16, 24, 9.0 / NS_PER_DIM, 1.65 / NS_PER_DIM,
               0.62 / NS_PER_DIM, 0, 0.053 / NS_PER_DIM, 0.104 / NS_PER_DIM,
               957 / NS_PER_DIM, 18.1 / NS_PER_DIM, 0.064 / NS_PER_DIM, 148000 / NS_PER_DIM,
               3.0 / NS_PER_DIM, 0.5 / NS_PER_DIM};
    // IVF units refit on fold-from-round-one scans (SIFT 128-d, miracl
    // 768-d, 4 segments): predicted/actual p10 0.87, p90 1.12. Fold doc and
    // word costs were too small to identify there; they keep 3 ns per
    // visited doc and 0.5 ns per initialized/copied 64-bit word.
    double requestCost = 16600 / NS_PER_DIM; // Common to both arms, once per query.
    // Fraction of one-bit RaBitQ candidates that pass the gate to the
    // extra-bit pass, for an independent filter at the selectivities where
    // IVF is chosen: 0.04-0.06 at 128-d (SIFT) and ~0.011 at 768-d (miracl),
    // i.e. about 0.046 * (128 / dims)^0.8. The runtime completion guard
    // uses the observed survival once a segment has scanned.
    double extraBitSurvival128 = 0.046;
    double extraBitSurvival(int32_t dims) const {
      return std::min(1.0, extraBitSurvival128 * std::pow(128.0 / std::max(1, dims), 0.8));
    }
    // Dimension-weighted terminal reads / new-list passes across the
    // calibration corpora was 0.107; independent filters alone were 0.004.
    // Correlation is unknown at prepare, so keep the pooled estimate.
    double intervalReadFraction = 0.11;
    double price(const Work& work) const {
      return std::inner_product(work.begin(), work.end(), units.begin(), 0.0);
    }
  };
  struct Segment {
    int64_t vectors = 0, liveVectors = 0, docsWithValue = 0;
    int64_t eligibleDocs = 0, eligibleVectors = 0, domainSize = 0, maxDoc = 0;
    int32_t dims = 0, nlist = 0, pqM = 0, pqBits = 0, rabitqBits = 0;
    Codec codec = Codec::FLAT;
    bool domain = false, arrayDomain = false, unfiltered = true, deletes = false;
    bool sparse = false, multi = false, bounded = false, vectorsEstimated = false;
  };
  struct Request {
    int64_t k = 0, boundedTarget = 0;
    int32_t nprobe = 0, referenceBreadth = 1;
    double minScanFraction = 0, dimFactor = 1, alpha = 0.5, flatDensity = 0.6;
    Selector selector = Selector::AUTO;
  };
  struct Price {
    Work exact{}, linear{}, fixed{};
    double exactCost = 0, a = 0, b = 0;
    // Selector correction to the bitmap-priced linear arm. The threshold
    // partitions folded first rounds from resolve-first, fold-on-deepen.
    double resolveExtra = 0, foldCost = 0, foldThreshold = -1, deepenCopy = 0;
  };
  struct Totals {
    double exact = 0, a = 0, b = 0;
    double boundedDims = 0, intervalDims = 0, boundedReadLimit = 0;
    int64_t live = 0, eligibleVectors = 0, eligibleDocs = 0, exactDocs = 0;
    int64_t boundedDocs = 0, boundedLive = 0, ivf = 0;
  };
  struct Result {
    std::vector<char> ivf;
    std::vector<Price> prices;
    double cost = 0, allIvfCost = 0, allExactCost = 0, coverage = 0, allIvfCoverage = 0;
  };

  static bool walkDomain(bool domain, bool sparse, bool multi, int64_t card,
                         int64_t docsWithValue, double threshold) {
    return domain && threshold >= 0 && (sparse || multi
        || (double)card / std::max((int64_t)1, docsWithValue) <= threshold);
  }
  static double initialScanFraction(const Request& request, double selectivity) {
    // Effort is a scan fraction of a hypothetical single IVF index of
    // nlist = referenceBreadth = defaultIvfNList(all live vectors): nprobe
    // lists of it, or by default sqrt(nlist) lists (times the dimension factor).
    // Depending only on the total live count makes it MERGE-STABLE: a raw
    // per-segment list count would scan ~sqrt(2)x more before a merge than
    // after. A per-segment sum of defaults was rejected for the same reason
    // (sum_s sqrt(N_s) / sqrt(N) = sqrt(numSegments) >= 1).
    double reference = (double)std::max(1, request.referenceBreadth);
    double fraction = request.nprobe > 0 ? (double)request.nprobe / reference
        : request.dimFactor / std::sqrt(reference);
    if (request.nprobe == 0 && selectivity < 1)
      fraction *= std::pow(std::max(selectivity, std::numeric_limits<double>::min()), -request.alpha);
    return std::clamp(std::max(fraction, request.minScanFraction), 0.0, 1.0);
  }
  static double arraySteps(int64_t card) { return (double)std::bit_width((uint64_t)std::max((int64_t)1, card)); }
  static bool canIvf(const Segment& s) {
    return s.codec != Codec::FLAT && s.liveVectors > 0 && s.eligibleDocs > 0;
  }
  static double predictedChecks(const Segment& s, double fraction) { return fraction * s.vectors; }
  static bool borrowDomain(const Segment& s) {
    return !s.unfiltered && !s.sparse && !s.multi && !s.arrayDomain;
  }
  static bool foldComplement(const Segment& s) {
    return !s.arrayDomain && s.domainSize > s.maxDoc - s.domainSize;
  }
  static Work foldWork(const Segment& s) {
    Work w{};
    if (borrowDomain(s)) return w;
    double docs = (double)(foldComplement(s) ? s.maxDoc - s.domainSize : s.domainSize);
    w[FOLD_DOC] = docs; w[FOLD_WORD] = (double)((s.vectors + 63) / 64);
    if (s.sparse) w[SPARSE_MAP] = docs;
    if (s.multi) w[MULTI_MAP] = docs;
    return w;
  }
  static double resolveCost(const Segment& s, const Costs& costs) {
    return costs.units[s.multi ? RESOLVE_MONO : s.sparse ? RESOLVE_SELECT : RESOLVE_IDENTITY]
        + (s.arrayDomain ? arraySteps(s.domainSize) * costs.units[ARRAY_STEP] : costs.units[BIT_PROBE]);
  }
  static double foldThreshold(const Segment& s, Selector selector, const Costs& costs) {
    if (selector == Selector::FOLD || (selector == Selector::AUTO && borrowDomain(s))) return -1;
    double extra = predictedChecks(s, 1) * (resolveCost(s, costs) - costs.units[ENTRY_BITMAP]);
    return selector == Selector::RESOLVE || extra <= 0 ? INFINITY : costs.price(foldWork(s)) / extra;
  }
  static bool foldFirst(const Segment& s, double fraction, Selector selector, const Costs& costs) {
    return fraction > foldThreshold(s, selector, costs);
  }
  static double selectorCost(const Price& p, double initial, bool deepen) {
    double cost = initial > p.foldThreshold ? p.foldCost : initial * p.resolveExtra + (deepen ? p.foldCost : 0);
    return cost + (deepen ? p.deepenCopy : 0);
  }
  static Price price(const Segment& s, const Request& request, const Costs& costs) {
    Price p;
    if (s.eligibleDocs <= 0 || s.liveVectors <= 0) return p;
    bool walk = walkDomain(s.domain, s.sparse, s.multi, s.domainSize, s.docsWithValue, request.flatDensity);
    p.exact[EXACT_DIM] = (double)s.eligibleVectors * s.dims;
    p.exact[DOMAIN_DOC] = walk ? (double)s.domainSize : 0;
    p.exact[RANK_VISIT] = walk ? 0 : (double)s.vectors;
    p.exact[FLAT_PROBE] = !walk && s.domain ? (double)s.docsWithValue : 0;
    p.exact[SPARSE_MAP] = s.sparse && (walk || !s.multi) ? (double)(walk ? s.eligibleDocs : s.vectors) : 0;
    p.exact[MULTI_MAP] = s.multi ? (double)(walk ? s.eligibleDocs : s.vectors) : 0;
    p.exactCost = costs.price(p.exact);
    if (!canIvf(s)) return p;
    if (s.unfiltered) p.linear[s.deletes ? ENTRY_BITMAP : ENTRY_NONE] = (double)s.vectors;
    else {
      p.linear[ENTRY_BITMAP] = (double)s.vectors;
      p.resolveExtra = predictedChecks(s, 1) * (resolveCost(s, costs) - costs.units[ENTRY_BITMAP]);
      p.foldCost = costs.price(foldWork(s));
      p.foldThreshold = foldThreshold(s, request.selector, costs);
      // A borrowed domain stays read-only until bounded pooling clears ranks.
      if (borrowDomain(s) && s.bounded) p.deepenCopy = ((s.vectors + 63) / 64) * costs.units[FOLD_WORD];
    }
    if (s.deletes) {
      // Allocation probes physical ids for liveness too. Allow one extra
      // head list for nextFraction's lookahead at the round boundary.
      p.linear[ENTRY_BITMAP] += (double)s.vectors;
      p.fixed[ENTRY_BITMAP] = (double)s.vectors / std::max(1, s.nlist);
    }
    if (s.codec == Codec::PQ) {
      p.linear[PQ_SUBCODE] = (double)s.eligibleVectors * s.pqM;
      p.linear[PQ_BIT] = p.linear[PQ_SUBCODE] * s.pqBits;
    } else {
      p.linear[ONE_BIT_DIM] = (double)s.eligibleVectors * s.dims;
      if (s.rabitqBits > 1) p.linear[EXTRA_BIT_DIM] = p.linear[ONE_BIT_DIM] * (s.rabitqBits - 1) * costs.extraBitSurvival(s.dims);
    }
    if (s.codec == Codec::PQ) p.linear[PQ_LIST_SETUP] = s.nlist;
    else p.linear[RABITQ_LIST_DIM] = (double)s.nlist * s.dims;
    p.fixed[COARSE_DIM] = (double)s.nlist * s.dims;
    p.a = costs.price(p.linear);
    p.b = costs.price(p.fixed);
    return p;
  }
  static void addIvf(Totals& t, const Segment& s, const Price& p, int sign) {
    t.exact -= sign * p.exactCost; t.a += sign * p.a; t.b += sign * p.b;
    t.live += sign * s.liveVectors;
    t.eligibleVectors += sign * s.eligibleVectors;
    t.eligibleDocs += sign * s.eligibleDocs;
    t.exactDocs -= sign * s.eligibleDocs;
    t.ivf += sign;
    if (s.bounded) {
      t.boundedDocs += sign * s.eligibleDocs; t.boundedLive += sign * s.liveVectors;
      t.boundedDims += sign * (double)s.eligibleVectors * s.dims;
    } else t.intervalDims += sign * (double)s.eligibleVectors * s.dims;
  }
  struct Effort {
    double initial = 0, coverage = 0, rounds = 0;
  };
  static Effort effort(const Totals& t, const Request& request) {
    if (!t.ivf) return {};
    double p0 = initialScanFraction(request, (double)t.eligibleVectors / std::max((int64_t)1, t.live));
    double p = p0;
    if (request.nprobe == 0) {
      double fill = t.boundedDocs > 0
          ? (double)std::min({request.boundedTarget, t.boundedDocs, t.boundedLive}) / t.boundedDocs : 0;
      double units = (double)std::max((int64_t)0, request.k - t.exactDocs) / std::max((int64_t)1, t.eligibleDocs);
      p = std::min(1.0, std::max({p, fill, units}));
    }
    double rounds = p <= p0 ? 1 : 1 + std::ceil(std::log2(p / std::max(p0, std::numeric_limits<double>::min())));
    return {p0, p, rounds};
  }
  static double coverage(const Totals& t, const Request& request) { return effort(t, request).coverage; }
  static double rounds(const Totals& t, const Request& request) { return effort(t, request).rounds; }
  static double terminalDims(const Totals& t, double coverage, const Costs& costs) {
    return std::min(t.boundedDims, t.boundedReadLimit) + coverage * t.intervalDims * costs.intervalReadFraction;
  }
  static double totalCost(const Totals& t, const Request& request, const Costs& costs, double selector) {
    auto e = effort(t, request);
    return costs.requestCost + t.exact + e.coverage * t.a + t.b + selector
        + t.ivf * e.rounds * costs.units[ROUND]
        + terminalDims(t, e.coverage, costs) * costs.units[TERMINAL_DIM];
  }
  static Totals exactTotals(std::span<const Segment> segments, std::span<const Price> prices,
                            const Request& request) {
    Totals t;
    double vectorDims = 0;
    int64_t docs = 0;
    for (size_t i = 0; i < segments.size(); i++) {
      const auto& s = segments[i];
      t.exact += prices[i].exactCost; t.exactDocs += s.eligibleDocs;
      if (canIvf(s) && s.bounded) { vectorDims += (double)s.vectors * s.dims; docs += s.docsWithValue; }
    }
    // One shared bounded pool. Freeze the physical average dimensions per
    // document before mode choice; do not multiply the pool by slot count.
    // Eligible dimensions cap the allowance when fewer vectors can be read.
    // Selection can favor higher-multiplicity docs, so this is an estimate.
    t.boundedReadLimit = request.boundedTarget * vectorDims / std::max((int64_t)1, docs);
    return t;
  }
  // Routing changes P0, hence which selectors fold in round one. A Fenwick
  // tree keeps exact sums on each side of the crossover in O(log S) per
  // substitution, without walking all selected segments for every trial.
  class SelectorSums {
    struct Sum { double fold = 0, resolve = 0; };
    std::vector<double> thresholds;
    std::vector<size_t> ranks;
    std::vector<Sum> tree;
    Sum all;
    double deepenCopy = 0;
  public:
    SelectorSums(std::span<const Price> prices) : ranks(prices.size()) {
      std::vector<size_t> order(prices.size());
      std::iota(order.begin(), order.end(), 0);
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return prices[a].foldThreshold < prices[b].foldThreshold; });
      for (size_t i : order) { ranks[i] = thresholds.size() + 1; thresholds.push_back(prices[i].foldThreshold); }
      tree.resize(prices.size() + 1);
    }
    void clear() { std::fill(tree.begin(), tree.end(), Sum{}); all = {}; deepenCopy = 0; }
    void add(size_t i, const Price& p, int sign) {
      double f = sign * p.foldCost, r = sign * p.resolveExtra;
      all.fold += f; all.resolve += r; deepenCopy += sign * p.deepenCopy;
      for (size_t j = ranks[i]; j < tree.size(); j += j & -j) { tree[j].fold += f; tree[j].resolve += r; }
    }
    double cost(const Effort& e) const {
      // Configurations can move coverage either way; this is not a forward cursor.
      size_t i = (size_t)(std::lower_bound(thresholds.begin(), thresholds.end(), e.initial) - thresholds.begin());
      Sum folded;
      for (; i; i -= i & -i) { folded.fold += tree[i].fold; folded.resolve += tree[i].resolve; }
      return (e.coverage > e.initial ? all.fold + deepenCopy : folded.fold) + e.initial * (all.resolve - folded.resolve);
    }
  };
  static Result choose(std::span<const Segment> segments, const Request& request,
                       const Costs& costs, Routing routing = Routing::AUTO,
                       Search search = DEFAULT_SEARCH) {
    Result result;
    result.prices.reserve(segments.size());
    for (const auto& s : segments) result.prices.push_back(price(s, request, costs));
    SelectorSums selectors(result.prices);
    auto cost = [&](const Totals& t) { return totalCost(t, request, costs, t.ivf ? selectors.cost(effort(t, request)) : 0); };
    Totals exact = exactTotals(segments, result.prices, request), all = exact;
    std::vector<size_t> order;
    result.ivf.resize(segments.size());
    for (size_t i = 0; i < segments.size(); i++) if (canIvf(segments[i])) {
      result.ivf[i] = true; order.push_back(i); addIvf(all, segments[i], result.prices[i], 1);
      selectors.add(i, result.prices[i], 1);
    }
    result.allExactCost = totalCost(exact, request, costs, 0);
    result.cost = result.allIvfCost = cost(all);
    result.coverage = result.allIvfCoverage = coverage(all, request);
    if (routing == Routing::FORCE_IVF || order.empty()) return result;
    Totals best = all;
    auto keep = [&](const Totals& t, const std::vector<char>& modes) {
      double trialCost = cost(t);
      if (trialCost < result.cost) { best = t; result.cost = trialCost; result.ivf = modes; }
    };
    std::vector<char> modes(segments.size());
    if (routing == Routing::FORCE_EXACT) {
      result.ivf = modes; result.cost = result.allExactCost; result.coverage = 0;
      return result;
    }
    selectors.clear();
    keep(exact, modes);
    if (request.nprobe > 0) {
      // Pinned effort makes everything except the shared bounded allowance
      // additive. min(sum eligible dimensions, pool limit) is the lower
      // envelope of two additive objectives; minimize each independently.
      double p = result.allIvfCoverage;
      for (bool capped : {false, true}) {
        Totals t = exact;
        selectors.clear();
        std::fill(modes.begin(), modes.end(), false);
        for (size_t i : order) {
          const auto& s = segments[i]; const auto& price = result.prices[i];
          double terminal = (double)s.eligibleVectors * s.dims
              * (s.bounded ? capped ? 0 : 1 : p * costs.intervalReadFraction);
          double ivf = p * price.a + price.b + selectorCost(price, p, false) + costs.units[ROUND] + terminal * costs.units[TERMINAL_DIM];
          if (ivf <= price.exactCost) { modes[i] = true; addIvf(t, s, price, 1); selectors.add(i, price, 1); }
        }
        keep(t, modes);
      }
    } else {
      double p = result.allIvfCoverage, roundCost = rounds(all, request) * costs.units[ROUND];
      std::vector<double> savings(segments.size());
      for (size_t i : order) {
        const auto& s = segments[i]; const auto& price = result.prices[i];
        double terminal = (double)s.eligibleVectors * s.dims * (s.bounded
            ? std::min(1.0, all.boundedReadLimit / std::max(1.0, all.boundedDims))
            : p * costs.intervalReadFraction);
        savings[i] = price.exactCost - p * price.a - price.b
            - selectorCost(price, effort(all, request).initial, p > effort(all, request).initial) - roundCost - terminal * costs.units[TERMINAL_DIM];
      }
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        double x = savings[a], y = savings[b]; return x != y ? x < y : a < b;
      });
      auto flip = [&](Totals& t, double& currentCost, size_t i) {
        Totals trial = t;
        int sign = modes[i] ? -1 : 1;
        addIvf(trial, segments[i], result.prices[i], sign);
        selectors.add(i, result.prices[i], sign);
        double trialCost = cost(trial);
        if (trialCost < currentCost) { t = trial; currentCost = trialCost; modes[i] = !modes[i]; }
        else selectors.add(i, result.prices[i], -sign);
      };
      // Forward sweeps from all-IVF and the two channel-removal seeds.
      for (int seed = 0; seed < (search >= Search::SEEDED ? 3 : 1); seed++) {
        if (seed && (all.boundedDims == 0 || all.intervalDims == 0)) continue;
        Totals t = exact;
        selectors.clear();
        std::fill(modes.begin(), modes.end(), false);
        for (size_t i : order) {
          bool selected = seed == 0 || (seed == 1 ? !segments[i].bounded : segments[i].bounded);
          if (selected) { modes[i] = true; addIvf(t, segments[i], result.prices[i], 1); selectors.add(i, result.prices[i], 1); }
        }
        double currentCost = cost(t);
        for (size_t i : order) if (modes[i]) flip(t, currentCost, i);
        keep(t, modes);
      }
      if (search >= Search::REVERSE) {
        Totals t = exact;
        selectors.clear();
        std::fill(modes.begin(), modes.end(), false);
        double currentCost = result.allExactCost;
        for (auto it = order.rbegin(); it != order.rend(); ++it) flip(t, currentCost, *it);
        keep(t, modes);
      }
      if (search >= Search::IMPROVE) {
        Totals t = best;
        modes = result.ivf;
        selectors.clear();
        for (size_t i : order) if (modes[i]) selectors.add(i, result.prices[i], 1);
        double currentCost = result.cost;
        for (size_t i : order) flip(t, currentCost, i);
        keep(t, modes);
      }
    }
    result.coverage = coverage(best, request);
    return result;
  }

  static Work actualWork(const Segment& s, const VectorSearchWork& w) {
    Work f{};
    f[EXACT_DIM] = (double)w.exactVectors * s.dims;
    f[TERMINAL_DIM] = (double)(w.seedReads + w.remainingReads) * s.dims;
    f[DOMAIN_DOC] = (double)w.domainDocs; f[RANK_VISIT] = (double)w.rankVisits;
    f[FLAT_PROBE] = (double)w.domainProbes;
    f[SPARSE_MAP] = (double)w.sparseMaps; f[MULTI_MAP] = (double)w.multiMaps;
    f[ENTRY_NONE] = (double)w.selectorNone; f[ENTRY_BITMAP] = (double)(w.selectorBitmap + w.allocatorEntries);
    f[RESOLVE_IDENTITY] = (double)w.resolveIdentity; f[RESOLVE_SELECT] = (double)w.resolveSelect;
    f[RESOLVE_MONO] = (double)w.resolveMono;
    f[BIT_PROBE] = (double)w.bitProbes; f[ARRAY_STEP] = (double)w.arraySteps;
    f[PQ_SUBCODE] = (double)w.pqScores * s.pqM; f[PQ_BIT] = f[PQ_SUBCODE] * s.pqBits;
    f[ONE_BIT_DIM] = (double)w.oneBitScores * s.dims;
    f[EXTRA_BIT_DIM] = (double)w.extraBitScores * std::max(0, s.rabitqBits - 1) * s.dims;
    f[PQ_LIST_SETUP] = s.codec == Codec::PQ ? (double)w.listSetups : 0;
    f[RABITQ_LIST_DIM] = s.codec == Codec::RABITQ ? (double)w.listSetups * s.dims : 0;
    f[FOLD_DOC] = (double)w.foldDocs; f[FOLD_WORD] = (double)w.foldWords;
    f[COARSE_DIM] = (double)w.coarseDims; f[ROUND] = (double)w.ivfRounds;
    return f;
  }
};
}
