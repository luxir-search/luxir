// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

namespace luxir {

// Recursive graph bisection (Dhulipala et al., "Compressing graphs and
// indexes with recursive graph bisection", KDD 2016), as Lucene's
// BPIndexReorderer implements it. Documents and their terms form a bipartite
// graph; the order is built by splitting the documents in two halves,
// moving documents between the halves so that each term's documents
// concentrate on one side, and recursing into each half. Documents that
// share terms end up close together, so postings get small doc id gaps.
//
// Each round of a split follows Lucene: every doc gets a bias
// sum_t log2(dfRight) - log2(dfLeft) (log2(0) taken as 0; negative when the
// doc is drawn to the left), and when the largest bias on the left exceeds
// the smallest on the right by more than the round number (annealing), the
// partition is re-split around its median bias. Deviations from Lucene: a
// 4096-entry log2 table instead of 256 entries, and an exact selection of
// the median with ties broken by doc id.
//
// Cost per level: one pass over the forward index to count terms per half,
// then per round one pass to compute biases plus a linear-time selection.
// Levels: log2(docs / MIN_PARTITION).
//
// The result depends only on the forward index: term counts are integers,
// each bias is summed in the document's term order, sort keys are unique,
// selections run serially, and parallel tasks work on disjoint ranges.
// Thread count does not change it.
class DocBisection {
public:
  // A partition smaller than twice this is not split further.
  static constexpr int32_t MIN_PARTITION = 32;
  // Rounds per split. Round i re-splits only while the bias gap between the
  // halves exceeds i, so later rounds settle (annealing).
  static constexpr int32_t MAX_ITERS = 20;
  // Partitions with more documents count and score in parallel and recurse
  // into their halves concurrently.
  static constexpr int32_t PARALLEL_DOCS = 8192;

  // Document d's term ids are terms[offsets[d], offsets[d + 1]), each below
  // numTerms.
  struct ForwardIndex {
    std::vector<uint32_t> offsets;  // numDocs() + 1 entries
    std::vector<uint32_t> terms;
    uint32_t numTerms = 0;

    uint32_t numDocs() const { return offsets.empty() ? 0 : (uint32_t)(offsets.size() - 1); }
  };

  // RAM order() allocates beyond the forward index: the returned order, a
  // sort key and a side flag per document, and two term counts per
  // concurrently split partition (at most one per thread).
  static int64_t scratchBytes(int64_t numDocs, int64_t numTerms, int64_t concurrency) {
    return numDocs * (int64_t)(sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint8_t))
        + numTerms * 2 * (int64_t)sizeof(int32_t) * concurrency;
  }

  // The bisection order: position -> document, a permutation of
  // [0, numDocs) starting from the identity.
  static std::vector<uint32_t> order(const ForwardIndex& index) {
    Run run(index);
    // Isolated: while this thread waits on its partitions it must not pick
    // up unrelated tasks (a merge runs this inside the writer's graph).
    oneapi::tbb::this_task_arena::isolate([&] { run.bisect(0, index.numDocs()); });
    return std::move(run.docs);
  }

  // log2(x) for x >= 1, from a table of LOG_TABLE entries: exact below it,
  // within the table's resolution above.
  static float fastLog2(uint32_t x) {
    static const std::array<float, LOG_TABLE> table = [] {
      std::array<float, LOG_TABLE> out{};
      for (uint32_t i = 1; i < LOG_TABLE; i++) out[i] = (float)std::log2((double)i);
      return out;
    }();
    assert(x >= 1);
    if (x < LOG_TABLE) return table[x];
    int shift = std::bit_width(x) - LOG_TABLE_BITS;
    return (float)shift + table[x >> shift];
  }

private:
  static constexpr int LOG_TABLE_BITS = 12;
  static constexpr uint32_t LOG_TABLE = 1u << LOG_TABLE_BITS;

  // Per-term document counts of the two halves of the partition being split.
  struct Counts {
    std::vector<int32_t> left;
    std::vector<int32_t> right;
  };

  // A position's sort key: ascending bias, then ascending doc id.
  static uint64_t sortKey(float bias, uint32_t doc) {
    uint32_t bits = std::bit_cast<uint32_t>(bias);
    uint32_t ascending = (bits & 0x80000000u) != 0 ? ~bits : bits | 0x80000000u;
    return (uint64_t)ascending << 32 | doc;
  }
  static float biasOf(uint64_t key) {
    uint32_t ascending = (uint32_t)(key >> 32);
    uint32_t bits = (ascending & 0x80000000u) != 0 ? ascending & 0x7fffffffu : ~ascending;
    return std::bit_cast<float>(bits);
  }
  static float log2OrZero(int32_t df) { return df == 0 ? 0.0f : fastLog2((uint32_t)df); }
  static uint32_t docOf(uint64_t key) { return (uint32_t)key; }

  struct Run {
    const ForwardIndex& index;
    std::vector<uint32_t> docs;  // position -> doc
    std::vector<uint64_t> keys;  // position -> sort key, while its partition is split
    std::vector<uint8_t> onRight;  // doc -> on the right half of its partition
    std::mutex poolMutex;
    std::vector<std::unique_ptr<Counts>> pool;

