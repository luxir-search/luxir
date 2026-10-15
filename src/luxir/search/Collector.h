// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>

#include "luxir/query/Query.h"
#include "luxir/search/DocSet.h"
#include "luxir/util/ExecutionBudget.h"

namespace luxir {

LUXIR_UNALIGNED_START
class segdoc {
  // docid must come first in little-endian for it to make up the low bytes of the int64_t
  int32_t docid;
  int32_t seg;
public:
  segdoc() {}
  segdoc(int32_t segment, int32_t docid) : docid(docid), seg(segment) {}

  int32_t segment() const {
    return seg;
  }

  int32_t docId() const {
    return docid;
  }

  auto operator<=>(const segdoc &other) const {
    return std::bit_cast<int64_t>(*this) <=> std::bit_cast<int64_t>(other);
  }

  bool operator==(const segdoc &other) const {
    return std::bit_cast<int64_t>(*this) == std::bit_cast<int64_t>(other);
  }

  // return negative if this < other, positive if this > other, 0 if equal
  int64_t compare(const segdoc &other) const {
    return std::bit_cast<int64_t>(*this) - std::bit_cast<int64_t>(other);
  }

} LUXIR_UNALIGNED_END;




class TopDocsCollector {
  public:

  // Having this packed helped both memory and CPU (presumably better cache hits?)
  LUXIR_UNALIGNED_START
  struct ScoreDoc {
    float score;
    segdoc doc;
  } LUXIR_UNALIGNED_END;


  int64_t hitCount = 0;

  float minCompetitiveVal = std::numeric_limits<float>::lowest();

  constexpr static auto scoreComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score > b.score;
  };

  constexpr static auto scoreAndDocComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score == b.score ? (a.doc < b.doc) : (a.score > b.score);
  };

  int64_t topCount;
  // Heap orders by (score, then doc) so ties at the k-th score are broken by (seg, docid):
  // the kept top-K is a deterministic total order, independent of collection/merge order
  // (and, once it exists, slicing).  Required for sliced==unsliced to hold at exact ties.
  // The storage expands on demand: topCount bounds the heap but is not allocated up
  // front, which matters for deep or unbounded limits (limit=-1 maps to maxDoc) and for
  // the per-slice collectors that never see topCount hits.
  ExpandingPQ<ScoreDoc, decltype(scoreAndDocComp)> pq;

  TopDocsCollector(int64_t topCount)
      : topCount(topCount), pq((size_t)std::max(topCount, (int64_t)0)) {
    // topCount == 0 is valid ("count/aggregate only, no docs", e.g. limit 0): the heap is
    // empty and collect() only counts.  Negative counts are a bug in the caller.
    assert(topCount >= 0);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;

    // topCount == 0: keep no docs, just count hits (the heap has zero capacity, so any
    // insert/top() would be out of bounds).  hitCount above still yields an accurate
    // total, so get_number and sub-op domains are unaffected.
    if (topCount == 0) {
      return;
    }

    // Admit anything that can match OR beat the k-th best score (>=, not >): a doc whose
    // score ties the k-th score is still competitive if its (seg, docid) sorts ahead of the
    // current worst tied doc.  insertWithOverflow resolves that via the (score, then doc)
    // heap comparator, so a losing tie is a cheap no-op insert and a winning tie evicts the
    // worst.  This is what makes the kept top-K order-independent (collection, merge, slice).
    if (score >= minCompetitiveVal) {
      pq.insertWithOverflow({score, segdoc(segment, docid)});
      // Once the heap holds topCount docs, its root (a min-heap on score) is the k-th best
      // so far - the competitive threshold a later doc must beat to enter.  Publish it as
      // soon as the heap fills, not only when a doc evicts one: best-docs-first / index-
      // sorted / clustered input never evicts, so the old "update on overflow only" left the
      // threshold at lowest() forever and defeated impact pruning exactly when it helps most.
      if (pq.size() >= topCount) {
        minCompetitiveVal = pq.top().score;
      }
    }
  }

  // The heap floor stays inclusive because an equal-scoring doc with a
  // smaller total-order key can replace its root. For a monotone segment
  // stream, once its next key is past the root, equal scores cannot enter and
  // the scorer may use the next representable float as an exclusive floor.
  float minCompetitiveScoreForNextDoc(int32_t segment, int32_t docid) const {
    if (topCount == 0 || pq.size() < topCount) {
      return std::numeric_limits<float>::lowest();
    }
    const ScoreDoc& floor = pq.top();
    if (segdoc(segment, docid) > floor.doc) {
      return std::nextafter(floor.score, std::numeric_limits<float>::infinity());
    }
    return floor.score;
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return pq.size();
  }

  // merge other into this.
  void merge(TopDocsCollector& other) {
    // Self-merge would iterate a span that collect() can reallocate out from under us.
    assert(this != &other);
    // In some scenarios, popping the top of the other heap until it's no longer competitive will be faster,
    // while in other scenarios just a linear scan of the other heap will be faster.
    // We'll just do a linear scan for now.
    auto newHitCount = hitCount + other.hitCount;
    std::span<ScoreDoc> otherDocs = other.pq.span();
    // Since min-heap has smallest element at position 0, it should be more efficient to start from the other end.
    // Future possible optimization: if we do a whole level of a min-tree without any insertions, we could stop early.
    for (int64_t i = (int64_t)otherDocs.size() - 1; i >= 0; i--) {
      collect(otherDocs[i].doc.segment(), otherDocs[i].doc.docId(), otherDocs[i].score);
    }
    hitCount = newHitCount;
  }

  // Since we use min-heap comparators in our priority queues, the list will be reverse-sorted (smallest last)
  // Repeated calls to pop() on the priority queue will also return the docs in order.
  std::span<ScoreDoc> sort() {
    // sort_heap must use the same comparator the heap was built with (scoreAndDocComp),
    // which also gives the stable response order: score desc, then (seg, docid) asc at ties.
    std::span<ScoreDoc> docs = pq.span();
    std::sort_heap(docs.begin(), docs.end(), scoreAndDocComp);
    return docs;
  }

  std::span<ScoreDoc> scoreDocs() {
    return pq.span();
  }
};


// A different way to get top-k results: Collect results in an array until we have 2k of them.
// The find the median (kth value), and drop all values less than the median. Repeat collection more results.
// This has the advantage of not having the bad worst-case behavior of a priority queue and collecting something
// that is already mostly sorted.  It is also more linear and doesn't degrade as much as k increases.
// The obvious downside is double the memory usage... we would want a different strategy if someone wants the
// whole index sorted for example.
// Also, since minCompetitiveVal is only updated every k docs, this is perhaps more compatible with
// multi-threaded collection (i.e. check/update a global minCompetitiveVal at the same time you calculate the
// new minCometitiveVal from the median.)  It also may just be less important to update this minCompetitiveVal
// in any case.
//
// I don't know who created this algorithm, but I learned about it from a blog post by Paul Masurel:
// https://quickwit.io/blog/top-k-complexity
//
// Performance: see the benchmarks in CollectorBM.cpp
//   For random order, this was slower across the board.  I think perhaps because there are more swaps.
//   For more complex sorts, we're going to need indirection anyway, so that may change things.
//   The tricky part of implementing indirection for this would be reusing the slots after you have
//   dropped half of them.  Seems worse for cache locality.
//   Update: I tried indirection (see TopScoreCollectorI) and it did improve performance.
class TopScoreCollector {
public:
  using ScoreDoc = TopDocsCollector::ScoreDoc;

  // use the same reversed comparator as the priority-queue based collector so larger values are first
  // and we can drop values less than the median by simply resizing the vector.
  constexpr static auto scoreComp = TopDocsCollector::scoreComp;


  int64_t hitCount = 0;
  int64_t k;  // number of top docs we want to find
  std::vector<ScoreDoc> topDocs;
  float minCompetitiveVal = std::numeric_limits<float>::lowest();

  TopScoreCollector(int64_t topCount) : k(topCount) {
    assert(topCount > 0);
    // topDocs.reserve(topCount*2);  // reserve or not?
  }

