// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "luxir/query/Query.h"
#include "luxir/search/DocSet.h"

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
inline int64_t countMatchesWindowed(BulkScorer* bulk, DocSet* filter,
                                    DocSetBuilder* builder, int32_t maxDoc) {
  assert(bulk != nullptr);
  int64_t count = 0;
  int32_t cursor = 0;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
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
  int64_t count = 0;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
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

// Collect match-only windows in doc order. Scores are intentionally absent
// from this path; collectors receive the unscored sentinel literal.
// allowPruning: when true and no domain builder is attached, a collector
// exposing nextCompetitiveRange() has noncompetitive doc blocks jumped over
// before window production, and every window request is bounded by the
// competitive range end - a one-doc candidate range costs one doc, not a
// whole overshooting window of key-rejected neighbors. Skipped docs are
// uncounted. visitedBlocks (with its block geometry) marks key blocks an
// earlier seeded pass already enumerated: the walk jumps them, and window
// production never crosses a block boundary without re-consulting the mask.
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
      rangeEnd = std::min(rangeEnd, blockLimit);
    }
    feedMatchWindows(segnum, bulk, filter, builder, collector, cursor,
                     rangeEnd);
    cursor = rangeEnd;
  }
}

// Exact-domain field-sort economics, in units of one gathered doc (a key
// gather plus its heap admission test). A materialized domain (cached docs,
// or every doc for match-all with no deletes) is served in two phases: an
// optional bound-order phase over the leaf bounds with proof termination,
// then a forward doc-order sweep over the leaves it did not visit,
// classifying each against the maturing bottom. With uniform keys
// independent of the domain, both gather the ~k*leafSize domain docs of the
// expectedFloor leaves any correct traversal visits. Bound order adds
// kBoundOrderLeafCost per visited leaf (reading the next leaf and its bound
// from the persisted order, and a random-access gather). The sweep instead
// gathers the leaves it reaches before its bottom matures: at its j-th leaf
// the bottom ranks k among j*m domain docs (m = card/leafCount per leaf), so
// a leaf is still competitive with probability about floor/j, which adds
// floor*ln(leafCount/floor) leaves of m docs; and it classifies every leaf at
// kSweepLeafCost. Bound order therefore pays when one visit saves more
// gathers than it costs: kBoundOrderLeafCost < m*ln(leafCount/floor) +
// kSweepLeafCost*leafCount/floor.
//
// Calibrated on the 5M-doc benchgame corpus (9829 leaves), hugin 2026-10-04:
// per-query times of each phase alone over resident term-filter memberships
// of 10K-2.4M docs at k = 10, 100, 1000 (314 sub-saturating cases, two runs)
// cross at floors of about 3100 (k=10) and 4700 (k=100, just under the
// 2*floor < leafCount availability gate). The data pins only the k=10
// crossover (k=100 sits at the gate, k=1000 has no sub-saturating sweep
// win), so the sweep cost keeps its earlier fit and the visit cost is fitted
// alone: these costs cross at 3022 (k=10) and at the gate (k=100), and
// choosing by them loses 0.3% against the per-query best phase.
//
// A domain correlated with the sort key breaks the independence premise. When
// the correlation is known - a range on the sort field itself puts every
// member's key at or past a key floor (FieldSortCollector::primaryValues) -
// the leaves bounded before that floor (clipLeaves) can never be proven out,
// so bound order visits all of them before its first proof and the visit
// floor is at least clipLeaves: uniform wide zones clip nearly every leaf and
// sweep from the start, while tight (doc-order correlated) zones clip only
// the leaves lying past the range's edge, and bound order keeps paying when
// those are few. A correlation nothing announces
// (a filter on another field holding the same data) shows up only as missing
// proofs at the progress checkpoints below, which hand such a domain to the
// sweep.
struct ExactDomainSortCosts {
  static constexpr double kBoundOrderLeafCost = 8.5;
  static constexpr double kSweepLeafCost = 2;
  // The first progress checkpoint waits for this many expected proofs (heap
  // entries below every unvisited bound) under the independence premise, so
  // an independent domain rarely shows none: P ~ exp(-4); and for at least
  // kMinCheckpointLeaves gathers, which small floors rarely need.
  static constexpr int64_t kCheckpointExpectedProofs = 4;
  static constexpr int64_t kMinCheckpointLeaves = 16;
  // A proof-rate floor estimate from p proofs is Poisson-noisy (~1/sqrt(p))
  // and part of the phase is already paid, so a projection hands off only
  // when it overshoots the crossover by this factor; bailing on noise near
  // the crossover would pay both routes.
  static constexpr int64_t kProgressMargin = 2;

