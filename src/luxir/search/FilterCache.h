// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/unordered/concurrent_flat_map.hpp>

#include "luxir/search/DocSet.h"
#include "luxir/reader/Space.h"
#include "luxir/search/FilterKey.h"
#include "luxir/search/RequestMemTracker.h"
#include "luxir/util/Striped.h"

namespace luxir {

class IndexReader;

struct FilterCacheConfig {
  size_t maxBytes = 64ULL * 1024 * 1024;
  size_t maxEntryBytes = 0;
  // Segment values below this floor may be populated only by a WHOLE-lane Use:
  // complete reader residency unlocks Weight omission. CLAUSE-only Uses retain
  // the floor to prevent per-clause churn; shared values remain reusable by
  // either lane after publication.
  #ifdef NDEBUG
  int32_t minSegmentDocs = 1000;
  #else
  // Debug suites should broadly exercise both the admitted and skipped paths.
  // Explicit unit tests keep the skip case covered; 1-2 doc segments still skip.
  int32_t minSegmentDocs = 3;
  #endif
  size_t admissionHistorySize = 4096;
  // Zero inherits admissionHistorySize so both lanes have equal defaults
  // while remaining independently configurable.
  size_t wholeAdmissionHistorySize = 0;
  uint32_t admissionThreshold = 2;
  uint64_t recentWorkEpochs = 4096;
  size_t maxMetadataEntries = 8192;
  size_t maxMetadataBytes = 0;
  // A metadata sweep started by exceeding maxMetadataBytes trims to this
  // watermark, so steady slot growth does not start a sweep per request.
  // Zero means 90% of maxMetadataBytes.
  size_t lowWatermarkMetadataBytes = 0;
};

// SEGMENT_STABLE and CORE_STABLE keys name raw DocSet values, one per seg_id.
// live_gen is deliberately absent: delete flushes retain the entry, and each
// request folds liveness at use time; segments without deletes serve raw sets.
// READER_STABLE keys instead name one whole-reader, live-exact value tagged by
// commitTime and seg_id. They are admitted only at a canonical reader-live
// PrepareContext, published and evicted atomically as a unit, and never enter
// the raw per-segment APIs. The semantic map key contains no reader version.
// The owning ReaderManager bounds the cache lifetime so its seg_id namespace can
// never be reused underneath either value kind.
//
// Use performs the one outer-map lookup for a query/filter pair, snapshots the
// entry's COW slot vector, and aligns it to reader ordinals. Segment tasks then
// index that request-local vector without hashing. No map entry is allocated
// for a key until either bounded observation lane has admitted it (second
// sighting by default); clause and whole-query traffic use separate equal-
// policy histories. Oversized keys bypass before ring recording, and key
// metadata has its own maxMetadataBytes budget. One recent-work sweeper
// trims segment payloads individually and ReaderValues as whole units until
// payloads fit their budget.
//
// Query::Context and SearchRequest's request-destructed registry own borrowed
// shared_ptr pins and request-local raw values. MemPool-allocated weights and
// scorers retain raw pointers because their destructors never run. The registry
// exists even when this shared cache is absent or disabled.
class FilterCache {
  struct ActiveSnapshot;
  struct FilterEntry;
  struct SegmentSlot;
  struct Ledger;

public:
  class Use;
  class UseRegistry;
  class ExistingCandidate;
  class SegmentCandidate;

  enum class AdmissionLane : uint8_t {
    CLAUSE,
    WHOLE
  };

  enum class Population : uint8_t {
    ALLOW,
    BORROW_ONLY
  };

  class Work {
  public:
    enum class Tag : uint8_t { PENDING, ASSUMED, OBSERVED };

  private:
    double amount;
    Tag tag;
    constexpr Work(double amount, Tag tag) : amount(amount), tag(tag) {}

  public:
    static constexpr Work assumed() { return Work(0, Tag::ASSUMED); }
    static constexpr Work pending() { return Work(0, Tag::PENDING); }
    static Work nanoseconds(double amount) {
      if (!std::isfinite(amount) || amount < 0) {
        throw std::invalid_argument("cache work must be finite nonnegative nanoseconds");
      }
      return Work(amount, Tag::OBSERVED);
    }
    bool known() const { return tag != Tag::PENDING; }
    bool isAssumed() const { return tag == Tag::ASSUMED; }
    double resolve(double productionNanos) const {
      assert(known());
      return isAssumed() ? productionNanos : amount;
    }
  };

  struct SegmentIdentity {
    uint64_t segId;
    int32_t maxDoc;
    SpaceOrdinal space = SpaceOrdinal::ROOT;

    friend bool operator==(const SegmentIdentity&, const SegmentIdentity&) = default;
  };

