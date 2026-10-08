// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "SearchOp.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/OrdMap.h"

namespace luxir {

// These identities coincide for an ordinary top-N result, but are distinct in
// the execution model. Owner slots are reusable counter routes; output slots
// are positions in a materialized response; bucket ids are local to the
// reader/ordinal epoch.
struct FacetBucketId {
  int64_t value;
};

struct FacetOwnerSlot {
  int32_t value;
};

struct FacetOutputSlot {
  int32_t value;
};

enum class FacetBucketFlags : uint8_t {
  NONE = 0,
  PINNED = 1 << 0
};

enum class FacetFeedKind : uint8_t {
  BUCKET_DOMAINS,
  STRING_COLUMN_REPLAY
};

template <typename Key>
struct SelectedFacetBucket {
  // Borrowed keys are valid because post-selection execution is synchronous.
  // An asynchronous executor must own or otherwise pin them.
  Key key;
  std::optional<FacetBucketId> id;
  int64_t count;
  FacetOwnerSlot owner;
  FacetOutputSlot output;
  FacetBucketFlags flags = FacetBucketFlags::NONE;
};

// One concrete parent source offered to result children. The selected bucket
// ids are global ordinals for this OrdMap epoch; domains are the parent's
// incoming domains and remain pinned for synchronous binding execution.
struct StringFacetColumnSource {
  std::string_view field;
  const OrdMap& ordMap;
  std::span<const DomainHandle> domains;
  std::span<const SelectedFacetBucket<std::string_view>> buckets;
};

struct FacetChildContext {
  SearchOp::Calculator& parent;
  const StringFacetColumnSource* stringColumn = nullptr;
};

// Child-owned executable binding. The virtual call is at the whole binding
// boundary; concrete implementations own every per-document loop.
class FacetChildExecutor {
public:
  virtual FacetFeedKind feedKind() const = 0;
  // A true result is a measured dominance rule, not merely capability. AUTO
  // may select the binding without a cost race; false keeps the baseline.
  virtual bool dominatesBucketDomains() const { return false; }
  virtual void execute() = 0;
  virtual ~FacetChildExecutor() = default;
};

// A producer delivers one segment's domain for every bucket of a block, each
// exactly once, as feed(index in block, domain). Granularity is the
// producer's: an independent producer materializes, feeds and drops one domain
// at a time; a joint column producer builds byte-bounded chunks
// (feedFacetBucketDomainChunks) and hands each domain over as it feeds it.
using FacetBucketFeed = std::function_ref<void(size_t, DomainHandle)>;

// Block bindings by retained child state, then feed each segment's bucket
// domains to that bucket's children. A bucket's bindings open at its first
// feed and close after its last, so whatever a completed child holds does not
// accumulate across the block.
class FacetBucketBlockExecutor {
public:
  static constexpr size_t BINDING_BYTES = 64 * 1024 * 1024;

