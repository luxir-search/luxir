#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/unordered/concurrent_flat_map.hpp>

#include "solux/search/DocSet.h"
#include "solux/search/FilterKey.h"

namespace solux {

class IndexReader;

struct FilterCacheConfig {
  size_t maxBytes = 64ULL * 1024 * 1024;
  size_t lowWatermarkBytes = 0;
  size_t maxEntryBytes = 0;
  #ifdef NDEBUG
  int32_t minSegmentDocs = 1000;
  #else
  // Debug suites should broadly exercise both the admitted and skipped paths.
  // Explicit unit tests keep the skip case covered; 1-2 doc segments still skip.
  int32_t minSegmentDocs = 3;
  #endif
  size_t admissionHistorySize = 4096;
  uint32_t admissionThreshold = 2;
  size_t maxMetadataEntries = 8192;
  size_t maxMetadataBytes = 0;
};

// SEGMENT_STABLE and CORE_STABLE keys name raw DocSet values, one per seg_id.
// live_gen is deliberately absent: delete flushes retain the entry, and each
// request folds liveness at use time; segments without deletes serve raw sets.
// READER_STABLE keys instead name one whole-reader, live-exact value tagged by
// commitTime and seg_id. They are admitted only at a canonical reader-live
// PrepareContext, published and evicted atomically as a unit, and never enter
// the raw per-segment APIs. The semantic map key contains no reader version.
// The owning IndexWriter bounds the cache lifetime so its seg_id namespace can
// never be reused underneath either value kind.
//
// Use performs the one outer-map lookup for a query/filter pair, snapshots the
// entry's COW slot vector, and aligns it to reader ordinals. Segment tasks then
// index that request-local vector without hashing. No map entry is allocated
// for a key until a bounded frequency ring has admitted it (second sighting by
// default); a massive one-off key costs one 8-byte ring cell. Oversized keys
// bypass before ring recording, and key metadata has its own maxMetadataBytes
// budget. One epoch sweeper trims segment payloads individually and
// ReaderValues as whole units from the high watermark to the low watermark.
//
// Query::Context and SearchRequest's request-destructed registry own all
// shared_ptr pins. MemPool-allocated weights and scorers retain raw pointers
// because their destructors never run.
class FilterCache {
  struct ActiveSnapshot;
  struct FilterEntry;
  struct SegmentSlot;

public:
  struct SegmentIdentity {
    uint64_t segId;
    int32_t maxDoc;

    friend bool operator==(const SegmentIdentity&, const SegmentIdentity&) = default;
  };

  struct CounterValues {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t admissions = 0;
    uint64_t builds = 0;
    uint64_t byproductInserts = 0;
    uint64_t publishRejects = 0;
    uint64_t evictions = 0;
    uint64_t purges = 0;
    uint64_t oversizedKeyBypasses = 0;
    uint64_t readerStableHits = 0;
    uint64_t readerStableRefreshes = 0;
    uint64_t readerStableRetires = 0;
    size_t residentBytes = 0;
    size_t metadataBytes = 0;
  };

  class SegmentValue {
    friend class FilterCache;

    std::unique_ptr<DocSet> docs;
    // DocSet payload charge only - key bytes are deliberately excluded
    // (entry-lifetime metadataBytes budget, not value lifetime).
    uint32_t charge;
    mutable std::atomic<uint64_t> lastUsed;
    #ifndef NDEBUG
    SegmentIdentity identityForTest;
    #endif

  public:
    SegmentValue(std::unique_ptr<DocSet> docs, uint64_t epoch,
                 SegmentIdentity identityForTest);

    DocSet* docSet() const { return docs.get(); }
    int32_t card() const {
      // Publication forces card() before visibility, making this immutable.
      assert(docs->cachedCard() >= 0);
      return docs->cachedCard();
    }
    size_t ramBytesUsed() const { return charge; }
    void touch(uint64_t epoch) const {
      lastUsed.store(epoch, std::memory_order_relaxed);
    }
    uint64_t usedEpoch() const {
      return lastUsed.load(std::memory_order_relaxed);
    }
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
    };

    uint64_t readerVersion_;
    std::vector<SegmentDocs> segments;
    size_t charge;
    mutable std::atomic<uint64_t> lastUsed;

    ReaderValue(uint64_t readerVersion,
                std::span<const SegmentIdentity> identities,
                std::vector<std::unique_ptr<DocSet>> docs, uint64_t epoch);

  public:
    uint64_t readerVersion() const { return readerVersion_; }
    size_t ramBytesUsed() const { return charge; }
    DocSet* docSet(size_t segmentOrd, const SegmentIdentity& identity) const;
    void touch(uint64_t epoch) const {
      lastUsed.store(epoch, std::memory_order_relaxed);
    }
    uint64_t usedEpoch() const {
      return lastUsed.load(std::memory_order_relaxed);
    }
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
    Kind kind_ = Kind::BYPASS;
    bool ownsClaim = false;