  void dropHalf() {
    // iterator at the kth element is at index k-1.
    std::nth_element(topDocs.begin(), topDocs.begin()+k-1, topDocs.end(), scoreComp);
    minCompetitiveVal = topDocs[k-1].score;  // kth value is at index k-1
    topDocs.resize(k);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (score > minCompetitiveVal) {
      topDocs.push_back({score, segdoc(segment, docid)});
      if (topDocs.size() >= uint64_t(k<<1)) {
        dropHalf();
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return topDocs.size();
  }

  void sort() {
    if (topDocs.size() > uint64_t(k)) {
      dropHalf();
    }
    std::sort(topDocs.begin(), topDocs.end(), scoreComp);
  }

  // merge other into this.
  void merge(TopDocsCollector& other) {
    auto newHitCount = hitCount + other.hitCount;
    // The generic way...  Could probably be optimized.
    // 1) if other.minCompetitiveVal is greater than our minCompetitiveVal, we can just append all of other's docs.
    // 2) we could skip the comparisons anyway if there is space?
    for (auto& sd: other.scoreDocs()) {
      collect(sd.doc.segment(), sd.doc.docId(), sd.score);
    }
    hitCount = newHitCount;
  }


};

class TopScoreCollectorI {  // I for "indirect"
public:
  using index_t = int16_t;
  using ScoreDoc = TopDocsCollector::ScoreDoc;

  int64_t hitCount = 0;
  int64_t k;  // number of top docs we want to find
  std::vector<ScoreDoc> topDocs;
  std::vector<index_t> indexes;
  int64_t held = 0;  // number of valid indexes currently held.  We don't downside indexes.
  float minCompetitiveVal = std::numeric_limits<float>::lowest();


  TopScoreCollectorI(int64_t topCount) : k(topCount) {
    assert(topCount > 0);
    // assign slot indexes to start off with, just to see what the performance would be.
    // downside is that this uses max memory to start, even if no hits.
    topDocs.resize(topCount*2);
    indexes.reserve(topCount*2);
    for (int i = 0; i < topCount*2; i++) {
      indexes.push_back(i);
    }
  }

  void dropHalf() {
    auto* arr = &topDocs[0];  // does this help not direct through this for every comparison?
    auto scoreCompI = [arr](index_t a, const index_t b) {
      return arr[a].score > arr[b].score;
    };
    // iterator at the kth element is at index k-1.
    std::nth_element(indexes.begin(), indexes.begin()+k-1, indexes.end(), scoreCompI);
    minCompetitiveVal = arr[indexes[k-1]].score;  // kth value is at index k-1
    held = k;  // drops half
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (score > minCompetitiveVal) {
      topDocs[indexes[held++]] = {score, segdoc(segment, docid)};
      if (held >= (k<<1)) {
        dropHalf();
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return held;
  }

  void sort() {
    if (held > k) {
      dropHalf();
    }
    auto* arr = &topDocs[0];  // does this help not direct through this for every comparison?
    auto scoreCompI = [arr](index_t a, const index_t b) {
      return arr[a].score > arr[b].score;
    };
    std::sort(indexes.begin(), indexes.end(), scoreCompI);
  }

  // merge other into this.
  void merge(TopDocsCollector& other) {
    unused(other);
    /*
     * not implemented yet.
     */
  }


};

class MaxScoreAccumulator {
public:
  std::atomic<float> maxScore;

  MaxScoreAccumulator() : maxScore(std::numeric_limits<float>::lowest()) {
  }

  void accumulate(float score) {
    float cur = maxScore.load(std::memory_order_acquire);
    while (score > cur
           && !maxScore.compare_exchange_weak(
             cur, score, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
  }

  float get() const {
    return maxScore.load(std::memory_order_acquire);
  }
};

enum class DomainBuildMode : uint8_t {
  FILTERED_MATCHES,
  RAW_QUERY_MATCHES,
};

// Drive a per-segment Scorer through the optional `filter` domain, feeding (doc, score)
// into `collector`.  If `builder` is non-null, also records every matched doc for use as
// a sub-op domain (see TopDocsReq's per-segment subCalc dispatch).  Templated on Collector
// so both score-only and field-sort collectors can use the same loop, and so FusionOp can
// reuse it for per-source evaluation without dragging TopDocsReq's MergeableCollector along.
//
// Precondition: caller is responsible for any per-segment setup on `collector`.  In
// particular, FieldSortCollector requires `setSegment(segnum, &postingsReader)` to be
// called before this; TopDocsCollector has no per-segment setup.
// allowPruning: when false (e.g. the request asks for an exact total hit count via
// get_number), the rising threshold is NOT pushed to the scorer, so impact block
// skipping stays off and every matching doc is visited.  Dynamic pruning and an exact
// total count are mutually exclusive - skipping does not visit (cannot count) the docs
// it skips - so a query that needs the count must forgo pruning.
template <typename Collector>
void collectTopK(int32_t segnum, Query::Scorer* scorer, DocSet* filter,
                 DocSetBuilder* builder, Collector& collector, bool allowPruning = true,
                 MaxScoreAccumulator* accumulator = nullptr,
                 DomainBuildMode buildMode = DomainBuildMode::FILTERED_MATCHES,
                 DocSet* rawDomain = nullptr,
                 Query::ReportedTwoPhase reportedTwoPhase =
                     Query::ReportedTwoPhase::UNKNOWN) {
  constexpr int32_t kAccumulatorPollPeriod = 1024;
  float lastPushedMinCompetitiveScore = std::numeric_limits<float>::lowest();
  int32_t accumulatorPollCount = 0;
  auto localMinCompetitiveScore = [&](int32_t nextDoc) {
    if constexpr (requires {
        collector.minCompetitiveScoreForNextDoc(segnum, nextDoc);
      }) {
      return collector.minCompetitiveScoreForNextDoc(segnum, nextDoc);
    } else if constexpr (requires { collector.minCompetitiveVal; }) {
      unused(nextDoc);
      return collector.minCompetitiveVal;
    } else {
      unused(nextDoc);
      return std::numeric_limits<float>::lowest();
    }
  };
  auto pushMinCompetitiveScore = [&](float localScore, bool localRise,
                                     bool periodicPoll) {
    if constexpr (requires { collector.minCompetitiveVal; }) {
      if (!allowPruning || builder != nullptr) {
        return;
      }
      float minCompetitiveScore = localScore;
      if (accumulator != nullptr) {
        if (localRise) {
          // Cross-segment publication stays inclusive: a sibling stream can
          // still contain an equal-scoring doc with a smaller total-order key.
          accumulator->accumulate(collector.minCompetitiveVal);
        }
        if (localRise || periodicPoll) {
          minCompetitiveScore = std::max(minCompetitiveScore, accumulator->get());
        }
      }
      if (minCompetitiveScore > lastPushedMinCompetitiveScore) {
        scorer->setMinCompetitiveScore(minCompetitiveScore);
        lastPushedMinCompetitiveScore = minCompetitiveScore;
      }
    } else {
      unused(localScore, localRise, periodicPoll, lastPushedMinCompetitiveScore,
             allowPruning, accumulator);
    }
  };
  auto collectOne = [&](int32_t doc, float score) {
    if constexpr (requires { collector.minCompetitiveVal; }) {
      float oldMinCompetitiveVal = collector.minCompetitiveVal;
      collector.collect(segnum, doc, score);
      bool localRise = collector.minCompetitiveVal > oldMinCompetitiveVal;
      bool periodicPoll = false;
      if (allowPruning && builder == nullptr && accumulator != nullptr) {
        accumulatorPollCount++;
        if (accumulatorPollCount >= kAccumulatorPollPeriod) {
          accumulatorPollCount = 0;
          periodicPoll = true;
        }
      }
      pushMinCompetitiveScore(localMinCompetitiveScore(doc + 1), localRise,
                              periodicPoll);
    } else {
      unused(lastPushedMinCompetitiveScore, accumulatorPollCount, accumulator, allowPruning);
      collector.collect(segnum, doc, score);
    }
  };

  // Seed from both the local heap and the shared inclusive threshold so a
  // reused collector or a sibling segment can prune from the first doc.
  if constexpr (requires { collector.minCompetitiveVal; }) {
    if (allowPruning && builder == nullptr) {
      float seed = localMinCompetitiveScore(0);
      if (accumulator != nullptr) {
        seed = std::max(seed, accumulator->get());
      }
      if (seed > lastPushedMinCompetitiveScore) {
        scorer->setMinCompetitiveScore(seed);
        lastPushedMinCompetitiveScore = seed;
      }
    }
  }

  // Count-only collection (topCount == 0) never reads a score back out of the
  // collector, and under a request without NEED_SCORES the scorers may not
  // even be able to produce one - so score() must not be called at all.
  bool needScores = true;
  if constexpr (requires { collector.topCount; }) {
    needScores = collector.topCount > 0;
  }
  if constexpr (requires { collector.needsScores; }) {
    needScores = needScores && collector.needsScores;
  }

  // Field-sort competitive pruning: jump the scorer over doc blocks whose sort
  // key bounds prove them noncompetitive. Skipped docs are uncounted, so the
  // caller must not need an exact hit count or domain when pruning.
  [[maybe_unused]] int32_t competitiveEnd = 0;
  constexpr bool hasSortRanges =
      requires { collector.nextCompetitiveRange(segnum, (int32_t)0); };
  [[maybe_unused]] bool sortPrune = false;
  if constexpr (hasSortRanges) {
    sortPrune = allowPruning && builder == nullptr;
  }

  if (buildMode == DomainBuildMode::RAW_QUERY_MATCHES) {
    assert(builder != nullptr);
    DocSetProbe rawEligibility(rawDomain);
    DocSetProbe accepted(filter);
    for (;;) {
      int32_t doc = scorer->next();
      if (doc == PostingsReader::END) break;
      if (!rawEligibility.get(doc)) continue;
      // The builder captures M after the inherited domain but before routed
      // filters. Ranking still observes the complete default filter below.
      builder->add(doc);
      if (!accepted.get(doc)) continue;
      auto score = needScores ? scorer->score() : 0.0f;
      collectOne(doc, score);
    }
    return;
  }

  // A field-sort pull scorer with an externally driveable approximation must
  // consult sort competitiveness before exact verification. This is a protocol
  // choice for the scorer's whole lifetime: this arm drives only the exposed
  // DocsPosEnums and calls matchesAt(), then returns without reaching the
  // next()/advance() loops below.
  constexpr bool hasExternalSortGate = requires {
    collector.heapFull();
    collector.competitiveCandidate(segnum, (int32_t)0, 0.0f);
  };
  if constexpr (hasSortRanges && hasExternalSortGate) {
    bool externalApprox = buildMode == DomainBuildMode::FILTERED_MATCHES
        && sortPrune && !needScores
        && collector.topCount > 0
        && reportedTwoPhase == Query::ReportedTwoPhase::YES;
    std::span<DocsPosEnum*> approximations = externalApprox
        ? scorer->approximationEnums() : std::span<DocsPosEnum*>{};
    if (!approximations.empty()) {
      skipCount(SkipStats::fieldSortExternalApproxActivations);
      DocsPosEnum* lead = approximations[0];

      auto align = [&](int32_t target) {
        if (lead->docId() < target) target = lead->advance(target);
        for (;;) {
          if (target == PostingsReader::END) return target;
          bool restart = false;
          for (size_t i = 1; i < approximations.size(); i++) {
            DocsPosEnum* approximation = approximations[i];
            if (approximation->docId() < target) {
              int32_t doc = approximation->advance(target);
              assert(doc >= target);
              if (doc > target) {
                target = lead->advance(doc);
                restart = true;
                break;
              }
            }
          }
          if (!restart) return target;
        }
      };

      auto nextApproximation = [&]() {
        return align(lead->next());
      };

      BitDocSet* bitDocs = filter != nullptr && filter->type == DocSet::BITSET
          ? (BitDocSet*)filter : nullptr;
      assert(filter == nullptr || filter->type == DocSet::BITSET
          || filter->type == DocSet::ARRAY);
      const FixedBitSet* domainBits = bitDocs != nullptr
          ? &bitDocs->bits() : nullptr;
      std::span<const int32_t> domainDocs =
          filter != nullptr && filter->type == DocSet::ARRAY
              ? ((ArrDocSet*)filter)->docs()
              : std::span<const int32_t>{};
      if (filter != nullptr && filter->type == DocSet::ARRAY
          && domainDocs.empty()) {
        return;
      }
      const int32_t* domainCur = domainDocs.data();
      const int32_t* domainEnd = domainCur + domainDocs.size();

      int32_t candidate = -1;
      int32_t cursor = 0;
      for (;;) {
        auto range = collector.nextCompetitiveRange(segnum, cursor);
        if (range.begin == PostingsReader::END) return;
        if (candidate < range.begin) candidate = align(range.begin);
        if (candidate == PostingsReader::END) return;
        if (candidate >= range.end) {
          cursor = candidate;
          continue;
        }

        while (candidate < range.end) {
          skipCount(SkipStats::fieldSortExternalApproxCandidates);
          bool inDomain = true;
          if (domainBits != nullptr) {
            inDomain = domainBits->get(candidate);
          } else if (filter != nullptr) {
            domainCur = screaming::gallopLowerBound(
                domainCur, domainEnd, candidate);
            if (domainCur == domainEnd) return;
            if (*domainCur != candidate) {
              skipCount(SkipStats::fieldSortExternalApproxDomainRejects);
              candidate = align(*domainCur);
              if (candidate == PostingsReader::END) return;
              continue;
            }
            domainCur++;
          }
          if (!inDomain) {
            skipCount(SkipStats::fieldSortExternalApproxDomainRejects);
            candidate = nextApproximation();
            if (candidate == PostingsReader::END) return;
            continue;
          }
          if (collector.heapFull()
              && !collector.competitiveCandidate(segnum, candidate, 0.0f)) {
            skipCount(SkipStats::fieldSortExternalApproxBoundRejects);
            candidate = nextApproximation();
            if (candidate == PostingsReader::END) return;
            continue;
          }
          skipCount(SkipStats::fieldSortExternalApproxVerifications);
          if (scorer->matchesAt(candidate)) {
            collectOne(candidate, 0.0f);
          }
          candidate = nextApproximation();
          if (candidate == PostingsReader::END) return;
        }
        cursor = candidate;
      }
    }
  }

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*)filter;
    auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;
    for (;;) {
      auto doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
      }
      if constexpr (hasSortRanges) {
        while (sortPrune && doc >= competitiveEnd) {
          auto range = collector.nextCompetitiveRange(segnum, doc);
          competitiveEnd = range.end;
          if (range.begin <= doc) break;
          doc = scorer->advance(range.begin);
          if (doc == PostingsReader::END) break;
        }
        if (doc == PostingsReader::END) break;
      }
      if (domainBits && !domainBits->get(doc)) {
        continue;
      }
      if (builder) {
        builder->add(doc);
      }
      auto score = needScores ? scorer->score() : 0.0f;
      collectOne(doc, score);
    }
  } else {
    assert(filter->type == DocSet::ARRAY);
    ArrDocSet* arrDocs = (ArrDocSet*)filter;
    std::span<const int32_t> arr = arrDocs->docs();
    const int32_t* cur = arr.data();
    const int32_t* arrEnd = cur + arr.size();
    while (cur != arrEnd) {
      int32_t doc = *cur;
      if constexpr (hasSortRanges) {
        if (sortPrune && doc >= competitiveEnd) {
          auto range = collector.nextCompetitiveRange(segnum, doc);
          competitiveEnd = range.end;
          if (range.begin > doc) {
            cur = screaming::gallopLowerBound(cur, arrEnd, range.begin);
            continue;
          }
        }
      }
      cur++;
      if (scorer->docId() < doc) {
        scorer->advance(doc);
      }
      if (scorer->docId() != doc) {
        continue;
      }
      if (builder) {
        builder->add(doc);
      }
      auto score = needScores ? scorer->score() : 0.0f;
      collectOne(doc, score);
    }
  }
}

inline int64_t collectFirstKConstant(int32_t segnum, Query::Scorer* scorer,
                                     DocSet* filter, TopDocsCollector& collector,
                                     int64_t topCount) {
  assert(scorer != nullptr);
  assert(topCount > 0);
  int64_t collected = 0;
  auto collectOne = [&](int32_t doc) {
    collector.collect(segnum, doc, scorer->score());
    collected++;
  };

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*) filter;
    auto* domainBits = bitDocs == nullptr ? nullptr : &bitDocs->bits();
    while (collected < topCount) {
      int32_t doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
      }
      if (domainBits != nullptr && !domainBits->get(doc)) {
        continue;
      }
      collectOne(doc);
    }
  } else {
    for (int32_t doc : ((ArrDocSet*) filter)->docs()) {
      if (scorer->docId() < doc) {
        scorer->advance(doc);
      }
      if (scorer->docId() != doc) {
        continue;
      }
      collectOne(doc);
      if (collected >= topCount) {
        break;
      }
    }
  }
  return collected;
}

