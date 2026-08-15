#include "luxir/search/FilterCache.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "luxir/reader/SkipStats.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

namespace {

bool identityLess(const FilterCache::SegmentIdentity& a,
                  const FilterCache::SegmentIdentity& b) {
  return std::tie(a.segId, a.maxDoc) < std::tie(b.segId, b.maxDoc);
}

std::vector<FilterCache::SegmentIdentity> readerIdentities(IndexReader& reader) {
  std::vector<FilterCache::SegmentIdentity> identities;
  identities.reserve(reader.segments().size());
  for (const auto& segment : reader.segments()) {
    identities.push_back({segment.segInfo.seg_id, segment.maxDoc()});
  }
  return identities;
}

} // namespace

FilterCache::SegmentValue::SegmentValue(std::unique_ptr<DocSet> docs,
                                        uint64_t epoch,
                                        SegmentIdentity identityForTest,
                                        uint32_t buildCostMicros)
  : docs(std::move(docs)),
    charge((uint32_t) this->docs->ramBytesUsed()),
    buildCostMicros(buildCostMicros), lastUsed(epoch),
    priority(0)
    #ifndef NDEBUG
    , identityForTest(identityForTest)
    #endif
    {
  this->docs->card();
  unused(identityForTest);
}

FilterCache::ReaderValue::ReaderValue(
    uint64_t readerVersion, std::span<const SegmentIdentity> identities,
    std::vector<std::unique_ptr<DocSet>> docs, uint64_t epoch,
    uint32_t buildCostMicros)
  : readerVersion_(readerVersion), charge(0),
    buildCostMicros(buildCostMicros), lastUsed(epoch), priority(0) {
  if (identities.size() != docs.size()) {
    throw std::invalid_argument(
        "reader-stable DocSets must align with reader segments");
  }
  segments.reserve(docs.size());
  charge = sizeof(ReaderValue)
      + segments.capacity() * sizeof(ReaderValue::SegmentDocs);
  for (size_t i = 0; i < docs.size(); i++) {
    if (docs[i] == nullptr) {
      throw std::invalid_argument("reader-stable DocSet must not be null");
    }
    docs[i]->card();
    charge += docs[i]->ramBytesUsed();
    segments.push_back({identities[i].segId, identities[i].maxDoc,
                        std::move(docs[i])});
  }
}

DocSet* FilterCache::ReaderValue::docSet(
    size_t segmentOrd, const SegmentIdentity& identity) const {
  if (segmentOrd >= segments.size()) {
    throw std::logic_error(
        "reader-stable segment ordinal is outside the cached reader");
  }
  const auto& segment = segments[segmentOrd];
  if (segment.segId != identity.segId || segment.maxDoc != identity.maxDoc) {
    throw std::logic_error(
        "reader-stable segment identity does not match cached reader");
  }
  return segment.docs.get();
}

FilterCache::Probe::Probe(Kind kind, std::shared_ptr<SegmentSlot> slot,
                          std::shared_ptr<const SegmentValue> value,
                          bool ownsClaim, DocSet* requestValue,
                          Use* requestUse, size_t requestSegmentOrd,
                          bool ownsRequestClaim)
  : slot(std::move(slot)), value(std::move(value)),
    requestValue(requestValue), requestUse(requestUse),
    requestSegmentOrd(requestSegmentOrd), kind_(kind), ownsClaim(ownsClaim),
    ownsRequestClaim(ownsRequestClaim) {
}

void FilterCache::Probe::releaseClaims() {
  if (ownsClaim) {
    slot->building.store(false, std::memory_order_release);
    ownsClaim = false;
  }
  if (ownsRequestClaim) {
    requestUse->releaseRequestClaim(requestSegmentOrd);
    ownsRequestClaim = false;
  }
}

FilterCache::Probe::Probe(Probe&& other) noexcept
  : slot(std::move(other.slot)), value(std::move(other.value)),
    requestValue(other.requestValue), requestUse(other.requestUse),
    requestSegmentOrd(other.requestSegmentOrd), kind_(other.kind_),
    ownsClaim(other.ownsClaim), ownsRequestClaim(other.ownsRequestClaim) {
  other.kind_ = Kind::BYPASS;
  other.requestValue = nullptr;
  other.requestUse = nullptr;
  other.ownsClaim = false;
  other.ownsRequestClaim = false;
}

FilterCache::Probe& FilterCache::Probe::operator=(Probe&& other) noexcept {
  if (this == &other) return *this;
  releaseClaims();
  slot = std::move(other.slot);
  value = std::move(other.value);
  requestValue = other.requestValue;
  requestUse = other.requestUse;
  requestSegmentOrd = other.requestSegmentOrd;
  kind_ = other.kind_;
  ownsClaim = other.ownsClaim;
  ownsRequestClaim = other.ownsRequestClaim;
  other.kind_ = Kind::BYPASS;
  other.requestValue = nullptr;
  other.requestUse = nullptr;
  other.ownsClaim = false;
  other.ownsRequestClaim = false;
  return *this;
}

FilterCache::Probe::~Probe() {
  releaseClaims();
}

FilterCache::ReaderProbe::ReaderProbe(
    Kind kind, std::shared_ptr<FilterEntry> entry,
    std::shared_ptr<const ReaderValue> value, bool ownsClaim, bool refresh)
  : entry(std::move(entry)), value(std::move(value)), kind_(kind),
    ownsClaim(ownsClaim), refresh(refresh) {
}

void FilterCache::ReaderProbe::releaseClaim() {
  if (ownsClaim) {
    entry->readerBuilding.store(false, std::memory_order_release);
    ownsClaim = false;
  }
}

FilterCache::ReaderProbe::ReaderProbe(ReaderProbe&& other) noexcept
  : entry(std::move(other.entry)), value(std::move(other.value)),
    kind_(other.kind_), ownsClaim(other.ownsClaim), refresh(other.refresh) {
  other.kind_ = Kind::BYPASS;
  other.ownsClaim = false;
  other.refresh = false;
}

FilterCache::ReaderProbe& FilterCache::ReaderProbe::operator=(
    ReaderProbe&& other) noexcept {
  if (this == &other) return *this;
  releaseClaim();
  entry = std::move(other.entry);
  value = std::move(other.value);
  kind_ = other.kind_;
  ownsClaim = other.ownsClaim;
  refresh = other.refresh;
  other.kind_ = Kind::BYPASS;
  other.ownsClaim = false;
  other.refresh = false;
  return *this;
}

FilterCache::ReaderProbe::~ReaderProbe() {
  releaseClaim();
}

FilterCache::Use::Use(FilterCache* cache, FilterKey key,
                      FilterKeyScope scope,
                      std::shared_ptr<FilterEntry> entry,
                      std::vector<std::shared_ptr<SegmentSlot>> slotsByOrd,
                      std::vector<SegmentIdentity> readerSegments,
                      uint64_t readerCoreGen, uint64_t readerVersion,
                      bool admitted)
  : cache(cache), key(std::move(key)), scope_(scope), entry(std::move(entry)),
    slotsByOrd(std::move(slotsByOrd)),
    readerSegments(std::move(readerSegments)), readerCoreGen(readerCoreGen),
    readerVersion(readerVersion), admitted(admitted) {
  requestSlots.reserve(this->readerSegments.size());
  for (size_t i = 0; i < this->readerSegments.size(); i++) {
    requestSlots.push_back(std::make_unique<RequestSlot>());
  }
}