  static bool boundOrderPays(int64_t floor, int64_t card, int64_t leafCount) {
    if (floor <= 0 || floor >= leafCount || card <= 0) return false;
    double perLeaf = (double)card / (double)leafCount;
    double sweepExtra =
        (double)floor * perLeaf * std::log((double)leafCount / (double)floor)
        + kSweepLeafCost * (double)leafCount;
    return (double)floor * kBoundOrderLeafCost < sweepExtra;
  }

  // Checkpoint verdict after `gathers` bound-order leaves hold `proofs` final
  // heap entries. No proof at all is the signature of a domain correlated
  // with the key (its members sit below every leaf bound); otherwise the
  // proofs, all made past the clipLeaves visited first, project a floor of
  // clipLeaves + (gathers - clipLeaves)*k/proofs.
  static bool boundOrderKeepsPaying(int64_t gathers, int64_t proofs,
                                    int64_t topCount, int64_t card,
                                    int64_t leafCount,
                                    int64_t clipLeaves = 0) {
    if (proofs <= 0) return false;
    int64_t beyond = std::max<int64_t>(0, gathers - clipLeaves);
    int64_t observedFloor = clipLeaves
        + (beyond * topCount + proofs - 1) / proofs;
    return boundOrderPays(observedFloor / kProgressMargin, card, leafCount);
  }

  // First progress checkpoint, in bound-order leaf gathers: the clipped
  // leaves (no proof can precede them), then enough leaves to expect
  // kCheckpointExpectedProofs proofs at the independence premise's rate of
  // k/expectedFloor per leaf.
  static int64_t firstCheckpoint(int64_t expectedFloor, int64_t clipLeaves,
                                 int64_t topCount) {
    return std::max<int64_t>(
        kMinCheckpointLeaves,
        clipLeaves
            + (kCheckpointExpectedProofs * expectedFloor + topCount - 1)
                / topCount);
  }
};

// How a materialized domain is served; see ExactDomainSortCosts.
struct ExactDomainSortRoute {
  int64_t card = 0;
  // Leaves the independence premise expects a bounded traversal to visit,
  // ceil(k/d), and the leaves bounded before the domain's key floor
  // (FieldSortCollector::leavesBeforeKeyFloor), which any bounded traversal
  // visits. The router prices max(expectedFloor, clipLeaves).
  int64_t expectedFloor = 0;
  int64_t clipLeaves = 0;
  // Start in bound order; false sweeps the whole domain in doc order.
  bool boundOrder = false;
  // Judge the proof progress bound order actually makes at checkpoints
  // (boundOrderKeepsPaying) and hand off to the sweep when it falls short.
  // Off only when a test forces the bound-order route.
  bool checkProgress = true;
  // Test override: hand off after this many bound-order leaf gathers.
  int64_t gatherCapForTests = 0;
};

