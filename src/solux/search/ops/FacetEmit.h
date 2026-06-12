#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "protos/solux_types.pb.h"

namespace solux {

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

inline void emitBuckets(proto::FacetResult& facetResultProto, const std::vector<std::pair<int64_t, int64_t>>& buckets) {
  auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_i();
  auto& bucketIdsArr = *bucketIds.mutable_v();
  auto& countsArr = *facetResultProto.mutable_counts();
  bucketIdsArr.Reserve(buckets.size());
  countsArr.Reserve(buckets.size());
  for (auto [val, count] : buckets) {
    bucketIdsArr.Add(val);
    countsArr.Add(count);
  }
}

inline void emitBuckets(proto::FacetResult& facetResultProto, const std::vector<std::pair<std::string, int64_t>>& buckets) {
  auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_s();
  auto& bucketIdsArr = *bucketIds.mutable_v();
  auto& countsArr = *facetResultProto.mutable_counts();
  bucketIdsArr.Reserve(buckets.size());
  countsArr.Reserve(buckets.size());
  for (auto [val, count] : buckets) {
    auto* strptr = bucketIdsArr.Add();
    *strptr = val;
    countsArr.Add(count);
  }
}

}