  struct CounterValues {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t admissions = 0;
    uint64_t buildAttempts = 0;
    uint64_t builds = 0;
    uint64_t buildNanos = 0;
    uint64_t estimatedSavedNanos = 0;
    uint64_t unpaidBuildNanos = 0;
    uint64_t byproductInserts = 0;
    uint64_t publishRejects = 0;
    uint64_t evictions = 0;
    uint64_t unrepaidEvictions = 0;
    uint64_t populationDeferred = 0;
    uint64_t capacityDeferred = 0;
    uint64_t purges = 0;
    uint64_t oversizedKeyBypasses = 0;
    uint64_t readerStableHits = 0;
    uint64_t readerStableRefreshes = 0;
    uint64_t readerStableRetires = 0;
    uint64_t assumedCharges = 0;
    uint64_t observedCharges = 0;
    uint64_t assumedCredits = 0;
    uint64_t observedCredits = 0;
    uint64_t outstandingDebtNanos = 0;
    size_t residentBytes = 0;
    size_t metadataBytes = 0;

    friend bool operator==(const CounterValues&, const CounterValues&) = default;
  };

  // Recency only needs to be approximate. Skip the store when a Use with the
  // same or a newer epoch already recorded it (all segments of one Use share
  // an epoch), so a hot entry is written once per Use rather than per hit.
  static void touchEpoch(std::atomic<uint64_t>& lastUsed, uint64_t epoch) {
    if (lastUsed.load(std::memory_order_relaxed) < epoch) {
      lastUsed.store(epoch, std::memory_order_relaxed);
    }
  }

  class SegmentValue {
    friend class FilterCache;

    std::unique_ptr<DocSet> docs;
    // DocSet payload charge only - key bytes are deliberately excluded
    // (entry-lifetime metadataBytes budget, not value lifetime).
    uint32_t charge;
    uint64_t buildCostNanos;
    const bool claimedBuild;
    mutable std::atomic<double> creditedTotal{0};
    double residencyBytes = 0;
    uint64_t published = 0;
    #ifndef NDEBUG
    SegmentIdentity identityForTest;
    #endif

  public:
    SegmentValue(std::unique_ptr<DocSet> docs,
                 SegmentIdentity identityForTest, uint64_t buildCostNanos,
                 bool claimedBuild);

    DocSet* docSet() const { return docs.get(); }
    int32_t card() const {
      // Publication forces card() before visibility, making this immutable.
      assert(docs->cachedCard() >= 0);
      return docs->cachedCard();
    }
    size_t ramBytesUsed() const { return charge; }
  };

  // A globally bounded query such as kNN selects membership across all
  // segments, so independently publishable segment slots cannot represent it.
  // ReaderValue is live-exact for one reader version and is one accounting,
  // publication, retirement, and eviction unit.
  class ReaderValue {
    friend class FilterCache;

    struct SegmentDocs {
      uint64_t segId;
      int32_t maxDoc;
      std::unique_ptr<DocSet> docs;
      SpaceOrdinal space;
    };

    uint64_t readerVersion_;
    std::vector<SegmentDocs> segments;
    size_t charge;
    uint64_t buildCostNanos;
    const bool claimedBuild;
    mutable std::atomic<double> creditedTotal{0};
    double residencyBytes = 0;
    uint64_t published = 0;

    ReaderValue(uint64_t readerVersion,
                std::span<const SegmentIdentity> identities,
                std::vector<std::unique_ptr<DocSet>> docs,
                uint64_t buildCostNanos, bool claimedBuild);

  public:
    uint64_t readerVersion() const { return readerVersion_; }
    size_t ramBytesUsed() const { return charge; }
    DocSet* docSet(size_t segmentOrd, const SegmentIdentity& identity) const;
  };

  class Probe {
  public:
    enum class Kind : uint8_t {
      HIT,
      BUILD,
      BYPASS
    };

  private:
    std::shared_ptr<SegmentSlot> slot;
    std::shared_ptr<const SegmentValue> value;
    DocSet* requestValue = nullptr;
    Use* requestUse = nullptr;
    size_t requestSegmentOrd = 0;
    Kind kind_ = Kind::BYPASS;
    bool ownsClaim = false;
    bool ownsRequestClaim = false;

    Probe(Kind kind, std::shared_ptr<SegmentSlot> slot,
          std::shared_ptr<const SegmentValue> value, bool ownsClaim,
          DocSet* requestValue = nullptr, Use* requestUse = nullptr,
          size_t requestSegmentOrd = 0, bool ownsRequestClaim = false);
    void releaseClaims();

    friend class FilterCache;
    friend class Use;

  public:
    Probe() = default;
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    Probe(Probe&& other) noexcept;
    Probe& operator=(Probe&& other) noexcept;
    ~Probe();

    Kind kind() const { return kind_; }
    DocSet* docSet() const {
      return requestValue != nullptr ? requestValue
          : value == nullptr ? nullptr : value->docSet();
    }
    int32_t card() const {
      DocSet* docs = docSet();
      return docs == nullptr ? 0 : docs->card();
    }
  };

  class ReaderProbe {
  public:
    enum class Kind : uint8_t {
      HIT,
      BUILD,
      BYPASS
    };

  private:
    std::shared_ptr<FilterEntry> entry;
    std::shared_ptr<const ReaderValue> value;
    Kind kind_ = Kind::BYPASS;
    bool ownsClaim = false;
    bool refresh = false;