void FilterCache::Use::pinValue(
    size_t segmentOrd, const std::shared_ptr<const SegmentValue>& value) {
  if (value == nullptr || segmentOrd >= requestSlots.size()) return;
  auto& requestSlot = *requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(requestSlot.mutex);
  if (requestSlot.raw == nullptr) {
    requestSlot.raw = value->docSet();
    requestSlot.rawOwned = false;
  }
  if (requestSlot.pins.empty() || requestSlot.pins.back() != value) {
    requestSlot.pins.push_back(value);
  }
}

void FilterCache::Use::releaseRequestClaim(size_t segmentOrd) {
  if (segmentOrd >= requestSlots.size()) return;
  auto& requestSlot = *requestSlots[segmentOrd];
  {
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    assert(requestSlot.resolving);
    requestSlot.resolving = false;
  }
  requestSlot.condition.notify_all();
}

void FilterCache::Use::pinReaderValue(
    const std::shared_ptr<const ReaderValue>& value) {
  if (value == nullptr) return;
  std::lock_guard<std::mutex> lock(readerStateMutex);
  if (readerPin != nullptr && readerPin != value) {
    throw std::logic_error(
        "one reader-stable Use cannot borrow multiple ReaderValues");
  }
  readerPin = value;
}

FilterCache::Probe FilterCache::Use::probe(size_t segmentOrd) {
  if (scope_ == FilterKeyScope::READER_STABLE
      || segmentOrd >= readerSegments.size()
      || segmentOrd >= requestSlots.size()) {
    return {};
  }

  auto& requestSlot = *requestSlots[segmentOrd];
  {
    std::unique_lock<std::mutex> lock(requestSlot.mutex);
    requestSlot.condition.wait(lock, [&] {
      return !requestSlot.resolving || requestSlot.raw != nullptr;
    });
    if (requestSlot.raw != nullptr) {
      DocSet* raw = requestSlot.raw;
      bool owned = requestSlot.rawOwned;
      lock.unlock();
      if (owned) skipCount(SkipStats::ownedFilterServes);
      return Probe(Probe::Kind::HIT, nullptr, nullptr, false, raw);
    }
    requestSlot.resolving = true;
  }

  auto bypass = [&] {
    return Probe(Probe::Kind::BYPASS, nullptr, nullptr, false, nullptr,
                 this, segmentOrd, true);
  };

  if (cache == nullptr || !cache->enabled()) {
    return bypass();
  }

  if (entry == nullptr || segmentOrd >= slotsByOrd.size()
      || slotsByOrd[segmentOrd] == nullptr) {
    cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
    return bypass();
  }

  auto slot = slotsByOrd[segmentOrd];
  if (!slot->active.load(std::memory_order_acquire)
      || slot->segId != readerSegments[segmentOrd].segId
      || slot->maxDoc != readerSegments[segmentOrd].maxDoc) {
    cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
    return bypass();
  }

  auto value = slot->value.load(std::memory_order_acquire);
  if (value != nullptr) {
    uint64_t now = cache->nextEpoch();
    value->touch(now, cache->evictionClock.load(std::memory_order_relaxed));
    entry->lastUsed.store(now, std::memory_order_relaxed);
    pinValue(segmentOrd, value);
    releaseRequestClaim(segmentOrd);
    cache->counter.hits.fetch_add(1, std::memory_order_relaxed);
    DocSet* raw = value->docSet();
    return Probe(Probe::Kind::HIT, std::move(slot), std::move(value), false,
                 raw);
  }

  cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
  if (!admitted || slot->maxDoc < cache->config.minSegmentDocs
      || readerCoreGen < cache->publishedCoreGen.load(std::memory_order_acquire)) {
    return bypass();
  }

  bool expected = false;
  if (!slot->building.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return bypass();
  }
  uint64_t now = cache->nextEpoch();
  entry->lastUsed.store(now, std::memory_order_relaxed);
  return Probe(Probe::Kind::BUILD, std::move(slot), nullptr, true, nullptr,
               this, segmentOrd, true);
}

FilterCache::ReaderProbe FilterCache::Use::probeReaderStable(
    IndexReader& reader, std::span<DocSet* const> domainPerSeg) {
  if (cache == nullptr || !cache->enabled()
      || scope_ != FilterKeyScope::READER_STABLE
      || reader.commitTime() != readerVersion
      || reader.segments().size() != readerSegments.size()) {
    return {};
  }
  if (key.bytes().size() > cache->config.maxEntryBytes) {
    cache->counter.oversizedKeyBypasses.fetch_add(
        1, std::memory_order_relaxed);
    return {};
  }

  // The cache value is live-exact. Only the root reader-live domain produces
  // that value: no deletes is represented by nullptr, while a deleted segment
  // must carry the exact LiveDocs DocSet object. Gate before admission so a
  // nested facet/Fusion domain cannot manufacture a ring sighting.
  bool emptyMeansCanonical = domainPerSeg.empty();
  if (!emptyMeansCanonical && domainPerSeg.size() != reader.segments().size()) {
    return {};
  }
  for (size_t i = 0; i < reader.segments().size(); i++) {
    auto& segment = reader.segments()[i];
    DocSet* canonical = segment.liveDocs() == nullptr
        ? nullptr : &segment.liveDocs()->docset();
    DocSet* actual = emptyMeansCanonical ? nullptr : domainPerSeg[i];
    if (actual != canonical) return {};
  }

  std::shared_ptr<FilterEntry> localEntry;
  bool staleAfterCreate = false;
  {
    std::lock_guard<std::mutex> lock(readerStateMutex);
    if (entry == nullptr) {
      entry = cache->findEntry(key);
      if (entry == nullptr) {
        if (readerAdmissionConsidered) {
          cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
          return {};
        }
        readerAdmissionConsidered = true;
        admitted = cache->admission.record(key.hash());
        if (admitted) {
          entry = cache->findOrCreateEntry(key, scope_);
          // A publication can advance after its entry snapshot but before this
          // reader-lane insertion. Do not let that new entry claim old-reader
          // work; publish-time validation remains the final backstop.
          staleAfterCreate = readerVersion
              != cache->publishedReaderVersion.load(
                  std::memory_order_acquire);
        }
      } else {
        // Entry existence is proof of prior admission. Refreshes deliberately
        // do not consult the ring again.
        admitted = true;
      }
    }
    localEntry = entry;
  }
  if (localEntry == nullptr || localEntry->scope != scope_) {
    cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
    return {};
  }
  if (staleAfterCreate) {
    cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
    return {};
  }

  auto value = localEntry->readerValue.load(std::memory_order_acquire);
  if (value != nullptr && value->readerVersion() == readerVersion) {
    uint64_t now = cache->nextEpoch();
    value->touch(now, cache->evictionClock.load(std::memory_order_relaxed));
    localEntry->lastUsed.store(now, std::memory_order_relaxed);
    cache->counter.hits.fetch_add(1, std::memory_order_relaxed);
    cache->counter.readerStableHits.fetch_add(1, std::memory_order_relaxed);
    // Pin at the cache boundary: consumers may drop the wrapper that holds
    // the only other reference (a prepared-source local) while borrowed
    // DocSet pointers are still live in the request. The Use outlives them.
    pinReaderValue(value);
    return ReaderProbe(ReaderProbe::Kind::HIT, std::move(localEntry),
                       std::move(value), false, false);
  }

  cache->counter.misses.fetch_add(1, std::memory_order_relaxed);
  if (readerVersion
      != cache->publishedReaderVersion.load(std::memory_order_acquire)) {
    return {};
  }
  bool expected = false;
  if (!localEntry->readerBuilding.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return {};
  }
  uint64_t now = cache->nextEpoch();
  localEntry->lastUsed.store(now, std::memory_order_relaxed);
  bool refresh = localEntry->readerValueEverPublished.load(
      std::memory_order_relaxed);
  return ReaderProbe(ReaderProbe::Kind::BUILD, std::move(localEntry),
                     nullptr, true, refresh);
}