inline void collectConstantTopKAndDomain(
    int32_t segnum, Query::Scorer* scorer, DocSet* filter,
    DocSetBuilder& builder, TopDocsCollector& collector) {
  assert(scorer != nullptr);
  assert(collector.topCount > 0);
  skipCount(SkipStats::constantPullDomainCollections);
  int64_t collected = 0;
  auto collectOne = [&](int32_t doc) {
    builder.add(doc);
    if (collected < collector.topCount) {
      collector.collect(segnum, doc, scorer->score());
    } else {
      collector.hitCount++;
    }
    collected++;
  };

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*) filter;
    auto* domainBits = bitDocs == nullptr ? nullptr : &bitDocs->bits();
    for (;;) {
      int32_t doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
      }
      if (domainBits != nullptr && !domainBits->get(doc)) {
        continue;
      }
      collectOne(doc);
    }
  } else {
    assert(filter->type == DocSet::ARRAY);
    for (int32_t doc : ((ArrDocSet*) filter)->docs()) {
      if (scorer->docId() < doc) {
        scorer->advance(doc);
      }
      if (scorer->docId() != doc) {
        continue;
      }
      collectOne(doc);
    }
  }
}

// Exhaust the windowed bulk path, counting matches and optionally building the
// complete domain. No docs or scores are otherwise materialized.
//
// Every window driver below stops early once the calling thread's execution
// budget is exhausted (ExecutionBudget): the count, domain and collector then
// cover the windows produced so far, consistently with one another.  Only
// per-request results are built here; filter sets that may be cached or used
// as exclusions (QueryPrep::materialize) always run to completion.
inline int64_t countMatchesWindowed(BulkScorer* bulk, DocSet* filter,
                                    DocSetBuilder* builder, int32_t maxDoc) {
  assert(bulk != nullptr);
  int64_t count = 0;
  int32_t cursor = 0;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    if (ExecutionBudget::currentExhausted()) break;
    int32_t next = bulk->countNextWindow(count, builder, filter, cursor, maxDoc);
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  assert(builder == nullptr || count == builder->card());
  return count;
}

inline void collectCountWindowed(BulkScorer* bulk, DocSet* filter,
                                 DocSetBuilder* builder,
                                 TopDocsCollector& collector, int32_t maxDoc) {
  assert(bulk != nullptr);
  assert(collector.topCount == 0);
  collector.hitCount += countMatchesWindowed(bulk, filter, builder, maxDoc);
}

enum class ConstantScoreDrain : uint8_t {
  LIMIT_ONLY,
  COMPLETE,
};

