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
#include "luxir/search/SearchOverrides.h"

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
  IndexReader::FieldSegments segInfos;
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

// How a parent produces one segment's bucket domains for a block. A joint
// producer builds a chunk's domains in one column pass, so every block repeats
// that pass; an independent producer builds each domain on its own.
enum class FacetBucketProducer : uint8_t {
  INDEPENDENT,
  JOINT
};

// Block bindings, then feed each segment's bucket domains to that bucket's
// children. A bucket's bindings open at its first feed and close after its
// last, so whatever a completed child holds does not accumulate across the
// block.
//
// The block width trades per-bucket locality for per-segment sharing. A
// segment pass cycles through the state of every binding in the block, while
// each extra block repeats the segment work its buckets share: a joint
// producer's column pass, and the per-segment query state children release
// after each segment (multiterm expansions). With shared work, only the
// binding byte budget bounds a block. Without it, the block keeps its
// bindings' state within CACHE_BYTES, so a heavy child (a nested facet's
// counts) runs a bucket at a time and keeps its state hot across segments.
class FacetBucketBlockExecutor {
public:
  static constexpr size_t BINDING_BYTES = 64 * 1024 * 1024;
  // A share of a 1 MiB L2: a segment pass also touches the bucket's domain
  // and the child's column and ord-map decoding.
  static constexpr size_t CACHE_BYTES = 256 * 1024;

  struct InputDomainBytes { size_t largestSegment = 0; size_t wholeReader = 0; };
  // Conservative retained-domain bound, shared by structural routing and
  // child plans that keep their input domains for result-stage execution.
  static InputDomainBytes inputDomainBytes(IndexReader& reader);

  struct StructuralRoute {
    bool scopeNeeded = false;
    bool overrides = false;
    bool selectionPath = false;
    bool wholeReaderPrefixes = false;
    bool complete = false;
    bool needInherited = false;
    size_t retainedBytesPerBucket = 0;
    const StructuralConstraint* ancestor = nullptr;
    std::vector<BucketDomainPlan*> plans;

    StructuralRoute(SearchOp::Calculator& parent, std::span<SearchOp* const> children,
                    IndexReader& reader, bool hasSelection);
  };

  using Bindings = std::span<std::unique_ptr<SearchOp::Calculator>>;
  using SegmentSources = std::vector<std::pair<BucketDomainPlan*,
      std::unique_ptr<DomainVariantPlan::SegmentSources>>>;
  using Feed = std::function_ref<void(size_t, DomainHandle)>;
  using BucketSource = std::function_ref<void(size_t, Feed)>;
  using BucketAction = std::function_ref<void(size_t)>;

  static void executeComplete(SearchOp::Calculator& parent,
      std::span<SearchOp* const> children, IndexReader& reader, size_t blockSize,
      std::span<BucketDomainPlan* const> plans, Bindings bindings, bool needInherited,
      BucketAction open, BucketAction close, BucketSource bucketDomains);
  static void deliverStructural(SearchOp::Calculator& parent,
      std::span<SearchOp* const> children, IndexReader& reader, int32_t segnum,
      Bindings bindings, const DomainHandle& domain, SegmentSources& sources);

  template <typename Key, typename DomainSource, typename IdentitySource = std::nullptr_t,
            typename SelectionSource = std::nullptr_t>
  static void execute(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      IndexReader& reader, FacetBucketProducer producer,
      DomainSource&& bucketDomains, size_t bindingStateBytes,
      std::span<const DomainHandle> input = {}, IdentitySource&& identityDomains = nullptr,
      SelectionSource&& selection = nullptr) {
    if (children.empty() || buckets.empty()) return;
    bool structural = parent.structuralConstraint() != nullptr
        || std::ranges::any_of(children, [](auto* child) {
          return child->bucketDomain != nullptr || child->needsStructuralScope;
        });
    if (structural) executeRoute<true>(parent, children, buckets, reader, producer,
        bucketDomains, bindingStateBytes, input, identityDomains, selection);
    else executeRoute<false>(parent, children, buckets, reader, producer,
        bucketDomains, bindingStateBytes, input, identityDomains, selection);
  }