    ReaderProbe(Kind kind, std::shared_ptr<FilterEntry> entry,
                std::shared_ptr<const ReaderValue> value, bool ownsClaim,
                bool refresh);
    void releaseClaim();

    friend class FilterCache;
    friend class Use;

  public:
    ReaderProbe() = default;
    ReaderProbe(const ReaderProbe&) = delete;
    ReaderProbe& operator=(const ReaderProbe&) = delete;
    ReaderProbe(ReaderProbe&& other) noexcept;
    ReaderProbe& operator=(ReaderProbe&& other) noexcept;
    ~ReaderProbe();

    Kind kind() const { return kind_; }
    const std::shared_ptr<const ReaderValue>& sharedValue() const {
      return value;
    }
  };

  // A segment lookup has no cache effects until its Use accepts it. It holds
  // either a pin on the shared value or the value this request already
  // resolved, which lives as long as the Use.
  class SegmentCandidate {
    Use* use;
    size_t segmentOrd;
    std::shared_ptr<const SegmentValue> value;
    DocSet* requestValue;

    SegmentCandidate(Use* use, size_t segmentOrd,
                     std::shared_ptr<const SegmentValue> value,
                     DocSet* requestValue = nullptr)
      : use(use), segmentOrd(segmentOrd),
        value(std::move(value)), requestValue(requestValue) {}

    friend class Use;

  public:
    SegmentCandidate(const SegmentCandidate&) = delete;
    SegmentCandidate& operator=(const SegmentCandidate&) = delete;
    SegmentCandidate(SegmentCandidate&&) noexcept = default;
    SegmentCandidate& operator=(SegmentCandidate&&) noexcept = default;

    DocSet* docSet() const {
      return value != nullptr ? value->docSet() : requestValue;
    }
    int32_t card() const { return docSet()->card(); }
  };

  class Use {
    enum class Deferral { FUNDING, CAPACITY };
    struct Accounting {
      bool ordinary = false;
      bool charged = false;
      bool credited = false;
      bool populationAllowed = false;
      bool deferred = false;
      bool pending = false;
      bool built = false;
      bool pricedClaim = false;

      template<class Value>
      const Value* takeCredit(const Value* value, Work work) {
        if (value == nullptr || !work.known() || credited) return nullptr;
        credited = true;
        return value;
      }
    };
    struct RequestSlot {
      Accounting accounting;
      const SegmentValue* servedValue = nullptr;
      std::mutex mutex;
      std::condition_variable condition;
      bool resolving = false;
      DocSet* raw = nullptr;
      bool rawOwned = false;
      std::unique_ptr<DocSet> ownedRaw;
      // Almost every slot pins exactly one value; keep it inline so a pure
      // hit allocates nothing. Further distinct values go to pins.
      std::shared_ptr<const SegmentValue> firstPin;
      std::vector<std::shared_ptr<const SegmentValue>> pins;
      std::vector<std::shared_ptr<const SegmentValue>> requestOwnedPins;
      std::unique_ptr<DocSet> liveEffective;
      RequestMemTracker* routedTracker = nullptr;
      size_t routedBytes = 0;
      std::string routedDetail;

      void chargeRouted(size_t bytes) {
        if (routedTracker == nullptr || bytes == 0) return;
        if (bytes > std::numeric_limits<size_t>::max() - routedBytes) {
          routedTracker->chargeOverflow("domain variants", routedDetail);
        }
        routedTracker->charge(bytes, "domain variants", routedDetail);
        routedBytes += bytes;
      }

      ~RequestSlot() {
        if (routedTracker != nullptr) routedTracker->release(routedBytes);
      }
    };

    FilterCache* cache;
    UseRegistry* registry = nullptr;
    bool observed = false;
    FilterKey key;
    FilterKeyScope scope_;
    std::shared_ptr<FilterEntry> entry;
    std::vector<std::shared_ptr<SegmentSlot>> slotsByOrd;
    // Pure preflight pins. They are not request-visible and carry no hit,
    // recency, admission, or ledger effects until acceptExisting commits
    // the complete candidate.
    std::vector<std::shared_ptr<const SegmentValue>> existingCandidates;
    std::shared_ptr<const ReaderValue> existingReaderCandidate;
    bool existingAccepted = false;
    // One allocation per Use; RequestSlot holds a mutex, so it is sized once
    // and never moved.
    std::unique_ptr<RequestSlot[]> requestSlots;
    size_t requestSlotCount;
    std::vector<SegmentIdentity> readerSegments;
    uint64_t readerCoreGen;
    uint64_t readerVersion;
    bool admitted;
    std::mutex readerStateMutex;
    std::shared_ptr<const ReaderValue> readerPin;
    bool readerPinRequestOwned = false;
    Accounting readerAccounting;
    const ReaderValue* readerServedValue = nullptr;
    RequestMemTracker* readerRoutedTracker = nullptr;
    size_t readerRoutedBytes = 0;
    std::string readerRoutedDetail;
    uint8_t observedAdmissionLanes = 0;
    uint8_t readerAdmissionRecordedLanes = 0;
    // One recency epoch for every hit this Use accepts.
    uint64_t epoch;