  template <typename Key, typename DomainSource, typename BlockStarted>
  static void execute(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      IndexReader& reader, DomainSource&& bucketDomains,
      size_t bindingStateBytes, BlockStarted&& blockStarted) {
    if (children.empty() || buckets.empty()) return;
    size_t residentBytesPerBucket = 0;
    for (SearchOp* child : children) {
      residentBytesPerBucket = saturatingAdd(
          residentBytesPerBucket, child->facetBucketResidentBytes());
    }
    size_t bindingBlockSize = std::max<size_t>(
        1, bindingStateBytes
               / std::max<size_t>(1, residentBytesPerBucket));
    auto segments = reader.segments();

    for (size_t blockBegin = 0; blockBegin < buckets.size();
         blockBegin += bindingBlockSize) {
      blockStarted();
      size_t blockSize = std::min(
          bindingBlockSize, buckets.size() - blockBegin);
      auto block = std::span<const SelectedFacetBucket<Key>>(buckets)
                       .subspan(blockBegin, blockSize);

      std::vector<std::unique_ptr<SearchOp::Calculator>> bindings(
          blockSize * children.size());
      auto open = [&](size_t bucket) {
        assert(block[bucket].output.value >= 0);
        assert(block[bucket].output.value < (int32_t)buckets.size());
        for (size_t child = 0; child < children.size(); child++) {
          auto& binding = bindings[bucket * children.size() + child];
          assert(binding == nullptr);
          binding.reset(children[child]->createCalculator(
              &parent, block[bucket].output.value, (int64_t)buckets.size()));
        }
      };
      auto close = [&](size_t bucket) {
        for (size_t child = 0; child < children.size(); child++) {
          bindings[bucket * children.size() + child].reset();
        }
      };

      if (segments.empty()) {
        std::span<const DomainHandle> noDomains;
        for (size_t bucket = 0; bucket < blockSize; bucket++) {
          open(bucket);
          for (size_t child = 0; child < children.size(); child++) {
            bindings[bucket * children.size() + child]
                ->calcAll(nullptr, noDomains);
          }
          close(bucket);
        }
        continue;
      }

      for (size_t segnum = 0; segnum < segments.size(); segnum++) {
        bool first = segnum == 0;
        bool last = segnum + 1 == segments.size();
        // Final delivery can synchronously execute all segments of a prepared
        // binding or its result children. Retain those refills across bindings.
        auto retained = last ? segments : segments.subspan(segnum, 1);
        auto release = scope_guard([&]() noexcept {
          for (auto& segment : retained) {
            for (auto& binding : bindings) {
              if (binding != nullptr) binding->releaseSegmentState(segment);
            }
          }
        });
        SearchOp::SegmentStateRetention retain(children, retained);
        [[maybe_unused]] size_t fed = 0;
        auto deliver = [&](size_t bucket, DomainHandle domain) {
          assert(bucket < blockSize);
          assert(domain.isDeliverable());
          if (first) open(bucket);
          for (size_t child = 0; child < children.size(); child++) {
            bindings[bucket * children.size() + child]
                ->calc(nullptr, (int32_t)segnum, domain);
          }
          if (last) close(bucket);
          fed++;
        };
        bucketDomains(segnum, block, FacetBucketFeed(deliver));
        assert(fed == blockSize);
      }
    }
  }
};

// Joint producers build every domain of a chunk in one column pass, so a
// chunk's builders are alive together. Chunks are bounded by worst-case
// builder bytes; a block wider than the budget rescans the column per chunk.
inline constexpr size_t FACET_BUCKET_DOMAIN_BYTES = 64 * 1024 * 1024;

template <typename Key, typename BuildChunk>
void feedFacetBucketDomainChunks(
    int32_t maxDoc, std::span<const SelectedFacetBucket<Key>> block,
    size_t domainBytes, FacetBucketFeed feed, BuildChunk&& buildChunk) {
  constexpr size_t BUILDER_FIXED_BYTES = 128;
  size_t builderBytes = (size_t)(((uint64_t)maxDoc + 63) / 64) * 8
      + BUILDER_FIXED_BYTES;
  size_t chunkBuckets = std::max<size_t>(1, domainBytes / builderBytes);
  for (size_t chunkBegin = 0; chunkBegin < block.size();
       chunkBegin += chunkBuckets) {
    auto chunk = block.subspan(
        chunkBegin, std::min(chunkBuckets, block.size() - chunkBegin));
    std::vector<DomainHandle> domains = buildChunk(chunk);
    assert(domains.size() == chunk.size());
    for (size_t i = 0; i < chunk.size(); i++) {
      feed(chunkBegin + i, std::move(domains[i]));
    }
  }
}

// Values arrive in document order, but a multi-valued column may route the
// same document to a bucket more than once. Domains contain each document once.
template <typename VisitValues, typename BucketOf>
std::vector<DomainHandle> buildFacetBucketDomains(
    int32_t maxDoc, size_t numBuckets, VisitValues&& visitValues,
    BucketOf&& bucketOf) {
  std::vector<DocSetBuilder> builders;
  builders.reserve(numBuckets);
  for (size_t i = 0; i < numBuckets; i++) builders.emplace_back(maxDoc);
  std::vector<int32_t> lastAdded(numBuckets, -1);
  visitValues([&](int32_t docid, int64_t value) LUXIR_INLINE {
    int32_t bucket = bucketOf(value);
    if (bucket < 0 || lastAdded[(size_t)bucket] == docid) return;
    lastAdded[(size_t)bucket] = docid;
    builders[(size_t)bucket].add(docid);
  });
  std::vector<DomainHandle> domains;
  domains.reserve(numBuckets);
  for (auto& builder : builders) domains.emplace_back(builder.build());
  return domains;
}

} // namespace luxir