// Constant scores rank by doc order, so a segment's top K docs are its first
// K matches. LIMIT_ONLY stops as soon as K are captured. COMPLETE continues
// with count windows to satisfy an exact-count or domain consumer. One bulk
// arrangement then serves ranking, count, and domain; an independent capture
// scorer would rebuild every clause (a multiterm clause re-runs its dictionary
// scan per build). Score windows carry the weight's constant, so reported
// scores match what a pull scorer from the same weight returns.
inline void collectFirstKConstantWindowed(
    int32_t segnum, BulkScorer* bulk, DocSet* filter, DocSetBuilder* builder,
    TopDocsCollector& collector, int32_t maxDoc,
    ConstantScoreDrain drain) {
  assert(bulk != nullptr);
  assert(collector.topCount > 0);
  assert(drain == ConstantScoreDrain::COMPLETE || builder == nullptr);
  skipCount(SkipStats::constantWindowCaptures);
  bulk->setTopKDepth((int32_t) collector.topCount, false);
  int64_t collected = 0;
  int64_t overshoot = 0;
  int32_t cursor = 0;
  ScoreWindow window;
  while (cursor != PostingsReader::END && cursor < maxDoc
         && collected < collector.topCount) {
    if (ExecutionBudget::currentExhausted()) return;
    int32_t next = bulk->scoreNextWindow(window, filter, cursor, maxDoc,
                                         std::numeric_limits<float>::lowest());
    if (builder != nullptr) {
      skipCount(SkipStats::bulkDomainWindowsFed);
    }
    for (int32_t i = 0; i < window.size; i++) {
      int32_t doc = window.docs[(size_t) i];
      if (builder != nullptr) {
        builder->add(doc);
      }
      if (collected < collector.topCount) {
        collector.collect(segnum, doc, window.scores[(size_t) i]);
        collected++;
      } else if (drain == ConstantScoreDrain::COMPLETE) {
        // The capture window ran past K; these are count-only.
        collector.hitCount++;
        overshoot++;
      } else {
        break;
      }
    }
    if (next == PostingsReader::END) {
      cursor = next;
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  if (drain == ConstantScoreDrain::LIMIT_ONLY) {
    return;
  }
  if (cursor != PostingsReader::END && cursor < maxDoc) {
    bulk->prepareMembership();
  }
  int64_t count = 0;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    if (ExecutionBudget::currentExhausted()) break;
    int32_t next = bulk->countNextWindow(count, builder, filter, cursor, maxDoc);
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  collector.hitCount += count;
  assert(builder == nullptr
         || builder->card() == collected + overshoot + count);
  unused(overshoot);
}

// Consume every match window of [begin, end): produce, feed the domain
// builder and the collector, stop at the bound. END from a bounded request
// only means the bound was reached (BulkScorer::matchNextWindow contract),
// so callers may keep issuing later ranges afterwards. Returns the number
// of docs emitted to the collector.
template <typename Collector>
int64_t feedMatchWindows(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                         DocSetBuilder* builder, Collector& collector,
                         int32_t begin, int32_t end) {
  ScoreWindow window;
  int32_t cursor = begin;
  int64_t emitted = 0;
  while (cursor < end) {
    if (ExecutionBudget::currentExhausted()) break;
    int32_t next = bulk->matchNextWindow(window, filter, cursor, end);
    emitted += window.size;
    if (builder != nullptr) {
      for (int32_t i = 0; i < window.size; i++) {
        builder->add(window.docs[(size_t) i]);
      }
    }
    if constexpr (requires(Collector& c, std::span<const int32_t> docs) {
        c.collectWindow(int32_t{}, docs);
      }) {
      collector.collectWindow(
          segnum, window.docs.first((size_t)window.size));
    } else {
      for (int32_t i = 0; i < window.size; i++) {
        collector.collect(segnum, window.docs[(size_t)i], 0.0f);
      }
    }
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  return emitted;
}

// The first visited leaf at or after `leaf`, or leafCount.
inline int64_t nextVisitedLeaf(std::span<const uint64_t> visited,
                               int64_t leaf, int64_t leafCount) {
  size_t w = (size_t)(leaf >> 6);
  size_t end = std::min(visited.size(), (size_t)((leafCount + 63) >> 6));
  if (w >= end) return leafCount;
  uint64_t bits = visited[w] & (~0ULL << (leaf & 63));
  while (bits == 0) {
    if (++w >= end) return leafCount;
    bits = visited[w];
  }
  return std::min(leafCount, (int64_t)(w << 6) + std::countr_zero(bits));
}

// Collect match-only windows in doc order. Scores are intentionally absent
// from this path; collectors receive the unscored sentinel literal.
// allowPruning: when true and no domain builder is attached, a collector
// exposing nextCompetitiveRange() has noncompetitive doc blocks jumped over
// before window production, and every window request is bounded by the
// competitive range end - a one-doc candidate range costs one doc, not a
// whole overshooting window of key-rejected neighbors. Skipped docs are
// uncounted. visitedBlocks (with its block geometry) marks key blocks an
// earlier seeded pass already enumerated: the walk jumps them, and window
// production stops before the next visited block, coalescing unvisited runs.
template <typename Collector>
void collectTopKMatchWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                              DocSetBuilder* builder, Collector& collector,
                              int32_t maxDoc, bool allowPruning = false,
                              std::span<const uint64_t> visitedBlocks = {},
                              int32_t visitedBlockSize = 0) {
  assert(bulk != nullptr);
  assert(visitedBlocks.empty() || visitedBlockSize > 0);
  int32_t cursor = 0;
  constexpr bool hasSortRanges =
      requires { collector.nextCompetitiveRange(segnum, (int32_t)0); };
  [[maybe_unused]] bool sortPrune = false;
  if constexpr (hasSortRanges) {
    sortPrune = allowPruning && builder == nullptr;
  }
  while (cursor < maxDoc) {
    if (ExecutionBudget::currentExhausted()) break;
    int32_t rangeEnd = maxDoc;
    if constexpr (hasSortRanges) {
      if (sortPrune) {
        auto range = collector.nextCompetitiveRange(segnum, cursor);
        if (range.begin == PostingsReader::END) {
          break;
        }
        cursor = std::max(cursor, range.begin);
        rangeEnd = std::min(rangeEnd, range.end);
      }
    }
    if (!visitedBlocks.empty()) {
      int64_t block = (int64_t)cursor / visitedBlockSize;
      int32_t blockLimit = (int32_t)std::min<int64_t>(
          (block + 1) * (int64_t)visitedBlockSize, (int64_t)maxDoc);
      if ((visitedBlocks[(size_t)(block >> 6)] >> (block & 63)) & 1) {
        skipCount(SkipStats::fieldSortSeedPass2Skips);
        cursor = blockLimit;
        continue;
      }
      // Leave the producer its ordinary window width through each unvisited
      // run. Splitting every remaining leaf charges sparse streams for empty
      // windows even when seeding visited only a handful of leaves.
      int64_t limit = ((int64_t)rangeEnd + visitedBlockSize - 1) / visitedBlockSize;
      int64_t next = nextVisitedLeaf(visitedBlocks, block + 1, limit);
      rangeEnd = (int32_t)std::min<int64_t>(rangeEnd, next * visitedBlockSize);
    }
    feedMatchWindows(segnum, bulk, filter, builder, collector, cursor,
                     rangeEnd);
    cursor = rangeEnd;
  }
}

// How the exact-domain driver reads a materialized domain (cached docs, or
// every doc for match-all with no deletes); ExactDomainSortCosts picks one,
// and re-picks for whatever is left at checkpoints.
enum class ExactDomainSortPhase : uint8_t {
  // Leaves in the column's persisted bound order, until a bound proves the
  // rest out.
  BOUND_ORDER,
  // The walk: the leaves holding domain docs in doc order, against the
  // maturing bottom, either gathering every doc unclassified,
  SCAN,
  // or classifying each coarse block and then each leaf of a competitive one.
  SWEEP,
};

// Exact-domain field-sort economics, in units of one scanned doc (a key
// gather plus its test against the bottom), over the leaves of a segment not
// yet settled. Every decision reads the same quantities:
// - the visit floor: the leaves bound order is expected to visit, those
//   bounded at or before the bottom the heap is predicted to reach
//   (FieldSortCollector::predictedVisitFloor); at a checkpoint, refined by
//   the proofs bound order has made;
// - the admitted leaves: those bounded at or before the heap's current
//   bottom (FieldSortCollector::leavesWithinBottom), which no traversal from
//   here on can exceed - sound, where the floor is an estimate;
// - the leaves outside the domain's value envelope (SegmentKeys::
//   leavesOutside), which hold no domain doc: those past its best edge lead
//   bound order and cost it a visit each but no gather, and the domain's m
//   docs per leaf spread over the rest, the inside leaves;
// - on keys that improve with doc order (FieldSortCollector::
//   keysImproveWithDocOrder), where in doc order the walk meets the best
//   keys: the head of the bound order (FieldSortCollector::headLeaf)
//   against the domain's doc span, read as the inside leaves the walk
//   gathers first (its lead).
// Bound order visits the floor's leaves, each a visit plus its m docs
// gathered out of order. The walk gathers the leaves it reaches before its
// bottom matures: with the best keys spread over the domain, at its j-th
// leaf the bottom ranks k among j*m docs, so a leaf is still competitive
// with probability about floor/j, which adds floor*ln(inside/floor) leaves;
// with them late in doc order it gathers its lead first. A sweep classifies
// every inside leaf holding a domain doc and skips what classification
// rules out; a scan classifies nothing and gathers every doc, in batches.
// Every phase also pays for the docs that enter the heap (admissions), a
// sift through its log2(k) levels each: few for bound order, which meets
// the best keys first, but every doc of a walk's lead, which on keys that
// improve with doc order beats all before it: a newest-first walk of a
// time-ordered field admits each doc it gathers.
//
// The plan is the cheapest over the whole segment, except that a domain
// whose every leaf is bounded before its key floor (a range on the sort field
// itself: FieldSortCollector::leavesBeforeKeyFloor) is scanned, since no
// classification can skip a leaf there.
//
// Re-decisions. A running phase is re-judged at checkpoints - bound order at
// doubling counts of its order walked past the empty leaves, from enough
// leaves to expect kCheckpointExpectedProofs proofs on its floor; the walk at
// doubling gathered counts once the heap is full - by one rule (redecide):
// the remaining costs of every phase over the unsettled rest, work done
// being sunk, the incumbent keeping the rest unless the cheapest costs less.
// The one margin, kProgressMargin, absorbs what each decision reads with
// most error. For bound order that is its proofs (heap entries below the
// next leaf's bound, which are final: p proofs after w leaves past the
// empty ones project w*k/p more), one proof being far noisier than any cost
// constant, so it reads them kProgressMargin times over and keeps the rest
// only while the costs, so read, favor it. A checkpoint that finds a full
// heap and no proof - placed where the prior expects several - falsifies
// the prior: bound order then prices every leaf the bottom admits and keeps
// no advantage, the signature of a domain correlated against the key in a
// way nothing announced (a range on another field holding the same data).
// The walk reads the same floor estimate and the admitted leaves, both of
// which tell it what bound order would still visit; its costs carry the
// model's error alone, so it keeps the rest unless another phase costs
// under 1/kProgressMargin of it.
//
// Calibrated on the 5M-doc benchgame corpus (9829 leaves), hugin
// 2026-10-06: per-query times of each phase alone (best of 3 after 2 warm
// passes) over 13,686 resident memberships - queries.txt's filter sample
// under term filters of 48%, 10%, 1% and 0.1%, ranges of 50% to 0.1% on the
// uniform sort field, the newest 10% and 1% and oldest 50% and 10% of the
// time-ordered field, and no filter; uniform keys at k = 10, 100 and 1000,
// time-ordered keys newest first at k = 10 and 1000 and oldest first at
// k = 100; 12,780 ARRAY, 906 BITSET. Choosing by these costs takes 1.008
// times the per-query best phase, against 1.030 without the admissions
// (1.12 to 1.15 on the time-ordered postures, where a scan costs 100 ns a
// doc at k = 1000 against 7 on uniform keys) and 1.11 for the costs this
// model replaced. The fit is flat along ridges: the leaf visit trades with
// the out-of-order gather (H + R*m) and the admission with the sweep's
// leaf, settings from H = 7 to 12 lying within 0.2% of each other. A third
// walk level that classified blocks alone was never the cheapest at any
// fit. Ranges on a sibling field holding the same data are left out of the
// fit: the prior cannot see that correlation, and re-decisions serve those
// (1.27 of the best phase for the plan alone).
struct ExactDomainSortCosts {
  static constexpr double kBoundOrderLeafCost = 8;
  static constexpr double kBoundOrderEmptyLeafCost = 2;
  static constexpr double kBoundOrderDocCost = 2;
  static constexpr double kSweepLeafCost = 3;
  static constexpr double kScanDocCost = 1;
  // Per level of the heap (log2 k) a doc entering the top k sifts through.
  static constexpr double kAdmitLevelCost = 3;
  // The first bound-order checkpoint waits for this many expected proofs
  // under the prior, so a domain that meets it rarely shows none: P ~
  // exp(-8); and for at least kMinCheckpointLeaves leaves, which small floors
  // rarely need. Proofs early in the order run a few behind the prior's rate
  // on the benchgame corpus (0 where 4 were expected on several uniform-key
  // queries, the domain catching up by about 8), which a first reading at 4
  // took for a falsified prior.
  static constexpr int64_t kCheckpointExpectedProofs = 8;
  static constexpr int64_t kMinCheckpointLeaves = 16;
  // Bound order reads its proofs this many times over, and the walk hands
  // the rest on only to a phase costing under 1/kProgressMargin of its own
  // (see Re-decisions).
  static constexpr int64_t kProgressMargin = 2;

  // Leaves holding at least one of card docs spread uniformly over them.
  static double occupiedLeaves(double card, double leaves) {
    if (leaves <= 0) return 0;
    return -leaves * std::expm1(-card / leaves);
  }

  // Heap admissions among `docs` gathered docs, at most `admittable` (those
  // of the leaves the bottom admits): with keys in random order every doc of
  // the first k enters, then the i-th with chance k/i, k*(1 + ln(docs/k)) in
  // all; and every doc of a walk's lead (`leadDocs`), where keys improving
  // with doc order (time-ordered keys newest first) put each doc ahead of
  // all before it.
  static double admissions(double docs, double k, double leadDocs,
                           double admittable) {
    double records = k * (1 + std::log(std::max(1.0, docs / k)));
    return std::min({docs, leadDocs + records, admittable});
  }

  // Leaves the walk gathers at a visit floor with the best keys spread over
  // the domain, at most every leaf.
  static double sweptLeaves(double floor, double leaves) {
    if (floor >= leaves) return leaves;
    if (floor <= 0) return 0;
    return floor * (1 + std::log(leaves / floor));
  }

  // The unsettled rest of a segment as a decision sees it.
  struct Rest {
    int64_t card = 0;      // domain docs not yet gathered
    int64_t leaves = 0;    // leaves not yet settled
    int64_t floor = 0;     // leaves bound order is expected to visit
    int64_t admitted = 0;  // leaves the bottom still admits
    // Of the leaves, those wholly past the envelope's best edge, which bound
    // order visits first and finds empty, and those that may hold domain
    // docs (inside the envelope).
    int64_t empty = 0;
    int64_t inside = 0;
    // Inside leaves the walk gathers before it meets the best keys.
    int64_t lead = 0;
    // The heap's size, k.
    int64_t topCount = 1;
  };

  struct PhaseCosts {
    double boundOrder = 0;
    double scan = 0;
    double sweep = 0;

    // Ties go to the walk, and within it to the scan.
    ExactDomainSortPhase cheapest() const {
      ExactDomainSortPhase walk = cheapestWalk();
      return boundOrder < of(walk) ? ExactDomainSortPhase::BOUND_ORDER : walk;
    }

    ExactDomainSortPhase cheapestWalk() const {
      return sweep < scan ? ExactDomainSortPhase::SWEEP
                          : ExactDomainSortPhase::SCAN;
    }

    double of(ExactDomainSortPhase phase) const {
      switch (phase) {
        case ExactDomainSortPhase::BOUND_ORDER: return boundOrder;
        case ExactDomainSortPhase::SCAN: return scan;
        case ExactDomainSortPhase::SWEEP: return sweep;
      }
      return scan;
    }

    // The one re-decision rule (see Re-decisions): the incumbent keeps the
    // rest unless the cheapest phase costs under 1/margin of it.
    ExactDomainSortPhase redecide(ExactDomainSortPhase incumbent,
                                  double margin) const {
      ExactDomainSortPhase best = cheapest();
      return margin * of(best) < of(incumbent) ? best : incumbent;
    }
  };

  // A rest as every phase reads it: the inside leaves and the m domain docs
  // each holds; bound order's visits, of which the first `empty` are the
  // empty leaves leading its order; the inside leaves the bottom admits; the
  // walk's lead; and the inside leaves the walk gathers (swept).
  struct Reading {
    double inside = 1;
    double m = 0;
    double visits = 0;
    double empty = 0;
    double admitted = 0;
    double lead = 0;
    double swept = 0;
  };

  static Reading read(const Rest& rest) {
    Reading r;
    r.inside = (double)std::clamp<int64_t>(rest.inside, 1, rest.leaves);
    r.m = (double)rest.card / r.inside;
    r.visits = (double)std::min({rest.floor, rest.admitted, rest.leaves});
    r.empty = std::min((double)std::max<int64_t>(0, rest.empty), r.visits);
    // The walk passes the empty leaves without a look, so it matures and
    // gathers over the inside ones alone.
    r.admitted = std::max(
        0.0, (double)std::min(rest.admitted, rest.leaves) - r.empty);
    double floor = std::min(r.visits - r.empty, r.inside);
    r.lead = std::clamp((double)rest.lead, 0.0, r.inside);
    r.swept = std::min(std::max(sweptLeaves(floor, r.inside),
                                std::min(r.lead + floor, r.inside)),
                       r.admitted);
    return r;
  }

  static PhaseCosts costs(const Rest& rest) {
    if (rest.leaves <= 0 || rest.card <= 0) return {};
    double card = (double)rest.card;
    Reading r = read(rest);
    double m = r.m;
    double k = (double)std::max<int64_t>(1, rest.topCount);
    double admit = kAdmitLevelCost * std::log2(std::max(2.0, k));
    double gathered = (r.visits - r.empty) * m;
    double admittable = r.admitted * m;
    PhaseCosts costs;
    costs.boundOrder = r.empty * kBoundOrderEmptyLeafCost
        + (r.visits - r.empty) * kBoundOrderLeafCost
        + gathered * kBoundOrderDocCost
        + admit * admissions(gathered, k, 0, admittable);
    costs.scan = kScanDocCost * card
        + admit * admissions(card, k, r.lead * m, admittable);
    costs.sweep = kSweepLeafCost * occupiedLeaves(card, r.inside)
        + m * r.swept
        + admit * admissions(m * r.swept, k, r.lead * m, admittable);
    return costs;
  }

  // Leaf descent in the doc-order match-window walk (FieldSortCollector::
  // nextCompetitiveRange, over a query-driven or unrouted domain): the
  // leaves of each coarse block the bottom cannot rule out are classified,
  // and the runs it rules out are neither produced nor gathered. The card
  // is what the walk expects to collect (TopDocsReq::walkMembershipWork): a
  // conjunction's yield, not its lead's cost, which put its bottom 10x too
  // low and its skipped leaves 10x too full (AND of a 1% and a 10% term on
  // 10M docs: 2388 us descending vs 2123 not). A two-phase query's card is
  // its approximation's, the candidates the gate checks. The walk
  // finds about `swept` of the inside leaves competitive, so descent skips
  // the domain's other card - m*swept docs, for classifying the inside
  // leaves at kWalkLeafCost walked docs each (the bound test plus the
  // producer call each skipped run splits off). The share of leaves
  // skipped cannot decide it alone: a leaf's bound covers all of its docs,
  // so a domain of a given size admits the same share of leaves at any
  // density, while the docs each skipped leaf saves fall with the density.
  //
  // Measured on single 5M and 10M-doc segments (munin, uniform keys,
  // term-query domains, 2026-10-09), forced descent against none: the
  // skipped docs per inside leaf at which descent lost were 0.04-0.41
  // (10M at 0.1%, k = 10: 620 vs 398 us; at k = 1: 220 vs 195), and at
  // which it won 1.19-4.1 (10M at 1%, k = 100: 2221 vs 2323 us; k = 10:
  // 569 vs 1590); the price sits mid-bracket. On a 10M-doc segment at
  // k = 10 it descends from about 1.4 domain docs a leaf (0.27%).
  static constexpr double kWalkLeafCost = 0.7;

  static bool walkDescends(const Rest& rest) {
    if (rest.leaves <= 0 || rest.card <= 0) return false;
    Reading r = read(rest);
    return (double)rest.card - r.m * r.swept > kWalkLeafCost * r.inside;
  }

  // Whether a domain of `card` docs is scanned without planning: bound order
  // visits at least one leaf, which costs more than scanning it.
  static bool scansOutright(int64_t card) {
    return (double)card <= kBoundOrderLeafCost;
  }

  // A whole segment's plan: the cheapest phase, or the scan when no bound
  // can skip a leaf (`unskippable`).
  static ExactDomainSortPhase plan(const Rest& whole, bool unskippable) {
    if (unskippable) return ExactDomainSortPhase::SCAN;
    return costs(whole).cheapest();
  }

  // Bound order's floor at a checkpoint, `walked` leaves of its order behind
  // with `proofs` final heap entries, its prior `prior` and the bottom
  // admitting `admitted` leaves (see Re-decisions). No proof can precede the
  // `empty` leaves that lead the order, so proofs project from past them,
  // read kProgressMargin times over: empty + (walked - empty)*k/(margin*p).
  // A checkpoint past them that finds a full heap and no proof falsifies the
  // prior, leaving only the admitted leaves; one with the heap still filling
  // proves nothing yet, and the prior stands.
  static int64_t readBoundOrder(int64_t prior, int64_t walked, int64_t proofs,
                                int64_t topCount, int64_t admitted,
                                int64_t empty, bool heapFull) {
    int64_t beyond = walked - empty;
    int64_t projected = std::numeric_limits<int64_t>::max();
    if (proofs > 0) {
      projected = empty
          + (int64_t)std::ceil((double)std::max<int64_t>(0, beyond)
                               * (double)topCount
                               / ((double)proofs * kProgressMargin));
    } else if (beyond <= 0 || !heapFull) {
      projected = prior;
    }
    return std::min(projected, admitted);
  }

  // First bound-order checkpoint, in leaves of the order walked: past the
  // `empty` leaves, enough to expect kCheckpointExpectedProofs proofs at the
  // prior's rate of k/(floor - empty) per leaf.
  static int64_t firstCheckpoint(int64_t floor, int64_t topCount,
                                 int64_t empty) {
    int64_t dense = std::max<int64_t>(0, floor - empty);
    return std::max<int64_t>(
        kMinCheckpointLeaves,
        empty + (kCheckpointExpectedProofs * dense + topCount - 1) / topCount);
  }
};

// Compare added work with removed work after cancelling the payload in the
// final competitive floor, which both routes must produce and collect. The
// same margin applies to every round. Narrow-window overhead does not cancel:
// price its extra calls, prefix seeks, sorting and the unvisited sweep too.
struct QueryDrivenSeedCosts {
  Query::ScorerSupplier::MembershipWork work;
  // Earlier segments may already have filled K. Never credit their pruning.
  ExactDomainSortCosts::Rest baseline;
  bool lateHead;
  int32_t leafSize;
  double docOrderTrend;

  double roundCost(int64_t count, double lastPosition) const {
    // std::sort on persisted leaf IDs measured 7.5-8.2 instructions per
    // n*log2(n) for scattered orders and about 5 for reversed orders.
    return work.seek(lastPosition) + 64
        + 9 * count * std::log2(std::max<int64_t>(2, count));
  }

  bool worth(int64_t produced, int64_t gathered, int64_t count,
             double lastPosition, int64_t floor, double matches,
             int64_t settled = 0) const {
    if (matches <= 0 || settled + count < floor) return false;
    double leaves = (double)std::max<int64_t>(1, baseline.leaves);
    double inside = (double)std::clamp<int64_t>(baseline.inside, 1, baseline.leaves);
    double visits = (double)std::min({floor, baseline.admitted, baseline.leaves});
    double empty = std::min((double)std::max<int64_t>(0, baseline.empty), visits);
    double dense = std::min(visits - empty, inside);
    double admitted = std::max(0.0, (double)baseline.admitted - empty);
    double swept = docOrderTrend < -0.5 ? dense : std::max(
        ExactDomainSortCosts::sweptLeaves(dense, inside),
        std::min((double)baseline.lead + dense, inside));
    swept = std::min(swept, admitted);
    double coverage = std::max(0.0, (double)(settled + count) - empty);
    // Price the expected positive remainder of the floor's sqrt(K) yield
    // fluctuation, after oversampling. Charging a full sigma here and then
    // applying the margin would price the same uncertainty twice.
    double k = (double)std::max<int64_t>(1, baseline.topCount);
    double sigma = dense / std::sqrt(k);
    double z = sigma > 0 ? (coverage - dense) / sigma : 0;
    double expectedRest = sigma * (std::exp(-0.5 * z * z)
        / std::sqrt(2 * std::numbers::pi) - z * 0.5 * std::erfc(z / std::sqrt(2.0)));
    double rest = std::clamp(expectedRest, 0.0, std::max(0.0, admitted - coverage));
    double seeds = (double)(produced + count);
    double m = matches / inside;
    double beforeDocs = swept * m;
    // Keep already gathered work, even if a later sample lowers the yield.
    // Only the first proposal owns the complete leading empty prefix. Later
    // proposals conservatively price every selected leaf as possibly emitting.
    double emitting = produced == 0 && settled == 0
        ? std::max(0.0, (double)count - empty) : (double)count;
    double afterDocs = gathered + (emitting + rest) * m;
    double removed = 32 * std::max(0.0, beforeDocs - dense * m)
        + work.savedProduce * std::max(0.0, swept - dense) / leaves;
    double added = 32 * std::max(0.0, afterDocs - dense * m)
        + work.produce * std::max(0.0, seeds + rest - dense) / leaves;
    double beforeAdmissions = ExactDomainSortCosts::admissions(
        beforeDocs, k, baseline.lead * m, admitted * m);
    double afterAdmissions = ExactDomainSortCosts::admissions(
        afterDocs, k, docOrderTrend > 0.5 ? afterDocs : 0, admitted * m);
    double heap = 32 * ExactDomainSortCosts::kAdmitLevelCost
        * std::log2(std::max(2.0, k)) * (beforeAdmissions - afterAdmissions);
    removed += std::max(0.0, heap);
    added += std::max(0.0, -heap);

    // A random competitive mask has about n*(1-n/N) runs. Correlated keys
    // leave one contiguous run. Both amortize complete producer windows;
    // seeds pay a call per leaf, and price the remaining sweep conservatively.
    double fullWindows = swept * leafSize / DocsEnumMeta::L1_DOCS;
    double beforeWindows = std::abs(docOrderTrend) > 0.5 ? fullWindows
        : std::min(swept, swept * (1 - swept / leaves) + fullWindows);
    added += work.window * std::max(0.0, seeds + rest - beforeWindows);
    // One final restart and visited-mask sweep, plus each round's actual
    // selection/sort and prefix navigation. Neither reserve restarts per round.
    added += work.seek(0) + 4 * std::ceil(leaves / 64)
        + 24 * (seeds + rest) + roundCost(count, lastPosition);
    return ExactDomainSortCosts::kProgressMargin * added < removed;
  }
};

// How a materialized domain is served; see ExactDomainSortCosts.
struct ExactDomainSortRoute {
  int64_t card = 0;
  // The predicted visit floor (FieldSortCollector::predictedVisitFloor).
  int64_t floor = 0;
  // Leaves holding no value of the domain's envelope: past its best edge
  // (`empty`, bound order's first), and in all (`outside`).
  int64_t empty = 0;
  int64_t outside = 0;
  // Inside leaves the walk gathers before it meets the best keys, on keys
  // that improve with doc order (0 otherwise).
  int64_t lead = 0;
  ExactDomainSortPhase phase = ExactDomainSortPhase::SWEEP;
  // Re-decide at checkpoints. Off only when a test forces bound order.
  bool checkProgress = true;
  // Test override: bound order hands off after this many leaf gathers.
  int64_t gatherCapForTests = 0;
};

// Leaves an earlier bound-order phase settled - gathered, or proven out on a
// tie - one bit each (empty: none). Later phases pass them.
inline bool leafVisited(std::span<const uint64_t> visited, int64_t leaf) {
  return !visited.empty()
      && ((visited[(size_t)(leaf >> 6)] >> (leaf & 63)) & 1);
}

// The exact-domain driver reads a materialized domain three ways, each
// feeding ascending (docs, keys) batches to admitGathered: gatherLeaf(leaf)
// feeds one leaf, leaves arriving in any order (bound order); nextLeaf(leaf)
// is the first leaf at or after `leaf` holding a domain doc, and
// gatherNext(leaf) feeds the leaf nextLeaf just returned, the pair walking
// the domain forward (the classifying walk, in increasing leaves);
// scanFrom(leaf, visited, budget) feeds the domain docs of the unvisited
// leaves from `leaf` on in doc order, unclassified, until at least `budget`
// docs and the end of a leaf, returning the leaf to resume at (the scan).
// The forward walks share one cursor, so the walk resumes at any level
// where it stopped.
//
// Bitset domain: a whole-segment word span (a cached set or liveDocs), or an
// empty span meaning every doc (match-all with no deletes). Leaves gather
// through the comparator's fused masked gather, in any order; a leaf with no
// domain doc costs the check of its words.
template <typename Collector>
class BitSetSortDomain {
  using KeyBlockPlan = typename Collector::KeyBlockPlan;
  int32_t segnum;
  std::span<const uint64_t> words;
  Collector& collector;
  KeyBlockPlan plan;
  std::span<int32_t> outDocs;
  std::span<int64_t> outKeys;
  size_t wordsPerLeaf;

public:
  BitSetSortDomain(int32_t segnum, std::span<const uint64_t> words,
                   Collector& collector, const KeyBlockPlan& plan,
                   MemPool& pool)
      : segnum(segnum), words(words), collector(collector), plan(plan),
        outDocs(pool.make_span<int32_t>((size_t)plan.leafSize)),
        outKeys(pool.make_span<int64_t>((size_t)plan.leafSize)),
        wordsPerLeaf((size_t)plan.leafSize / 64) {
    assert(plan.leafSize % 64 == 0);
  }

  int32_t gatherLeaf(int64_t leaf) {
    int32_t n = plan.batch->gatherLeafMasked(leaf, words, outDocs, outKeys);
    if (n > 0) {
      collector.admitGathered(segnum, outDocs.first((size_t)n),
                              outKeys.first((size_t)n), plan.batch);
    }
    return n;
  }

  int64_t nextLeaf(int64_t leaf) const {
    if (words.empty()) return leaf;
    size_t w = (size_t)leaf * wordsPerLeaf;
    while (w < words.size() && words[w] == 0) w++;
    // Past the last word no leaf holds a domain doc (a partial last leaf
    // ends mid-span, so dividing the span size would name it).
    if (w >= words.size()) return plan.leafCount;
    return (int64_t)(w / wordsPerLeaf);
  }

  void gatherNext(int64_t leaf) { gatherLeaf(leaf); }

  int64_t scanFrom(int64_t leaf, std::span<const uint64_t> visited,
                   int64_t budget) {
    for (leaf = nextLeaf(leaf); leaf < plan.leafCount;
         leaf = nextLeaf(leaf + 1)) {
      if (leafVisited(visited, leaf)) continue;
      budget -= gatherLeaf(leaf);
      if (budget <= 0) return leaf + 1;
    }
    return plan.leafCount;
  }
};

// Array domain: the whole sorted materialized set (an ARRAY DocSet, below
// the bitset promotion threshold). Keys come from the comparator's
// order-independent key gather; the docs themselves need no copy. A
// bound-order visit binary-searches its leaf's docs (no forward cursor to
// gallop from); the forward walk gallops a cursor from one domain doc's leaf
// to the next, so the sweep costs the leaves holding domain docs, not every
// leaf; the scan gathers the whole array in batches.
template <typename Collector>
class ArraySortDomain {
  using KeyBlockPlan = typename Collector::KeyBlockPlan;
  int32_t segnum;
  std::span<const int32_t> docs;
  Collector& collector;
  KeyBlockPlan plan;
  std::span<int64_t> outKeys;
  const int32_t* cursor;

  int32_t leafStart(int64_t leaf) const {
    return (int32_t)std::min<int64_t>(
        leaf * (int64_t)plan.leafSize,
        (int64_t)std::numeric_limits<int32_t>::max());
  }

  void feed(const int32_t* lo, const int32_t* hi) {
    std::span<const int32_t> run(lo, hi);
    plan.batch->gatherKeys(run, outKeys.first(run.size()));
    collector.admitGathered(segnum, run, outKeys.first(run.size()),
                            plan.batch);
  }

public:
  ArraySortDomain(int32_t segnum, std::span<const int32_t> docs,
                  Collector& collector, const KeyBlockPlan& plan,
                  MemPool& pool)
      : segnum(segnum), docs(docs), collector(collector), plan(plan),
        outKeys(pool.make_span<int64_t>((size_t)plan.leafSize)),
        cursor(docs.data()) {}

  void gatherLeaf(int64_t leaf) {
    const int32_t* end = docs.data() + docs.size();
    const int32_t* lo = std::lower_bound(docs.data(), end, leafStart(leaf));
    const int32_t* hi =
        screaming::gallopLowerBound(lo, end, leafStart(leaf + 1));
    if (lo != hi) feed(lo, hi);
  }

  int64_t nextLeaf(int64_t leaf) {
    const int32_t* end = docs.data() + docs.size();
    cursor = screaming::gallopLowerBound(cursor, end, leafStart(leaf));
    return cursor == end ? plan.leafCount : *cursor / plan.leafSize;
  }

  void gatherNext(int64_t leaf) {
    const int32_t* hi = screaming::gallopLowerBound(
        cursor, docs.data() + docs.size(), leafStart(leaf + 1));
    feed(cursor, hi);
    cursor = hi;
  }

  // Batches run across leaf boundaries, stopping only at leaves an earlier
  // bound-order phase visited and at the end of the leaf holding the
  // budget-th doc. Leaves the forward cursor where the sweep resumes.
  int64_t scanFrom(int64_t leaf, std::span<const uint64_t> visited,
                   int64_t budget) {
    const int32_t* end = docs.data() + docs.size();
    cursor = screaming::gallopLowerBound(cursor, end, leafStart(leaf));
    while (cursor < end) {
      leaf = *cursor / plan.leafSize;
      if (leafVisited(visited, leaf)) {
        cursor = screaming::gallopLowerBound(cursor, end, leafStart(leaf + 1));
        continue;
      }
      const int32_t* hi = end;
      int64_t next = nextVisitedLeaf(visited, leaf + 1, plan.leafCount);
      if (next < plan.leafCount) {
        hi = screaming::gallopLowerBound(cursor, end, leafStart(next));
      }
      int64_t resume = -1;
      if (hi - cursor >= budget) {
        resume = cursor[budget - 1] / plan.leafSize + 1;
        hi = screaming::gallopLowerBound(cursor + budget, hi,
                                         leafStart(resume));
      }
      budget -= hi - cursor;
      while (cursor < hi) {
        const int32_t* batchEnd =
            cursor + std::min((size_t)(hi - cursor), outKeys.size());
        feed(cursor, batchEnd);
        cursor = batchEnd;
      }
      if (resume >= 0) return std::min(resume, plan.leafCount);
    }
    return plan.leafCount;
  }
};

// The walk at `level`: the leaves from `leaf` on holding domain docs that
// bound order has not settled, in doc order. A SWEEP classifies them against
// the current bottom once the heap is full - each coarse block once, at its
// first such leaf, then each leaf of a competitive block; a SCAN gathers in
// batches and classifies nothing. Returns the leaf to resume at once at least
// `budget` docs are gathered (at a leaf boundary), or leafCount at the end. A
// tie skip uses the leaf's start as its first unseen doc, which is exact
// here: the walk never revisits a doc, and the leaves before it in the block
// hold no unseen domain doc.
template <typename Collector, typename Domain>
int64_t walkInDocOrder(int32_t segnum, Collector& collector,
                       const typename Collector::KeyBlockPlan& plan,
                       std::span<const uint64_t> visited, Domain& domain,
                       int64_t leaf, ExactDomainSortPhase level,
                       int64_t budget) {
  assert(level != ExactDomainSortPhase::BOUND_ORDER);
  if (level == ExactDomainSortPhase::SCAN) {
    return domain.scanFrom(leaf, visited, budget);
  }
  using BC = typename Collector::BlockClass;
  int64_t leavesPerBlock = plan.blockSize / plan.leafSize;
  int64_t openBlock = -1;
  int64_t start = collector.hitCount;
  for (leaf = domain.nextLeaf(leaf); leaf < plan.leafCount;
       leaf = domain.nextLeaf(leaf + 1)) {
    if (leafVisited(visited, leaf)) continue;
    if (collector.hitCount - start >= budget) return leaf;
    if (collector.heapFull()) {
      int32_t firstUnseen = (int32_t)(leaf * (int64_t)plan.leafSize);
      int64_t block = leaf / leavesPerBlock;
      if (block != openBlock) {
        if (collector.classifyBlock(segnum, firstUnseen,
                                    plan.batch->blockBestKey(block),
                                    plan.batch)
            != BC::COLLECT) {
          skipCount(SkipStats::fieldSortBlocksSkipped);
          leaf = std::min((block + 1) * leavesPerBlock, plan.leafCount) - 1;
          continue;
        }
        openBlock = block;
      }
      if (collector.classifyBlock(segnum, firstUnseen,
                                  plan.batch->leafBestKey(leaf), plan.batch)
          != BC::COLLECT) {
        skipCount(SkipStats::fieldSortLeavesSkipped);
        continue;
      }
    }
    skipCount(SkipStats::fieldSortSweepLeaves);
    domain.gatherNext(leaf);
  }
  return plan.leafCount;
}

// The driver's state within a segment: the leaves bound order has settled
// (visited), its next rank in the persisted order and the rank at which it
// next re-decides; the walk's next leaf and the gathered count at which it
// next re-decides; and the one floor estimate both read - the prior, then
// whatever bound order's checkpoints read (ExactDomainSortCosts, Re-
// decisions).
struct ExactDomainSortState {
  std::span<uint64_t> visited;
  int64_t rank = 0;
  int64_t boundCheckpoint = -1;
  int64_t walked = 0;
  int64_t walkCheckpoint = 0;
  int64_t floor = 0;
  int64_t gatheredBefore = 0;
};

// The unsettled rest of a segment, `leaves` leaves of it, with `ranks` of
// bound order behind: the floor and the admitted leaves count from the
// order's head.
template <typename Collector>
ExactDomainSortCosts::Rest exactDomainRest(
    const Collector& collector, const typename Collector::KeyBlockPlan& plan,
    const ExactDomainSortRoute& route, const ExactDomainSortState& state,
    int64_t ranks, int64_t leaves, int64_t admitted) {
  int64_t gathered = collector.hitCount - state.gatheredBefore;
  // Bound order's empty leaves lead its order, and the rest's inside
  // leaves shrink with it as a share of the leaves.
  int64_t total = std::max<int64_t>(1, (int64_t)plan.leafCount);
  int64_t inside = (total - route.outside) * std::max<int64_t>(1, leaves)
      / total;
  return {std::max<int64_t>(1, route.card - gathered),
          std::max<int64_t>(1, leaves),
          std::max<int64_t>(0, std::min(state.floor, admitted) - ranks),
          std::max<int64_t>(0, admitted - ranks),
          std::max<int64_t>(0, route.empty - ranks),
          std::max<int64_t>(1, inside),
          std::max<int64_t>(0, route.lead - state.walked * inside / total),
          collector.topCount};
}

// Bound order: leaves in ascending (bound, leaf) order, which the column
// persists per direction (KeyBatch::boundOrderLeaf), so the phase is a scan
// of that order, resumed at state.rank. A SKIP_STRICT leaf proves global
// termination (every later leaf's bound is at least as bad, and the bottom
// only improves); a SKIP_TIE leaf is skipped alone, and stays settled for
// the walk. Leaves before state.walked (the walk's prefix) and visited
// leaves are already settled: they pass without a gather, though their
// bounds still terminate. Returns nothing when the phase settled the segment
// (a proof, or every leaf walked); otherwise the phase a checkpoint hands
// the rest to (ExactDomainSortCosts, Re-decisions), or the cheapest walk at
// the test cap.
template <typename Collector, typename Domain>
std::optional<ExactDomainSortPhase> collectTopKBoundOrder(
    int32_t segnum, Collector& collector,
    const typename Collector::KeyBlockPlan& plan,
    const ExactDomainSortRoute& route, Domain& domain,
    ExactDomainSortState& state) {
  using BC = typename Collector::BlockClass;
  using Costs = ExactDomainSortCosts;
  skipCount(SkipStats::fieldSortBestFirstActivations);
  int64_t topCount = collector.topCount;
  int64_t leafGathers = 0;
  std::span<uint64_t> visited = state.visited;
  auto settled = [&](int64_t leaf) {
    return leaf < state.walked || leafVisited(visited, leaf);
  };
  auto settle = [&](int64_t leaf) {
    visited[(size_t)(leaf >> 6)] |= 1ULL << (leaf & 63);
  };
  if (state.boundCheckpoint < 0) {
    state.boundCheckpoint = route.checkProgress
        ? Costs::firstCheckpoint(state.floor, topCount, route.empty)
        : std::numeric_limits<int64_t>::max();
  }
  for (; state.rank < plan.leafCount; state.rank++) {
    auto [leaf, bound] = plan.batch->boundOrderLeaf(state.rank);
    if (collector.heapFull()) {
      auto cls = collector.classifyBlock(
          segnum, (int32_t)(leaf * (int64_t)plan.leafSize), bound,
          plan.batch);
      if (cls == BC::SKIP_STRICT) {
        // Every leaf from here on not yet settled is skipped.
        skipCount(SkipStats::fieldSortBestFirstTerminations);
        if (SkipStats::enabled) {
          for (int64_t r = state.rank; r < plan.leafCount; r++) {
            SkipStats::fieldSortLeavesSkipped +=
                !settled(plan.batch->boundOrderLeaf(r).leaf);
          }
        }
        return std::nullopt;
      }
      if (cls == BC::SKIP_TIE) {
        if (!settled(leaf)) {
          skipCount(SkipStats::fieldSortLeavesSkipped);
          settle(leaf);
        }
        continue;
      }
    }
    if (settled(leaf)) continue;
    settle(leaf);
    skipCount(SkipStats::fieldSortBestFirstLeaves);
    domain.gatherLeaf(leaf);
    leafGathers++;
    int64_t ranks = state.rank + 1;
    if (ranks == plan.leafCount) return std::nullopt;
    bool capped = route.gatherCapForTests > 0
        && leafGathers >= route.gatherCapForTests;
    if (capped || ranks >= state.boundCheckpoint) {
      // Doubling the work past the empty leaves, which cost next to nothing.
      state.boundCheckpoint = ranks
          + std::max<int64_t>(ranks - route.empty, Costs::kMinCheckpointLeaves);
      int64_t admitted = collector.leavesWithinBottom();
      state.floor = capped
          ? plan.leafCount
          : Costs::readBoundOrder(
                state.floor, ranks,
                collector.countKeysBelow(
                    plan.batch->boundOrderLeaf(ranks).bound, plan.batch),
                topCount, admitted, route.empty, collector.heapFull());
      auto costs = Costs::costs(exactDomainRest(collector, plan, route, state,
                                                ranks, plan.leafCount - ranks,
                                                admitted));
      ExactDomainSortPhase next = capped
          ? costs.cheapestWalk()
          : costs.redecide(ExactDomainSortPhase::BOUND_ORDER, 1);
      if (next != ExactDomainSortPhase::BOUND_ORDER) {
        state.rank = ranks;
        skipCount(SkipStats::fieldSortBestFirstFallbacks);
        return next;
      }
    }
  }
  return std::nullopt;
}

// Exact-domain driver: the route's phase, then whatever the checkpoints hand
// the rest to (ExactDomainSortCosts, Re-decisions): bound order resumes its
// cursor over the leaves the walk has not passed, and the walk resumes its
// leaf past the ones bound order visited. Every in-domain doc is gathered at
// most once, so hitCount keeps the pruned-path semantics (counts gathered
// docs); an exact hit count may instead come from the known domain's
// cardinality. The representation lives in the Domain (BitSetSortDomain,
// ArraySortDomain); the route never depends on it.
template <typename Collector, typename Domain>
void collectTopKExactDomain(int32_t segnum, Collector& collector,
                            MemPool& pool,
                            const typename Collector::KeyBlockPlan& plan,
                            const ExactDomainSortRoute& route,
                            Domain& domain) {
  using Costs = ExactDomainSortCosts;
  assert(plan.batch != nullptr && plan.leafSize != 0);
  ExactDomainSortState state;
  state.floor = route.floor;
  state.gatheredBefore = collector.hitCount;
  int64_t firstWalkCheckpoint =
      std::max<int64_t>(collector.topCount, plan.leafSize);
  state.walkCheckpoint = firstWalkCheckpoint;
  ExactDomainSortPhase phase = route.phase;
  for (;;) {
    if (phase == ExactDomainSortPhase::BOUND_ORDER) {
      if (state.visited.empty()) {
        state.visited =
            pool.make_span<uint64_t>((size_t)((plan.leafCount + 63) >> 6));
        std::fill(state.visited.begin(), state.visited.end(), 0);
      }
      auto next =
          collectTopKBoundOrder(segnum, collector, plan, route, domain, state);
      if (!next.has_value()) return;
      phase = *next;
      continue;
    }
    skipCount(phase == ExactDomainSortPhase::SCAN
                  ? SkipStats::fieldSortDomainScans
                  : SkipStats::fieldSortDocOrderSweeps);
    ExactDomainSortPhase level = phase;
    while (phase == level) {
      int64_t gathered = collector.hitCount - state.gatheredBefore;
      state.walked = walkInDocOrder(
          segnum, collector, plan, state.visited, domain, state.walked, level,
          std::max<int64_t>(1, state.walkCheckpoint - gathered));
      if (state.walked >= plan.leafCount) return;
      gathered = collector.hitCount - state.gatheredBefore;
      if (!collector.heapFull()) {
        state.walkCheckpoint = gathered + firstWalkCheckpoint;
        continue;
      }
      state.walkCheckpoint = 2 * gathered;
      if (!route.checkProgress) continue;
      phase = Costs::costs(exactDomainRest(collector, plan, route, state,
                                           state.rank,
                                           plan.leafCount - state.walked,
                                           collector.leavesWithinBottom()))
                  .redecide(level, Costs::kProgressMargin);
      if (phase != level) skipCount(SkipStats::fieldSortWalkHandOffs);
    }
  }
}

// Bitset entry: domainWords is a whole-segment word span (cached filter or
// liveDocs), or empty meaning every doc (match-all, no deletes).
template <typename Collector>
void collectTopKBitSetDomain(int32_t segnum,
                             std::span<const uint64_t> domainWords,
                             Collector& collector, MemPool& pool,
                             const ExactDomainSortRoute& route) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  BitSetSortDomain<Collector> domain(segnum, domainWords, collector, plan,
                                     pool);
  collectTopKExactDomain(segnum, collector, pool, plan, route, domain);
}

// Array entry: domainDocs is the whole sorted materialized set.
template <typename Collector>
void collectTopKArrayDomain(int32_t segnum,
                            std::span<const int32_t> domainDocs,
                            Collector& collector, MemPool& pool,
                            const ExactDomainSortRoute& route) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  ArraySortDomain<Collector> domain(segnum, domainDocs, collector, plan,
                                    pool);
  collectTopKExactDomain(segnum, collector, pool, plan, route, domain);
}