std::shared_ptr<const FilterCache::SegmentValue>
FilterCache::Use::publishRaw(size_t segmentOrd, Probe& probe,
                             std::unique_ptr<DocSet> raw,
                             uint32_t buildCostMicros) {
  return cache->publish(*this, segmentOrd, &probe, std::move(raw), false,
                        buildCostMicros);
}

DocSet* FilterCache::Use::adoptOwnedRaw(
    size_t segmentOrd, Probe& probe, std::unique_ptr<DocSet> raw) {
  if (raw == nullptr) {
    throw std::invalid_argument("owned raw DocSet must not be null");
  }
  if (scope_ == FilterKeyScope::READER_STABLE) {
    throw std::logic_error(
        "reader-stable live-exact membership cannot use raw ownership");
  }
  if (segmentOrd >= requestSlots.size() || probe.requestUse != this
      || probe.requestSegmentOrd != segmentOrd || !probe.ownsRequestClaim) {
    throw std::logic_error("owned raw adoption requires the request claim");
  }

  raw->card();
  auto& requestSlot = *requestSlots[segmentOrd];
  bool inserted = false;
  bool owned = false;
  DocSet* result;
  {
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    if (requestSlot.raw == nullptr) {
      requestSlot.ownedRaw = std::move(raw);
      requestSlot.raw = requestSlot.ownedRaw.get();
      requestSlot.rawOwned = true;
      inserted = true;
    }
    result = requestSlot.raw;
    owned = requestSlot.rawOwned;
  }
  if (inserted) skipCount(SkipStats::ownedFilterMaterializations);
  if (owned) skipCount(SkipStats::ownedFilterServes);
  probe.releaseClaims();
  return result;
}

std::shared_ptr<const FilterCache::SegmentValue>
FilterCache::Use::publishRawByproduct(size_t segmentOrd, Probe& probe,
                                      std::unique_ptr<DocSet> raw,
                                      uint32_t buildCostMicros) {
  return cache->publish(*this, segmentOrd, &probe, std::move(raw), true,
                        buildCostMicros);
}

std::shared_ptr<const FilterCache::SegmentValue>
FilterCache::Use::offerRaw(size_t segmentOrd, std::unique_ptr<DocSet> raw,
                           uint32_t buildCostMicros) {
  return cache->publish(*this, segmentOrd, nullptr, std::move(raw), true,
                        buildCostMicros);
}

std::shared_ptr<const FilterCache::ReaderValue>
FilterCache::Use::publishReaderStable(
    ReaderProbe& probe, std::vector<std::unique_ptr<DocSet>> liveExact,
    uint32_t buildCostMicros) {
  auto value = cache->publishReaderValue(
      *this, probe, std::move(liveExact), buildCostMicros);
  pinReaderValue(value);
  return value;
}

DocSet* FilterCache::Use::rawDocSet(size_t segmentOrd) {
  if (segmentOrd >= requestSlots.size()) return nullptr;
  auto& requestSlot = *requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(requestSlot.mutex);
  return requestSlot.raw;
}

DocSet* FilterCache::Use::effectiveDocSet(
    size_t segmentOrd, IndexReader& reader, DocSet* domain) {
  // PrepareContext domains already carry liveness, so that path is raw AND
  // domain. Without a domain this is raw AND liveDocs. Any composed set is
  // memoized only in this request's Use and is never published to the cache.
  if (segmentOrd >= readerSegments.size()
      || segmentOrd >= reader.segments().size()) {
    return nullptr;
  }
  auto& segment = reader.segments()[segmentOrd];
  if (segment.segInfo.seg_id != readerSegments[segmentOrd].segId
      || segment.maxDoc() != readerSegments[segmentOrd].maxDoc) {
    return nullptr;
  }
  auto& requestSlot = *requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(requestSlot.mutex);
  DocSet* raw = requestSlot.raw;
  if (raw == nullptr) return nullptr;
  if (domain != nullptr) {
    for (auto& effective : requestSlot.domainEffective) {
      if (effective.domain == domain) return effective.docs.get();
    }
    std::array<DocSet*, 2> sets{raw, domain};
    auto docs = DocSet::intersect(sets);
    docs->card();
    DocSet* result = docs.get();
    requestSlot.domainEffective.push_back({domain, std::move(docs)});
    return result;
  }

  if (segment.liveDocs() == nullptr) return raw;
  if (requestSlot.liveEffective != nullptr) {
    return requestSlot.liveEffective.get();
  }
  std::array<DocSet*, 2> sets{
      raw, &segment.liveDocs()->docset()};
  requestSlot.liveEffective = DocSet::intersect(sets);
  requestSlot.liveEffective->card();
  return requestSlot.liveEffective.get();
}

size_t FilterCache::Use::ownedBytesForTest() {
  size_t result = 0;
  for (auto& requestSlotPtr : requestSlots) {
    auto& requestSlot = *requestSlotPtr;
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    if (requestSlot.ownedRaw != nullptr) {
      result += requestSlot.ownedRaw->ramBytesUsed();
    }
  }
  return result;
}

FilterCache::UseRegistry::UseRegistry(
    FilterCache* cache, uint64_t readerCoreGen,
    std::span<const SegmentIdentity> segments)
  : UseRegistry(cache, readerCoreGen, readerCoreGen, segments) {
}

FilterCache::UseRegistry::UseRegistry(
    FilterCache* cache, uint64_t readerCoreGen, uint64_t readerVersion,
    std::span<const SegmentIdentity> segments)
  : cache(cache), segments(segments.begin(), segments.end()),
    readerCoreGen(readerCoreGen), readerVersion(readerVersion)
    #ifndef NDEBUG
    , owningThread(std::this_thread::get_id())
    #endif
    {
}

FilterCache::UseRegistry::UseRegistry(FilterCache* cache, IndexReader& reader)
  : UseRegistry(cache, reader.coreGen(), reader.commitTime(),
                readerIdentities(reader)) {
}

FilterCache::UseRegistry::UseRegistry(
    FilterCache& cache, uint64_t readerCoreGen,
    std::span<const SegmentIdentity> segments)
  : UseRegistry(&cache, readerCoreGen, readerCoreGen, segments) {
}

