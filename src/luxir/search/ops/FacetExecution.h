// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
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

// Joint producers build every domain of a chunk in one column pass, so a
// chunk's builders are alive together. Chunks are bounded by worst-case
// builder bytes; a block wider than the budget rescans the column per chunk.
inline constexpr size_t FACET_BUCKET_DOMAIN_BYTES = 64 * 1024 * 1024;
// The joint builders of all concurrent segment tasks of a parallel block. A
// task's share bounds its chunks, so a narrow share rescans the column more.
inline constexpr size_t FACET_BUCKET_PARALLEL_DOMAIN_BYTES = 4 * FACET_BUCKET_DOMAIN_BYTES;

// Set by a parallel bucket executor's segment task for the producer call it
// makes on its own thread. Concurrent segment tasks split the joint builder
// budget, and a task that fans out fed domains hears where a chunk ends, so
// it hands the chunk's domains on before the next chunk is built.
class FacetBucketTaskScope {
  inline static thread_local FacetBucketTaskScope* current = nullptr;
  FacetBucketTaskScope* previous;

public:
  size_t domainBytes;
  std::optional<std::function_ref<void()>> chunkFed;

  explicit FacetBucketTaskScope(size_t domainBytes,
      std::optional<std::function_ref<void()>> chunkFed = std::nullopt)
      : previous(std::exchange(current, this)), domainBytes(domainBytes), chunkFed(chunkFed) {}
  FacetBucketTaskScope(const FacetBucketTaskScope&) = delete;
  FacetBucketTaskScope& operator=(const FacetBucketTaskScope&) = delete;
  ~FacetBucketTaskScope() { current = previous; }

  static FacetBucketTaskScope* active() { return current; }
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
  using Feed = std::function_ref<void(size_t, DomainHandle)>;
  using BucketSource = std::function_ref<void(size_t, Feed)>;
  using BucketAction = std::function_ref<void(size_t)>;

  static void executeComplete(SearchOp::Calculator& parent,
      std::span<SearchOp* const> children, IndexReader& reader, size_t blockSize,
      std::span<BucketDomainPlan* const> plans, Bindings bindings, bool needInherited,
      BucketAction open, BucketAction close, BucketSource bucketDomains);
  static void deliverStructural(SearchOp::Calculator& parent,
      std::span<SearchOp* const> children, IndexReader& reader, int32_t segnum,
      Bindings bindings, const DomainHandle& domain);

  template <typename Key, typename DomainSource, typename IdentitySource = std::nullptr_t,
            typename SelectionSource = std::nullptr_t>
  static void execute(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      IndexReader& reader, FacetBucketProducer producer,
      FacetBucketConcurrency producerConcurrency,
      DomainSource&& bucketDomains, size_t bindingStateBytes,
      std::span<const DomainHandle> input = {}, IdentitySource&& identityDomains = nullptr,
      SelectionSource&& selection = nullptr) {
    if (children.empty() || buckets.empty()) return;
    bool structural = parent.structuralConstraint() != nullptr
        || std::ranges::any_of(children, [](auto* child) {
          return child->bucketDomain != nullptr || child->needsStructuralScope;
        });
    if (structural) executeRoute<true>(parent, children, buckets, reader, producer,
        producerConcurrency, bucketDomains, bindingStateBytes, input, identityDomains, selection);
    else executeRoute<false>(parent, children, buckets, reader, producer,
        producerConcurrency, bucketDomains, bindingStateBytes, input, identityDomains, selection);
  }