// Seeded two-pass query-driven field-sort driver. Select the best-bounded
// leaves, visit each round in doc order, then reposition the same product
// for the sweep from doc 0. Each round matures the heap bottom without
// retaining another scorer graph in the segment pool. The visited mask
// records enumerated and classified-out leaves, so no match is collected
// twice and hitCount keeps pruned-path semantics. Classified-out leaves
// stay out because the bottom only improves.
//
// The prior prices the first round. Subsequent rounds use observed joint
// yield, and every round must repay production, L1 prefix navigation and the
// fragmented sweep under the same margin. Late keys can justify another
// round, but cannot waive the empty/underfull abort or the total work budget.
inline constexpr int64_t kSeedRoundLeaves = 16;
// Doubling reaches any leaf of a 2^20-leaf segment within 16 rounds. Keep
// a final bound on restart work even when observed yield fluctuates.
inline constexpr int64_t kMaxSeedRounds = 64;

template <typename Collector>
void collectTopKMatchWindowedSeeded(
    int32_t segnum, BulkScorer* bulk, DocSet* filter, Collector& collector,
    MemPool& pool, int32_t maxDoc, int64_t seedCount,
    const QueryDrivenSeedCosts& costs) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr && plan.leafSize != 0);
  assert(bulk != nullptr);
  using BC = typename Collector::BlockClass;
  skipCount(SkipStats::fieldSortSeededActivations);

  std::span<uint64_t> visited =
      pool.make_span<uint64_t>((size_t)((plan.leafCount + 63) >> 6));
  std::fill(visited.begin(), visited.end(), 0);
  auto isVisited = [&](int64_t leaf) {
    return (visited[(size_t)(leaf >> 6)] >> (leaf & 63)) & 1;
  };
  auto markVisited = [&](int64_t leaf) {
    visited[(size_t)(leaf >> 6)] |= 1ULL << (leaf & 63);
  };
  std::span<int64_t> seeds = pool.make_span<int64_t>((size_t)plan.leafCount);

  int64_t enumerated = 0;
  int64_t enumeratedInside = 0;
  int64_t gathered = 0;
  double yield = costs.work.matches;
  int64_t fillAbortBudget = std::max<int64_t>(8, (int64_t)std::ceil(
      4.0 * collector.topCount * plan.leafCount / std::max(1.0, yield)));
  int64_t consecutiveEmpty = 0;
  // Every leaf ranked before `rank` in bound order is visited.
  int64_t rank = 0;
  int64_t round = std::max<int64_t>(1, seedCount);
  int64_t rounds = 0;
  for (;;) {
    if (ExecutionBudget::currentExhausted()) break;
    // A completed round can spend the last underfull leaf without triggering
    // the in-round guard. Do not pay another selection, sort and restart.
    if (!collector.heapFull() && enumerated >= fillAbortBudget) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
    // Select the round's best-bounded unvisited leaves - the next stretch
    // of the column's persisted bound order, the order the exact-domain
    // driver walks - then visit them in ascending doc order so the seed
    // scorer only moves forward. Selection is metadata-only: no
    // classification, no postings work.
    int64_t selected = 0;
    int64_t lastLeaf = 0;
    int64_t next = rank;
    for (; next < plan.leafCount && selected < round; next++) {
      int64_t leaf = plan.batch->boundOrderLeaf(next).leaf;
      if (!isVisited(leaf)) {
        seeds[(size_t)selected++] = leaf;
        lastLeaf = std::max(lastLeaf, leaf);
      }
    }
    std::span<int64_t> roundSeeds = seeds.first((size_t)selected);
    if (roundSeeds.empty()) break;
    double lastPosition = (double)(lastLeaf + 1) / plan.leafCount;
    if (rounds != 0) {
      int64_t floor = collector.predictedVisitFloor((int64_t)std::max(1.0, yield));
      // Decide at the margin: earlier rounds' selection and seeks are sunk.
      // A short round alone cannot reach the floor that pays for seeding, so
      // price the rest of the schedule up to it, not just this round.
      int64_t remainder = std::max<int64_t>(selected, floor - rank);
      if (!costs.worth(enumerated, gathered, remainder, lastPosition, floor, yield, rank)) {
        skipCount(SkipStats::fieldSortSeedFillAborts);
        break;
      }
    }
    std::sort(roundSeeds.begin(), roundSeeds.end());
    if (rounds != 0 && !roundSeeds.empty()) {
      bulk->reposition((int32_t) (roundSeeds.front() * (int64_t) plan.leafSize));
    }
    // Admission prices the complete selection. Finish it unless the yield
    // abort fires; stopping at its first hit would forfeit the floor whose
    // pruning paid for the round. Classification can still prove leaves out.
    bool emitted = false;
    bool abandoned = false;
    for (int64_t leaf : roundSeeds) {
      if (collector.heapFull()) {
        if (collector.classifyBlock(
                segnum, (int32_t)(leaf * (int64_t)plan.leafSize),
                plan.batch->leafBestKey(leaf), plan.batch) != BC::COLLECT) {
          markVisited(leaf);
          skipCount(SkipStats::fieldSortSeedClassifiedOut);
          continue;
        }
      } else if (enumerated >= fillAbortBudget) {
        abandoned = true;
        break;
      }
      int32_t begin = (int32_t)(leaf * (int64_t)plan.leafSize);
      int32_t leafEnd = (int32_t)std::min<int64_t>(
          (leaf + 1) * (int64_t)plan.leafSize, (int64_t)maxDoc);
      int64_t emittedHere = feedMatchWindows(segnum, bulk, filter, nullptr,
                                             collector, begin, leafEnd);
      markVisited(leaf);
      enumerated++;
      if (costs.baseline.inside != plan.leafCount) {
        enumeratedInside += collector.primaryKeys->leafIntersectsValues(leaf);
      }
      gathered += emittedHere;
      skipCount(SkipStats::fieldSortSeedLeaves);
      if (emittedHere != 0) {
        consecutiveEmpty = 0;
        emitted = true;
      } else if (++consecutiveEmpty >= fillAbortBudget) {
        abandoned = true;
        break;
      }
    }
    if (abandoned) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
    rank = next;
    if (!costs.lateHead) break;
    // Zero-hit leaves inside the envelope count in the observed rate.
    // Outside leaves still pay production and abort budgets, but provide no
    // evidence about inside density. Neither budget restarts for late heads.
    int64_t insideSamples = costs.baseline.inside == plan.leafCount
        ? enumerated : enumeratedInside;
    if (insideSamples > 0) {
      yield = std::min(costs.work.matches,
          (double)gathered * costs.baseline.inside / insideSamples);
    }
    while (rank < plan.leafCount
           && isVisited(plan.batch->boundOrderLeaf(rank).leaf)) {
      rank++;
    }
    if (rank >= plan.leafCount) break;
    if (collector.heapFull()) break;
    if (++rounds > kMaxSeedRounds) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
    round = emitted ? kSeedRoundLeaves
                    : 2 * std::max(round, kSeedRoundLeaves / 2);
    skipCount(SkipStats::fieldSortSeedRounds);
  }

  bulk->reposition(0);
  collectTopKMatchWindowed(segnum, bulk, filter, nullptr, collector,
                           maxDoc, true, visited, plan.leafSize);
}