// Doc-order phase: every leaf the bound-order phase did not visit, in doc
// order, each coarse block and then each leaf classified against the current
// bottom. A tie skip uses the block or leaf start as its first unseen doc,
// which is exact here (the sweep never revisits a doc).
template <typename Collector, typename GatherLeaf>
void sweepLeavesInDocOrder(int32_t segnum, Collector& collector,
                           const typename Collector::KeyBlockPlan& plan,
                           std::span<const uint64_t> visited,
                           GatherLeaf& gather) {
  using BC = typename Collector::BlockClass;
  int64_t leavesPerBlock = plan.blockSize / plan.leafSize;
  for (int64_t block = 0; block < plan.blockCount; block++) {
    if (collector.heapFull()
        && collector.classifyBlock(
               segnum, (int32_t)(block * (int64_t)plan.blockSize),
               plan.batch->blockBestKey(block), plan.batch)
            != BC::COLLECT) {
      skipCount(SkipStats::fieldSortBlocksSkipped);
      continue;
    }
    int64_t leafLimit =
        std::min((block + 1) * leavesPerBlock, plan.leafCount);
    for (int64_t leaf = block * leavesPerBlock; leaf < leafLimit; leaf++) {
      if (!visited.empty()
          && ((visited[(size_t)(leaf >> 6)] >> (leaf & 63)) & 1)) {
        continue;
      }
      if (collector.heapFull()
          && collector.classifyBlock(
                 segnum, (int32_t)(leaf * (int64_t)plan.leafSize),
                 plan.batch->leafBestKey(leaf), plan.batch)
              != BC::COLLECT) {
        skipCount(SkipStats::fieldSortLeavesSkipped);
        continue;
      }
      skipCount(SkipStats::fieldSortSweepLeaves);
      gather(leaf, true);
    }
  }
}

// Bound-order phase: leaves are visited in ascending (bound, leaf) order,
// which the column persists per direction (KeyBatch::boundOrderLeaf), so the
// walk is a scan of that order. A SKIP_STRICT leaf proves global termination
// (every later leaf's bound is at least as bad, and the bottom only
// improves); a SKIP_TIE leaf is skipped alone. Returns true when the phase
// settled the segment (proof, or every leaf visited); false hands the
// unvisited leaves to the sweep. The hand-off comes from progress
// checkpoints (firstCheckpoint, past any clipped leaves, then at doubling
// gather counts): heap entries below the next leaf's bound are final, and
// boundOrderKeepsPaying judges how many there are.
template <typename Collector, typename GatherLeaf>
bool collectTopKBoundOrder(int32_t segnum, Collector& collector,
                           const typename Collector::KeyBlockPlan& plan,
                           const ExactDomainSortRoute& route,
                           std::span<uint64_t> visited, GatherLeaf& gather) {
  using BC = typename Collector::BlockClass;
  using Costs = ExactDomainSortCosts;
  skipCount(SkipStats::fieldSortBestFirstActivations);

  int64_t topCount = collector.topCount;
  int64_t checkpoint = std::numeric_limits<int64_t>::max();
  if (route.checkProgress) {
    checkpoint = Costs::firstCheckpoint(route.expectedFloor, route.clipLeaves,
                                        topCount);
  }
  int64_t leafGathers = 0;
  for (int64_t rank = 0; rank < plan.leafCount; rank++) {
    auto [leaf, bound] = plan.batch->boundOrderLeaf(rank);
    if (collector.heapFull()) {
      auto cls = collector.classifyBlock(
          segnum, (int32_t)(leaf * (int64_t)plan.leafSize), bound,
          plan.batch);
      if (cls == BC::SKIP_STRICT) {
        // Credit this leaf and every later one as skipped, so cross-arm
        // skip accounting stays comparable.
        skipCount(SkipStats::fieldSortBestFirstTerminations);
        if (SkipStats::enabled) {
          SkipStats::fieldSortLeavesSkipped += plan.leafCount - rank;
        }
        return true;
      }
      if (cls == BC::SKIP_TIE) {
        skipCount(SkipStats::fieldSortLeavesSkipped);
        continue;
      }
    }
    visited[(size_t)(leaf >> 6)] |= 1ULL << (leaf & 63);
    skipCount(SkipStats::fieldSortBestFirstLeaves);
    gather(leaf, false);
    leafGathers++;
    if (rank + 1 == plan.leafCount) return true;
    if (route.gatherCapForTests > 0 && leafGathers >= route.gatherCapForTests) {
      return false;
    }
    if (leafGathers >= checkpoint) {
      int64_t proofs = collector.countKeysBelow(
          plan.batch->boundOrderLeaf(rank + 1).bound, plan.batch);
      if (!Costs::boundOrderKeepsPaying(leafGathers, proofs, topCount,
                                        route.card, plan.leafCount,
                                        route.clipLeaves)) {
        return false;
      }
      checkpoint = 2 * leafGathers;
    }
  }
  return true;
}