  // Feeds one non-structural block over the request's task group. Every
  // binding opens before the fan-out and closes after the join, and each
  // binding is fed each segment exactly once, as a top-level calcAll fan-out
  // feeds it: completion, merging and emission already run concurrently.
  // The granularity is the finest that every child and the producer allow:
  //  - SEGMENTS: a task per segment feeds the whole block in bucket order and
  //    holds that segment's state like a serial segment pass. Only that task
  //    touches the segment's retention counts and query state.
  //  - BUCKETS with a per-bucket producer: a task per (segment, bucket).
  //  - BUCKETS with a per-segment producer (a joint column pass): a task per
  //    segment produces the block and fans each built chunk's domains out.
  // Concurrent segment tasks split the joint builder budget, and every fed
  // domain is charged to the request while a task holds it. Returns false,
  // having done nothing, when the block would be a single task.
  template <typename Key, typename DomainSource>
  static bool executeParallelBlock(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> block, IndexReader& reader,
      std::span<const DomainHandle> input, FacetBucketConcurrency producerConcurrency,
      FacetBucketConcurrency concurrency, size_t contributors, FanoutScope& requestScope,
      Bindings bindings, BucketAction open, BucketAction close, DomainSource& bucketDomains) {
    auto segments = reader.segments();
    bool perBucket = concurrency == FacetBucketConcurrency::BUCKETS
        && producerConcurrency == FacetBucketConcurrency::BUCKETS;
    bool perDomain = concurrency == FacetBucketConcurrency::BUCKETS && !perBucket;
    size_t perSegment = perBucket ? block.size() : 1;
    size_t tasks = segments.size() * perSegment;
    if (tasks < 2 && !(perDomain && block.size() > 1)) return false;
    if (auto* stats = facetBucketFanoutStatsForTests) {
      (perBucket ? stats->buckets : perDomain ? stats->domains : stats->segments)
          .fetch_add(1, std::memory_order_relaxed);
    }
    auto& tracker = parent.getOp().req.memoryTracker;
    uint64_t blockCount = 0;
    for (const auto& bucket : block) blockCount += (uint64_t)std::max<int64_t>(1, bucket.count);
    size_t domainBytes = std::min(FACET_BUCKET_DOMAIN_BYTES,
        FACET_BUCKET_PARALLEL_DOMAIN_BYTES / contributors);
    auto charge = [&](size_t segnum, DomainHandle domain) {
      if (domain.ownsStorage() && (input.empty() || domain.get() != input[segnum].get())) {
        domain = DomainVariantPlan::account(std::move(domain), tracker, "parallel bucket domains");
      }
      return domain;
    };
    auto deliver = [&](size_t bucket, size_t segnum, const DomainHandle& domain) {
      assert(domain.isDeliverable());
      for (size_t child = 0; child < children.size(); child++) {
        bindings[bucket * children.size() + child]->accept(nullptr, (int32_t)segnum, domain);
      }
    };
    auto units = [&](size_t segnum, const DomainHandle& domain) {
      return FanoutSize{std::max<uint64_t>(1,
          fanoutDomainUnits(segments[segnum], domain) * children.size()), 0};
    };
    for (size_t bucket = 0; bucket < block.size(); bucket++) open(bucket);
    {
      // BUCKETS children hold no releasable segment state of their own, so
      // the coordinator retains every segment around the fan-out.
      std::optional<SearchOp::SegmentStateRetention> retain;
      if (concurrency == FacetBucketConcurrency::BUCKETS) retain.emplace(children, segments);
      // Declared last: an exception joins the published tasks before
      // anything they borrow is destroyed.
      FanoutScope scope = requestScope.child();
      task_group_fanout(&scope, tasks, FanoutKind::EXHAUSTIVE, [&](size_t i) {
        size_t segnum = i / perSegment;
        auto size = units(segnum, input.empty() ? DomainHandle() : input[segnum]);
        if (perBucket) size.units = std::max<uint64_t>(1, (uint64_t)((unsigned __int128)size.units
            * (uint64_t)std::max<int64_t>(1, block[i % perSegment].count) / blockCount));
        return size;
      }, [&](size_t i) {
        return [&, i] {
          size_t segnum = i / perSegment;
          if (perBucket) {
            size_t bucket = i % perSegment;
            bucketDomains(segnum, block.subspan(bucket, 1),
                FacetBucketFeed([&](size_t, DomainHandle domain) {
                  deliver(bucket, segnum, charge(segnum, std::move(domain)));
                }));
            return;
          }
          if (!perDomain) {
            auto release = scope_guard([&]() noexcept {
              for (auto& binding : bindings) binding->releaseSegmentState(segments[segnum]);
            });
            SearchOp::SegmentStateRetention retainSegment(children, segments.subspan(segnum, 1));
            FacetBucketTaskScope task(domainBytes);
            [[maybe_unused]] size_t fed = 0;
            bucketDomains(segnum, block, FacetBucketFeed([&](size_t bucket, DomainHandle domain) {
              assert(bucket == fed++);
              deliver(bucket, segnum, charge(segnum, std::move(domain)));
            }));
            assert(fed == block.size());
            return;
          }
          std::vector<std::pair<size_t, DomainHandle>> pending;
          std::vector<FanoutSize> sizes;
          size_t pendingBytes = 0;
          auto flush = [&] {
            sizes.clear();
            for (const auto& entry : pending) sizes.push_back(units(segnum, entry.second));
            task_group_fanout(&scope, pending.size(), FanoutKind::EXHAUSTIVE,
                [&](size_t j) { return sizes[j]; },
                [&](size_t j) {
                  return [&deliver, segnum, bucket = pending[j].first,
                          domain = std::move(pending[j].second)] {
                    deliver(bucket, segnum, domain);
                  };
                });
            pending.clear();
            pendingBytes = 0;
          };
          FacetBucketTaskScope task(domainBytes, std::function_ref<void()>(flush));
          bucketDomains(segnum, block, FacetBucketFeed([&](size_t bucket, DomainHandle domain) {
            domain = charge(segnum, std::move(domain));
            pendingBytes += domain.get() != nullptr ? domain.get()->ramBytesUsed() : 0;
            pending.emplace_back(bucket, std::move(domain));
            if (pendingBytes >= domainBytes) flush();
          }));
          flush();
        };
      });
      scope.join();
    }
    for (size_t bucket = 0; bucket < block.size(); bucket++) close(bucket);
    return true;
  }