FilterCache::UseRegistry::UseRegistry(
    FilterCache& cache, uint64_t readerCoreGen, uint64_t readerVersion,
    std::span<const SegmentIdentity> segments)
  : UseRegistry(&cache, readerCoreGen, readerVersion, segments) {}

FilterCache::UseRegistry::UseRegistry(FilterCache& cache, IndexReader& reader)
  : UseRegistry(&cache, reader.coreGen(), reader.commitTime(),
                readerIdentities(reader)) {
}

FilterCache::Use* FilterCache::UseRegistry::get(const FilterKey& key,
                                                FilterKeyScope scope) {
  #ifndef NDEBUG
  assert(owningThread == std::this_thread::get_id());
  #endif
  auto iter = uses.find(key);
  if (iter != uses.end()) return iter->second.get();
  auto use = cache != nullptr
      ? cache->beginUse(key, scope, readerCoreGen, readerVersion, segments)
      : std::unique_ptr<Use>(new Use(
          nullptr, key, scope, nullptr,
          std::vector<std::shared_ptr<SegmentSlot>>(segments.size()),
          segments, readerCoreGen, readerVersion, false));
  Use* result = use.get();
  uses.emplace(key, std::move(use));
  return result;
}

size_t FilterCache::UseRegistry::ownedBytesForTest() {
  size_t result = 0;
  for (auto& [key, use] : uses) {
    unused(key);
    result += use->ownedBytesForTest();
  }
  return result;
}

FilterCache::AdmissionRing::AdmissionRing(size_t size, uint32_t threshold)
  : history(size), threshold(std::max<uint32_t>(1, threshold)) {
}

bool FilterCache::AdmissionRing::record(uint64_t fingerprint) {
  std::lock_guard<std::mutex> lock(mutex);
  uint32_t before = 0;
  auto found = frequencies.find(fingerprint);
  if (found != frequencies.end()) before = found->second;
  if (history.empty()) return before + 1 >= threshold;

  if (filled == history.size()) {
    uint64_t old = history[cursor];
    auto oldIter = frequencies.find(old);
    if (oldIter != frequencies.end()) {
      if (--oldIter->second == 0) frequencies.erase(oldIter);
    }
  } else {
    filled++;
  }
  history[cursor] = fingerprint;
  cursor = (cursor + 1) % history.size();
  frequencies[fingerprint]++;
  return before + 1 >= threshold;
}

FilterCache::FilterCache(FilterCacheConfig config)
  : config(config), admission(config.admissionHistorySize,
                              config.admissionThreshold) {
  if (this->config.lowWatermarkBytes == 0) {
    this->config.lowWatermarkBytes = this->config.maxBytes * 9 / 10;
  }
  this->config.lowWatermarkBytes = std::min(
      this->config.lowWatermarkBytes, this->config.maxBytes);
  if (this->config.maxEntryBytes == 0 && this->config.maxBytes != 0) {
    this->config.maxEntryBytes = std::max<size_t>(1,
        this->config.maxBytes / 5);
  }
  if (this->config.maxMetadataBytes == 0 && this->config.maxBytes != 0) {
    this->config.maxMetadataBytes = std::max<size_t>(1,
        this->config.maxBytes / 4);
  }
  this->config.minSegmentDocs = std::max(0, this->config.minSegmentDocs);
  activeSegments.store(std::make_shared<const ActiveSnapshot>(),
                       std::memory_order_release);
}

uint64_t FilterCache::nextEpoch() {
  return epoch.fetch_add(1, std::memory_order_relaxed);
}

size_t FilterCache::residentValueCount(const FilterEntry& entry) const {
  size_t count = entry.readerValue.load(std::memory_order_acquire) == nullptr
      ? 0 : 1;
  auto slots = entry.slots.load(std::memory_order_acquire);
  for (const auto& slot : *slots) {
    if (slot->value.load(std::memory_order_acquire) != nullptr) count++;
  }
  return count;
}

double FilterCache::effectiveValueBytes(
    const FilterEntry& entry, size_t payloadBytes,
    size_t residentValueCount) const {
  assert(residentValueCount != 0);
  // metadataCharge is the key bytes plus FilterKey/FilterEntry map overhead.
  // Freeze its current per-resident-value share into the immutable density.
  return (double) payloadBytes
      + (double) VALUE_RESIDENCY_OVERHEAD_BYTES
      + (double) entry.metadataCharge / (double) residentValueCount;
}

void FilterCache::advanceEvictionClock(double priority) {
  double clock = evictionClock.load(std::memory_order_relaxed);
  evictionClock.store(std::max(clock, priority), std::memory_order_relaxed);
}

bool FilterCache::isActive(const ActiveSnapshot& snapshot,
                           const SegmentIdentity& identity) const {
  auto iter = std::lower_bound(snapshot.segments.begin(), snapshot.segments.end(),
                               identity, identityLess);
  return iter != snapshot.segments.end() && *iter == identity;
}

std::shared_ptr<FilterCache::FilterEntry>
FilterCache::findEntry(const FilterKey& key) {
  std::shared_ptr<FilterEntry> result;
  entries.cvisit(key, [&](const auto& item) {
    result = item.second;
  });
  return result;
}

std::shared_ptr<FilterCache::FilterEntry>
FilterCache::findOrCreateEntry(const FilterKey& key, FilterKeyScope scope) {
  for (;;) {
    auto existing = findEntry(key);
    if (existing != nullptr) {
      std::lock_guard<std::mutex> lock(existing->mutex);
      if (existing->resident) {
        if (existing->scope != scope) {
          throw std::logic_error("FilterCache key reused with different scope");
        }
        return existing;
      }
      eraseEntry(key, existing);
      continue;
    }

    size_t metadataCharge = key.bytes().capacity() + sizeof(FilterEntry)
        + sizeof(FilterKey) + sizeof(std::shared_ptr<FilterEntry>);
    auto candidate = std::make_shared<FilterEntry>(scope, metadataCharge,
                                                   nextEpoch());
    std::shared_ptr<FilterEntry> result = candidate;
    // Charge before the map publishes the entry. A racing erase can therefore
    // never subtract an entry that has not yet been charged.
    metadataBytes.fetch_add(metadataCharge, std::memory_order_relaxed);
    bool inserted = entries.try_emplace_or_cvisit(
        key, candidate, [&](const auto& item) { result = item.second; });
    if (inserted) {
      counter.admissions.fetch_add(1, std::memory_order_relaxed);
      return result;
    }
    metadataBytes.fetch_sub(metadataCharge, std::memory_order_relaxed);
  }
}

bool FilterCache::eraseEntry(
    const FilterKey& key, const std::shared_ptr<FilterEntry>& entry) {
  size_t erased = entries.erase_if(key, [&](const auto& item) {
    return item.second == entry;
  });
  if (erased != 0) {
    metadataBytes.fetch_sub(entry->metadataCharge, std::memory_order_relaxed);
    return true;
  }
  return false;
}

