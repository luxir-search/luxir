#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "SearchOp.h"  // build:: alias + luxir::api types (via SearchRequest.h)

namespace luxir {

template<typename Key, typename Payload = std::monostate>
struct FacetCandidate {
  Key key;
  int64_t count;
  Payload payload;
};

template<typename Key, typename Payload = std::monostate>
struct FinalizedFacetBucket {
  std::optional<Key> key;
  int64_t count = 0;
  std::optional<Payload> payload;
  size_t pinIndex = std::numeric_limits<size_t>::max();

  bool pinned() const {
    return pinIndex != std::numeric_limits<size_t>::max();
  }
};

template<typename Key, typename Payload, typename Better>
class FieldBucketFinalizer {
  using Candidate = FacetCandidate<Key, Payload>;
  using Final = FinalizedFacetBucket<Key, Payload>;

  std::vector<Final> pins;
  std::unordered_map<Key, size_t> pinByKey;
  std::vector<Candidate> regular;
  [[no_unique_address]] Better better;
  int64_t minCount;
  size_t offset;
  size_t retain;
  bool bounded;

  void preservePin(Candidate candidate) {
    auto pin = pinByKey.find(candidate.key);
    if (pin == pinByKey.end()) return;
    Final& selected = pins[pin->second];
    selected.count = candidate.count;
    selected.payload = std::move(candidate.payload);
  }

public:
  // Pins compete in the ordinary page. A pin that mincount or the bounded heap
  // rejects is preserved with its exact payload so finish() can append it.
  // Callers need not materialize every candidate.
  FieldBucketFinalizer(std::span<const std::optional<Key>> requestedPins,
      int64_t minCount, int64_t offset, int64_t limit, Better better) :
      better(std::move(better)), minCount(minCount),
      offset(offset < 0 ? 0 : (size_t)offset), retain(0),
      bounded(limit >= 0) {
    pins.reserve(requestedPins.size());
    pinByKey.reserve(requestedPins.size());
    for (size_t i = 0; i < requestedPins.size(); i++) {
      pins.push_back({.key = requestedPins[i], .pinIndex = i});
      if (requestedPins[i].has_value()) {
        pinByKey.emplace(*requestedPins[i], i);
      }
    }
    if (bounded) {
      uint64_t requested = (uint64_t)this->offset + (uint64_t)limit;
      retain = requested > std::numeric_limits<size_t>::max()
          ? std::numeric_limits<size_t>::max() : (size_t)requested;
      regular.reserve(std::min<size_t>(retain, 64));
    }
  }

  void add(Candidate candidate) {
    if (minCount != -1 && candidate.count < minCount) {
      preservePin(std::move(candidate));
      return;
    }
    if (!bounded) {
      regular.push_back(std::move(candidate));
    } else if (regular.size() < retain) {
      regular.push_back(std::move(candidate));
      std::push_heap(regular.begin(), regular.end(), better);
    } else if (retain != 0 && better(candidate, regular.front())) {
      std::pop_heap(regular.begin(), regular.end(), better);
      preservePin(std::move(regular.back()));
      regular.back() = std::move(candidate);
      std::push_heap(regular.begin(), regular.end(), better);
    } else {
      preservePin(std::move(candidate));
    }
  }

  bool needsCandidates() const {
    return !pinByKey.empty() || !bounded || retain != 0;
  }

  std::vector<Final> finish() {
    std::sort(regular.begin(), regular.end(), better);
    size_t begin = std::min(offset, regular.size());
    for (size_t i = 0; i < begin; i++) {
      preservePin(std::move(regular[i]));
    }

    std::vector<uint8_t> coveredPins(pins.size());
    std::vector<Final> out;
    out.reserve(regular.size() - begin + pins.size());
    for (size_t i = begin; i < regular.size(); i++) {
      auto& bucket = regular[i];
      size_t pinIndex = std::numeric_limits<size_t>::max();
      auto pin = pinByKey.find(bucket.key);
      if (pin != pinByKey.end()) {
        pinIndex = pin->second;
        coveredPins[pinIndex] = 1;
      }
      out.push_back({
          .key = std::move(bucket.key),
          .count = bucket.count,
          .payload = std::move(bucket.payload),
          .pinIndex = pinIndex,
      });
    }
    for (size_t i = 0; i < pins.size(); i++) {
      if (coveredPins[i] == 0) out.push_back(std::move(pins[i]));
    }
    return out;
  }
};

template<typename Key, typename Payload, typename Better>
std::vector<FinalizedFacetBucket<Key, Payload>> finalizeFieldBuckets(
    std::vector<FacetCandidate<Key, Payload>> candidates,
    std::span<const std::optional<Key>> pins,
    int64_t minCount, int64_t offset, int64_t limit, Better better) {
  FieldBucketFinalizer<Key, Payload, Better> finalizer(
      pins, minCount, offset, limit, std::move(better));
  for (auto& candidate : candidates) finalizer.add(std::move(candidate));
  return finalizer.finish();
}

template<typename Key, typename Payload = std::monostate>
std::vector<FinalizedFacetBucket<Key, Payload>> finalizeCountFieldBuckets(
    std::vector<FacetCandidate<Key, Payload>> candidates,
    std::span<const std::optional<Key>> pins,
    int64_t minCount, int64_t offset, int64_t limit) {
  return finalizeFieldBuckets(
      std::move(candidates), pins, minCount, offset, limit,
      [](const auto& a, const auto& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.key < b.key;
      });
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