  template <bool Structural, typename Key, typename DomainSource, typename IdentitySource,
            typename SelectionSource>
  static void executeRoute(
      SearchOp::Calculator& parent, std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      IndexReader& reader, FacetBucketProducer producer,
      FacetBucketConcurrency producerConcurrency,
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
    bool scopeNeeded = false;
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
      wholeReaderPrefixes = routing.wholeReaderPrefixes;
      complete = routing.complete;
      needInherited = routing.needInherited;
      ancestor = routing.ancestor;
      residentBytesPerBucket = saturatingAdd(residentBytesPerBucket, routing.retainedBytesPerBucket);
    }
    // Non-structural blocks may fan out over the request's task group, at
    // the granularity every child and the producer allow (see
    // executeParallelBlock). Structural blocks stay serial: their bindings
    // share prefix producers and plan sources across segments.
    FanoutScope* requestScope = nullptr;
    auto concurrency = FacetBucketConcurrency::SERIAL;
    size_t contributors = 1;
    if constexpr (!Structural) {
      requestScope = parent.getOp().req.tg;
      if (requestScope != nullptr && fanoutCallerStartsAtBack(requestScope)
          && forcedFacetBucketFanout != FacetBucketFanout::SERIAL) {
        concurrency = producerConcurrency == FacetBucketConcurrency::SERIAL
            ? FacetBucketConcurrency::SERIAL : FacetBucketConcurrency::BUCKETS;
        for (SearchOp* child : children) {
          concurrency = std::min(concurrency, child->facetBucketConcurrency());
        }
      }
      if (concurrency != FacetBucketConcurrency::SERIAL) {
        auto* arena = requestScope->getArena();
        contributors = std::min(segments.size(), (size_t)computeWidth(arena != nullptr
            ? arena->max_concurrency() : oneapi::tbb::this_task_arena::max_concurrency()));
        contributors = std::max<size_t>(1, contributors);
      }
    }
    // A BUCKETS fan-out feeds each binding from its own tasks, not in a
    // segment pass over the block, so only the binding budget bounds it.
    size_t blockBytes = sharedSegmentWork || concurrency == FacetBucketConcurrency::BUCKETS
        ? bindingStateBytes
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
        // covers directory relocation overlap. A parallel block feeds a
        // binding's segments concurrently, and its merger keeps an
        // accumulator per concurrent contributor.
        bindingBlockSize = blockBegin == 0 ? 1 : std::max<size_t>(1,
            budget / std::max<size_t>(1, saturatingMultiply(observedBytesPerBucket,
                saturatingMultiply(2, contributors))));
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
            : StructuralConstraint::Producer(parent.getOp().req, parent.getOp().view,
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
            bindings[bucket * children.size() + child]->acceptAll(nullptr, noDomains);
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

      if constexpr (!Structural) {
        if (concurrency != FacetBucketConcurrency::SERIAL
            && executeParallelBlock<Key>(parent, children, block, reader, input,
                producerConcurrency, concurrency, contributors, *requestScope, bindings,
                open, close, bucketDomains)) {
          continue;
        }
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
        [[maybe_unused]] auto releasePlans = [&] {
          if constexpr (Structural) {
            return scope_guard([&]() noexcept {
              // Release after this block's last bucket in the segment.
              // Rebuilding next block bounds expansions to one segment.
              for (auto* plan : routing.plans) {
                if (std::ranges::none_of(children, [&](auto* child) {
                      return child->bucketDomain == plan
                          && child->segmentStateRetained(segments[segnum]);
                    })) plan->variants.releaseSegmentState(segments[segnum]);
              }
            });
          } else return nullptr;
        }();
        SearchOp::SegmentStateRetention retain(children, retained);
        [[maybe_unused]] size_t fed = 0;
        auto deliver = [&](size_t bucket, DomainHandle domain) {
          assert(bucket < blockSize);
          assert(domain.isDeliverable());
          if (first) open(bucket);
          if constexpr (!Structural) {
            for (size_t child = 0; child < children.size(); child++) {
              bindings[bucket * children.size() + child]
                  ->accept(nullptr, (int32_t)segnum, domain);
            }
          } else {
            deliverStructural(parent, children, reader, (int32_t)segnum,
                std::span(bindings).subspan(bucket * children.size(), children.size()),
                domain);
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

template <typename Key, typename BuildChunk>
void feedFacetBucketDomainChunks(
    int32_t maxDoc, std::span<const SelectedFacetBucket<Key>> block,
    size_t domainBytes, FacetBucketFeed feed, BuildChunk&& buildChunk) {
  constexpr size_t BUILDER_FIXED_BYTES = 128;
  auto* task = FacetBucketTaskScope::active();
  if (task != nullptr) domainBytes = std::min(domainBytes, task->domainBytes);
  size_t builderBytes = (size_t)(((uint64_t)maxDoc + 63) / 64) * 8
      + BUILDER_FIXED_BYTES;
  size_t chunkBuckets = std::max<size_t>(1, domainBytes / builderBytes);
  for (size_t chunkBegin = 0; chunkBegin < block.size();
       chunkBegin += chunkBuckets) {
    auto chunk = block.subspan(
        chunkBegin, std::min(chunkBuckets, block.size() - chunkBegin));
    {
      auto domains = buildChunk(chunk);
      assert(domains.size() == chunk.size());
      for (size_t i = 0; i < chunk.size(); i++) {
        feed(chunkBegin + i, std::move(domains[i]));
      }
    }
    if (task != nullptr && task->chunkFed) (*task->chunkFed)();
  }
}

// Values arrive in document order, but a multi-valued column may route the
// same document to a bucket more than once. Domains contain each document once.
template <typename VisitValues, typename BucketOf>
std::vector<DomainHandle> buildFacetBucketDomains(
    Segment& segment, size_t numBuckets, VisitValues&& visitValues,
    BucketOf&& bucketOf) {
  std::vector<DocSetBuilder> builders;
  builders.reserve(numBuckets);
  for (size_t i = 0; i < numBuckets; i++) builders.emplace_back(segment.maxDoc());
  std::vector<int32_t> lastAdded(numBuckets, -1);
  visitValues([&](int32_t docid, int64_t value) LUXIR_INLINE {
    int32_t bucket = bucketOf(value);
    if (bucket < 0 || lastAdded[(size_t)bucket] == docid) return;
    lastAdded[(size_t)bucket] = docid;
    builders[(size_t)bucket].add(docid);
  });
  std::vector<DomainHandle> domains;
  domains.reserve(numBuckets);
  for (auto& builder : builders) domains.push_back(DomainHandle(builder.build()).scoped(segment));
  return domains;
}

} // namespace luxir