    Use(FilterCache* cache, FilterKey key, FilterKeyScope scope,
        std::shared_ptr<FilterEntry> entry,
        std::vector<std::shared_ptr<SegmentSlot>> slotsByOrd,
        std::vector<SegmentIdentity> readerSegments, uint64_t readerCoreGen,
        uint64_t readerVersion, bool admitted,
        uint8_t observedAdmissionLanes);

    friend class FilterCache;
    friend class Probe;
    friend class UseRegistry;

    // Pinning requires the corresponding request mutex.
    void pinValue(size_t segmentOrd,
                  std::shared_ptr<const SegmentValue> value,
                  bool requestOwned = false);
    void pinReaderValue(
        const std::shared_ptr<const ReaderValue>& value,
        bool requestOwned = false);
    void releaseRequestClaim(size_t segmentOrd);
    bool allowsSegmentPopulation(int32_t maxDoc) const;
    void ordinaryUse(size_t segmentOrd, bool populationAllowed);
    void ordinaryReaderUse(bool populationAllowed);
    void chargeLedger(Ledger& ledger, Accounting& accounting, std::mutex& mutex, Work work);
    void deferPopulation(size_t segmentOrd, Deferral reason = Deferral::FUNDING);
    void deferReaderPopulation(Deferral reason = Deferral::FUNDING);
    void settle(size_t segmentOrd, uint64_t nanos, std::chrono::steady_clock::time_point started);

  public:
    ~Use();
    void observe() { observed = true; }
    Work work(size_t segmentOrd) const;

    // Look only pins existing membership. Acceptance has the same hit and
    // request-local deduplication effects as probe(), including late hits.
    std::optional<SegmentCandidate> look(size_t segmentOrd);
    Probe accept(SegmentCandidate&& candidate, Work work = Work::assumed());

    // Known work is charged once before checking the population threshold.
    // Pending work is finalized afterward with charge() or credit().
    // BORROW_ONLY never charges or claims population, but still credits serves.
    Probe probe(size_t segmentOrd, Population population = Population::ALLOW) {
      return probe(segmentOrd, Work::assumed(), population);
    }
    Probe probe(size_t segmentOrd, Work work, Population population = Population::ALLOW);
    ReaderProbe probeReaderStable(
        IndexReader& reader, std::span<DocSet* const> domainPerSeg,
        Work work = Work::assumed());
    void charge(size_t segmentOrd, Work work);
    void credit(size_t segmentOrd, Work work);
    void chargeReaderStable(Work work);
    void creditReaderStable(Work work);

    // Shared-cache insertion and request-local adoption both accept raw filter
    // membership. Domain and liveDocs composition happen afterward through the
    // effectiveDocSet / effectiveDomain seam.
    std::shared_ptr<const SegmentValue> publishRaw(
        size_t segmentOrd, Probe& probe, std::unique_ptr<DocSet> raw,
        uint64_t buildCostNanos);
    std::shared_ptr<const SegmentValue> publishRawByproduct(
        size_t segmentOrd, Probe& probe, std::unique_ptr<DocSet> raw,
        uint64_t buildCostNanos);
    std::shared_ptr<const SegmentValue> offerRaw(
        size_t segmentOrd, std::unique_ptr<DocSet> raw,
        uint64_t buildCostNanos);
    std::shared_ptr<const ReaderValue> publishReaderStable(
        ReaderProbe& probe, std::vector<std::unique_ptr<DocSet>> liveExact,
        uint64_t buildCostNanos);

    DocSet* adoptOwnedRaw(size_t segmentOrd, Probe& probe,
                          std::unique_ptr<DocSet> raw);
    DocSet* rawDocSet(size_t segmentOrd);
    // The request-stable membership for a segment: the raw value, or raw AND
    // liveDocs when the segment has deletes. Memoized for the request; the
    // returned pointer stays valid for the life of this Use. Null when the
    // membership is unavailable (no raw value, or the segment changed).
    DocSet* effectiveDocSet(size_t segmentOrd, IndexReader& reader);
    // Membership composed with an incoming domain. A null domain, or the
    // segment's canonical live set, borrows the request-stable value above.
    // Any other domain yields a caller-owned intersection: nothing about that
    // domain is retained, so short-lived domains (a facet bucket's per-segment
    // DocSet) are safe here and cannot alias a later domain at the same
    // address. A null view means the membership is unavailable.
    DomainHandle effectiveDomain(size_t segmentOrd, IndexReader& reader,
                                 DocSet* domain);
    void enableRoutedAccounting(
        size_t segmentOrd, RequestMemTracker& tracker,
        std::string_view detail);
    bool wasAdmitted() const { return admitted; }
    bool hasAcceptedExisting() const { return existingAccepted; }
    FilterKeyScope scope() const { return scope_; }
    const void* entryIdentityForTest() const { return entry.get(); }
    size_t ownedBytesForTest();
  };