  template <bool Structural, typename Key, typename DomainSource, typename IdentitySource,
            typename SelectionSource>
  static void executeRoute(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      IndexReader& reader, FacetBucketProducer producer,
      DomainSource&& bucketDomains, size_t bindingStateBytes,
      std::span<const DomainHandle> input, IdentitySource&& identityDomains,
      SelectionSource&& selection) {
    if (children.empty() || buckets.empty()) return;
    size_t residentBytesPerBucket = 0;
    bool sharedSegmentWork = producer == FacetBucketProducer::JOINT;
    bool adaptive = false;
    for (SearchOp* child : children) {
      adaptive = adaptive || child->facetBucketGrows();
      residentBytesPerBucket = saturatingAdd(
          residentBytesPerBucket, child->facetBucketResidentBytes());
      sharedSegmentWork = sharedSegmentWork
          || child->facetBucketSharesSegmentState();
    }
    auto segments = reader.segments();
    bool scopeNeeded = false, overrides = false;
    bool wholeReaderPrefixes = false, complete = false, needInherited = false;
    const StructuralConstraint* ancestor = nullptr;
    auto selectionAt = [&](const auto& bucket) -> Query::Weight* {
      if constexpr (std::is_same_v<std::remove_reference_t<SelectionSource>, std::nullptr_t>) return nullptr;
      else return selection(bucket);
    };
    auto routing = [&] {
      if constexpr (Structural) {
        bool hasSelection = (parent.structuralConstraint() && parent.structuralConstraint()->hasSelection())
            || std::ranges::any_of(buckets, [&](const auto& bucket) {
              return selectionAt(bucket) != nullptr;
            });
        return StructuralRoute(parent, children, reader, hasSelection);
      } else return nullptr;
    }();
    if constexpr (Structural) {
      scopeNeeded = routing.scopeNeeded;
      overrides = routing.overrides;
      wholeReaderPrefixes = routing.wholeReaderPrefixes;
      complete = routing.complete;
      needInherited = routing.needInherited;
      ancestor = routing.ancestor;
      residentBytesPerBucket = saturatingAdd(residentBytesPerBucket, routing.retainedBytesPerBucket);
    }
    size_t blockBytes = sharedSegmentWork ? bindingStateBytes
        : std::min(bindingStateBytes, forcedFacetBucketCacheBytes != 0
              ? forcedFacetBucketCacheBytes : CACHE_BYTES);
    size_t bindingBlockSize = std::max<size_t>(
        1, blockBytes / std::max<size_t>(1, residentBytesPerBucket));
    size_t observedBytesPerBucket = residentBytesPerBucket;
    auto& tracker = parent.getOp().req.memoryTracker;
    for (size_t blockBegin = 0; blockBegin < buckets.size();) {
      if (adaptive) {
        size_t budget = blockBytes;
        if (tracker.ceiling() != 0) {
          size_t remaining = tracker.ceiling() - std::min(tracker.ceiling(), tracker.bytes());
          budget = std::min(budget, remaining / 2);
        }
        // Probe one binding before multiplying an unknown directory by the
        // fan-out. Then use the observed high-water mark directly: joint
        // producers rescan the parent column for each block. The extra factor
        // covers directory relocation overlap.
        bindingBlockSize = blockBegin == 0 ? 1 : std::max<size_t>(1,
            budget / std::max<size_t>(1, saturatingMultiply(observedBytesPerBucket, 2)));
      }
      if (forcedFacetBucketBlockBuckets != 0) bindingBlockSize = forcedFacetBucketBlockBuckets;
      if (facetBucketBlockCounter != nullptr) (*facetBucketBlockCounter)++;
      size_t blockSize = std::min(
          bindingBlockSize, buckets.size() - blockBegin);
      auto block = std::span<const SelectedFacetBucket<Key>>(buckets)
                       .subspan(blockBegin, blockSize);
      blockBegin += blockSize;
      // Recoverable identities have their own predicate producer. MEMBERS
      // scheduling and cost rules remain independent of live-doc identity work.
      using Block = decltype(block);
      struct PrefixProducer final : StructuralConstraint::Producer {
        std::remove_reference_t<IdentitySource> source;
        RequestMemResource resource;
        std::pmr::vector<SelectedFacetBucket<Key>> selected;

        PrefixProducer(std::remove_reference_t<IdentitySource>& source, Block block,
                       SearchOp::Calculator& parent, size_t)
            : StructuralConstraint::Producer(parent.getOp().req,
                  parent.structuralConstraint(), block.size()), source(source),
              resource(parent.getOp().req.memoryTracker, "bucket keys", "domain variants"),
              selected(block.begin(), block.end(), &resource) {}

        void produce(size_t segment, size_t begin, size_t count,
                     DomainHandle base, Feed feed) override {
          if constexpr (std::is_same_v<std::remove_reference_t<IdentitySource>, std::nullptr_t>) {
            throw std::logic_error("bucket identity source is required for a reset");
          } else {
            // execute and every child feed are synchronous (tg == nullptr).
            // The closure's borrowed column maps and group page outlive this call.
            source(segment, Block(selected).subspan(begin, count), std::move(base), feed);
          }
        }
      };
      auto prefixes = [&] {
        if constexpr (Structural) return std::unique_ptr<PrefixProducer>{};
        else return nullptr;
      }();
      auto paths = [&] {
        if constexpr (Structural) return std::vector<std::optional<StructuralConstraint>>(blockSize);
        else return nullptr;
      }();
      if constexpr (Structural) if (scopeNeeded) {
        // Own the closure and bucket span. Borrowed key payloads still follow
        // the executor's synchronous result-stage lifetime.
        prefixes = std::make_unique<PrefixProducer>(identityDomains, block, parent, segments.size());
      }
      std::vector<std::unique_ptr<SearchOp::Calculator>> bindings(blockSize * children.size());
      auto open = [&](size_t bucket) {
        assert(block[bucket].output.value >= 0);
        assert(block[bucket].output.value < (int32_t)buckets.size());
        const StructuralConstraint* path = nullptr;
        if constexpr (Structural) if (scopeNeeded) {
          paths[bucket].emplace(StructuralConstraint{
              ancestor, prefixes.get(), bucket, selectionAt(block[bucket])});
          path = &*paths[bucket];
        }
        for (size_t child = 0; child < children.size(); child++) {
          auto& binding = bindings[bucket * children.size() + child];
          assert(binding == nullptr);
          binding.reset(children[child]->createCalculator(&parent,
              block[bucket].output.value, (int64_t)buckets.size(), path));
        }
      };
      auto close = [&](size_t bucket) {
        if (adaptive) {
          size_t bytes = 0;
          for (size_t child = 0; child < children.size(); child++) bytes = saturatingAdd(
              bytes, bindings[bucket * children.size() + child]->facetBucketObservedBytes());
          if constexpr (Structural) bytes = saturatingAdd(bytes, routing.retainedBytesPerBucket);
          observedBytesPerBucket = std::max(observedBytesPerBucket, bytes);
        }
        for (size_t child = 0; child < children.size(); child++) {
          bindings[bucket * children.size() + child].reset();
        }
      };
      if (segments.empty()) {
        std::span<const DomainHandle> noDomains;
        for (size_t bucket = 0; bucket < blockSize; bucket++) {
          open(bucket);
          for (size_t child = 0; child < children.size(); child++) {
            bindings[bucket * children.size() + child]->calcAll(nullptr, noDomains);
          }
          close(bucket);
        }
        continue;
      }
      if constexpr (Structural) if (complete) {
        auto ordinary = [&](size_t segnum, auto&& deliver, bool retained = false) {
          [[maybe_unused]] size_t fed = 0;
          auto feed = [&](size_t bucket, DomainHandle domain) {
            assert(bucket == fed++);
            assert(bucket < blockSize);
            assert(domain.isDeliverable());
            assert(input.empty() || !input[segnum].get() || domain.get());
            if (retained && domain.ownsStorage()
                && (input.empty() || domain.get() != input[segnum].get())) {
              domain = DomainVariantPlan::account(std::move(domain), tracker, "retained bucket inputs");
            }
            deliver(bucket, std::move(domain));
          };
          bucketDomains(segnum, block, FacetBucketFeed(feed));
          assert(fed == blockSize);
        };

        executeComplete(parent, children, reader, blockSize, routing.plans, bindings,
            needInherited, open, close, [&](size_t segment, FacetBucketFeed feed) {
              ordinary(segment, feed, needInherited);
            });
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
        auto sources = [&] {
          if constexpr (Structural) return SegmentSources{};
          else return nullptr;
        }();
        if constexpr (Structural) if (overrides) sources.reserve(children.size());
        [[maybe_unused]] auto releasePlans = [&] {
          if constexpr (Structural) {
            return scope_guard([&]() noexcept {
              // Release after this block's last bucket in the segment.
              // Rebuilding next block bounds expansions to one segment.
              for (auto& entry : sources) {
                if (std::ranges::none_of(children, [&](auto* child) {
                      return child->bucketDomain == entry.first
                          && child->segmentStateRetained(segments[segnum]);
                    })) entry.first->variants.releaseSegmentState(segments[segnum]);
              }
            });
          } else return nullptr;
        }();
        SearchOp::SegmentStateRetention retain(children, retained);
        if constexpr (Structural) for (auto* plan : routing.plans) {
          sources.emplace_back(plan, std::make_unique<DomainVariantPlan::SegmentSources>(
              plan->variants, reader, *plan->planning, (int32_t)segnum, tracker));
        }
        [[maybe_unused]] size_t fed = 0;
        auto deliver = [&](size_t bucket, DomainHandle domain) {
          assert(bucket < blockSize);
          assert(domain.isDeliverable());
          if (first) open(bucket);
          if constexpr (!Structural) {
            for (size_t child = 0; child < children.size(); child++) {
              bindings[bucket * children.size() + child]
                  ->calc(nullptr, (int32_t)segnum, domain);
            }
          } else {
            deliverStructural(parent, children, reader, (int32_t)segnum,
                std::span(bindings).subspan(bucket * children.size(), children.size()),
                domain, sources);
          }
          if (last) close(bucket);
          fed++;
        };
        bucketDomains(segnum, block, FacetBucketFeed(deliver));
        assert(fed == blockSize);
        if constexpr (Structural) {
          if (prefixes && !wholeReaderPrefixes) prefixes->releaseSegment((int32_t)segnum);
        }
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
    auto domains = buildChunk(chunk);
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
