#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "SearchOp.h"  // build:: alias + luxir::api types (via SearchRequest.h)

namespace luxir {

template<typename K>
void sortByCountDescAndLimit(std::vector<std::pair<K, int64_t>>& buckets, int64_t limit) {
  auto cmp = [](const auto& a, const auto& b) {
    if (a.second != b.second) {
      return a.second > b.second;
    }
    return a.first < b.first;
  };

  if (limit >= 0 && limit < (int64_t)buckets.size()) {
    std::partial_sort(buckets.begin(), buckets.begin() + limit, buckets.end(), cmp);
    buckets.resize(limit);
  } else {
    std::sort(buckets.begin(), buckets.end(), cmp);
  }
}

// Build bucket_ids (a Column) + counts (an int64 span) into the NON-OWNING
// FacetResult.  The bucket count is known up front (buckets.size()), so each
// span is allocArray'd once into the response arena `mr` and index-filled.
inline void emitBuckets(luxir::api::FacetResult& facetResultProto,
                        const std::vector<std::pair<int64_t, int64_t>>& buckets,
                        std::pmr::memory_resource& mr) {
  auto& bucketIds = facetResultProto.bucket_ids.emplace().kind.emplace<luxir::api::ColInt>();
  size_t n = buckets.size();
  int64_t* ids = build::allocArray(bucketIds.v, n, mr);
  int64_t* counts = build::allocArray(facetResultProto.counts, n, mr);
  for (size_t i = 0; i < n; i++) {
    ids[i] = buckets[i].first;
    counts[i] = buckets[i].second;
  }
}

inline void emitBuckets(luxir::api::FacetResult& facetResultProto,
                        const std::vector<std::pair<std::string, int64_t>>& buckets,
                        std::pmr::memory_resource& mr) {
  auto& bucketIds = facetResultProto.bucket_ids.emplace().kind.emplace<luxir::api::ColStr>();
  size_t n = buckets.size();
  std::string_view* ids = build::allocArray(bucketIds.v, n, mr);
  int64_t* counts = build::allocArray(facetResultProto.counts, n, mr);
  for (size_t i = 0; i < n; i++) {
    // bucket strings live in the transient countVec; copy into the arena.
    ids[i] = build::arenaStr(mr, buckets[i].first);
    counts[i] = buckets[i].second;
  }
}

}