  // Opaque complete-residency preflight. It cannot participate in execution
  // or request-local dedup until UseRegistry::acceptExisting consumes it.
  class ExistingCandidate {
    std::unique_ptr<Use> use;

    explicit ExistingCandidate(std::unique_ptr<Use> use)
      : use(std::move(use)) {}

    friend class UseRegistry;

  public:
    ExistingCandidate(const ExistingCandidate&) = delete;
    ExistingCandidate& operator=(const ExistingCandidate&) = delete;
    ExistingCandidate(ExistingCandidate&&) noexcept = default;
    ExistingCandidate& operator=(ExistingCandidate&&) noexcept = default;
  };

  // Request-local full-key dedup and value ownership. It always exists; cache
  // may be null or disabled. Execution objects retain raw Use pointers only.
  class UseRegistry {
    FilterCache* cache;
    std::vector<SegmentIdentity> segments;
    uint64_t readerCoreGen;
    uint64_t readerVersion;
    std::unordered_map<FilterKey, std::unique_ptr<Use>, FilterKeyHash> uses;
    struct Timing {
      std::atomic<uint64_t> nanos{0};
      std::atomic<bool> produced{false};
    };
    struct Observation {
      std::unique_ptr<Timing[]> timing;
      std::shared_ptr<const void> snapshot;
      std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

      explicit Observation(size_t segments) : timing(std::make_unique<Timing[]>(segments)) {}
    };
    std::shared_ptr<Observation> observation;
    bool settled = false;
    static thread_local Observation* timedObservation;
    static thread_local size_t timedSegment;

    static void recordProduction();
    UseRegistry(FilterCache* cache, IndexReader& reader, const UseRegistry* related);

    friend class FilterCache;
    friend class Use;
    #ifndef NDEBUG
    std::thread::id owningThread;
    #endif

  public:
    class SegmentScope {
      UseRegistry& registry;
      size_t segmentOrd;
      Observation* previousObservation;
      size_t previousSegment;
      bool outermost;
      std::chrono::steady_clock::time_point started;

    public:
      SegmentScope(UseRegistry& registry, size_t segmentOrd);
      ~SegmentScope();
      SegmentScope(const SegmentScope&) = delete;
      SegmentScope& operator=(const SegmentScope&) = delete;
    };

    void addSegmentNanos(size_t segmentOrd, uint64_t nanos);
    void settle(bool completed) noexcept;

    UseRegistry(FilterCache* cache, uint64_t readerCoreGen,
                std::span<const SegmentIdentity> segments);
    UseRegistry(FilterCache* cache, uint64_t readerCoreGen,
                uint64_t readerVersion,
                std::span<const SegmentIdentity> segments);
    UseRegistry(FilterCache* cache, IndexReader& reader);
    UseRegistry(FilterCache* cache, IndexReader& reader, const UseRegistry& related);
    UseRegistry(FilterCache& cache, uint64_t readerCoreGen,
                std::span<const SegmentIdentity> segments);
    UseRegistry(FilterCache& cache, uint64_t readerCoreGen,
                uint64_t readerVersion,
                std::span<const SegmentIdentity> segments);
    UseRegistry(FilterCache& cache, IndexReader& reader);

    // Registration finishes during serial parse-time weight construction,
    // before segment execution can fork. The registry is intentionally
    // unsynchronized.
    Use* get(const FilterKey& key,
             FilterKeyScope scope = FilterKeyScope::SEGMENT_STABLE,
             AdmissionLane lane = AdmissionLane::CLAUSE);
    // lookupExisting is an inert, complete-residency preflight. A successful
    // candidate remains invisible to execution until acceptExisting commits
    // it and applies the ordinary shared-hit effects for each resident value.
    // The preflight is first-registration-only for a request key.
    std::optional<ExistingCandidate> lookupExisting(
        const FilterKey& key,
        FilterKeyScope scope = FilterKeyScope::SEGMENT_STABLE);
    // READER_STABLE acceptance has no domain argument. Its caller must have
    // established that this request is on the exact reader and canonical root
    // domain before lookup; effectiveDocSet rechecks before serving values.
    Use* acceptExisting(
        ExistingCandidate&& candidate, AdmissionLane lane,
        Work work = Work::assumed());
    void enableRoutedAccounting(
        size_t segmentOrd, RequestMemTracker& tracker,
        std::string_view detail);
    size_t size() const { return uses.size(); }
    size_t ownedBytesForTest();
  };

private:
  using SlotVector = std::vector<std::shared_ptr<SegmentSlot>>;

  // Balances and observations are nanoseconds. The owning slot/entry lock
  // serializes charges, grants, production and repayment.
  struct Ledger {
    double owed = 0;
    double work = 0;
    double recent = 0;
    uint64_t recentEpochs = 0;
    double creditSnapshot = 0;
    uint64_t refusedVersion = 0;
    double refusedWork = 0;
    std::atomic<double> debt{0};
    uint64_t referenceNanos = 0;
    size_t referenceBytes = 0;
    bool measured = false;
    bool observed = false;
    struct Mean {
      double nanos = 0;
      uint64_t count = 0;
      void add(double value) {
        nanos = count++ == 0 ? value : nanos + (value - nanos) / 8;
      }
    };
    Mean absent;
    Mean served;
    std::chrono::steady_clock::time_point completed;
    std::atomic_flag transition = ATOMIC_FLAG_INIT;