// Rank score windows into a top-k collector. This is intentionally only a
// ranking primitive; callers that need an exact count or materialized domain
// compose it with countMatchesWindowed using an independent scorer supplier.
// allowPruning=false pins theta at lowest so every match is scored.
template <typename Collector>
void collectTopKWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                         Collector& collector, MaxScoreAccumulator* accumulator,
                         int32_t maxDoc, bool allowPruning = true,
                         BulkScorer* exactScorer = nullptr) {
  static_assert(requires(Collector& c) { c.minCompetitiveVal; },
                "collectTopKWindowed is only for score top-k collectors");

  assert(bulk != nullptr);
  bulk->setTopKDepth(collector.topCount, allowPruning);
  int32_t cursor = 0;
  ScoreWindow window;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    if (ExecutionBudget::currentExhausted()) break;
    float localTheta;
    if constexpr (requires {
        collector.minCompetitiveScoreForNextDoc(segnum, cursor);
      }) {
      localTheta = collector.minCompetitiveScoreForNextDoc(segnum, cursor);
    } else {
      localTheta = collector.minCompetitiveVal;
    }
    float theta = !allowPruning
      ? std::numeric_limits<float>::lowest()
      : accumulator != nullptr
        ? std::max(localTheta, accumulator->get())
        : localTheta;
    int32_t next = bulk->scoreNextWindow(window, filter, cursor, maxDoc, theta);

    if (exactScorer != nullptr && window.size > 0) {
      assert(exactScorer->supportsExactCandidateScoring());
      exactScorer->scoreCandidatesExact(
          window.docs.first((size_t) window.size),
          window.scores.first((size_t) window.size));
    }
    for (int32_t i = 0; i < window.size; i++) {
      int32_t doc = window.docs[(size_t) i];
      float score = window.scores[(size_t) i];
      float oldMinCompetitiveVal = collector.minCompetitiveVal;
      collector.collect(segnum, doc, score);
      if (accumulator != nullptr && collector.minCompetitiveVal > oldMinCompetitiveVal) {
        accumulator->accumulate(collector.minCompetitiveVal);
      }
    }

    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
}

} // namespace luxir