// Exact-domain driver: the route's optional bound-order phase, then the
// doc-order sweep over whatever it left. Every in-domain doc is gathered at
// most once, so hitCount keeps the pruned-path semantics (counts gathered
// docs); an exact hit count may instead come from the known domain's
// cardinality. The domain representation lives in `gather(leaf, inDocOrder)`,
// which feeds one leaf's in-domain (docs, keys) to admitGathered; the bitset
// and array entry points below supply it.
template <typename Collector, typename GatherLeaf>
void collectTopKBestFirst(int32_t segnum, Collector& collector, MemPool& pool,
                          const typename Collector::KeyBlockPlan& plan,
                          const ExactDomainSortRoute& route,
                          GatherLeaf&& gather) {
  assert(plan.batch != nullptr && plan.leafSize != 0);
  std::span<uint64_t> visited;
  if (route.boundOrder) {
    visited = pool.make_span<uint64_t>((size_t)((plan.leafCount + 63) >> 6));
    std::fill(visited.begin(), visited.end(), 0);
    if (collectTopKBoundOrder(segnum, collector, plan, route, visited,
                              gather)) {
      return;
    }
    skipCount(SkipStats::fieldSortBestFirstFallbacks);
  } else {
    skipCount(SkipStats::fieldSortDocOrderSweeps);
  }
  sweepLeavesInDocOrder(segnum, collector, plan, visited, gather);
}

// Bitset-domain entry: domainWords is a whole-segment word span (cached
// filter or liveDocs), or empty meaning every doc (match-all, no deletes).
// Leaves gather through the comparator's fused masked gather, in any order.
template <typename Collector>
void collectTopKBitSetBestFirst(int32_t segnum,
                                std::span<const uint64_t> domainWords,
                                Collector& collector, MemPool& pool,
                                const ExactDomainSortRoute& route) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  std::span<int32_t> outDocs = pool.make_span<int32_t>((size_t)plan.leafSize);
  std::span<int64_t> outKeys = pool.make_span<int64_t>((size_t)plan.leafSize);
  collectTopKBestFirst(
      segnum, collector, pool, plan, route, [&](int64_t leaf, bool) {
        int32_t n = plan.batch->gatherLeafMasked(leaf, domainWords, outDocs,
                                                 outKeys);
        if (n > 0) {
          collector.admitGathered(segnum, outDocs.first((size_t)n),
                                  outKeys.first((size_t)n), plan.batch);
        }
      });
}

// Array-domain entry: domainDocs is the whole sorted materialized set (an
// ARRAY DocSet, below the bitset promotion threshold). A bound-order visit
// binary-searches its leaf's doc sub-span (leaves arrive in bound order, so
// there is no forward cursor to gallop from); the sweep gallops forward from
// the end of the previous gathered leaf. Keys come from the comparator's
// order-independent key gather - the docs themselves need no copy.
template <typename Collector>
void collectTopKArrayBestFirst(int32_t segnum,
                               std::span<const int32_t> domainDocs,
                               Collector& collector, MemPool& pool,
                               const ExactDomainSortRoute& route) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  std::span<int64_t> outKeys = pool.make_span<int64_t>((size_t)plan.leafSize);
  const int32_t* domainEnd = domainDocs.data() + domainDocs.size();
  const int32_t* sweepCursor = domainDocs.data();
  collectTopKBestFirst(
      segnum, collector, pool, plan, route,
      [&](int64_t leaf, bool inDocOrder) {
        int32_t begin = (int32_t)std::min<int64_t>(
            leaf * (int64_t)plan.leafSize,
            (int64_t)std::numeric_limits<int32_t>::max());
        int32_t end = (int32_t)std::min<int64_t>(
            (leaf + 1) * (int64_t)plan.leafSize,
            (int64_t)std::numeric_limits<int32_t>::max());
        const int32_t* lo = inDocOrder
            ? screaming::gallopLowerBound(sweepCursor, domainEnd, begin)
            : std::lower_bound(domainDocs.data(), domainEnd, begin);
        const int32_t* hi = screaming::gallopLowerBound(lo, domainEnd, end);
        if (inDocOrder) sweepCursor = hi;
        if (lo == hi) return;
        std::span<const int32_t> docs(lo, hi);
        plan.batch->gatherKeys(docs, outKeys.first(docs.size()));
        collector.admitGathered(segnum, docs, outKeys.first(docs.size()),
                                plan.batch);
      });
}

