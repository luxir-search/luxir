#pragma once

#include "solux/query/Query.h"

namespace solux {


class segdoc {
  int32_t docid;  // docid must be first. It forms the lowest bits of a little-endian int64_t.
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
    // compare both segment and docid with a single comparison.
    return reinterpret_cast<const int64_t &>(*this) <=> reinterpret_cast<const int64_t &>(other);
  }
};




class TopDocsCollector {
  public:

  struct ScoreDoc {
   float score;
   segdoc doc;
  };

  int64_t hitCount = 0;

  float minCompetitiveVal = std::numeric_limits<float>::lowest();

  constexpr static auto scoreComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score > b.score;
  };

  constexpr static auto scoreAndDocComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score == b.score ? (a.doc < b.doc) : (a.score > b.score);
  };

  int64_t topCount;
  std::vector<ScoreDoc> topDocs;  // TODO: create an expanding PQ backed by a vector so we don't have to allocate a vector of size topCount
  DirectPQ<ScoreDoc, decltype(scoreComp)> pq;

  TopDocsCollector(int64_t topCount) : topCount(topCount), topDocs(topCount), pq(topDocs) {
    assert(topCount > 0);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;

    if (score > minCompetitiveVal) {
      bool overflow = pq.insertWithOverflow({score, segdoc(segment, docid)});
      if (overflow) {
        minCompetitiveVal = pq.top().score;
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return pq.size();
  }


  // Since we use min-heap comparators in our priority queues, the list will be reverse-sorted (smallest last)
  void sort() {
    std::sort_heap(topDocs.begin(), topDocs.begin() + pq.size(), scoreComp);
  }

};


} // namespace solux