std::vector<std::shared_ptr<FilterCache::SegmentSlot>>
FilterCache::alignSlots(const std::shared_ptr<FilterEntry>& entry,
                        std::span<const SegmentIdentity> readerSegments) {
  std::lock_guard<std::mutex> lock(entry->mutex);
  std::vector<std::shared_ptr<SegmentSlot>> aligned(readerSegments.size());
  if (!entry->resident) return aligned;

  auto active = activeSegments.load(std::memory_order_acquire);
  auto current = entry->slots.load(std::memory_order_acquire);
  auto updated = std::make_shared<SlotVector>(*current);
  bool changed = false;
  for (const auto& identity : readerSegments) {
    auto found = std::lower_bound(
        updated->begin(), updated->end(), identity,
        [](const std::shared_ptr<SegmentSlot>& slot,
           const SegmentIdentity& target) {
          return std::tie(slot->segId, slot->maxDoc)
              < std::tie(target.segId, target.maxDoc);
        });
    bool exact = found != updated->end()
        && (*found)->segId == identity.segId
        && (*found)->maxDoc == identity.maxDoc;
    if (!exact && isActive(*active, identity)) {
      updated->insert(found,
                      std::make_shared<SegmentSlot>(identity.segId,
                                                    identity.maxDoc));
      changed = true;
    }
  }
  if (changed) {
    entry->slots.store(updated, std::memory_order_release);
    current = updated;
  }

  for (size_t i = 0; i < readerSegments.size(); i++) {
    const auto& identity = readerSegments[i];
    auto found = std::lower_bound(
        current->begin(), current->end(), identity,
        [](const std::shared_ptr<SegmentSlot>& slot,
           const SegmentIdentity& target) {
          return std::tie(slot->segId, slot->maxDoc)
              < std::tie(target.segId, target.maxDoc);
        });
    if (found != current->end() && (*found)->segId == identity.segId
        && (*found)->maxDoc == identity.maxDoc
        && (*found)->active.load(std::memory_order_acquire)) {
      aligned[i] = *found;
    }
  }
  entry->lastUsed.store(nextEpoch(), std::memory_order_relaxed);
  return aligned;
}

std::unique_ptr<FilterCache::Use> FilterCache::beginUse(
    const FilterKey& key, FilterKeyScope scope, uint64_t readerCoreGen,
    uint64_t readerVersion,
    std::span<const SegmentIdentity> readerSegments) {
  // Keep oversized rejection and the admission ring ahead of allocation: a
  // key gets FilterEntry metadata only after admission, never on first sight
  // under the default threshold.
  if (enabled() && scope != FilterKeyScope::READER_STABLE
      && key.bytes().size() > config.maxEntryBytes) {
    counter.oversizedKeyBypasses.fetch_add(1, std::memory_order_relaxed);
    return std::unique_ptr<Use>(new Use(
        this, key, scope, nullptr,
        std::vector<std::shared_ptr<SegmentSlot>>(readerSegments.size()),
        std::vector<SegmentIdentity>(readerSegments.begin(),
                                     readerSegments.end()),
        readerCoreGen, readerVersion, false));
  }
  // Reader-stable admission is delayed until the canonical-domain prepare
  // seam. Nested domains must neither serve values nor record ring sightings.
  bool readerStable = scope == FilterKeyScope::READER_STABLE;
  bool admitted = !readerStable && enabled() && admission.record(key.hash());
  auto entry = !readerStable && enabled() ? findEntry(key) : nullptr;
  if (entry == nullptr && admitted) entry = findOrCreateEntry(key, scope);
  auto slots = readerStable || entry == nullptr
      ? std::vector<std::shared_ptr<SegmentSlot>>(readerSegments.size())
      : alignSlots(entry, readerSegments);
  auto use = std::unique_ptr<Use>(new Use(
      this, key, scope, std::move(entry), std::move(slots),
      std::vector<SegmentIdentity>(readerSegments.begin(), readerSegments.end()),
      readerCoreGen, readerVersion, admitted));
  maybeSweep();
  return use;
}

std::shared_ptr<const FilterCache::SegmentValue> FilterCache::publish(
    Use& use, size_t segmentOrd, Probe* probe, std::unique_ptr<DocSet> raw,
    bool byproduct, uint32_t buildCostMicros) {
  if (use.scope_ == FilterKeyScope::READER_STABLE) {
    counter.publishRejects.fetch_add(1, std::memory_order_relaxed);
    throw std::logic_error(
        "reader-stable live-exact membership cannot enter raw cache APIs");
  }
  if (raw == nullptr) throw std::invalid_argument("raw DocSet must not be null");
  SegmentIdentity identity = segmentOrd < use.readerSegments.size()
      ? use.readerSegments[segmentOrd] : SegmentIdentity{};
  auto local = std::make_shared<SegmentValue>(
      std::move(raw), nextEpoch(), identity, buildCostMicros);

  bool attempted = enabled() && use.entry != nullptr
      && segmentOrd < use.slotsByOrd.size()
      && use.slotsByOrd[segmentOrd] != nullptr;
  bool inserted = false;
  bool claimed = probe != nullptr && probe->ownsClaim;
  std::shared_ptr<const SegmentValue> chosen = local;

  if (attempted) {
    auto slot = use.slotsByOrd[segmentOrd];
    std::lock_guard<std::mutex> lock(use.entry->mutex);
    auto current = slot->value.load(std::memory_order_acquire);
    if (current != nullptr) {
      chosen = current;
      uint64_t now = nextEpoch();
      current->touch(
          now, evictionClock.load(std::memory_order_relaxed));
      use.entry->lastUsed.store(now, std::memory_order_relaxed);
    } else {
      auto active = activeSegments.load(std::memory_order_acquire);
      const auto& identity = use.readerSegments[segmentOrd];
      // readerCoreGen >= publishedCoreGen prevents an old-reader request from
      // resurrecting a segment purged by a newer publication. (segId, maxDoc)
      // is the defensive segment identity, and card != maxDoc rejects
      // match-all population.
      bool valid = use.entry->resident
          && slot->active.load(std::memory_order_acquire)
          && slot->segId == identity.segId
          && slot->maxDoc == identity.maxDoc
          && slot->maxDoc >= config.minSegmentDocs
          && use.readerCoreGen >= publishedCoreGen.load(std::memory_order_acquire)
          && isActive(*active, identity)
          && local->card() != slot->maxDoc
          && local->ramBytesUsed() <= config.maxEntryBytes;
      if (valid) {
        size_t valueCount = residentValueCount(*use.entry) + 1;
        double effectiveBytes = effectiveValueBytes(
            *use.entry, local->ramBytesUsed(), valueCount);
        // Density is written once before release-publication; touches only
        // combine this frozen value with the current inflation clock.
        local->density = (double) local->buildCostMicros / effectiveBytes;
        local->priority.store(
            evictionClock.load(std::memory_order_relaxed) + local->density,
            std::memory_order_relaxed);
        // Charge before visibility. Eviction detaches with a value CAS and
        // subtracts this exact immutable charge once; readers can pin the
        // value only after the charge is visible.
        residentBytes.fetch_add(local->ramBytesUsed(),
                                std::memory_order_relaxed);
        slot->value.store(local, std::memory_order_release);
        use.entry->lastUsed.store(nextEpoch(), std::memory_order_relaxed);
        inserted = true;
      }
    }
  }

  use.pinValue(segmentOrd, chosen);
  if (probe != nullptr) probe->releaseClaims();
  if (chosen == local && !inserted && attempted) {
    counter.publishRejects.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted && byproduct) {
    counter.byproductInserts.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted && claimed) {
    counter.builds.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted) maybeSweep();
  return chosen;
}