// Seeded two-pass query-driven field-sort driver. The domain is reachable
// only through forward scorers (streaming postings, no rewind), so
// bound-ordered visiting is reformulated as two monotone passes: select the
// seedCount best-bounded LEAVES, walk them in doc order with one bulk
// scorer so the heap bottom matures to near-final, then sweep from doc 0
// with a second, independent bulk scorer whose competitive-range walk now
// skips essentially everything. Both products must exist before pass 1 runs:
// a pass-2 scorer cannot be recovered after pass 1 advanced a shared one.
// Leaves pass 1 actually enumerated are recorded in a visited mask the sweep
// jumps over, so every match reaches the collector at most once and hitCount
// keeps pruned-path semantics. Once the heap fills, each remaining seed is
// classified against the maturing bottom before any postings work. Two abort
// rules bound an anti-correlated query (its matches only in high-key leaves,
// so seed probes come back empty): a heap still underfull after
// fillAbortBudget enumerated seeds abandons the schedule, and so does a
// consecutive run of fillAbortBudget matchless enumerated seeds - the
// latter also fires when a warm heap (filled by earlier segments) lets
// low-bound leaves classify competitive without the underfull test ever
// engaging. Either way the sweep completes correctness, inheriting whatever
// bottom the seeds bought.
template <typename Collector>
void collectTopKMatchWindowedSeeded(
    int32_t segnum, BulkScorer* seedBulk, BulkScorer* sweepBulk,
    DocSet* filter, Collector& collector, MemPool& pool, int32_t maxDoc,
    int64_t seedCount, int64_t fillAbortBudget) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr && plan.leafSize != 0);
  assert(seedBulk != nullptr && sweepBulk != nullptr);
  using BC = typename Collector::BlockClass;
  skipCount(SkipStats::fieldSortSeededActivations);

  // Select the seedCount globally best-bounded leaves - the head of the
  // column's persisted bound order, the same order the best-first driver
  // walks - then visit the selection in ascending doc order so the seed
  // scorer only moves forward. Selection is metadata-only: no
  // classification, no postings work.
  seedCount = std::min<int64_t>(seedCount, plan.leafCount);
  std::span<int64_t> seeds = pool.make_span<int64_t>((size_t)seedCount);
  for (int64_t rank = 0; rank < seedCount; rank++) {
    seeds[(size_t)rank] = plan.batch->boundOrderLeaf(rank).leaf;
  }
  std::sort(seeds.begin(), seeds.end());

  std::span<uint64_t> visited =
      pool.make_span<uint64_t>((size_t)((plan.leafCount + 63) >> 6));
  std::fill(visited.begin(), visited.end(), 0);

  int64_t enumerated = 0;
  int64_t consecutiveEmpty = 0;
  for (int64_t leaf : seeds) {
    if (collector.heapFull()) {
      if (collector.classifyBlock(
              segnum, (int32_t)(leaf * (int64_t)plan.leafSize),
              plan.batch->leafBestKey(leaf), plan.batch) != BC::COLLECT) {
        skipCount(SkipStats::fieldSortSeedClassifiedOut);
        continue;
      }
    } else if (enumerated >= fillAbortBudget) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
    int32_t begin = (int32_t)(leaf * (int64_t)plan.leafSize);
    int32_t end = (int32_t)std::min<int64_t>(
        (leaf + 1) * (int64_t)plan.leafSize, (int64_t)maxDoc);
    int64_t emitted = feedMatchWindows(segnum, seedBulk, filter, nullptr,
                                       collector, begin, end);
    visited[(size_t)(leaf >> 6)] |= 1ULL << (leaf & 63);
    enumerated++;
    skipCount(SkipStats::fieldSortSeedLeaves);
    if (emitted != 0) {
      consecutiveEmpty = 0;
    } else if (++consecutiveEmpty >= fillAbortBudget) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
  }

  collectTopKMatchWindowed(segnum, sweepBulk, filter, nullptr, collector,
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