    void lock() {
      while (transition.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    }
    void unlock() { transition.clear(std::memory_order_release); }
    void production(uint64_t nanos, size_t bytes, bool investment);
    void repay(double nanos);
    void collectCredit(double total) {
      work += total - creditSnapshot;
      creditSnapshot = total;
    }
    // Work per epoch over the completed interval and what has followed it. A
    // key observed for less than one window is not extrapolated from that.
    double rate(uint64_t sinceRoll, uint64_t window) const {
      return (recent + work) / (double)std::max(window, recentEpochs + sinceRoll);
    }
    bool canPopulate(double productionNanos) const;
  };

  // active means this exact (seg_id, maxDoc) belongs to the latest published
  // reader. A successful value CAS/exchange from non-null to null is the single
  // event that subtracts its immutable charge from residentBytes, exactly once
  // across sweeping, reader purge, metadata purge, and clear.
  struct SegmentSlot {
    uint64_t segId;
    int32_t maxDoc;
    SpaceOrdinal space;
    std::atomic<bool> active{true};
    std::atomic<bool> building{false};
    std::atomic<std::shared_ptr<const SegmentValue>> value;
    Ledger ledger;

    SegmentSlot(uint64_t segId, int32_t maxDoc, SpaceOrdinal space)
      : segId(segId), maxDoc(maxDoc), space(space) {}

    void lock() { ledger.lock(); }
    void unlock() { ledger.unlock(); }
  };

  // resident means the outer map still owns this key's metadata; false is
  // permanent. The entry mutex serializes value publication, COW slot-table
  // changes, reader purge, and the resident transition authorizing metadata
  // erase. A non-resident entry's slots are all inactive, and no slot of it
  // can become active again.
  //
  // The slot table holds only slots for the latest published reader once a
  // publication completes: onReaderPublished prunes merged-away slots. Each
  // slot is charged SLOT_METADATA_BYTES to metadataBytes; slotsCharge is the
  // entry's share, changed under the mutex only while resident and released
  // with metadataCharge when the entry leaves the map.
  struct FilterEntry {
    const FilterKeyScope scope;
    std::atomic<std::shared_ptr<const SlotVector>> slots;
    std::atomic<std::shared_ptr<const ReaderValue>> readerValue;
    std::atomic<bool> readerBuilding{false};
    Ledger ledger;
    std::mutex mutex;
    std::atomic<uint64_t> lastUsed{0};
    const size_t metadataCharge;
    size_t slotsCharge = 0;
    bool resident = true;
    std::atomic<bool> readerValueEverPublished{false};

    FilterEntry(FilterKeyScope scope, size_t metadataCharge, uint64_t epoch)
      : scope(scope), slots(std::make_shared<const SlotVector>()),
        lastUsed(epoch),
        metadataCharge(metadataCharge) {}

    void lock() { ledger.lock(); }
    void unlock() { ledger.unlock(); }
  };

  struct ActiveSnapshot {
    uint64_t coreGen = 0;
    uint64_t readerVersion = 0;
    std::vector<SegmentIdentity> segments;
    size_t bundleCount = 0;
  };

  struct AtomicCounters {
    // Shared serves and ordinary uses, each once per Use and value unit.
    StripedCounter hits;
    StripedCounter misses;
    // Estimated nanoseconds saved by credited serves.
    StripedCounter estimatedSavedNanos;
    std::atomic<uint64_t> buildNanos{0};
    std::atomic<uint64_t> unpaidBuildNanos{0};
    std::atomic<uint64_t> admissions{0};
    std::atomic<uint64_t> buildAttempts{0};
    std::atomic<uint64_t> builds{0};
    std::atomic<uint64_t> byproductInserts{0};
    std::atomic<uint64_t> publishRejects{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> unrepaidEvictions{0};
    std::atomic<uint64_t> populationDeferred{0};
    std::atomic<uint64_t> capacityDeferred{0};
    StripedCounter assumedCharges;
    StripedCounter observedCharges;
    StripedCounter assumedCredits;
    StripedCounter observedCredits;
    std::atomic<uint64_t> purges{0};
    std::atomic<uint64_t> oversizedKeyBypasses{0};
    std::atomic<uint64_t> readerStableHits{0};
    std::atomic<uint64_t> readerStableRefreshes{0};
    std::atomic<uint64_t> readerStableRetires{0};
  };

  // Fingerprints are admission and eviction hints. Collisions can admit early
  // or change a rank, but hits always compare full key bytes.
  class AdmissionRing {
    std::vector<uint64_t> history;
    std::unordered_map<uint64_t, uint32_t> frequencies;
    std::mutex mutex;
    size_t cursor = 0;
    size_t filled = 0;
    uint32_t threshold;