    Probe(Kind kind, std::shared_ptr<SegmentSlot> slot,
          std::shared_ptr<const SegmentValue> value, bool ownsClaim);
    void releaseClaim();

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
    DocSet* docSet() const { return value == nullptr ? nullptr : value->docSet(); }
    int32_t card() const { return value == nullptr ? 0 : value->card(); }
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

  class Use {
    struct RequestSlot {
      struct DomainEffective {
        DocSet* domain;
        std::unique_ptr<DocSet> docs;
      };

      std::mutex mutex;
      std::vector<std::shared_ptr<const SegmentValue>> pins;
      std::unique_ptr<DocSet> liveEffective;
      std::vector<DomainEffective> domainEffective;
    };

    FilterCache* cache;
    FilterKey key;
    FilterKeyScope scope_;
    std::shared_ptr<FilterEntry> entry;
    std::vector<std::shared_ptr<SegmentSlot>> slotsByOrd;
    std::vector<std::unique_ptr<RequestSlot>> requestSlots;
    std::vector<SegmentIdentity> readerSegments;
    uint64_t readerCoreGen;
    uint64_t readerVersion;
    bool admitted;
    std::mutex readerStateMutex;
    bool readerAdmissionConsidered = false;
    std::shared_ptr<const ReaderValue> readerPin;

    Use(FilterCache* cache, FilterKey key, FilterKeyScope scope,
        std::shared_ptr<FilterEntry> entry,
        std::vector<std::shared_ptr<SegmentSlot>> slotsByOrd,
        std::vector<SegmentIdentity> readerSegments, uint64_t readerCoreGen,
        uint64_t readerVersion, bool admitted);

    friend class FilterCache;
    friend class UseRegistry;

    void pinValue(size_t segmentOrd,
                  const std::shared_ptr<const SegmentValue>& value);
    void pinReaderValue(const std::shared_ptr<const ReaderValue>& value);

  public:
    Probe probe(size_t segmentOrd);
    ReaderProbe probeReaderStable(
        IndexReader& reader, std::span<DocSet* const> domainPerSeg);

    // The only insertion APIs accept raw filter membership. Domain and
    // liveDocs composition happen after publication through effectiveDocSet.
    std::shared_ptr<const SegmentValue> publishRaw(
        size_t segmentOrd, Probe& probe, std::unique_ptr<DocSet> raw);
    std::shared_ptr<const SegmentValue> publishRawByproduct(
        size_t segmentOrd, Probe& probe, std::unique_ptr<DocSet> raw);
    std::shared_ptr<const SegmentValue> offerRaw(
        size_t segmentOrd, std::unique_ptr<DocSet> raw);
    std::shared_ptr<const ReaderValue> publishReaderStable(
        ReaderProbe& probe, std::vector<std::unique_ptr<DocSet>> liveExact);

    DocSet* effectiveDocSet(size_t segmentOrd, IndexReader& reader,
                            const std::shared_ptr<const SegmentValue>& value,
                            DocSet* domain = nullptr);
    std::shared_ptr<const SegmentValue> pinnedValue(size_t segmentOrd) const;
    bool wasAdmitted() const { return admitted; }
    FilterKeyScope scope() const { return scope_; }
    const void* entryIdentityForTest() const { return entry.get(); }
  };

  // Request-local full-key dedup. It owns Use objects, while execution objects
  // retain raw Use pointers only.
  class UseRegistry {
    FilterCache* cache;
    std::vector<SegmentIdentity> segments;
    uint64_t readerCoreGen;
    uint64_t readerVersion;
    std::unordered_map<FilterKey, std::unique_ptr<Use>, FilterKeyHash> uses;
    #ifndef NDEBUG
    std::thread::id owningThread;
    #endif

  public:
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
             FilterKeyScope scope = FilterKeyScope::SEGMENT_STABLE);
    size_t size() const { return uses.size(); }
  };

private:
  using SlotVector = std::vector<std::shared_ptr<SegmentSlot>>;

  // active means this exact (seg_id, maxDoc) belongs to the latest published
  // reader. A successful value CAS/exchange from non-null to null is the single
  // event that subtracts its immutable charge from residentBytes, exactly once
  // across sweeping, reader purge, metadata purge, and clear.
  struct SegmentSlot {
    uint64_t segId;
    int32_t maxDoc;
    std::atomic<bool> active{true};
    std::atomic<bool> building{false};
    std::atomic<std::shared_ptr<const SegmentValue>> value;

    SegmentSlot(uint64_t segId, int32_t maxDoc)
      : segId(segId), maxDoc(maxDoc) {}
  };