std::shared_ptr<const FilterCache::ReaderValue>
FilterCache::publishReaderValue(
    Use& use, ReaderProbe& probe,
    std::vector<std::unique_ptr<DocSet>> liveExact,
    uint32_t buildCostMicros) {
  if (use.scope_ != FilterKeyScope::READER_STABLE) {
    counter.publishRejects.fetch_add(1, std::memory_order_relaxed);
    throw std::logic_error(
        "whole-reader publication requires READER_STABLE scope");
  }
  auto mutableLocal = std::shared_ptr<ReaderValue>(new ReaderValue(
      use.readerVersion, use.readerSegments, std::move(liveExact),
      nextEpoch(), buildCostMicros));
  std::shared_ptr<const ReaderValue> local = mutableLocal;
  auto localEntry = probe.entry;
  bool inserted = false;
  std::shared_ptr<const ReaderValue> chosen = local;
  std::shared_ptr<const ReaderValue> retired;

  if (enabled() && probe.ownsClaim && localEntry != nullptr) {
    std::lock_guard<std::mutex> lock(localEntry->mutex);
    auto current = localEntry->readerValue.load(std::memory_order_acquire);
    if (current != nullptr && current->readerVersion() == use.readerVersion) {
      chosen = current;
      uint64_t now = nextEpoch();
      current->touch(
          now, evictionClock.load(std::memory_order_relaxed));
      localEntry->lastUsed.store(now, std::memory_order_relaxed);
    } else {
      auto active = activeSegments.load(std::memory_order_acquire);
      bool identitiesMatch = active->segments.size() == use.readerSegments.size();
      for (const auto& identity : use.readerSegments) {
        identitiesMatch = identitiesMatch && isActive(*active, identity);
      }
      bool valid = localEntry->resident
          && localEntry->scope == FilterKeyScope::READER_STABLE
          && use.readerVersion
              == publishedReaderVersion.load(std::memory_order_acquire)
          && active->readerVersion == use.readerVersion
          && identitiesMatch
          && local->ramBytesUsed() <= config.maxEntryBytes;
      if (valid) {
        size_t valueCount = residentValueCount(*localEntry);
        if (current == nullptr) valueCount++;
        valueCount = std::max<size_t>(valueCount, 1);
        double effectiveBytes = effectiveValueBytes(
            *localEntry, local->ramBytesUsed(), valueCount);
        mutableLocal->density =
            (double) mutableLocal->buildCostMicros / effectiveBytes;
        mutableLocal->priority.store(
            evictionClock.load(std::memory_order_relaxed)
                + mutableLocal->density,
            std::memory_order_relaxed);
        // Charge before visibility. The exchange is the one detach event for
        // any prior whole-reader value; its immutable charge is then retired
        // exactly once. Segment slots are not involved in either operation.
        residentBytes.fetch_add(local->ramBytesUsed(),
                                std::memory_order_relaxed);
        retired = localEntry->readerValue.exchange(
            local, std::memory_order_acq_rel);
        if (retired != nullptr) {
          residentBytes.fetch_sub(retired->ramBytesUsed(),
                                  std::memory_order_relaxed);
        }
        localEntry->readerValueEverPublished.store(
            true, std::memory_order_relaxed);
        localEntry->lastUsed.store(nextEpoch(), std::memory_order_relaxed);
        inserted = true;
      }
    }
  }

  bool refresh = probe.refresh;
  probe.releaseClaim();
  if (!inserted && chosen == local) {
    counter.publishRejects.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted && refresh) {
    counter.readerStableRefreshes.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted) {
    counter.builds.fetch_add(1, std::memory_order_relaxed);
  }
  if (inserted) maybeSweep();
  return chosen;
}

bool FilterCache::onReaderPublished(
    uint64_t coreGen, std::span<const SegmentIdentity> segments) {
  return onReaderPublished(coreGen, coreGen, segments);
}