  public:
    AdmissionRing(size_t size, uint32_t threshold);
    bool record(uint64_t fingerprint);
    void reset();
  };

  struct Resident {
    std::weak_ptr<FilterEntry> entry;
    std::weak_ptr<SegmentSlot> slot;
    std::weak_ptr<const SegmentValue> segment;
    std::weak_ptr<const ReaderValue> reader;
    double priority;
    uint64_t published;
    bool spokenFor = false;
    bool operator<(const Resident& other) const {
      return priority != other.priority ? priority < other.priority : published < other.published;
    }
  };

  // Immutable policy and explicit-or-zero overrides. config.maxBytes is only
  // the construction input; all runtime budget reads go through budgetBytes().
  const FilterCacheConfig config;
  std::atomic<size_t> budget;
  boost::unordered::concurrent_flat_map<
      FilterKey, std::shared_ptr<FilterEntry>, FilterKeyHash> entries;
  AdmissionRing clauseAdmission;
  AdmissionRing wholeAdmission;
  std::atomic<std::shared_ptr<const ActiveSnapshot>> activeSegments;
  std::atomic<size_t> residentBytes{0};
  std::atomic<size_t> metadataBytes{0};
  std::atomic<uint64_t> epoch{1};
  // Publication joins the list only after releasing its entry and ledger locks.
  std::mutex residentsMutex;
  std::vector<Resident> residents;
  bool residentsReady = false;
  std::atomic<uint64_t> refreshEpoch{0};
  std::atomic<uint64_t> residentsVersion{1};
  std::atomic<uint64_t> publishedCoreGen{0};
  std::atomic<uint64_t> publishedReaderVersion{0};
  std::atomic<uint64_t> readerPublications{0};
  std::atomic<bool> sweepRequested{false};
  std::atomic_flag sweeping = ATOMIC_FLAG_INIT;
  std::mutex publicationMutex;
  AtomicCounters counter;
  std::atomic<double> priorNanosPerDoc{-1};

  double productionNanos(const Ledger& ledger, int32_t maxDoc, bool observed) const;
  void recordProduction(uint64_t nanos, int32_t maxDoc);

  // Approximate value-object, shared_ptr control-block, and segment-slot share.
  // Metadata is added separately and amortized over resident values.
  static constexpr size_t VALUE_RESIDENCY_OVERHEAD_BYTES = 256;
  // Approximate per-slot metadata: the slot with its make_shared control block
  // plus the slot-vector element.
  static constexpr size_t SLOT_METADATA_BYTES = sizeof(SegmentSlot)
      + 2 * sizeof(std::shared_ptr<SegmentSlot>);

  uint64_t nextEpoch();
  size_t residentValueCount(const FilterEntry& entry) const;
  double effectiveValueBytes(const FilterEntry& entry, size_t payloadBytes,
                             size_t residentValueCount) const;
  bool isActive(const ActiveSnapshot& snapshot,
                const SegmentIdentity& identity) const;
  std::shared_ptr<FilterEntry> findEntry(const FilterKey& key);
  static std::shared_ptr<const SegmentValue> lookSegmentValue(
      const std::shared_ptr<SegmentSlot>& slot,
      const SegmentIdentity& identity);
  std::shared_ptr<FilterEntry> findOrCreateEntry(const FilterKey& key,
                                                 FilterKeyScope scope);
  bool eraseEntry(const FilterKey& key,
                  const std::shared_ptr<FilterEntry>& entry);
  std::vector<std::shared_ptr<SegmentSlot>> alignSlots(
      const std::shared_ptr<FilterEntry>& entry,
      std::span<const SegmentIdentity> readerSegments);
  std::unique_ptr<Use> beginUse(const FilterKey& key, FilterKeyScope scope,
                                uint64_t readerCoreGen, uint64_t readerVersion,
                                std::span<const SegmentIdentity> readerSegments,
                                AdmissionLane lane);
  std::unique_ptr<Use> lookupExisting(
      const FilterKey& key, FilterKeyScope scope,
      uint64_t readerCoreGen, uint64_t readerVersion,
      std::span<const SegmentIdentity> readerSegments);
  bool acceptExisting(Use& use, AdmissionLane lane, Work work);
  template<class Value>
  void creditValue(Ledger& ledger, const Value& value, Work work);
  void acceptSharedHit(
      Use& use, size_t segmentOrd,
      std::shared_ptr<const SegmentValue> value, Work work = Work::assumed());
  void acceptReaderSharedHit(
      Use& use, const std::shared_ptr<FilterEntry>& entry,
      const std::shared_ptr<const ReaderValue>& value, Work work = Work::assumed());
  void observeAdmissionLane(Use& use, AdmissionLane lane);
  AdmissionRing& admissionFor(AdmissionLane lane);
  void recordCapacityEviction(
      const std::shared_ptr<const SegmentValue>& value);
  void recordReaderCapacityEviction(
      const std::shared_ptr<const ReaderValue>& value);
  std::shared_ptr<const SegmentValue> publish(
      Use& use, size_t segmentOrd, Probe* probe, std::unique_ptr<DocSet> raw,
      bool byproduct, uint64_t buildCostNanos);
  std::shared_ptr<const ReaderValue> publishReaderValue(
      Use& use, ReaderProbe& probe,
      std::vector<std::unique_ptr<DocSet>> liveExact,
      uint64_t buildCostNanos);
  size_t budgetBytes() const { return budget.load(std::memory_order_relaxed); }
  size_t maxEntryBytes(size_t bytes) const;
  size_t maxEntryBytes() const { return maxEntryBytes(budgetBytes()); }
  size_t maxMetadataBytes(size_t bytes) const;
  size_t lowWatermarkMetadataBytes(size_t bytes) const;
  void detachEntry(const std::shared_ptr<FilterEntry>& entry);
  void refreshResidents();
  bool capacityAllows(const FilterEntry& entry, Ledger& ledger, size_t bytes, double production);
  void rememberResident(const std::shared_ptr<FilterEntry>& entry,
      const std::shared_ptr<SegmentSlot>& slot,
      const std::shared_ptr<const SegmentValue>& segment,
      const std::shared_ptr<const ReaderValue>& reader);
  template<class Value>
  std::shared_ptr<const Value> detachValue(
      std::atomic<std::shared_ptr<const Value>>& source, Ledger& ledger,
      std::shared_ptr<const Value> expected = {});
  void maybeSweep();
  void sweepMetadata();

public:
  explicit FilterCache(FilterCacheConfig config = {});

