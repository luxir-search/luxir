#pragma once

#include <algorithm>
#include <boost/unordered/unordered_flat_map.hpp>
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
  // A count-only facet carries no payload, and the empty member must not cost
  // it anything: with monostate padded to a member the candidate is 24 bytes,
  // which makes every `regular.size()` in the selection loop a division.
  [[no_unique_address]] Payload payload;
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

// Bounded selection of a facet's ordinary page. The finalizer knows nothing
// about selected values: it sees every counted key exactly once and spends one
// comparison on it. Pins are reconciled against the finished page by
// mergePinnedBuckets below, which costs the caller nothing when none are set.
template<typename Key, typename Payload, typename Better>
class FieldBucketFinalizer {
  using Candidate = FacetCandidate<Key, Payload>;
  using Final = FinalizedFacetBucket<Key, Payload>;

  std::vector<Candidate> regular;
  [[no_unique_address]] Better better;
  int64_t minCount;
  size_t offset;
  size_t retain;
  bool bounded;

public:
  FieldBucketFinalizer(int64_t minCount, int64_t offset, int64_t limit,
                       Better better) :
      better(std::move(better)), minCount(minCount),
      offset(offset < 0 ? 0 : (size_t)offset), retain(0),
      bounded(limit >= 0) {
    if (bounded) {
      uint64_t requested = (uint64_t)this->offset + (uint64_t)limit;
      retain = requested > std::numeric_limits<size_t>::max()
          ? std::numeric_limits<size_t>::max() : (size_t)requested;
      regular.reserve(std::min<size_t>(retain, 64));
    }
  }

  void add(Candidate candidate) {
    if (minCount != -1 && candidate.count < minCount) return;
    if (!bounded) {
      regular.push_back(std::move(candidate));
    } else if (regular.size() < retain) {
      regular.push_back(std::move(candidate));
      std::push_heap(regular.begin(), regular.end(), better);
    } else if (retain != 0 && better(candidate, regular.front())) {
      std::pop_heap(regular.begin(), regular.end(), better);
      regular.back() = std::move(candidate);
      std::push_heap(regular.begin(), regular.end(), better);
    }
  }

  // Selection over a whole range, for a caller that produces its candidates one
  // at a time. mincount and the retained page size are fixed for the request,
  // but a per-candidate add() cannot hold them in registers - push_heap writes
  // through the same vector those members sit beside, so each key reloads them.
  template<typename It, typename ToCandidate>
  void addRange(It first, It last, ToCandidate toCandidate) {
    const int64_t min = minCount;
    const size_t keep = retain;
    if (!bounded || keep == 0) {
      for (It it = first; it != last; ++it) add(toCandidate(*it));
      return;
    }
    for (It it = first; it != last; ++it) {
      Candidate candidate = toCandidate(*it);
      if (min != -1 && candidate.count < min) continue;
      if (regular.size() < keep) {
        regular.push_back(candidate);
        std::push_heap(regular.begin(), regular.end(), better);
      } else if (better(candidate, regular.front())) {
        std::pop_heap(regular.begin(), regular.end(), better);
        regular.back() = candidate;
        std::push_heap(regular.begin(), regular.end(), better);
      }
    }
  }

  bool needsCandidates() const {
    return !bounded || retain != 0;
  }

  std::vector<Final> finish() {
    std::sort(regular.begin(), regular.end(), better);
    size_t begin = std::min(offset, regular.size());
    std::vector<Final> out;
    out.reserve(regular.size() - begin);
    for (size_t i = begin; i < regular.size(); i++) {
      auto& bucket = regular[i];
      out.push_back({
          .key = std::move(bucket.key),
          .count = bucket.count,
          .payload = std::move(bucket.payload),
      });
    }
    return out;
  }
};

// What a selected value is worth once the page is known: the count (and
// payload) the caller recovered for it by point lookup, for the pins the page
// does not already hold.
template<typename Payload = std::monostate>
struct PinnedBucketValue {
  int64_t count = 0;
  std::optional<Payload> payload;
};

// Selected values merge into the natural page: one the page already holds is
// marked in place, one it does not hold is appended with the value looked up
// for it. The page is at most `limit` buckets and pins are few, so this is a
// scan over the result rather than anything the selection loop has to carry.
template<typename Key, typename Payload>
void mergePinnedBuckets(
    std::vector<FinalizedFacetBucket<Key, Payload>>& page,
    std::span<const std::optional<Key>> pinKeys,
    std::span<const PinnedBucketValue<Payload>> pinValues) {
  if (pinKeys.empty()) return;
  assert(pinKeys.size() == pinValues.size());
  for (size_t i = 0; i < pinKeys.size(); i++) {
    bool covered = false;
    if (pinKeys[i].has_value()) {
      for (auto& bucket : page) {
        if (bucket.key.has_value() && *bucket.key == *pinKeys[i]) {
          bucket.pinIndex = i;
          covered = true;
          break;
        }
      }
    }
    if (covered) continue;
    page.push_back({
        .key = pinKeys[i],
        .count = pinValues[i].count,
        .payload = pinValues[i].payload,
        .pinIndex = i,
    });
  }
}

template<typename Key, typename Payload, typename Better>
std::vector<FinalizedFacetBucket<Key, Payload>> finalizeFieldBuckets(
    std::vector<FacetCandidate<Key, Payload>> candidates,
    int64_t minCount, int64_t offset, int64_t limit, Better better) {
  FieldBucketFinalizer<Key, Payload, Better> finalizer(
      minCount, offset, limit, std::move(better));
  for (auto& candidate : candidates) finalizer.add(std::move(candidate));
  return finalizer.finish();
}

// Default field-facet order: count desc, key asc. Named so a caller that
// streams into FieldBucketFinalizer orders its page the same way the
// vector-taking wrapper below does.
inline constexpr auto countFieldBucketOrder =
    [](const auto& a, const auto& b) {
      if (a.count != b.count) return a.count > b.count;
      return a.key < b.key;
    };

template<typename Key, typename Payload = std::monostate>
std::vector<FinalizedFacetBucket<Key, Payload>> finalizeCountFieldBuckets(
    std::vector<FacetCandidate<Key, Payload>> candidates,
    int64_t minCount, int64_t offset, int64_t limit) {
  return finalizeFieldBuckets(
      std::move(candidates), minCount, offset, limit, countFieldBucketOrder);
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
