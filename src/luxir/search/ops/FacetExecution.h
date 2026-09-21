// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstdint>
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

// Baseline post-selection binding. The parent supplies the selected buckets
// and the concrete source that materializes one repeatable domain per segment.
// SearchOp::createCalculator remains the child-owned binding factory.
//
// Execution is deliberately synchronous and segment-at-a-time within an owner
// block. Each freshly materialized domain reaches all children while it is hot;
// children that need cross-segment state retain the owned handles through their
// ordinary calc contract. A null task group keeps submitted work inline so the
// calculator bindings cannot outlive the executor.
class FacetBucketDomainExecutor {
public:
  template <typename Key, typename DomainSource>
  static void execute(
      SearchOp::Calculator& parent,
      std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      int32_t numSegments,
      DomainSource&& materializeDomain) {
    if (children.empty()) return;

    for (const auto& bucket : buckets) {
      assert(bucket.owner.value >= 0);
      assert(bucket.output.value >= 0);
      assert(bucket.output.value < (int32_t)buckets.size());

      std::vector<std::unique_ptr<SearchOp::Calculator>> bindings;
      bindings.reserve(children.size());
      for (SearchOp* child : children) {
        bindings.emplace_back(child->createCalculator(
            &parent, bucket.output.value, (int64_t)buckets.size()));
      }

      if (numSegments == 0) {
        std::span<const DomainHandle> noDomains;
        for (auto& binding : bindings) {
          binding->calcAll(nullptr, noDomains);
        }
        continue;
      }

      for (int32_t segment = 0; segment < numSegments; segment++) {
        DomainHandle domain = materializeDomain(segment, bucket);
        assert(domain.isDeliverable());
        for (auto& binding : bindings) {
          binding->calc(nullptr, segment, domain);
        }
      }
    }
  }
};

// Block bindings by retained child state, then chunk each segment's domain
// builders by bytes. A column producer scans once per chunk and feeds every
// child before releasing those domains.
class FacetBucketBlockExecutor {
  static constexpr size_t BUCKET_BUILDER_FIXED_BYTES = 128;
public:
  static constexpr size_t DOMAIN_BYTES = 64 * 1024 * 1024;
  static constexpr size_t BINDING_BYTES = 64 * 1024 * 1024;

  template <typename Key, typename DomainSource, typename BlockStarted>
  static void execute(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      IndexReader& reader, DomainSource&& bucketDomains,
      size_t bindingStateBytes, size_t domainBytes,
      BlockStarted&& blockStarted) {
    if (children.empty() || buckets.empty()) return;
    size_t residentBytesPerBucket = 0;
    for (SearchOp* child : children) {
      residentBytesPerBucket = saturatingAdd(
          residentBytesPerBucket, child->facetBucketResidentBytes());
    }
    size_t bindingBlockSize = std::max<size_t>(
        1, bindingStateBytes
               / std::max<size_t>(1, residentBytesPerBucket));

    for (size_t blockBegin = 0; blockBegin < buckets.size();
         blockBegin += bindingBlockSize) {
      blockStarted();
      size_t blockSize = std::min(
          bindingBlockSize, buckets.size() - blockBegin);
      auto block = std::span<const SelectedFacetBucket<Key>>(buckets)
                       .subspan(blockBegin, blockSize);

      std::vector<std::unique_ptr<SearchOp::Calculator>> bindings;
      bindings.reserve(blockSize * children.size());
      for (const auto& bucket : block) {
        assert(bucket.output.value >= 0);
        assert(bucket.output.value < (int32_t)buckets.size());
        for (SearchOp* child : children) {
          bindings.emplace_back(child->createCalculator(
              &parent, bucket.output.value, (int64_t)buckets.size()));
        }
      }

      if (reader.segments().empty()) {
        std::span<const DomainHandle> noDomains;
        for (auto& binding : bindings) {
          binding->calcAll(nullptr, noDomains);
        }
        continue;
      }

      for (size_t segnum = 0; segnum < reader.segments().size(); segnum++) {
        int32_t maxDoc = reader.segments()[segnum].maxDoc();
        size_t builderBytes =
            (size_t)(((uint64_t)maxDoc + 63) / 64) * 8
            + BUCKET_BUILDER_FIXED_BYTES;
        size_t bucketsPerChunk = std::max<size_t>(
            1, domainBytes / builderBytes);
        for (size_t chunkBegin = 0; chunkBegin < block.size();
             chunkBegin += bucketsPerChunk) {
          size_t chunkSize = std::min(
              bucketsPerChunk, block.size() - chunkBegin);
          auto chunk = block.subspan(chunkBegin, chunkSize);
          std::vector<DomainHandle> domains = bucketDomains(segnum, chunk);
          for (size_t bucket = 0; bucket < chunkSize; bucket++) {
            for (size_t child = 0; child < children.size(); child++) {
              bindings[(chunkBegin + bucket) * children.size() + child]
                  ->calc(nullptr, (int32_t)segnum, domains[bucket]);
            }
          }
        }
      }
    }
  }
};

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