    explicit Run(const ForwardIndex& index) : index(index), docs(index.numDocs()), keys(index.numDocs()),
        onRight(index.numDocs()) {
      for (uint32_t i = 0; i < (uint32_t)docs.size(); i++) docs[i] = i;
    }

    std::unique_ptr<Counts> acquire() {
      {
        std::lock_guard<std::mutex> lock(poolMutex);
        if (!pool.empty()) {
          auto counts = std::move(pool.back());
          pool.pop_back();
          return counts;
        }
      }
      auto counts = std::make_unique<Counts>();
      counts->left.assign(index.numTerms, 0);
      counts->right.assign(index.numTerms, 0);
      return counts;
    }

    // Counts come back zeroed.
    void release(std::unique_ptr<Counts>&& counts) {
      std::lock_guard<std::mutex> lock(poolMutex);
      pool.push_back(std::move(counts));
    }

    std::span<const uint32_t> termsOf(uint32_t doc) const {
      return std::span<const uint32_t>(index.terms).subspan(index.offsets[doc], index.offsets[doc + 1] - index.offsets[doc]);
    }

    // fn(position) for every position in [begin, end); in parallel for a
    // large range, isolated so a waiting thread does not start another
    // partition (which would take another Counts).
    template<typename Fn>
    void forEach(uint32_t begin, uint32_t end, Fn&& fn) {
      if (end - begin <= (uint32_t)PARALLEL_DOCS) {
        for (uint32_t pos = begin; pos < end; pos++) fn(pos);
        return;
      }
      oneapi::tbb::this_task_arena::isolate([&] {
        oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<uint32_t>(begin, end, 1024),
                                  [&](const oneapi::tbb::blocked_range<uint32_t>& range) {
          for (uint32_t pos = range.begin(); pos < range.end(); pos++) fn(pos);
        });
      });
    }

    void count(uint32_t begin, uint32_t end, std::vector<int32_t>& counts) {
      bool parallel = end - begin > (uint32_t)PARALLEL_DOCS;
      forEach(begin, end, [&](uint32_t pos) {
        for (uint32_t term : termsOf(docs[pos])) {
          if (parallel) std::atomic_ref<int32_t>(counts[term]).fetch_add(1, std::memory_order_relaxed);
          else counts[term]++;
        }
      });
    }

    // Each doc's bias: negative when its terms are more common on the left.
    void score(uint32_t begin, uint32_t end, const std::vector<int32_t>& left, const std::vector<int32_t>& rightCounts) {
      forEach(begin, end, [&](uint32_t pos) {
        uint32_t doc = docs[pos];
        float bias = 0;
        for (uint32_t term : termsOf(doc)) bias += log2OrZero(rightCounts[term]) - log2OrZero(left[term]);
        keys[pos] = sortKey(bias, doc);
      });
    }

    void move(uint32_t doc, std::vector<int32_t>& from, std::vector<int32_t>& to) {
      for (uint32_t term : termsOf(doc)) {
        from[term]--;
        to[term]++;
      }
    }

    void bisect(uint32_t begin, uint32_t end) {
      if (end - begin < 2 * (uint32_t)MIN_PARTITION) return;
      uint32_t mid = begin + (end - begin) / 2;
      auto counts = acquire();
      auto& left = counts->left;
      auto& rightCounts = counts->right;
      count(begin, mid, left);
      count(mid, end, rightCounts);
      for (uint32_t pos = begin; pos < end; pos++) onRight[docs[pos]] = pos >= mid;
      for (int32_t iter = 0; iter < MAX_ITERS; iter++) {
        score(begin, end, left, rightCounts);
        float maxLeft = -INFINITY, minRight = INFINITY;
        for (uint32_t pos = begin; pos < mid; pos++) maxLeft = std::max(maxLeft, biasOf(keys[pos]));
        for (uint32_t pos = mid; pos < end; pos++) minRight = std::min(minRight, biasOf(keys[pos]));
        if (maxLeft - minRight <= (float)iter) break;
        // Re-split around the median bias; docs that changed sides move
        // their term counts.
        std::nth_element(keys.begin() + begin, keys.begin() + mid, keys.begin() + end);
        for (uint32_t pos = begin; pos < end; pos++) {
          uint32_t doc = docOf(keys[pos]);
          docs[pos] = doc;
          bool toRight = pos >= mid;
          if ((bool)onRight[doc] == toRight) continue;
          if (toRight) move(doc, left, rightCounts);
          else move(doc, rightCounts, left);
          onRight[doc] = toRight;
        }
      }
      // Zero only the terms this partition touched.
      forEach(begin, end, [&](uint32_t pos) {
        for (uint32_t term : termsOf(docs[pos])) {
          std::atomic_ref<int32_t>(left[term]).store(0, std::memory_order_relaxed);
          std::atomic_ref<int32_t>(rightCounts[term]).store(0, std::memory_order_relaxed);
        }
      });
      release(std::move(counts));

      if (end - begin > (uint32_t)PARALLEL_DOCS) {
        oneapi::tbb::task_group group;
        group.run([&] { bisect(begin, mid); });
        bisect(mid, end);
        group.wait();
      } else {
        bisect(begin, mid);
        bisect(mid, end);
      }
    }
  };
};

}  // namespace luxir