  bool enabled() const {
    return budgetBytes() != 0;
  }
  // The effective configuration, with derived budgets filled in.
  FilterCacheConfig configuration() const;
  // Changes the byte budget at runtime. Budgets the original config left at
  // zero (metadata watermark, maxEntryBytes, maxMetadataBytes) are rederived from the
  // new size; explicit ones are kept and clamped as at construction. A
  // shrink is swept down to the new budget, on this thread unless a
  // sweep is already running (that sweeper then loops with the new limits).
  // Zero disables the cache and clears its entries. An insertion racing clear
  // detaches itself if still disabled. Existing request pins remain valid.
  // Positive grow/repeat calls do not evict; an overlapping zero or earlier
  // sweep may still evict after a grow. Admission history and policy stay fixed.
  void resize(size_t maxBytes);
  bool onReaderPublished(uint64_t coreGen,
                         std::span<const SegmentIdentity> segments);
  bool onReaderPublished(uint64_t coreGen, uint64_t readerVersion,
                         std::span<const SegmentIdentity> segments);
  bool onReaderPublished(IndexReader& reader);
  void sweep();
  void clear();
  // Forgets both admission-lane sighting histories.
  void resetAdmission();
  void resetCounters();

  // Diagnostic snapshot, optionally including payload-free ledgers, largest
  // payload first, capped at
  // limit. keyText is the printable-byte extraction from the serialized key
  // (field names and terms survive; structure bytes do not). totalResident
  // receives the uncapped count of entries holding at least one value.
  struct ValueDump {
    uint64_t segId = 0;
    int32_t maxDoc = 0;
    SpaceOrdinal space = SpaceOrdinal::ROOT;
    uint64_t readerVersion = 0;
    bool resident = false;
    double owed = 0;
    double debt = 0;
    double creditedTotal = 0;
    double productionNanos = 0;
    std::string_view productionSource = "unknown";
    double absentNanos = 0;
    uint64_t absentCount = 0;
    double servedNanos = 0;
    uint64_t servedCount = 0;
    size_t referenceBytes = 0;
    double recent = 0;
    uint64_t recentEpochs = 0;
    double work = 0;
    double residencyBytes = 0;
    uint64_t published = 0;
    double rankedRate = 0;
    bool spokenFor = false;
  };
  struct EntryDump {
    uint64_t keyHash = 0;
    size_t keyBytes = 0;
    std::string keyText;
    FilterKeyScope scope = FilterKeyScope::SEGMENT_STABLE;
    bool readerValue = false;
    size_t bytes = 0;
    uint32_t segmentsResident = 0;
    uint64_t lastUsed = 0;
    uint64_t buildNanos = 0;
    std::vector<ValueDump> values;
  };
  std::vector<EntryDump> dump(size_t limit, size_t* totalResident, bool includeGhosts = false);

  uint64_t currentEpoch() const { return epoch.load(std::memory_order_relaxed); }
  uint64_t lastRefreshEpoch() const { return refreshEpoch.load(std::memory_order_relaxed); }
  CounterValues counters() const;
  size_t bytesUsed() const { return residentBytes.load(std::memory_order_relaxed); }
  size_t metadataBytesUsed() const {
    return metadataBytes.load(std::memory_order_relaxed);
  }
  size_t entryCountForTest() const { return entries.size(); }
  const void* entryIdentityForTest(const FilterKey& key) {
    return findEntry(key).get();
  }
  size_t slotCountForTest(const FilterKey& key);
  uint64_t readerPublicationsForTest() const {
    return readerPublications.load(std::memory_order_relaxed);
  }
  void validateForTest();
};

} // namespace luxir
