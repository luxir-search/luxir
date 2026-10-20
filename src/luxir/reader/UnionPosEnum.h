// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>

#include "PosEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Docs side of a multi-term phrase slot (Lucene's MultiPhraseQuery union):
// one cursor over the union of several terms' postings. It offers the subset
// of the DocsPosEnum interface that PhraseQuery's scorer and matchers use, so
// they run over a slot holding a term set unchanged. A min-heap on docId
// merges the term cursors; termFreq() is the sum of the freqs of the terms on
// the current doc, an upper bound on the slot's distinct positions there.
class UnionDocsPosEnum {
  std::span<DocsPosEnum*> subs;
  std::span<int32_t> heap;     // sub indexes; heap[0] has the smallest docId
  std::span<int32_t> current;  // subs positioned on docid, after collectCurrent()
  int32_t currentCount = 0;
  bool currentValid = false;
  int32_t docid = -1;
  int32_t freq = 0;
  int32_t cost;
  int32_t ttf;

  bool later(int32_t a, int32_t b) const {
    return subs[(size_t) a]->docId() > subs[(size_t) b]->docId();
  }

  // The subs on docid form a subtree rooted at heap[0]: every child's docId
  // is >= its parent's, so the walk stops at the first later doc.
  void collect(size_t node) {
    if (node >= heap.size()) return;
    int32_t sub = heap[node];
    if (subs[(size_t) sub]->docId() != docid) return;
    current[(size_t) currentCount++] = sub;
    freq += subs[(size_t) sub]->termFreq();
    collect(2 * node + 1);
    collect(2 * node + 2);
  }

  void collectCurrent() {
    if (currentValid) return;
    currentCount = 0;
    freq = 0;
    collect(0);
    currentValid = true;
  }

public:
  // `cost` and `totalTermFreq` are the slot's estimator stats for this
  // segment (summed over the terms), reported back by numDocs() and
  // totalTermFreq() like a single term's enum would.
  UnionDocsPosEnum(MemPool& pool, std::span<DocsPosEnum*> subs, int32_t cost,
                   int32_t totalTermFreq)
      : subs(subs), cost(cost), ttf(totalTermFreq) {
    assert(!subs.empty());
    heap = pool.make_span<int32_t>(subs.size());
    current = pool.make_span<int32_t>(subs.size());
    for (size_t i = 0; i < subs.size(); i++) heap[i] = (int32_t) i;
  }

  int32_t docId() const { return docid; }
  int32_t numDocs() const { return cost; }
  int32_t totalTermFreq() const { return ttf; }

  int32_t advance(int32_t target) {
    auto cmp = [this](int32_t a, int32_t b) { return later(a, b); };
    while (subs[(size_t) heap[0]]->docId() < target) {
      std::pop_heap(heap.begin(), heap.end(), cmp);
      DocsPosEnum* sub = subs[(size_t) heap.back()];
      if (sub->docId() + 1 == target) {
        sub->next();
      } else {
        sub->advance(target);
      }
      std::push_heap(heap.begin(), heap.end(), cmp);
    }
    docid = subs[(size_t) heap[0]]->docId();
    currentValid = false;
    return docid;
  }

  int32_t next() {
    if (docid == PostingsReader::END) return docid;
    return advance(docid + 1);
  }

  int32_t termFreq() {
    collectCurrent();
    return freq;
  }

  // Indexes (into the constructor's subs) of the terms on the current doc.
  std::span<const int32_t> currentSubs() {
    collectCurrent();
    return current.first((size_t) currentCount);
  }
};

// Positions side of a multi-term phrase slot: merges the position streams of
// the terms on the current doc in increasing order, the PosEnum interface the
// phrase matchers use. Two terms at one position (synonym-style input) yield
// that position once.
class UnionPosEnum {
  UnionDocsPosEnum& docsEnum;
  std::span<PosEnum*> subs;    // parallel to the docs enum's subs
  std::span<int32_t> heads;    // each sub's next unconsumed position
  std::span<int32_t> heap;     // sub indexes; heap[0] has the smallest head
  int32_t heapSize = 0;
  int32_t pos = -1;

public:
  UnionPosEnum(MemPool& pool, UnionDocsPosEnum& docsEnum, std::span<PosEnum*> subs)
      : docsEnum(docsEnum), subs(subs) {
    heads = pool.make_span<int32_t>(subs.size());
    heap = pool.make_span<int32_t>(subs.size());
  }

  UnionPosEnum(const UnionPosEnum&) = delete;

  void startPositions() {
    auto cmp = [this](int32_t a, int32_t b) {
      return heads[(size_t) a] > heads[(size_t) b];
    };
    heapSize = 0;
    pos = -1;
    for (int32_t sub : docsEnum.currentSubs()) {
      subs[(size_t) sub]->startPositions();
      heads[(size_t) sub] = subs[(size_t) sub]->nextPosition();
      if (heads[(size_t) sub] == PostingsReader::END) continue;
      heap[(size_t) heapSize++] = sub;
      std::push_heap(heap.begin(), heap.begin() + heapSize, cmp);
    }
  }

  int32_t nextPosition() {
    auto cmp = [this](int32_t a, int32_t b) {
      return heads[(size_t) a] > heads[(size_t) b];
    };
    for (;;) {
      if (heapSize == 0) {
        pos = PostingsReader::END;
        return pos;
      }
      std::pop_heap(heap.begin(), heap.begin() + heapSize, cmp);
      int32_t sub = heap[(size_t) heapSize - 1];
      int32_t position = heads[(size_t) sub];
      heads[(size_t) sub] = subs[(size_t) sub]->nextPosition();
      if (heads[(size_t) sub] == PostingsReader::END) {
        heapSize--;
      } else {
        std::push_heap(heap.begin(), heap.begin() + heapSize, cmp);
      }
      if (position != pos) {
        pos = position;
        return pos;
      }
    }
  }

  int32_t advancePosition(int32_t target) {
    while (pos < target) nextPosition();
    return pos;
  }
};

} // namespace luxir