  // resident means the outer map still owns this key's metadata; false is
  // permanent. The entry mutex serializes value publication, COW slot-table
  // changes, reader purge, and the resident transition authorizing metadata
  // erase.
  struct FilterEntry {
    const FilterKeyScope scope;
    std::atomic<std::shared_ptr<const SlotVector>> slots;
    std::atomic<std::shared_ptr<const ReaderValue>> readerValue;
    std::atomic<bool> readerBuilding{false};
    std::mutex mutex;
    std::atomic<uint64_t> lastUsed{0};
    const size_t metadataCharge;
    bool resident = true;
    std::atomic<bool> readerValueEverPublished{false};

    FilterEntry(FilterKeyScope scope, size_t metadataCharge, uint64_t epoch)
      : scope(scope), slots(std::make_shared<const SlotVector>()),
        lastUsed(epoch),
        metadataCharge(metadataCharge) {}
  };

  struct ActiveSnapshot {
    uint64_t coreGen = 0;
    uint64_t readerVersion = 0;
    std::vector<SegmentIdentity> segments;
  };

  struct AtomicCounters {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> admissions{0};
    std::atomic<uint64_t> builds{0};
    std::atomic<uint64_t> byproductInserts{0};
    std::atomic<uint64_t> publishRejects{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> purges{0};
    std::atomic<uint64_t> oversizedKeyBypasses{0};
    std::atomic<uint64_t> readerStableHits{0};
    std::atomic<uint64_t> readerStableRefreshes{0};
    std::atomic<uint64_t> readerStableRetires{0};
  };

  // Fingerprints are admission hints only. A collision can admit early, but
  // never causes a false hit because the data map compares full key bytes.
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
  };

  FilterCacheConfig config;
  boost::unordered::concurrent_flat_map<
      FilterKey, std::shared_ptr<FilterEntry>, FilterKeyHash> entries;
  AdmissionRing admission;
  std::atomic<std::shared_ptr<const ActiveSnapshot>> activeSegments;
  std::atomic<size_t> residentBytes{0};
  std::atomic<size_t> metadataBytes{0};
  std::atomic<uint64_t> epoch{1};
  std::atomic<uint64_t> publishedCoreGen{0};
  std::atomic<uint64_t> publishedReaderVersion{0};
  std::atomic<uint64_t> readerPublications{0};
  std::atomic<bool> sweepRequested{false};
  std::atomic_flag sweeping = ATOMIC_FLAG_INIT;
  std::mutex publicationMutex;
  AtomicCounters counter;

  uint64_t nextEpoch();
  bool isActive(const ActiveSnapshot& snapshot,
                const SegmentIdentity& identity) const;
  std::shared_ptr<FilterEntry> findEntry(const FilterKey& key);
  std::shared_ptr<FilterEntry> findOrCreateEntry(const FilterKey& key,
                                                 FilterKeyScope scope);
  bool eraseEntry(const FilterKey& key,
                  const std::shared_ptr<FilterEntry>& entry);
  std::vector<std::shared_ptr<SegmentSlot>> alignSlots(
      const std::shared_ptr<FilterEntry>& entry,
      std::span<const SegmentIdentity> readerSegments);
  std::unique_ptr<Use> beginUse(const FilterKey& key, FilterKeyScope scope,
                                uint64_t readerCoreGen, uint64_t readerVersion,
                                std::span<const SegmentIdentity> readerSegments);
  std::shared_ptr<const SegmentValue> publish(
      Use& use, size_t segmentOrd, Probe* probe, std::unique_ptr<DocSet> raw,
      bool byproduct);
  std::shared_ptr<const ReaderValue> publishReaderValue(
      Use& use, ReaderProbe& probe,
      std::vector<std::unique_ptr<DocSet>> liveExact);
  void maybeSweep();
  void sweepMetadata();

public:
  explicit FilterCache(FilterCacheConfig config = {});

  bool enabled() const { return config.maxBytes != 0; }
  FilterCacheConfig configuration() const { return config; }
  bool onReaderPublished(uint64_t coreGen,
                         std::span<const SegmentIdentity> segments);
  bool onReaderPublished(uint64_t coreGen, uint64_t readerVersion,
                         std::span<const SegmentIdentity> segments);
  bool onReaderPublished(IndexReader& reader);
  void sweep();
  void clear();

  CounterValues counters() const;
  size_t bytesUsed() const { return residentBytes.load(std::memory_order_relaxed); }
  size_t metadataBytesUsed() const {
    return metadataBytes.load(std::memory_order_relaxed);
  }
  size_t entryCountForTest() const { return entries.size(); }
  const void* entryIdentityForTest(const FilterKey& key) {
    return findEntry(key).get();
  }
  uint64_t readerPublicationsForTest() const {
    return readerPublications.load(std::memory_order_relaxed);
  }
  void validateForTest();
};

} // namespace solux