bool FilterCache::onReaderPublished(
    uint64_t coreGen, uint64_t readerVersion,
    std::span<const SegmentIdentity> segments) {
  std::lock_guard<std::mutex> publicationLock(publicationMutex);
  uint64_t currentVersion = publishedReaderVersion.load(
      std::memory_order_acquire);
  uint64_t currentCore = publishedCoreGen.load(std::memory_order_acquire);
  if (readerVersion < currentVersion
      || (readerVersion == currentVersion && coreGen < currentCore)) {
    return false;
  }

  auto next = std::make_shared<ActiveSnapshot>();
  next->coreGen = coreGen;
  next->readerVersion = readerVersion;
  next->segments.assign(segments.begin(), segments.end());
  std::sort(next->segments.begin(), next->segments.end(), identityLess);
  publishedCoreGen.store(coreGen, std::memory_order_release);
  publishedReaderVersion.store(readerVersion, std::memory_order_release);
  activeSegments.store(next, std::memory_order_release);
  readerPublications.fetch_add(1, std::memory_order_relaxed);

  std::vector<std::shared_ptr<FilterEntry>> snapshot;
  snapshot.reserve(entries.size());
  entries.cvisit_all([&](const auto& item) {
    snapshot.push_back(item.second);
  });
  for (const auto& entry : snapshot) {
    std::lock_guard<std::mutex> lock(entry->mutex);
    auto readerValue = entry->readerValue.load(std::memory_order_acquire);
    bool readerSegmentsCurrent = readerValue != nullptr
        && readerValue->segments.size() == next->segments.size();
    if (readerValue != nullptr) {
      for (const auto& segment : readerValue->segments) {
        readerSegmentsCurrent = readerSegmentsCurrent
            && isActive(*next, {segment.segId, segment.maxDoc});
      }
    }
    if (readerValue != nullptr
        && (readerValue->readerVersion() < readerVersion
            || (readerValue->readerVersion() == readerVersion
                && !readerSegmentsCurrent))) {
      auto expected = readerValue;
      if (entry->readerValue.compare_exchange_strong(
              expected, nullptr, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        residentBytes.fetch_sub(readerValue->ramBytesUsed(),
                                std::memory_order_relaxed);
        counter.readerStableRetires.fetch_add(1,
                                              std::memory_order_relaxed);
      }
    }
    auto slots = entry->slots.load(std::memory_order_acquire);
    for (const auto& slot : *slots) {
      SegmentIdentity identity{slot->segId, slot->maxDoc};
      bool active = isActive(*next, identity);
      slot->active.store(active, std::memory_order_release);
      if (!active) {
        auto value = slot->value.exchange(nullptr, std::memory_order_acq_rel);
        if (value != nullptr) {
          residentBytes.fetch_sub(value->ramBytesUsed(),
                                  std::memory_order_relaxed);
          counter.purges.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }
  maybeSweep();
  return true;
}

bool FilterCache::onReaderPublished(IndexReader& reader) {
  auto identities = readerIdentities(reader);
  return onReaderPublished(reader.coreGen(), reader.commitTime(), identities);
}

void FilterCache::maybeSweep() {
  if (residentBytes.load(std::memory_order_relaxed) > config.maxBytes
      || entries.size() > config.maxMetadataEntries
      || metadataBytes.load(std::memory_order_relaxed)
          > config.maxMetadataBytes) {
    sweepRequested.store(true, std::memory_order_release);
    sweep();
  }
}

void FilterCache::sweep() {
  struct SweepGuard {
    std::atomic_flag& flag;
    ~SweepGuard() { flag.clear(std::memory_order_release); }
  };

  struct Candidate {
    std::shared_ptr<SegmentSlot> slot;
    std::shared_ptr<FilterEntry> entry;
    std::shared_ptr<const SegmentValue> segmentValue;
    std::shared_ptr<const ReaderValue> readerValue;
    double priority;
  };
  for (;;) {
    // sweeping elects one worker. Publishers set sweepRequested only after
    // publication is visible; the winner consumes that handoff before taking
    // its snapshots.
    if (sweeping.test_and_set(std::memory_order_acquire)) return;
    sweepRequested.store(false, std::memory_order_release);
    {
      SweepGuard guard{sweeping};
      while (residentBytes.load(std::memory_order_relaxed)
             > config.lowWatermarkBytes) {
        std::vector<std::shared_ptr<FilterEntry>> entrySnapshot;
        entrySnapshot.reserve(entries.size());
        entries.cvisit_all([&](const auto& item) {
          entrySnapshot.push_back(item.second);
        });
        std::vector<Candidate> candidates;
        for (const auto& entry : entrySnapshot) {
          auto readerValue = entry->readerValue.load(std::memory_order_acquire);
          if (readerValue != nullptr) {
            candidates.push_back({nullptr, entry, nullptr, readerValue,
                                  readerValue->evictionPriority()});
          }
          auto slots = entry->slots.load(std::memory_order_acquire);
          for (const auto& slot : *slots) {
            auto value = slot->value.load(std::memory_order_acquire);
            if (value != nullptr) {
              candidates.push_back({slot, nullptr, value, nullptr,
                                    value->evictionPriority()});
            }
          }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                    return a.priority < b.priority;
                  });
        bool detached = false;
        for (const auto& candidate : candidates) {
          if (residentBytes.load(std::memory_order_relaxed)
              <= config.lowWatermarkBytes) break;
          if (candidate.readerValue != nullptr) {
            auto expected = candidate.readerValue;
            if (candidate.entry->readerValue.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
              residentBytes.fetch_sub(
                  candidate.readerValue->ramBytesUsed(),
                  std::memory_order_relaxed);
              advanceEvictionClock(
                  candidate.readerValue->evictionPriority());
              counter.evictions.fetch_add(1, std::memory_order_relaxed);
              detached = true;
            }
          } else {
            auto expected = candidate.segmentValue;
            if (candidate.slot->value.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
              residentBytes.fetch_sub(
                  candidate.segmentValue->ramBytesUsed(),
                  std::memory_order_relaxed);
              advanceEvictionClock(
                  candidate.segmentValue->evictionPriority());
              counter.evictions.fetch_add(1, std::memory_order_relaxed);
              detached = true;
            }
          }
        }
        if (!detached) break;
      }
      sweepMetadata();
    }
    // After the guard clears the single-sweeper flag, consume the handoff
    // again to close the missed-wakeup window. A publisher that lost the flag
    // makes this worker reacquire it; a later publisher can acquire it itself.
    if (!sweepRequested.exchange(false, std::memory_order_acq_rel)) return;
  }
}

void FilterCache::sweepMetadata() {
  if (entries.size() <= config.maxMetadataEntries
      && metadataBytes.load(std::memory_order_relaxed)
          <= config.maxMetadataBytes) {
    return;
  }
  struct Candidate {
    FilterKey key;
    std::shared_ptr<FilterEntry> entry;
    uint64_t used;
  };
  std::vector<Candidate> candidates;
  entries.cvisit_all([&](const auto& item) {
    candidates.push_back({item.first, item.second,
                          item.second->lastUsed.load(std::memory_order_relaxed)});
  });
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.used < b.used;
            });
  for (const auto& candidate : candidates) {
    bool overCount = entries.size() > config.maxMetadataEntries;
    bool overBytes = metadataBytes.load(std::memory_order_relaxed)
        > config.maxMetadataBytes;
    if (!overCount && !overBytes) break;
    bool canErase = true;
    {
      std::lock_guard<std::mutex> lock(candidate.entry->mutex);
      if (!candidate.entry->resident) continue;
      overCount = entries.size() > config.maxMetadataEntries;
      overBytes = metadataBytes.load(std::memory_order_relaxed)
          > config.maxMetadataBytes;
      if (!overCount && !overBytes) break;
      auto readerValue = candidate.entry->readerValue.load(
          std::memory_order_acquire);
      if (readerValue != nullptr && !overBytes) {
        canErase = false;
      }
      auto slots = candidate.entry->slots.load(std::memory_order_acquire);
      for (const auto& slot : *slots) {
        auto value = slot->value.load(std::memory_order_acquire);
        if (value != nullptr && !overBytes) {
          canErase = false;
          break;
        }
      }
      if (canErase) {
        candidate.entry->resident = false;
        if (overBytes) {
          readerValue = candidate.entry->readerValue.exchange(
              nullptr, std::memory_order_acq_rel);
          if (readerValue != nullptr) {
            residentBytes.fetch_sub(readerValue->ramBytesUsed(),
                                    std::memory_order_relaxed);
            advanceEvictionClock(readerValue->evictionPriority());
            counter.evictions.fetch_add(1, std::memory_order_relaxed);
          }
        }
        for (const auto& slot : *slots) {
          slot->active.store(false, std::memory_order_release);
          if (overBytes) {
            auto value = slot->value.exchange(nullptr,
                                               std::memory_order_acq_rel);
            if (value != nullptr) {
              residentBytes.fetch_sub(value->ramBytesUsed(),
                                      std::memory_order_relaxed);
              advanceEvictionClock(value->evictionPriority());
              counter.evictions.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    }
    if (canErase) eraseEntry(candidate.key, candidate.entry);
  }
}

void FilterCache::clear() {
  struct Candidate {
    FilterKey key;
    std::shared_ptr<FilterEntry> entry;
  };
  std::vector<Candidate> snapshot;
  snapshot.reserve(entries.size());
  entries.cvisit_all([&](const auto& item) {
    snapshot.push_back({item.first, item.second});
  });
  for (const auto& candidate : snapshot) {
    {
      std::lock_guard<std::mutex> lock(candidate.entry->mutex);
      candidate.entry->resident = false;
      auto readerValue = candidate.entry->readerValue.exchange(
          nullptr, std::memory_order_acq_rel);
      if (readerValue != nullptr) {
        residentBytes.fetch_sub(readerValue->ramBytesUsed(),
                                std::memory_order_relaxed);
      }
      auto slots = candidate.entry->slots.load(std::memory_order_acquire);
      for (const auto& slot : *slots) {
        slot->active.store(false, std::memory_order_release);
        auto value = slot->value.exchange(nullptr, std::memory_order_acq_rel);
        if (value != nullptr) {
          residentBytes.fetch_sub(value->ramBytesUsed(),
                                  std::memory_order_relaxed);
        }
      }
    }
    eraseEntry(candidate.key, candidate.entry);
  }
}

FilterCache::CounterValues FilterCache::counters() const {
  return {
      .hits = counter.hits.load(std::memory_order_relaxed),
      .misses = counter.misses.load(std::memory_order_relaxed),
      .admissions = counter.admissions.load(std::memory_order_relaxed),
      .builds = counter.builds.load(std::memory_order_relaxed),
      .byproductInserts = counter.byproductInserts.load(std::memory_order_relaxed),
      .publishRejects = counter.publishRejects.load(std::memory_order_relaxed),
      .evictions = counter.evictions.load(std::memory_order_relaxed),
      .purges = counter.purges.load(std::memory_order_relaxed),
      .oversizedKeyBypasses = counter.oversizedKeyBypasses.load(
          std::memory_order_relaxed),
      .readerStableHits = counter.readerStableHits.load(
          std::memory_order_relaxed),
      .readerStableRefreshes = counter.readerStableRefreshes.load(
          std::memory_order_relaxed),
      .readerStableRetires = counter.readerStableRetires.load(
          std::memory_order_relaxed),
      .residentBytes = residentBytes.load(std::memory_order_relaxed),
      .metadataBytes = metadataBytes.load(std::memory_order_relaxed)};
}

void FilterCache::validateForTest() {
  struct SnapshotEntry {
    FilterKey key;
    std::shared_ptr<FilterEntry> entry;
  };
  std::vector<SnapshotEntry> snapshot;
  snapshot.reserve(entries.size());
  entries.cvisit_all([&](const auto& item) {
    snapshot.push_back({item.first, item.second});
  });
  std::sort(snapshot.begin(), snapshot.end(),
            [](const SnapshotEntry& a, const SnapshotEntry& b) {
              return a.entry.get() < b.entry.get();
            });

  // Tests call this at quiesced phase boundaries. Locking every snapshotted
  // entry at once makes the payload and metadata totals one coherent view and
  // also keeps the hook useful if a stray publication is still winding down.
  std::vector<std::unique_lock<std::mutex>> locks;
  locks.reserve(snapshot.size());
  for (const auto& item : snapshot) {
    locks.emplace_back(item.entry->mutex);
  }

  size_t expectedResidentBytes = 0;
  size_t expectedMetadataBytes = 0;
  double clock = evictionClock.load(std::memory_order_relaxed);
  if (!std::isfinite(clock) || clock < 0) {
    throw std::logic_error(
        "FilterCache validation: invalid eviction clock");
  }
  auto active = activeSegments.load(std::memory_order_acquire);
  uint64_t currentReaderVersion = publishedReaderVersion.load(
      std::memory_order_acquire);
  for (const auto& item : snapshot) {
    auto& entry = *item.entry;
    if (!entry.resident) {
      throw std::logic_error(
          "FilterCache validation: map owns non-resident entry");
    }
    size_t expectedCharge = item.key.bytes().capacity() + sizeof(FilterEntry)
        + sizeof(FilterKey) + sizeof(std::shared_ptr<FilterEntry>);
    if (entry.metadataCharge != expectedCharge) {
      throw std::logic_error(
          "FilterCache validation: entry metadata charge mismatch");
    }
    expectedMetadataBytes += entry.metadataCharge;

    auto slots = entry.slots.load(std::memory_order_acquire);
    auto readerValue = entry.readerValue.load(std::memory_order_acquire);
    if (entry.scope == FilterKeyScope::READER_STABLE) {
      if (!slots->empty()) {
        throw std::logic_error(
            "FilterCache validation: reader-stable entry owns segment slots");
      }
      if (readerValue != nullptr) {
        double priority = readerValue->evictionPriority();
        if (!std::isfinite(readerValue->density)
            || readerValue->density < 0
            || !std::isfinite(priority) || priority < 0) {
          throw std::logic_error(
              "FilterCache validation: invalid reader-stable priority");
        }
        if (readerValue->readerVersion() != currentReaderVersion
            || readerValue->readerVersion() != active->readerVersion) {
          throw std::logic_error(
              "FilterCache validation: stale reader-stable value is resident");
        }
        if (readerValue->segments.size() != active->segments.size()) {
          throw std::logic_error(
              "FilterCache validation: reader-stable segment count mismatch");
        }
        size_t expectedReaderCharge = sizeof(ReaderValue)
            + readerValue->segments.capacity()
                * sizeof(ReaderValue::SegmentDocs);
        for (const auto& segment : readerValue->segments) {
          SegmentIdentity identity{segment.segId, segment.maxDoc};
          if (segment.docs == nullptr || !isActive(*active, identity)) {
            throw std::logic_error(
                "FilterCache validation: reader-stable segment identity mismatch");
          }
          expectedReaderCharge += segment.docs->ramBytesUsed();
        }
        if (expectedReaderCharge != readerValue->ramBytesUsed()) {
          throw std::logic_error(
              "FilterCache validation: reader-stable charge mismatch");
        }
        expectedResidentBytes += expectedReaderCharge;
      }
    } else if (readerValue != nullptr) {
      throw std::logic_error(
          "FilterCache validation: slot entry owns reader-stable value");
    }
    uint64_t previousSegId = 0;
    bool first = true;
    for (const auto& slot : *slots) {
      if (!first && slot->segId <= previousSegId) {
        throw std::logic_error(
            "FilterCache validation: slot segIds are not strictly sorted");
      }
      first = false;
      previousSegId = slot->segId;
      auto value = slot->value.load(std::memory_order_acquire);
      if (value == nullptr) continue;
      double priority = value->evictionPriority();
      if (!std::isfinite(value->density) || value->density < 0
          || !std::isfinite(priority) || priority < 0) {
        throw std::logic_error(
            "FilterCache validation: invalid segment priority");
      }
      if (entry.scope == FilterKeyScope::READER_STABLE) {
        throw std::logic_error(
            "FilterCache validation: reader-stable entry has raw value");
      }
      if (!slot->active.load(std::memory_order_acquire)) {
        throw std::logic_error(
            "FilterCache validation: inactive slot retains resident value");
      }
      #ifndef NDEBUG
      if (value->identityForTest.segId != slot->segId
          || value->identityForTest.maxDoc != slot->maxDoc) {
        throw std::logic_error(
            "FilterCache validation: resident value identity mismatch");
      }
      #endif
      expectedResidentBytes += value->ramBytesUsed();
    }
  }

  if (snapshot.size() != entries.size()) {
    throw std::logic_error(
        "FilterCache validation: entry count changed during validation");
  }
  if (expectedResidentBytes
      != residentBytes.load(std::memory_order_relaxed)) {
    throw std::logic_error(
        "FilterCache validation: resident byte accounting mismatch");
  }
  if (expectedMetadataBytes
      != metadataBytes.load(std::memory_order_relaxed)) {
    throw std::logic_error(
        "FilterCache validation: metadata byte accounting mismatch");
  }
}

} // namespace luxir
