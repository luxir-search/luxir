// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/FilterCache.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "luxir/reader/SkipStats.h"
#include "luxir/search/IndexReader.h"
#include "luxir/util/Signal.h"

namespace luxir {

namespace {

FilterCacheConfig normalizeConfig(FilterCacheConfig config) {
  if (config.wholeAdmissionHistorySize == 0) {
    config.wholeAdmissionHistorySize = config.admissionHistorySize;
  }
  config.minSegmentDocs = std::max(0, config.minSegmentDocs);
  config.recentWorkEpochs = std::max<uint64_t>(1, config.recentWorkEpochs);
  return config;
}

size_t ninetyPercent(size_t bytes) {
  return bytes / 10 * 9 + bytes % 10 * 9 / 10;
}

bool identityLess(const FilterCache::SegmentIdentity& a,
                  const FilterCache::SegmentIdentity& b) {
  return std::tie(a.segId, a.space, a.maxDoc) < std::tie(b.segId, b.space, b.maxDoc);
}

std::vector<FilterCache::SegmentIdentity> readerIdentities(IndexReader& reader) {
  std::vector<FilterCache::SegmentIdentity> identities;
  identities.reserve(reader.segments().size());
  for (const auto& segment : reader.segments()) {
    identities.push_back({segment.segInfo.seg_id, segment.maxDoc(), segment.spaceOrdinal()});
  }
  return identities;
}

uint64_t nanosFromWork(double amount) {
  if (!(amount > 0)) return 0;
  if (amount >= (double)UINT64_MAX) return UINT64_MAX;
  return (uint64_t)amount;
}

} // namespace

FilterCache::Work FilterCache::Ledger::productionDemand(
    Work requestWork, const std::optional<double>& charged, double price) const {
  // Known work already entered the rate at the probe. A first assumed use
  // charged zero because its production time was not yet measured.
  if (requestWork.isAssumed() && !measured && charged.value_or(0) == 0) {
    return Work::assumed();
  }
  if (charged || requestWork.known()) return Work::nanoseconds(0);
  double saving = absent.count
      ? std::max(0.0, absent.nanos - (served.count ? served.nanos : 0)) : 0;
  return Work::nanoseconds(price >= 0 ? std::min(price, saving) : saving);
}

void FilterCache::Ledger::production(
    uint64_t nanos, size_t bytes, bool investment, Work requestWork) {
  referenceBytes = bytes;
  if (investment) {
    work += requestWork.resolve((double)nanos);
    referenceNanos = nanos;
    measured = true;
    completed = std::chrono::steady_clock::now();
    owed = 0;
    debt.store(debt.load(std::memory_order_relaxed) + nanos, std::memory_order_relaxed);
  }
}

void FilterCache::Ledger::repay(double nanos) {
  if (debt.load(std::memory_order_relaxed) == 0) return;
  std::lock_guard<Ledger> lock(*this);
  debt.store(std::max(0.0, debt.load(std::memory_order_relaxed) - nanos), std::memory_order_relaxed);
}

bool FilterCache::Ledger::canPopulate(double productionNanos) const {
  return owed >= std::nextafter(std::max(0.0, productionNanos)
      + debt.load(std::memory_order_relaxed), 0.0);
}

double FilterCache::productionNanos(const Ledger& ledger, int32_t maxDoc,
                                    bool observed, bool wholeQuery) const {
  if (ledger.measured) return (double)ledger.referenceNanos;
  if (observed && wholeQuery) return ledger.absent.count ? ledger.absent.nanos : -1;
  double prior = observed ? priorNanosPerDoc.load(std::memory_order_relaxed) : -1;
  return prior < 0 ? -1 : prior * maxDoc;
}

void FilterCache::recordProduction(uint64_t nanos, int32_t maxDoc) {
  if (maxDoc <= 0) return;
  double sample = (double)nanos / maxDoc;
  double old = priorNanosPerDoc.load(std::memory_order_relaxed);
  while (!priorNanosPerDoc.compare_exchange_weak(old,
      old < 0 ? sample : old + (sample - old) / 8, std::memory_order_relaxed)) {}
}

FilterCache::SegmentValue::SegmentValue(std::unique_ptr<DocSet> docs,
                                        SegmentIdentity identityForTest,
                                        uint64_t buildCostNanos,
                                        bool claimedBuild)
  : docs(std::move(docs)),
    charge((uint32_t) this->docs->ramBytesUsed()),
    buildCostNanos(buildCostNanos), claimedBuild(claimedBuild)
    #ifndef NDEBUG
    , identityForTest(identityForTest)
    #endif
    {
  this->docs->card();
  unused(identityForTest);
}

FilterCache::ReaderValue::ReaderValue(
    uint64_t readerVersion, std::span<const SegmentIdentity> identities,
    std::vector<std::unique_ptr<DocSet>> docs,
    uint64_t buildCostNanos, bool claimedBuild)
  : readerVersion_(readerVersion), charge(0),
    buildCostNanos(buildCostNanos), claimedBuild(claimedBuild) {
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
                        std::move(docs[i]), identities[i].space});
  }
}

DocSet* FilterCache::ReaderValue::docSet(
    size_t segmentOrd, const SegmentIdentity& identity) const {
  if (segmentOrd >= segments.size()) {
    throw std::logic_error(
        "reader-stable segment ordinal is outside the cached reader");
  }
  const auto& segment = segments[segmentOrd];
  if (segment.segId != identity.segId || segment.maxDoc != identity.maxDoc || segment.space != identity.space) {
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
    requestSegmentOrd(other.requestSegmentOrd), producedCardinality(other.producedCardinality),
    kind_(other.kind_), ownsClaim(other.ownsClaim), ownsRequestClaim(other.ownsRequestClaim),
    requestWork(other.requestWork) {
  other.kind_ = Kind::BYPASS;
  other.producedCardinality = -1;
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
  producedCardinality = other.producedCardinality;
  ownsClaim = other.ownsClaim;
  ownsRequestClaim = other.ownsRequestClaim;
  requestWork = other.requestWork;
  other.kind_ = Kind::BYPASS;
  other.producedCardinality = -1;
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
    kind_(other.kind_), ownsClaim(other.ownsClaim), refresh(other.refresh),
    requestWork(other.requestWork) {
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
  requestWork = other.requestWork;
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
                      bool admitted, uint8_t observedAdmissionLanes)
  : cache(cache), key(std::move(key)), scope_(scope), entry(std::move(entry)),
    slotsByOrd(std::move(slotsByOrd)),
    readerSegments(std::move(readerSegments)), readerCoreGen(readerCoreGen),
    readerVersion(readerVersion), admitted(admitted),
    observedAdmissionLanes(observedAdmissionLanes),
    epoch(cache != nullptr ? cache->nextEpoch() : 0) {
  requestSlotCount = this->readerSegments.size();
  requestSlots = std::make_unique<RequestSlot[]>(requestSlotCount);
}

FilterCache::Use::~Use() {
  if (readerRoutedTracker != nullptr) {
    readerRoutedTracker->release(readerRoutedBytes);
  }
}

void FilterCache::Use::pinValue(
    size_t segmentOrd, std::shared_ptr<const SegmentValue> value,
    bool requestOwned) {
  if (value == nullptr || segmentOrd >= requestSlotCount) return;
  auto& requestSlot = requestSlots[segmentOrd];
  bool alreadyPinned = requestSlot.firstPin == value
      || std::ranges::find(requestSlot.pins, value) != requestSlot.pins.end();
  // Everything that can throw precedes publishing raw: a throw here may
  // release the only owner of the value.
  if (!alreadyPinned) {
    if (requestSlot.firstPin != nullptr) {
      requestSlot.pins.reserve(requestSlot.pins.size() + 1);
    }
    if (requestOwned) {
      requestSlot.requestOwnedPins.reserve(requestSlot.requestOwnedPins.size() + 1);
      requestSlot.chargeRouted(value->ramBytesUsed());
      requestSlot.requestOwnedPins.push_back(value);
    }
  }
  if (requestSlot.raw == nullptr) {
    requestSlot.raw = value->docSet();
    requestSlot.rawOwned = false;
  }
  if (!alreadyPinned) {
    if (requestSlot.firstPin == nullptr) {
      requestSlot.firstPin = std::move(value);
    } else {
      requestSlot.pins.push_back(std::move(value));
    }
  } else {
    assert(!requestOwned
           || std::ranges::find(requestSlot.requestOwnedPins, value)
                  != requestSlot.requestOwnedPins.end());
  }
}

bool FilterCache::Use::allowsSegmentPopulation(int32_t maxDoc) const {
  uint8_t wholeLaneBit =
      (uint8_t)(1U << (uint8_t)AdmissionLane::WHOLE);
  return cache != nullptr
      && (maxDoc >= cache->config.minSegmentDocs
          || (observedAdmissionLanes & wholeLaneBit) != 0);
}

void FilterCache::Use::releaseRequestClaim(size_t segmentOrd) {
  if (segmentOrd >= requestSlotCount) return;
  auto& requestSlot = requestSlots[segmentOrd];
  {
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    assert(requestSlot.resolving);
    requestSlot.resolving = false;
  }
  requestSlot.condition.notify_all();
}

void FilterCache::Use::ordinaryUse(size_t segmentOrd, bool populationAllowed) {
  if (cache == nullptr || !cache->enabled()) return;
  auto& slot = requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!slot.accounting.ordinary) {
    slot.accounting.ordinary = true;
    cache->counter.misses.add(1);
  }
  slot.accounting.populationAllowed |= populationAllowed;
}

void FilterCache::Use::ordinaryReaderUse(bool populationAllowed) {
  std::lock_guard<std::mutex> lock(readerStateMutex);
  if (!readerAccounting.ordinary) {
    readerAccounting.ordinary = true;
    cache->counter.misses.add(1);
  }
  readerAccounting.populationAllowed |= populationAllowed;
}

void FilterCache::Use::chargeLedger(Ledger& ledger, Accounting& accounting,
                                    std::mutex& mutex, Work work) {
  if (!work.known() || work.isUnfunded()) return;
  std::lock_guard<std::mutex> lock(mutex);
  if (!accounting.populationAllowed || accounting.charged) return;
  double nanos = work.resolve(ledger.measured ? (double)ledger.referenceNanos : 0);
  accounting.charged = nanos;
  ledger.owed += nanos;
  ledger.work += nanos;
  auto& count = work.isAssumed() ? cache->counter.assumedCharges : cache->counter.observedCharges;
  count.add(1);
}

void FilterCache::Use::charge(size_t segmentOrd, Work work) {
  if (cache == nullptr || !cache->enabled() || scope_ == FilterKeyScope::READER_STABLE
      || segmentOrd >= slotsByOrd.size()) return;
  const auto& slot = slotsByOrd[segmentOrd];
  if (slot == nullptr) return;
  std::lock_guard<SegmentSlot> lock(*slot);
  chargeLedger(slot->ledger, requestSlots[segmentOrd].accounting, requestSlots[segmentOrd].mutex, work);
}

void FilterCache::Use::chargeReaderStable(Work work) {
  if (cache == nullptr || !cache->enabled() || scope_ != FilterKeyScope::READER_STABLE
      || entry == nullptr) return;
  std::lock_guard<FilterEntry> lock(*entry);
  chargeLedger(entry->ledger, readerAccounting, readerStateMutex, work);
}

template<class Value>
void FilterCache::creditValue(Ledger& ledger, const Value& value, Work work) {
  // An assumed serve is worth the production of the value it was served from.
  double nanos = work.resolve((double)value.buildCostNanos);
  value.creditedTotal.fetch_add(nanos, std::memory_order_relaxed);
  ledger.repay(nanos);
  auto& count = work.isAssumed() ? counter.assumedCredits : counter.observedCredits;
  count.add(1);
  counter.estimatedSavedNanos.add(nanosFromWork(nanos));
}

void FilterCache::Use::credit(size_t segmentOrd, Work work) {
  if (cache == nullptr || segmentOrd >= requestSlotCount) return;
  auto& slot = requestSlots[segmentOrd];
  const SegmentValue* value;
  {
    std::lock_guard<std::mutex> lock(slot.mutex);
    value = slot.accounting.takeCredit(slot.servedValue, work);
  }
  if (value) cache->creditValue(slotsByOrd[segmentOrd]->ledger, *value, work);
}

void FilterCache::Use::creditReaderStable(Work work) {
  if (cache == nullptr) return;
  const ReaderValue* value;
  {
    std::lock_guard<std::mutex> lock(readerStateMutex);
    value = readerAccounting.takeCredit(readerServedValue, work);
  }
  if (value) cache->creditValue(entry->ledger, *value, work);
}

void FilterCache::Use::deferPopulation(size_t segmentOrd, Deferral reason) {
  auto& slot = requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (slot.accounting.deferred) return;
  slot.accounting.deferred = true;
  auto& counter = reason == Deferral::FUNDING
      ? cache->counter.populationDeferred : cache->counter.capacityDeferred;
  counter.fetch_add(1, std::memory_order_relaxed);
}

void FilterCache::Use::deferReaderPopulation(Deferral reason) {
  std::lock_guard<std::mutex> lock(readerStateMutex);
  if (readerAccounting.deferred) return;
  readerAccounting.deferred = true;
  auto& counter = reason == Deferral::FUNDING
      ? cache->counter.populationDeferred : cache->counter.capacityDeferred;
  counter.fetch_add(1, std::memory_order_relaxed);
}

void FilterCache::acceptSharedHit(
    Use& use, size_t segmentOrd,
    std::shared_ptr<const SegmentValue> value, Work work) {
  const SegmentValue* credited;
  {
    auto& request = use.requestSlots[segmentOrd];
    std::lock_guard<std::mutex> lock(request.mutex);
    const auto* served = value.get();
    request.accounting.pend(work);
    use.pinValue(segmentOrd, std::move(value));
    if (request.servedValue == nullptr) {
      request.servedValue = served;
      touchEpoch(use.entry->lastUsed, use.epoch);
      counter.hits.add(1);
    }
    credited = request.accounting.takeCredit(request.servedValue, work);
  }
  if (credited) creditValue(use.slotsByOrd[segmentOrd]->ledger, *credited, work);
}

void FilterCache::acceptReaderSharedHit(
    Use& use, const std::shared_ptr<FilterEntry>& entry,
    const std::shared_ptr<const ReaderValue>& value, Work work) {
  const ReaderValue* credited;
  {
    std::lock_guard<std::mutex> lock(use.readerStateMutex);
    use.pinReaderValue(value);
    if (use.readerServedValue == nullptr) {
      use.readerServedValue = value.get();
      touchEpoch(entry->lastUsed, use.epoch);
      counter.hits.add(1);
      counter.readerStableHits.fetch_add(1, std::memory_order_relaxed);
    }
    credited = use.readerAccounting.takeCredit(use.readerServedValue, work);
  }
  if (credited) creditValue(entry->ledger, *credited, work);
}

void FilterCache::Use::pinReaderValue(
    const std::shared_ptr<const ReaderValue>& value,
    bool requestOwned) {
  if (value == nullptr) return;
  if (readerPin != nullptr && readerPin != value) {
    throw std::logic_error(
        "one reader-stable Use cannot borrow multiple ReaderValues");
  }
  if (readerPin != nullptr) {
    assert(readerPinRequestOwned == requestOwned);
  }
  if (requestOwned && readerRoutedTracker != nullptr
      && readerRoutedBytes == 0) {
    readerRoutedTracker->charge(
        value->ramBytesUsed(), "domain variants", readerRoutedDetail);
    readerRoutedBytes = value->ramBytesUsed();
  }
  readerPin = value;
  readerPinRequestOwned = requestOwned;
}

std::shared_ptr<const FilterCache::SegmentValue> FilterCache::lookSegmentValue(
    const std::shared_ptr<SegmentSlot>& slot, const SegmentIdentity& identity) {
  if (slot == nullptr || !slot->active.load(std::memory_order_acquire)
      || slot->segId != identity.segId || slot->maxDoc != identity.maxDoc
      || slot->space != identity.space) {
    return nullptr;
  }
  return slot->value.load(std::memory_order_acquire);
}

std::atomic<int32_t>* FilterCache::Use::cardinalityFact(size_t segmentOrd) const {
  if (cache == nullptr || !cache->enabled() || scope_ == FilterKeyScope::READER_STABLE
      || segmentOrd >= slotsByOrd.size()) return nullptr;
  const auto& slot = slotsByOrd[segmentOrd];
  const auto& identity = readerSegments[segmentOrd];
  if (slot == nullptr || !slot->active.load(std::memory_order_acquire)
      || slot->segId != identity.segId || slot->maxDoc != identity.maxDoc
      || slot->space != identity.space) return nullptr;
  return &slot->ledger.rawCardinality;
}

std::optional<int32_t> FilterCache::Use::cardinality(size_t segmentOrd) const {
  auto* fact = cardinalityFact(segmentOrd);
  int32_t count = fact ? fact->load(std::memory_order_relaxed) : -1;
  return count < 0 ? std::nullopt : std::optional<int32_t>(count);
}

std::optional<int32_t> FilterCache::Use::acceptedCardinality(size_t segmentOrd) const {
  if (!existingAccepted || !existingCounts || segmentOrd >= slotsByOrd.size()) return std::nullopt;
  // The accepted slot pin retains this immutable fact after metadata retirement.
  return slotsByOrd[segmentOrd]->ledger.rawCardinality.load(std::memory_order_relaxed);
}

bool FilterCache::Use::recordCardinality(size_t segmentOrd, int32_t count) {
  auto* fact = cardinalityFact(segmentOrd);
  if (fact == nullptr || count < 0 || count > readerSegments[segmentOrd].maxDoc
      || fact->load(std::memory_order_relaxed) >= 0) return false;
  int32_t unknown = -1;
  return fact->compare_exchange_strong(unknown, count, std::memory_order_relaxed);
}

std::optional<FilterCache::SegmentCandidate> FilterCache::Use::look(size_t segmentOrd) {
  if (scope_ == FilterKeyScope::READER_STABLE || segmentOrd >= requestSlotCount) {
    return std::nullopt;
  }
  auto& requestSlot = requestSlots[segmentOrd];
  {
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    if (requestSlot.raw != nullptr) {
      return SegmentCandidate(this, segmentOrd, nullptr, requestSlot.raw);
    }
  }
  if (cache == nullptr || !cache->enabled() || segmentOrd >= slotsByOrd.size()) {
    return std::nullopt;
  }
  auto value = lookSegmentValue(slotsByOrd[segmentOrd], readerSegments[segmentOrd]);
  if (value == nullptr) return std::nullopt;
  return SegmentCandidate(this, segmentOrd, std::move(value));
}

FilterCache::Probe FilterCache::Use::accept(SegmentCandidate&& candidate, Work work) {
  if (candidate.use != this || candidate.segmentOrd >= requestSlotCount) {
    throw std::logic_error("segment candidate belongs to a different filter use");
  }
  size_t segmentOrd = candidate.segmentOrd;
  auto& requestSlot = requestSlots[segmentOrd];
  {
    std::unique_lock<std::mutex> lock(requestSlot.mutex);
    requestSlot.accounting.pend(work);
    requestSlot.condition.wait(lock, [&] {
      return !requestSlot.resolving || requestSlot.raw != nullptr;
    });
    if (requestSlot.raw != nullptr) {
      DocSet* raw = requestSlot.raw;
      bool owned = requestSlot.rawOwned;
      candidate.use = nullptr;
      auto* credited = requestSlot.accounting.takeCredit(requestSlot.servedValue, work);
      lock.unlock();
      if (owned) skipCount(SkipStats::ownedFilterServes);
      if (credited) cache->creditValue(slotsByOrd[segmentOrd]->ledger, *credited, work);
      return Probe(Probe::Kind::HIT, nullptr, nullptr, false, raw);
    }
    if (candidate.value == nullptr) {
      throw std::logic_error("segment candidate has no pinned membership");
    }
    requestSlot.resolving = true;
  }
  Probe claim(Probe::Kind::BYPASS, nullptr, nullptr, false, nullptr,
              this, segmentOrd, true);
  DocSet* raw = candidate.value->docSet();
  cache->acceptSharedHit(*this, segmentOrd, std::move(candidate.value), work);
  candidate.use = nullptr;
  claim.releaseClaims();
  return Probe(Probe::Kind::HIT, nullptr, nullptr, false, raw);
}

FilterCache::Probe FilterCache::Use::probe(size_t segmentOrd, Work work) {
  if (scope_ == FilterKeyScope::READER_STABLE
      || segmentOrd >= readerSegments.size()
      || segmentOrd >= requestSlotCount) {
    return {};
  }

  auto& requestSlot = requestSlots[segmentOrd];
  {
    std::unique_lock<std::mutex> lock(requestSlot.mutex);
    requestSlot.accounting.pend(work);
    requestSlot.condition.wait(lock, [&] {
      return !requestSlot.resolving || requestSlot.raw != nullptr;
    });
    if (requestSlot.raw != nullptr) {
      DocSet* raw = requestSlot.raw;
      bool owned = requestSlot.rawOwned;
      auto* credited = requestSlot.accounting.takeCredit(requestSlot.servedValue, work);
      lock.unlock();
      if (owned) skipCount(SkipStats::ownedFilterServes);
      if (credited) cache->creditValue(slotsByOrd[segmentOrd]->ledger, *credited, work);
      return Probe(Probe::Kind::HIT, nullptr, nullptr, false, raw);
    }
    requestSlot.resolving = true;
  }

  auto bypass = [&] {
    auto result = Probe(Probe::Kind::BYPASS, nullptr, nullptr, false, nullptr,
                        this, segmentOrd, true);
    if (segmentOrd < slotsByOrd.size() && slotsByOrd[segmentOrd] != nullptr) {
      result.producedCardinality = slotsByOrd[segmentOrd]->ledger.rawCardinality.load(
          std::memory_order_relaxed);
    }
    return result;
  };
  // The request slot pins the value for the life of this Use, so a HIT probe
  // borrows the raw pointer and holds no shared_ptr of its own: every
  // refcount change on a hot value is a write all cores contend on.
  auto sharedHit = [&](std::shared_ptr<const SegmentValue> value) {
    DocSet* raw = value->docSet();
    cache->acceptSharedHit(*this, segmentOrd, std::move(value), work);
    releaseRequestClaim(segmentOrd);
    return Probe(Probe::Kind::HIT, nullptr, nullptr, false, raw);
  };

  if (cache == nullptr || !cache->enabled()) {
    return bypass();
  }

  if (entry == nullptr || segmentOrd >= slotsByOrd.size()
      || slotsByOrd[segmentOrd] == nullptr) {
    ordinaryUse(segmentOrd, false);
    return bypass();
  }

  const auto& slot = slotsByOrd[segmentOrd];
  if (!slot->active.load(std::memory_order_acquire)
      || slot->segId != readerSegments[segmentOrd].segId
      || slot->maxDoc != readerSegments[segmentOrd].maxDoc
      || slot->space != readerSegments[segmentOrd].space) {
    ordinaryUse(segmentOrd, false);
    return bypass();
  }

  auto value = slot->value.load(std::memory_order_acquire);
  if (value != nullptr) {
    return sharedHit(std::move(value));
  }

  if (work.isUnfunded() || !admitted || !allowsSegmentPopulation(slot->maxDoc)
      || readerCoreGen < cache->publishedCoreGen.load(std::memory_order_acquire)) {
    ordinaryUse(segmentOrd, false);
    return bypass();
  }

  std::unique_lock<SegmentSlot> ledgerLock(*slot);
  if (!slot->active.load(std::memory_order_acquire)
      || slot->segId != readerSegments[segmentOrd].segId
      || slot->maxDoc != readerSegments[segmentOrd].maxDoc
      || slot->space != readerSegments[segmentOrd].space
      || readerCoreGen
          < cache->publishedCoreGen.load(std::memory_order_acquire)) {
    ordinaryUse(segmentOrd, false);
    return bypass();
  }
  value = slot->value.load(std::memory_order_acquire);
  if (value != nullptr) {
    ledgerLock.unlock();
    return sharedHit(std::move(value));
  }

  ordinaryUse(segmentOrd, true);
  if (work.known()) {
    chargeLedger(slot->ledger, requestSlot.accounting, requestSlot.mutex, work);
  }
  if (!work.isAssumed()) {
    slot->ledger.observed = true;
    slot->ledger.wholeQuery = work.isWholeQuery();
  }
  double production = cache->productionNanos(
      slot->ledger, slot->maxDoc, !work.isAssumed(), work.isWholeQuery());
  if ((!work.known() && slot->ledger.owed <= 0) || !slot->ledger.canPopulate(production)) {
    deferPopulation(segmentOrd);
    return bypass();
  }

  size_t bytes = slot->ledger.referenceBytes;
  if (bytes == 0) {
    int32_t card = slot->ledger.rawCardinality.load(std::memory_order_relaxed);
    if (card >= 0) {
      bytes = DocSetBuilder::estimateBytes(slot->maxDoc, card);
    } else if (slot->ledger.measured || slot->ledger.absent.count == 0
               || work.isAssumed() || !work.isWholeQuery()) {
      bytes = RAMBitDocSet::ramBytesForSize(slot->maxDoc);
    }
  }
  // Unknown whole-query size is measured once. Keep its ledger locked through
  // the claim so a completed production cannot leave a stale size decision.
  if (bytes != 0) {
    ledgerLock.unlock();
    if (!cache->capacityAllows(*entry, slot->ledger, bytes, production)) {
      deferPopulation(segmentOrd, Deferral::CAPACITY);
      return bypass();
    }
  }
  bool expected = false;
  if (!slot->building.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return bypass();
  }
  // The comparison ran without the slot's lock; a build may have finished since.
  if (slot->value.load(std::memory_order_acquire) != nullptr) {
    slot->building.store(false, std::memory_order_release);
    return bypass();
  }
  {
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    requestSlot.accounting.clauseClaim = !work.isAssumed() && !work.isWholeConsumer();
  }
  cache->counter.buildAttempts.fetch_add(1, std::memory_order_relaxed);
  uint64_t now = cache->nextEpoch();
  entry->lastUsed.store(now, std::memory_order_relaxed);
  Probe result(Probe::Kind::BUILD, slot, nullptr, true, nullptr, this, segmentOrd, true);
  result.requestWork = work;
  return result;
}

FilterCache::ReaderProbe FilterCache::Use::probeReaderStable(
    IndexReader& reader, std::span<DocSet* const> domainPerSeg, Work work) {
  if (cache == nullptr || !cache->enabled()
      || scope_ != FilterKeyScope::READER_STABLE
      || reader.coreCommitTime() != readerVersion
      || reader.segments().size() != readerSegments.size()) {
    return {};
  }
  if (key.bytes().size() > cache->maxEntryBytes()) {
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
    DocSet* canonical = segment.liveDocSet();
    DocSet* actual = emptyMeansCanonical ? nullptr : domainPerSeg[i];
    if (actual != canonical) return {};
  }

  std::shared_ptr<const ReaderValue> pinned;
  const ReaderValue* credited;
  {
    std::lock_guard<std::mutex> lock(readerStateMutex);
    pinned = readerPin;
    credited = readerAccounting.takeCredit(readerServedValue, work);
  }
  if (pinned != nullptr) {
    if (credited) cache->creditValue(entry->ledger, *credited, work);
    return ReaderProbe(ReaderProbe::Kind::HIT, entry, std::move(pinned), false, false);
  }

  std::shared_ptr<FilterEntry> localEntry;
  bool staleAfterCreate = false;
  {
    std::unique_lock<std::mutex> stateLock(readerStateMutex);
    uint8_t pendingLanes = observedAdmissionLanes
        & (uint8_t)~readerAdmissionRecordedLanes;
    for (AdmissionLane lane : {AdmissionLane::CLAUSE,
                               AdmissionLane::WHOLE}) {
      uint8_t laneBit = (uint8_t)(1U << (uint8_t)lane);
      if ((pendingLanes & laneBit) != 0) {
        admitted = cache->admissionFor(lane).record(key.hash()) || admitted;
      }
    }
    readerAdmissionRecordedLanes |= pendingLanes;
    if (entry == nullptr) {
      entry = cache->findEntry(key);
      if (entry == nullptr) {
        if (!admitted) {
          stateLock.unlock();
          ordinaryReaderUse(false);
          return {};
        }
        entry = cache->findOrCreateEntry(key, scope_);
        // A publication can advance after its entry snapshot but before this
        // reader-lane insertion. Do not let that new entry claim old-reader
        // work; publish-time validation remains the final backstop.
        staleAfterCreate = readerVersion
            != cache->publishedReaderVersion.load(
                std::memory_order_acquire);
      } else {
        // Entry existence is proof of prior admission.
        admitted = true;
      }
    }
    localEntry = entry;
  }
  if (localEntry == nullptr || localEntry->scope != scope_) {
    ordinaryReaderUse(false);
    return {};
  }
  if (staleAfterCreate) {
    ordinaryReaderUse(false);
    return {};
  }

  auto sharedHit = [&](std::shared_ptr<const ReaderValue> hitValue) {
    cache->acceptReaderSharedHit(*this, localEntry, hitValue, work);
    return ReaderProbe(ReaderProbe::Kind::HIT, std::move(localEntry),
                       std::move(hitValue), false, false);
  };

  auto value = localEntry->readerValue.load(std::memory_order_acquire);
  if (value != nullptr && value->readerVersion() == readerVersion) {
    return sharedHit(std::move(value));
  }

  if (work.isUnfunded() || readerVersion
      != cache->publishedReaderVersion.load(std::memory_order_acquire)) {
    ordinaryReaderUse(false);
    return {};
  }
  std::unique_lock<std::mutex> entryLock(localEntry->mutex);
  if (!localEntry->resident) {
    ordinaryReaderUse(false);
    return {};
  }
  std::unique_lock<FilterEntry> ledgerLock(*localEntry);
  entryLock.unlock();
  value = localEntry->readerValue.load(std::memory_order_acquire);
  if (value != nullptr && value->readerVersion() == readerVersion) {
    ledgerLock.unlock();
    return sharedHit(std::move(value));
  }

  ordinaryReaderUse(true);
  if (work.known()) chargeLedger(localEntry->ledger, readerAccounting, readerStateMutex, work);
  double production = cache->productionNanos(localEntry->ledger, 0, false);
  if (!localEntry->ledger.canPopulate(production)) {
    deferReaderPopulation();
    return {};
  }
  size_t bytes = localEntry->ledger.referenceBytes;
  ledgerLock.unlock();
  if (!cache->capacityAllows(*localEntry, localEntry->ledger, bytes, production)) {
    deferReaderPopulation(Deferral::CAPACITY);
    return {};
  }
  bool expected = false;
  if (!localEntry->readerBuilding.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_relaxed)) {
    return {};
  }
  value = localEntry->readerValue.load(std::memory_order_acquire);
  if (value != nullptr && value->readerVersion() == readerVersion) {
    localEntry->readerBuilding.store(false, std::memory_order_release);
    return {};
  }
  cache->counter.buildAttempts.fetch_add(1, std::memory_order_relaxed);
  uint64_t now = cache->nextEpoch();
  localEntry->lastUsed.store(now, std::memory_order_relaxed);
  bool refresh = localEntry->readerValueEverPublished.load(
      std::memory_order_relaxed);
  ReaderProbe result(ReaderProbe::Kind::BUILD, localEntry, nullptr, true, refresh);
  result.requestWork = work;
  return result;
}

std::shared_ptr<const FilterCache::SegmentValue>
FilterCache::Use::publishRaw(size_t segmentOrd, Probe& probe,
                             std::unique_ptr<DocSet> raw,
                             uint64_t buildCostNanos) {
  return cache->publish(*this, segmentOrd, &probe, std::move(raw), false,
                        buildCostNanos);
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
  if (segmentOrd >= requestSlotCount || probe.requestUse != this
      || probe.requestSegmentOrd != segmentOrd || !probe.ownsRequestClaim) {
    throw std::logic_error("owned raw adoption requires the request claim");
  }

  raw->card();
  auto& requestSlot = requestSlots[segmentOrd];
  bool inserted = false;
  bool owned = false;
  DocSet* result;
  {
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    if (requestSlot.raw == nullptr) {
      requestSlot.chargeRouted(raw->ramBytesUsed());
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
                                      uint64_t buildCostNanos) {
  return cache->publish(*this, segmentOrd, &probe, std::move(raw), true,
                        buildCostNanos);
}

std::shared_ptr<const FilterCache::SegmentValue>
FilterCache::Use::offerRaw(size_t segmentOrd, std::unique_ptr<DocSet> raw,
                           uint64_t buildCostNanos) {
  return cache->publish(*this, segmentOrd, nullptr, std::move(raw), true,
                        buildCostNanos);
}

std::shared_ptr<const FilterCache::ReaderValue>
FilterCache::Use::publishReaderStable(
    ReaderProbe& probe, std::vector<std::unique_ptr<DocSet>> liveExact,
    uint64_t buildCostNanos) {
  auto value = cache->publishReaderValue(
      *this, probe, std::move(liveExact), buildCostNanos);
  return value;
}

DocSet* FilterCache::Use::rawDocSet(size_t segmentOrd) {
  if (segmentOrd >= requestSlotCount) return nullptr;
  auto& requestSlot = requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(requestSlot.mutex);
  return requestSlot.raw;
}

void FilterCache::Use::enableRoutedAccounting(
    size_t segmentOrd, RequestMemTracker& tracker,
    std::string_view detail) {
  if (segmentOrd >= requestSlotCount) return;
  if (scope_ == FilterKeyScope::READER_STABLE) {
    std::lock_guard<std::mutex> lock(readerStateMutex);
    if (readerRoutedTracker != nullptr
        && readerRoutedTracker != &tracker) {
      throw std::logic_error(
          "one filter use cannot span request memory trackers");
    }
    if (readerRoutedTracker == nullptr) {
      if (readerPinRequestOwned && readerPin != nullptr) {
        tracker.charge(
            readerPin->ramBytesUsed(), "domain variants", detail);
        readerRoutedBytes = readerPin->ramBytesUsed();
      }
      readerRoutedTracker = &tracker;
      readerRoutedDetail = detail;
    }
    return;
  }
  auto& requestSlot = requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(requestSlot.mutex);
  if (requestSlot.routedTracker != nullptr
      && requestSlot.routedTracker != &tracker) {
    throw std::logic_error(
        "one filter use cannot span request memory trackers");
  }
  if (requestSlot.routedTracker != nullptr) return;

  size_t bytes = 0;
  auto include = [&](DocSet* docs) {
    if (docs == nullptr) return;
    size_t artifactBytes = docs->ramBytesUsed();
    if (artifactBytes > std::numeric_limits<size_t>::max() - bytes) {
      tracker.chargeOverflow("domain variants", detail);
    }
    bytes += artifactBytes;
  };
  auto includeBytes = [&](size_t artifactBytes) {
    if (artifactBytes > std::numeric_limits<size_t>::max() - bytes) {
      tracker.chargeOverflow("domain variants", detail);
    }
    bytes += artifactBytes;
  };
  if (requestSlot.rawOwned) include(requestSlot.ownedRaw.get());
  for (auto& value : requestSlot.requestOwnedPins) {
    includeBytes(value->ramBytesUsed());
  }
  include(requestSlot.liveEffective.get());
  tracker.charge(bytes, "domain variants", detail);
  requestSlot.routedTracker = &tracker;
  requestSlot.routedDetail = detail;
  requestSlot.routedBytes = bytes;
}

DocSet* FilterCache::Use::effectiveDocSet(
    size_t segmentOrd, IndexReader& reader) {
  // Raw AND liveDocs, memoized only in this request's Use and never published
  // to the cache. Domain composition lives in effectiveDomain.
  if (segmentOrd >= readerSegments.size()
      || segmentOrd >= reader.segments().size()) {
    return nullptr;
  }
  auto& segment = reader.segments()[segmentOrd];
  if (segment.segInfo.seg_id != readerSegments[segmentOrd].segId
      || segment.maxDoc() != readerSegments[segmentOrd].maxDoc
      || segment.spaceOrdinal() != readerSegments[segmentOrd].space) {
    return nullptr;
  }
  if (scope_ == FilterKeyScope::READER_STABLE) {
    std::shared_ptr<const ReaderValue> value;
    {
      std::lock_guard<std::mutex> lock(readerStateMutex);
      value = readerPin;
    }
    if (value == nullptr || value->readerVersion() != readerVersion
        || reader.coreCommitTime() != readerVersion) {
      return nullptr;
    }
    return value->docSet(segmentOrd, readerSegments[segmentOrd]);
  }
  auto& requestSlot = requestSlots[segmentOrd];
  std::lock_guard<std::mutex> lock(requestSlot.mutex);
  DocSet* raw = requestSlot.raw;
  if (raw == nullptr) return nullptr;

  if (!segment.hasDeletes()) return raw;
  if (requestSlot.liveEffective != nullptr) {
    return requestSlot.liveEffective.get();
  }
  std::array<DocSet*, 2> sets{
      raw, segment.liveDocSet()};
  auto docs = DocSet::intersect(sets);
  docs->card();
  requestSlot.chargeRouted(docs->ramBytesUsed());
  requestSlot.liveEffective = std::move(docs);
  return requestSlot.liveEffective.get();
}

DomainHandle FilterCache::Use::effectiveDomain(
    size_t segmentOrd, IndexReader& reader, DocSet* domain) {
  // PrepareContext domains already carry liveness, so a composed set is raw
  // AND domain. The canonical live set is the request-stable value itself.
  if (domain != nullptr && segmentOrd < reader.segments().size()) {
    auto& segment = reader.segments()[segmentOrd];
    DocSet* canonical = segment.liveDocSet();
    if (domain == canonical) domain = nullptr;
  }
  if (domain == nullptr) {
    return DomainHandle::borrowed(effectiveDocSet(segmentOrd, reader)).scoped(reader.segments()[segmentOrd]);
  }
  // A reader-stable value is live-exact for the canonical domain only.
  if (scope_ == FilterKeyScope::READER_STABLE) return {};
  DocSet* stable = effectiveDocSet(segmentOrd, reader);
  if (stable == nullptr) return {};
  std::array<DocSet*, 2> sets{stable, domain};
  auto docs = DocSet::intersect(sets);
  docs->card();
  return DomainHandle(std::move(docs)).scoped(reader.segments()[segmentOrd]);
}

size_t FilterCache::Use::ownedBytesForTest() {
  size_t result = 0;
  for (size_t i = 0; i < requestSlotCount; i++) {
    auto& requestSlot = requestSlots[i];
    std::lock_guard<std::mutex> lock(requestSlot.mutex);
    if (requestSlot.ownedRaw != nullptr) {
      result += requestSlot.ownedRaw->ramBytesUsed();
    }
  }
  return result;
}

thread_local FilterCache::UseRegistry::Observation* FilterCache::UseRegistry::timedObservation = nullptr;
thread_local size_t FilterCache::UseRegistry::timedSegment = 0;

FilterCache::Work FilterCache::Use::work(size_t segmentOrd) const {
  return observed && registry != nullptr && registry->observation.get() == UseRegistry::timedObservation
      && segmentOrd == UseRegistry::timedSegment ? Work::pending() : Work::assumed();
}

void FilterCache::Use::settle(size_t segmentOrd, uint64_t nanos,
    std::chrono::steady_clock::time_point started) {
  if (cache == nullptr || segmentOrd >= requestSlotCount
      || segmentOrd >= slotsByOrd.size() || slotsByOrd[segmentOrd] == nullptr) return;
  auto& request = requestSlots[segmentOrd];
  const auto& accounting = request.accounting;
  if (!accounting.pending || accounting.built) return;
  bool served = request.servedValue != nullptr;
  if (nanos == 0 || (accounting.ordinary && served)) {
    if (accounting.ordinary) charge(segmentOrd, Work::assumed());
    if (served) credit(segmentOrd, Work::assumed());
    return;
  }
  auto& slot = *slotsByOrd[segmentOrd];
  Work payment = Work::pending();
  {
    std::lock_guard<SegmentSlot> lock(slot);
    auto& ledger = slot.ledger;
    ledger.observed = true;
    ledger.wholeQuery = accounting.wholeQuery;
    if (accounting.ordinary) {
      if (ledger.measured && started < ledger.completed) return;
      ledger.absent.add((double)nanos);
    } else if (served) {
      ledger.served.add((double)nanos);
    } else {
      return;
    }
    double saving = ledger.absent.count && ledger.served.count
        ? std::max(0.0, ledger.absent.nanos - ledger.served.nanos)
        : accounting.ordinary ? (double)nanos : 0;
    if (accounting.ordinary) {
      double price = cache->productionNanos(ledger, slot.maxDoc, true, accounting.wholeQuery);
      if (price >= 0) saving = std::min(price, saving);
    }
    payment = Work::nanoseconds(saving);
    if (accounting.ordinary) {
      chargeLedger(ledger, request.accounting, request.mutex, payment);
      return;
    }
  }
  credit(segmentOrd, payment);
}

FilterCache::UseRegistry::SegmentScope::SegmentScope(UseRegistry& registry, size_t segmentOrd)
    : registry(registry), segmentOrd(segmentOrd), previousObservation(timedObservation),
      previousSegment(timedSegment), outermost(timedObservation != registry.observation.get() || timedSegment != segmentOrd) {
  assert(segmentOrd < registry.segments.size());
  if (!outermost) return;
  started = std::chrono::steady_clock::now();
  timedObservation = registry.observation.get();
  timedSegment = segmentOrd;
}

FilterCache::UseRegistry::SegmentScope::~SegmentScope() {
  if (!outermost) return;
  registry.addSegmentNanos(segmentOrd, (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - started).count());
  timedObservation = previousObservation;
  timedSegment = previousSegment;
}

void FilterCache::UseRegistry::addSegmentNanos(size_t segmentOrd, uint64_t nanos) {
  if (segmentOrd >= segments.size()) throw std::out_of_range("cache timing segment ordinal");
  observation->timing[segmentOrd].nanos.fetch_add(nanos, std::memory_order_relaxed);
}

void FilterCache::UseRegistry::recordProduction() {
  if (timedObservation != nullptr) {
    timedObservation->timing[timedSegment].produced.store(true, std::memory_order_relaxed);
  }
}

void FilterCache::UseRegistry::settle(bool completed) noexcept {
  if (settled) return;
  settled = true;
  if (!completed) return;
  for (size_t i = 0; i < segments.size(); i++) {
    const auto& timing = observation->timing[i];
    // Investment changes the executed plan and cannot teach an ordinary price.
    if (timing.produced.load(std::memory_order_relaxed)) continue;
    uint64_t nanos = timing.nanos.load(std::memory_order_relaxed);
    for (auto& [key, use] : uses) {
      use->settle(i, nanos, observation->started);
    }
  }
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
    readerCoreGen(readerCoreGen), readerVersion(readerVersion),
    observation(std::make_shared<Observation>(segments.size()))
    #ifndef NDEBUG
    , owningThread(std::this_thread::get_id())
    #endif
    {
}

FilterCache::UseRegistry::UseRegistry(FilterCache* cache, IndexReader& reader)
  : UseRegistry(cache, reader, nullptr) {}

FilterCache::UseRegistry::UseRegistry(FilterCache* cache, IndexReader& reader, const UseRegistry& related)
  : UseRegistry(cache, reader, &related) {}

FilterCache::UseRegistry::UseRegistry(FilterCache* cache, IndexReader& reader, const UseRegistry* related)
  : cache(cache), segments(readerIdentities(reader)), readerCoreGen(reader.coreGen()),
    readerVersion(reader.coreCommitTime()),
    observation(related ? related->observation : std::make_shared<Observation>(segments.size()))
    #ifndef NDEBUG
    , owningThread(std::this_thread::get_id())
    #endif
    {
  if (related) {
    if (observation->snapshot != reader.snapshot || segments.size() != related->segments.size()) {
      throw std::invalid_argument("cache observations require aligned views of one snapshot");
    }
    for (size_t i = 0; i < segments.size(); i++) {
      if (segments[i].segId != related->segments[i].segId) {
        throw std::invalid_argument("cache observation views have different physical segments");
      }
    }
  } else {
    observation->snapshot = reader.snapshot;
  }
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
  : UseRegistry(&cache, reader) {
}

FilterCache::Use* FilterCache::UseRegistry::get(const FilterKey& key,
                                                FilterKeyScope scope,
                                                AdmissionLane lane) {
  #ifndef NDEBUG
  assert(owningThread == std::this_thread::get_id());
  #endif
  auto iter = uses.find(key);
  if (iter != uses.end()) {
    auto* use = iter->second.get();
    if (use->scope_ != scope) {
      throw std::logic_error(
          "FilterCache request key reused with different scope");
    }
    if (use->cache != nullptr) use->cache->observeAdmissionLane(*use, lane);
    return use;
  }
  auto use = cache != nullptr && usesSharedFilterCache(scope)
      ? cache->beginUse(
          key, scope, readerCoreGen, readerVersion, segments, lane)
      : std::unique_ptr<Use>(new Use(
          nullptr, key, scope, nullptr,
          std::vector<std::shared_ptr<SegmentSlot>>(segments.size()),
          segments, readerCoreGen, readerVersion, false,
          (uint8_t)(1U << (uint8_t)lane)));
  Use* result = use.get();
  result->registry = this;
  uses.emplace(key, std::move(use));
  return result;
}

std::optional<FilterCache::ExistingCandidate>
FilterCache::UseRegistry::lookupExisting(
    const FilterKey& key, FilterKeyScope scope, bool countsOnly) {
  #ifndef NDEBUG
  assert(owningThread == std::this_thread::get_id());
  #endif
  if (cache == nullptr || uses.contains(key)) return std::nullopt;
  auto use = cache->lookupExisting(
      key, scope, readerCoreGen, readerVersion, segments, countsOnly);
  if (use == nullptr) return std::nullopt;
  return ExistingCandidate(std::move(use));
}

FilterCache::Use* FilterCache::UseRegistry::acceptExisting(
    ExistingCandidate&& candidate, AdmissionLane lane, Work work) {
  #ifndef NDEBUG
  assert(owningThread == std::this_thread::get_id());
  #endif
  if (cache == nullptr || candidate.use == nullptr
      || uses.contains(candidate.use->key)
      || !cache->acceptExisting(*candidate.use, lane, work)) {
    return nullptr;
  }
  Use* result = candidate.use.get();
  result->registry = this;
  uses.emplace(result->key, std::move(candidate.use));
  return result;
}

void FilterCache::UseRegistry::enableRoutedAccounting(
    size_t segmentOrd, RequestMemTracker& tracker,
    std::string_view detail) {
  for (auto& [key, use] : uses) {
    unused(key);
    use->enableRoutedAccounting(segmentOrd, tracker, detail);
  }
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

void FilterCache::AdmissionRing::reset() {
  std::lock_guard<std::mutex> lock(mutex);
  std::fill(history.begin(), history.end(), 0);
  frequencies.clear();
  cursor = 0;
  filled = 0;
}

FilterCache::AdmissionRing& FilterCache::admissionFor(AdmissionLane lane) {
  return lane == AdmissionLane::CLAUSE
      ? clauseAdmission : wholeAdmission;
}

size_t FilterCache::maxEntryBytes(size_t bytes) const {
  return config.maxEntryBytes != 0 ? config.maxEntryBytes
      : bytes == 0 ? 0 : std::max<size_t>(1, bytes / 5);
}

size_t FilterCache::maxMetadataBytes(size_t bytes) const {
  return config.maxMetadataBytes != 0 ? config.maxMetadataBytes
      : bytes == 0 ? 0 : std::max<size_t>(1, bytes / 4);
}

size_t FilterCache::lowWatermarkMetadataBytes(size_t bytes) const {
  size_t metadata = maxMetadataBytes(bytes);
  return config.lowWatermarkMetadataBytes == 0 ? ninetyPercent(metadata)
      : std::min(config.lowWatermarkMetadataBytes, metadata);
}

FilterCache::FilterCache(FilterCacheConfig config)
  : config(normalizeConfig(config)), budget(config.maxBytes),
    clauseAdmission(this->config.admissionHistorySize,
                    this->config.admissionThreshold),
    wholeAdmission(this->config.wholeAdmissionHistorySize,
                   this->config.admissionThreshold) {
  activeSegments.store(std::make_shared<const ActiveSnapshot>(),
                       std::memory_order_release);
}

FilterCacheConfig FilterCache::configuration() const {
  size_t bytes = budgetBytes();
  FilterCacheConfig out = config;
  out.maxBytes = bytes;
  out.maxEntryBytes = maxEntryBytes(bytes);
  out.maxMetadataBytes = maxMetadataBytes(bytes);
  out.lowWatermarkMetadataBytes = lowWatermarkMetadataBytes(bytes);
  return out;
}

void FilterCache::resize(size_t maxBytes) {
  size_t old = budget.exchange(maxBytes, std::memory_order_acq_rel);
  if (maxBytes == 0) {
    Signal::emit("filterCacheDisabled", this);
    clear();
  } else if (maxBytes != old) {
    {
      std::lock_guard<std::mutex> lock(residentsMutex);
      residentsReady = false;
      residentsVersion.fetch_add(1, std::memory_order_release);
      residents.clear();
    }
    if (maxBytes < old || residentBytes.load(std::memory_order_relaxed) > maxBytes) {
      sweepRequested.store(true, std::memory_order_release);
      sweep();
    }
  }
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
  // Freeze its current per-resident-value share into the residency charge.
  return (double) payloadBytes
      + (double) VALUE_RESIDENCY_OVERHEAD_BYTES
      + (double) entry.metadataCharge / (double) residentValueCount;
}

template<class Value>
std::shared_ptr<const Value> FilterCache::detachValue(
    std::atomic<std::shared_ptr<const Value>>& source, Ledger& ledger,
    std::shared_ptr<const Value> expected) {
  std::lock_guard<Ledger> lock(ledger);
  if (expected) {
    auto current = expected;
    if (!source.compare_exchange_strong(current, nullptr, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) return {};
  } else {
    expected = source.exchange(nullptr, std::memory_order_acq_rel);
  }
  if (expected) {
    // Credits arriving after this fold still repay debt, but no longer affect recency.
    ledger.collectCredit(expected->creditedTotal.load(std::memory_order_relaxed));
    ledger.creditSnapshot = 0;
  }
  return expected;
}

void FilterCache::recordCapacityEviction(
    const std::shared_ptr<const SegmentValue>& value) {
  double unpaid = std::max(0.0, (double)value->buildCostNanos - value->creditedTotal.load(std::memory_order_relaxed));
  if (!value->claimedBuild || unpaid == 0) return;
  counter.unrepaidEvictions.fetch_add(1, std::memory_order_relaxed);
  counter.unpaidBuildNanos.fetch_add(nanosFromWork(unpaid), std::memory_order_relaxed);
}

void FilterCache::recordReaderCapacityEviction(
    const std::shared_ptr<const ReaderValue>& value) {
  double unpaid = std::max(0.0, (double)value->buildCostNanos - value->creditedTotal.load(std::memory_order_relaxed));
  if (!value->claimedBuild || unpaid == 0) return;
  counter.unrepaidEvictions.fetch_add(1, std::memory_order_relaxed);
  counter.unpaidBuildNanos.fetch_add(nanosFromWork(unpaid), std::memory_order_relaxed);
}

bool FilterCache::isActive(const ActiveSnapshot& snapshot,
                           const SegmentIdentity& identity) const {
  if (identity.space == (SpaceOrdinal)UINT32_MAX && identity.maxDoc == 0) {
    auto bundle = std::lower_bound(snapshot.segments.begin(), snapshot.segments.end(), identity.segId,
        [](const auto& item, uint64_t segId) { return item.segId < segId; });
    return bundle != snapshot.segments.end() && bundle->segId == identity.segId;
  }
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
    Signal::emit("filterCacheBeforeInsert", this);
    bool inserted = entries.try_emplace_or_cvisit(
        key, candidate, [&](const auto& item) { result = item.second; });
    if (inserted) {
      counter.admissions.fetch_add(1, std::memory_order_relaxed);
      Signal::emit("filterCacheEntryInserted", this);
      // clear visits every group under its lock. If it passed this group
      // before insertion, that lock handoff makes its zero visible here.
      if (!enabled()) {
        detachEntry(result);
        eraseEntry(key, result);
      }
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
    // slotsCharge is frozen once resident is false, and every caller observed
    // that transition under the entry mutex before erasing.
    metadataBytes.fetch_sub(entry->metadataCharge + entry->slotsCharge,
                            std::memory_order_relaxed);
    return true;
  }
  return false;
}

std::vector<std::shared_ptr<FilterCache::SegmentSlot>>
FilterCache::alignSlots(const std::shared_ptr<FilterEntry>& entry,
                        std::span<const SegmentIdentity> readerSegments) {
  auto lowerBound = [](const SlotVector& slots,
                       const SegmentIdentity& identity) {
    return std::lower_bound(
        slots.begin(), slots.end(), identity,
        [](const std::shared_ptr<SegmentSlot>& slot,
           const SegmentIdentity& target) {
          return std::tie(slot->segId, slot->space, slot->maxDoc)
              < std::tie(target.segId, target.space, target.maxDoc);
        });
  };
  auto exactSlot = [&](const SlotVector& slots,
                       const SegmentIdentity& identity)
      -> const std::shared_ptr<SegmentSlot>* {
    auto found = lowerBound(slots, identity);
    bool exact = found != slots.end() && (*found)->segId == identity.segId
        && (*found)->maxDoc == identity.maxDoc && (*found)->space == identity.space;
    return exact ? &*found : nullptr;
  };
  auto needsInsert = [&](const SlotVector& slots,
                         const ActiveSnapshot& active) {
    for (const auto& identity : readerSegments) {
      if (exactSlot(slots, identity) == nullptr
          && isActive(active, identity)) {
        return true;
      }
    }
    return false;
  };

  std::vector<std::shared_ptr<SegmentSlot>> aligned(readerSegments.size());
  // Aligns the active slots of a table; true when a reader segment has none.
  auto gather = [&](const SlotVector& slots) {
    bool missing = false;
    for (size_t i = 0; i < readerSegments.size(); i++) {
      const auto* slot = exactSlot(slots, readerSegments[i]);
      if (slot == nullptr) {
        missing = true;
      } else if ((*slot)->active.load(std::memory_order_acquire)) {
        aligned[i] = *slot;
      }
    }
    return missing;
  };

  // Fast path: align from the entry's COW snapshot without the entry mutex or
  // a copy. The cache-wide activeSegments snapshot is read only when a reader
  // segment has no slot. Load slots before activeSegments: a snapshot that
  // already lacks a pruned slot was stored after its publication's
  // activeSegments, so the identity reads as inactive and is not re-inserted.
  // resident is not checked here: a non-resident entry's slots are all
  // inactive, so gather aligns nothing, exactly like the locked early return.
  auto current = entry->slots.load(std::memory_order_acquire);
  if (gather(*current)
      && needsInsert(*current,
                     *activeSegments.load(std::memory_order_acquire))) {
    std::lock_guard<std::mutex> lock(entry->mutex);
    std::fill(aligned.begin(), aligned.end(), nullptr);
    if (!entry->resident) return aligned;
    auto active = activeSegments.load(std::memory_order_acquire);
    current = entry->slots.load(std::memory_order_acquire);
    if (needsInsert(*current, *active)) {
      auto updated = std::make_shared<SlotVector>(*current);
      size_t added = 0;
      for (const auto& identity : readerSegments) {
        if (exactSlot(*updated, identity) != nullptr
            || !isActive(*active, identity)) {
          continue;
        }
        updated->insert(lowerBound(*updated, identity),
                        std::make_shared<SegmentSlot>(identity.segId,
                                                      identity.maxDoc, identity.space));
        added++;
      }
      entry->slotsCharge += added * SLOT_METADATA_BYTES;
      metadataBytes.fetch_add(added * SLOT_METADATA_BYTES,
                              std::memory_order_relaxed);
      entry->slots.store(updated, std::memory_order_release);
      current = updated;
    }
    gather(*current);
  }
  entry->lastUsed.store(nextEpoch(), std::memory_order_relaxed);
  return aligned;
}

std::unique_ptr<FilterCache::Use> FilterCache::beginUse(
    const FilterKey& key, FilterKeyScope scope, uint64_t readerCoreGen,
    uint64_t readerVersion,
    std::span<const SegmentIdentity> readerSegments, AdmissionLane lane) {
  assert(usesSharedFilterCache(scope));
  auto use = std::unique_ptr<Use>(new Use(
      this, key, scope, nullptr,
      std::vector<std::shared_ptr<SegmentSlot>>(readerSegments.size()),
      std::vector<SegmentIdentity>(readerSegments.begin(), readerSegments.end()),
      readerCoreGen, readerVersion, false, 0));
  observeAdmissionLane(*use, lane);
  maybeSweep();
  return use;
}

std::unique_ptr<FilterCache::Use> FilterCache::lookupExisting(
    const FilterKey& key, FilterKeyScope scope, uint64_t readerCoreGen,
    uint64_t readerVersion,
    std::span<const SegmentIdentity> readerSegments, bool countsOnly) {
  if (!enabled() || readerSegments.empty()
      || !usesSharedFilterCache(scope)) {
    return nullptr;
  }
  auto entry = findEntry(key);
  if (entry == nullptr || entry->scope != scope
      || (countsOnly && scope > FilterKeyScope::CORE_STABLE)) return nullptr;

  if (scope == FilterKeyScope::READER_STABLE) {
    auto value = entry->readerValue.load(std::memory_order_acquire);
    if (value == nullptr || value->readerVersion() != readerVersion) {
      return nullptr;
    }
    auto use = std::unique_ptr<Use>(new Use(
        this, key, scope, entry, {},
        std::vector<SegmentIdentity>(readerSegments.begin(),
                                     readerSegments.end()),
        readerCoreGen, readerVersion, false, 0));
    use->existingReaderCandidate = std::move(value);
    return use;
  }

  auto active = activeSegments.load(std::memory_order_acquire);
  auto current = entry->slots.load(std::memory_order_acquire);
  std::vector<std::shared_ptr<SegmentSlot>> aligned(readerSegments.size());
  std::vector<std::shared_ptr<const SegmentValue>> candidates(
      readerSegments.size());
  for (size_t i = 0; i < readerSegments.size(); i++) {
    const auto& identity = readerSegments[i];
    auto found = std::lower_bound(
        current->begin(), current->end(), identity,
        [](const std::shared_ptr<SegmentSlot>& slot,
           const SegmentIdentity& target) {
          return std::tie(slot->segId, slot->space, slot->maxDoc)
              < std::tie(target.segId, target.space, target.maxDoc);
        });
    if (found == current->end() || !isActive(*active, identity)) {
      return nullptr;
    }
    bool fact = countsOnly && (*found)->active.load(std::memory_order_acquire)
        && (*found)->segId == identity.segId && (*found)->maxDoc == identity.maxDoc
        && (*found)->space == identity.space
        && (*found)->ledger.rawCardinality.load(std::memory_order_relaxed) >= 0;
    if (countsOnly ? !fact : (candidates[i] = lookSegmentValue(*found, identity)) == nullptr) return nullptr;
    aligned[i] = *found;
  }

  auto use = std::unique_ptr<Use>(new Use(
      this, key, scope, entry, std::move(aligned),
      std::vector<SegmentIdentity>(readerSegments.begin(),
                                   readerSegments.end()),
      readerCoreGen, readerVersion, false, 0));
  use->existingCandidates = std::move(candidates);
  use->existingCounts = countsOnly;
  return use;
}

bool FilterCache::acceptExisting(Use& use, AdmissionLane lane, Work work) {
  if (use.cache != this) return false;
  if (use.existingAccepted) {
    observeAdmissionLane(use, lane);
    if (use.scope_ == FilterKeyScope::READER_STABLE) {
      uint8_t laneBit = (uint8_t)(1U << (uint8_t)lane);
      if ((use.readerAdmissionRecordedLanes & laneBit) == 0) {
        admissionFor(lane).record(use.key.hash());
        use.readerAdmissionRecordedLanes |= laneBit;
      }
    }
    return true;
  }
  if (use.entry == nullptr) return false;
  if (use.scope_ == FilterKeyScope::READER_STABLE) {
    // Unlike probeReaderStable, acceptance has no domain argument. Its caller
    // must have established an exact-reader canonical root domain before
    // lookup. The accepted Use still rechecks that domain in effectiveDocSet
    // before exposing the value.
    auto value = use.existingReaderCandidate;
    auto active = activeSegments.load(std::memory_order_acquire);
    bool valid = value != nullptr
        && value->readerVersion() == use.readerVersion
        && active->readerVersion == use.readerVersion
        && active->bundleCount == use.readerSegments.size();
    for (const auto& identity : use.readerSegments) {
      valid = valid && isActive(*active, identity);
    }
    if (!valid) return false;

    observeAdmissionLane(use, lane);
    uint8_t laneBit = (uint8_t)(1U << (uint8_t)lane);
    if ((use.readerAdmissionRecordedLanes & laneBit) == 0) {
      admissionFor(lane).record(use.key.hash());
      use.readerAdmissionRecordedLanes |= laneBit;
    }
    use.admitted = true;
    acceptReaderSharedHit(use, use.entry, value, work);
    use.existingReaderCandidate.reset();
    use.existingAccepted = true;
    maybeSweep();
    return true;
  }
  if (use.existingCandidates.size() != use.readerSegments.size()
      || use.slotsByOrd.size() != use.readerSegments.size()) {
    return false;
  }

  // Revalidate the whole candidate before applying any effect. A subsequent
  // capacity detach is safe: each candidate pin remains a valid immutable
  // value whose credit repays the same ledger after detach.
  auto active = activeSegments.load(std::memory_order_acquire);
  for (size_t i = 0; i < use.readerSegments.size(); i++) {
    const auto& identity = use.readerSegments[i];
    const auto& slot = use.slotsByOrd[i];
    if (slot == nullptr || (!use.existingCounts && use.existingCandidates[i] == nullptr)
        || !slot->active.load(std::memory_order_acquire)
        || slot->segId != identity.segId || slot->maxDoc != identity.maxDoc || slot->space != identity.space
        || !isActive(*active, identity)) {
      return false;
    }
  }

  observeAdmissionLane(use, lane);
  if (use.existingCounts) touchEpoch(use.entry->lastUsed, use.epoch);
  for (size_t i = 0; !use.existingCounts && i < use.readerSegments.size(); i++) {
    acceptSharedHit(use, i,
                    std::move(use.existingCandidates[i]), work);
  }
  use.existingCandidates.clear();
  use.existingAccepted = true;
  maybeSweep();
  return true;
}

void FilterCache::observeAdmissionLane(Use& use, AdmissionLane lane) {
  assert(use.cache == this && usesSharedFilterCache(use.scope_));
  uint8_t laneBit = (uint8_t)(1U << (uint8_t)lane);
  if ((use.observedAdmissionLanes & laneBit) != 0) return;
  use.observedAdmissionLanes |= laneBit;
  if (!enabled()) return;

  // Reader-stable admission stays behind its canonical-domain gate. The lane
  // observations are retained here and recorded by probeReaderStable only
  // after that gate accepts the request.
  if (use.scope_ == FilterKeyScope::READER_STABLE) return;
  if (use.key.bytes().size() > maxEntryBytes()) {
    counter.oversizedKeyBypasses.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  use.admitted = admissionFor(lane).record(use.key.hash()) || use.admitted;
  auto entry = use.entry != nullptr ? use.entry : findEntry(use.key);
  if (entry == nullptr && use.admitted) {
    entry = findOrCreateEntry(use.key, use.scope_);
  }
  if (entry == nullptr) return;
  if (entry->scope != use.scope_) {
    throw std::logic_error("FilterCache key reused with different scope");
  }
  if (use.entry == nullptr) {
    use.entry = entry;
    use.slotsByOrd = alignSlots(entry, use.readerSegments);
  }
}

std::shared_ptr<const FilterCache::SegmentValue> FilterCache::publish(
    Use& use, size_t segmentOrd, Probe* probe, std::unique_ptr<DocSet> raw,
    bool byproduct, uint64_t buildCostNanos) {
  if (use.scope_ == FilterKeyScope::READER_STABLE) {
    counter.publishRejects.fetch_add(1, std::memory_order_relaxed);
    throw std::logic_error(
        "reader-stable live-exact membership cannot enter raw cache APIs");
  }
  if (raw == nullptr) throw std::invalid_argument("raw DocSet must not be null");
  SegmentIdentity identity = segmentOrd < use.readerSegments.size()
      ? use.readerSegments[segmentOrd] : SegmentIdentity{};
  bool claimed = probe != nullptr && probe->ownsClaim;
  // Count claimed work even if publication rejects it. offerRaw and other
  // byproducts were materialized for the query, not at the cache's request.
  if (claimed && !byproduct) {
    UseRegistry::recordProduction();
    counter.buildNanos.fetch_add(buildCostNanos, std::memory_order_relaxed);
  }
  auto local = std::make_shared<SegmentValue>(
      std::move(raw), identity, buildCostNanos,
      claimed && !byproduct);

  bool attempted = enabled() && use.entry != nullptr
      && segmentOrd < use.slotsByOrd.size()
      && use.slotsByOrd[segmentOrd] != nullptr;
  bool inserted = false;
  std::shared_ptr<const SegmentValue> chosen = local;

  if (attempted) {
    auto slot = use.slotsByOrd[segmentOrd];
    std::lock_guard<std::mutex> lock(use.entry->mutex);
    std::lock_guard<SegmentSlot> ledgerLock(*slot);
    if (use.entry->resident && slot->active.load(std::memory_order_acquire)) {
      auto demand = claimed && !byproduct
          ? slot->ledger.productionDemand(probe->requestWork,
              use.requestSlots[segmentOrd].accounting.charged,
              productionNanos(slot->ledger, slot->maxDoc,
                  !probe->requestWork.isAssumed(), probe->requestWork.isWholeQuery()))
          : Work::nanoseconds(0);
      slot->ledger.production(buildCostNanos, local->ramBytesUsed(), claimed && !byproduct, demand);
      slot->ledger.rawCardinality.store(local->card(), std::memory_order_relaxed);
    }
    auto current = slot->value.load(std::memory_order_acquire);
    if (current != nullptr) {
      chosen = current;
      use.entry->lastUsed.store(nextEpoch(), std::memory_order_relaxed);
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
          && slot->maxDoc == identity.maxDoc && slot->space == identity.space
          && use.allowsSegmentPopulation(slot->maxDoc)
          && use.readerCoreGen >= publishedCoreGen.load(std::memory_order_acquire)
          && isActive(*active, identity)
          && local->card() != slot->maxDoc
          && local->ramBytesUsed() <= maxEntryBytes();
      if (valid) {
        size_t valueCount = residentValueCount(*use.entry) + 1;
        double effectiveBytes = effectiveValueBytes(
            *use.entry, local->ramBytesUsed(), valueCount);
        local->residencyBytes = effectiveBytes;
        local->published = nextEpoch();
        // Charge before visibility. Eviction detaches with a value CAS and
        // subtracts this exact immutable charge once; readers can pin the
        // value only after the charge is visible.
        residentBytes.fetch_add(local->ramBytesUsed(),
                                std::memory_order_relaxed);
        slot->value.store(local, std::memory_order_release);
        use.entry->lastUsed.store(local->published, std::memory_order_relaxed);
        inserted = true;
      }
    }
  }

  if (segmentOrd < use.requestSlotCount) {
    std::lock_guard<std::mutex> lock(use.requestSlots[segmentOrd].mutex);
    auto& accounting = use.requestSlots[segmentOrd].accounting;
    accounting.built |= claimed && !byproduct;
    if (claimed && !byproduct && accounting.clauseClaim && attempted) {
      recordProduction(buildCostNanos, identity.maxDoc);
    }
    use.pinValue(segmentOrd, chosen, chosen == local && !inserted);
  }
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
  if (inserted) {
    rememberResident(use.entry, use.slotsByOrd[segmentOrd], local, {});
    maybeSweep();
  }
  return chosen;
}

std::shared_ptr<const FilterCache::ReaderValue>
FilterCache::publishReaderValue(
    Use& use, ReaderProbe& probe,
    std::vector<std::unique_ptr<DocSet>> liveExact,
    uint64_t buildCostNanos) {
  if (use.scope_ != FilterKeyScope::READER_STABLE) {
    counter.publishRejects.fetch_add(1, std::memory_order_relaxed);
    throw std::logic_error(
        "whole-reader publication requires READER_STABLE scope");
  }
  if (probe.ownsClaim) {
    UseRegistry::recordProduction();
    counter.buildNanos.fetch_add(buildCostNanos, std::memory_order_relaxed);
  }
  auto mutableLocal = std::shared_ptr<ReaderValue>(new ReaderValue(
      use.readerVersion, use.readerSegments, std::move(liveExact),
      buildCostNanos, probe.ownsClaim));
  std::shared_ptr<const ReaderValue> local = mutableLocal;
  auto localEntry = probe.entry;
  bool inserted = false;
  std::shared_ptr<const ReaderValue> chosen = local;
  std::shared_ptr<const ReaderValue> retired;

  if (enabled() && probe.ownsClaim && localEntry != nullptr) {
    std::lock_guard<std::mutex> lock(localEntry->mutex);
    std::lock_guard<FilterEntry> ledgerLock(*localEntry);
    if (localEntry->resident) {
      auto demand = localEntry->ledger.productionDemand(probe.requestWork,
          use.readerAccounting.charged, productionNanos(localEntry->ledger, 0, false));
      localEntry->ledger.production(buildCostNanos, local->ramBytesUsed(), true, demand);
    }
    auto current = localEntry->readerValue.load(std::memory_order_acquire);
    if (current != nullptr && current->readerVersion() == use.readerVersion) {
      chosen = current;
      localEntry->lastUsed.store(nextEpoch(), std::memory_order_relaxed);
    } else {
      auto active = activeSegments.load(std::memory_order_acquire);
      bool identitiesMatch = active->bundleCount == use.readerSegments.size();
      for (const auto& identity : use.readerSegments) {
        identitiesMatch = identitiesMatch && isActive(*active, identity);
      }
      bool valid = localEntry->resident
          && localEntry->scope == FilterKeyScope::READER_STABLE
          && use.readerVersion
              == publishedReaderVersion.load(std::memory_order_acquire)
          && active->readerVersion == use.readerVersion
          && identitiesMatch
          && local->ramBytesUsed() <= maxEntryBytes();
      if (valid) {
        size_t valueCount = residentValueCount(*localEntry);
        if (current == nullptr) valueCount++;
        valueCount = std::max<size_t>(valueCount, 1);
        double effectiveBytes = effectiveValueBytes(
            *localEntry, local->ramBytesUsed(), valueCount);
        mutableLocal->residencyBytes = effectiveBytes;
        mutableLocal->published = nextEpoch();
        // Charge before visibility. The exchange is the one detach event for
        // any prior whole-reader value; its immutable charge is then retired
        // exactly once. Segment slots are not involved in either operation.
        residentBytes.fetch_add(local->ramBytesUsed(),
                                std::memory_order_relaxed);
        if (current) {
          localEntry->ledger.collectCredit(current->creditedTotal.load(std::memory_order_relaxed));
          localEntry->ledger.creditSnapshot = 0;
        }
        retired = localEntry->readerValue.exchange(local, std::memory_order_acq_rel);
        if (retired != nullptr) {
          residentBytes.fetch_sub(retired->ramBytesUsed(),
                                  std::memory_order_relaxed);
        }
        localEntry->readerValueEverPublished.store(
            true, std::memory_order_relaxed);
        localEntry->lastUsed.store(local->published, std::memory_order_relaxed);
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
  if (inserted) {
    rememberResident(localEntry, {}, {}, local);
    maybeSweep();
  }
  {
    std::lock_guard<std::mutex> lock(use.readerStateMutex);
    use.pinReaderValue(chosen, chosen == local && !inserted);
  }
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
  uint64_t lastSegId = 0;
  for (const auto& identity : next->segments) {
    if (next->bundleCount == 0 || identity.segId != lastSegId) next->bundleCount++;
    lastSegId = identity.segId;
  }
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
        && readerValue->segments.size() == next->bundleCount;
    if (readerValue != nullptr) {
      for (const auto& segment : readerValue->segments) {
        readerSegmentsCurrent = readerSegmentsCurrent
            && isActive(*next, {segment.segId, segment.maxDoc, segment.space});
      }
    }
    if (readerValue != nullptr
        && (readerValue->readerVersion() < readerVersion
            || (readerValue->readerVersion() == readerVersion
                && !readerSegmentsCurrent))) {
      if (detachValue(entry->readerValue, entry->ledger, readerValue)) {
        residentBytes.fetch_sub(readerValue->ramBytesUsed(),
                                std::memory_order_relaxed);
        counter.readerStableRetires.fetch_add(1,
                                              std::memory_order_relaxed);
      }
    }
    // A non-resident entry is leaving the map: keep all of its slots
    // inactive and its slot table (and slotsCharge) frozen.
    bool resident = entry->resident;
    auto slots = entry->slots.load(std::memory_order_acquire);
    std::shared_ptr<SlotVector> kept;
    for (size_t i = 0; i < slots->size(); i++) {
      const auto& slot = (*slots)[i];
      SegmentIdentity identity{slot->segId, slot->maxDoc, slot->space};
      bool active = resident && isActive(*next, identity);
      slot->active.store(active, std::memory_order_release);
      if (resident && !active && kept == nullptr) {
        kept = std::make_shared<SlotVector>(slots->begin(),
                                            slots->begin() + i);
      } else if (active && kept != nullptr) {
        kept->push_back(slot);
      }
      if (!active) {
        auto value = detachValue(slot->value, slot->ledger);
        if (value != nullptr) {
          residentBytes.fetch_sub(value->ramBytesUsed(),
                                  std::memory_order_relaxed);
          counter.purges.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    if (kept != nullptr) {
      // Drop merged-away slots so a long-lived entry does not accumulate one
      // per segment it has ever seen. A Use already holding a pruned slot
      // keeps it alive; it is permanently inactive (only this publication
      // path could reactivate it, and it is no longer in the table), so
      // probe and publish keep treating it as a miss. A segment that
      // reappears gets a fresh slot from alignSlots.
      size_t dropped = slots->size() - kept->size();
      entry->slotsCharge -= dropped * SLOT_METADATA_BYTES;
      metadataBytes.fetch_sub(dropped * SLOT_METADATA_BYTES,
                              std::memory_order_relaxed);
      entry->slots.store(kept, std::memory_order_release);
    }
  }
  {
    std::lock_guard<std::mutex> lock(residentsMutex);
    residents.clear();
    residentsReady = false;
    residentsVersion.fetch_add(1, std::memory_order_release);
  }
  maybeSweep();
  return true;
}

bool FilterCache::onReaderPublished(IndexReader& reader) {
  std::vector<SegmentIdentity> identities;
  for (const auto& bundle : reader.snapshot->core->bundles) {
    const auto& catalog = bundle.core->spaces();
    for (size_t i = 0; i < catalog.size(); i++) {
      identities.push_back({bundle.info.seg_id, catalog[i].maxDoc, (SpaceOrdinal)i});
    }
  }
  return onReaderPublished(reader.coreGen(), reader.coreCommitTime(), identities);
}

void FilterCache::maybeSweep() {
  size_t bytes = budgetBytes();
  if (residentBytes.load(std::memory_order_relaxed) > bytes
      || entries.size() > config.maxMetadataEntries
      || metadataBytes.load(std::memory_order_relaxed)
          > maxMetadataBytes(bytes)) {
    sweepRequested.store(true, std::memory_order_release);
    sweep();
  }
}

bool FilterCache::capacityAllows(const FilterEntry& entry, Ledger& ledger, size_t bytes, double production) {
  // The byte budget counts payloads; retain its exact free-space fast path.
  auto fits = [&] {
    size_t used = residentBytes.load(std::memory_order_relaxed);
    size_t budget = budgetBytes();
    return used <= budget && bytes <= budget - used;
  };
  if (bytes == 0 || production < 0 || fits()) return true;
  uint64_t now = currentEpoch();
  {
    std::lock_guard<Ledger> lock(ledger);
    if (ledger.refusedBytes == bytes
        && ledger.refusedVersion == residentsVersion.load(std::memory_order_acquire)
        && now - lastRefreshEpoch() < config.recentWorkEpochs
        && ledger.recent + ledger.work <= ledger.refusedWork) return false;
  }
  std::lock_guard<std::mutex> listLock(residentsMutex);
  Signal::emit("filterCacheCapacityCompared", this);
  if (fits()) return true;
  refreshResidents();
  std::lock_guard<Ledger> lock(ledger);
  double required = (double)bytes + (double)residentBytes.load(std::memory_order_relaxed)
      - (double)budgetBytes();
  double victims = 0;
  double outranked = 0;
  auto end = residents.begin();
  while (end != residents.end() && required > 0) {
    if (!end->spokenFor) {
      double size = 0;
      size_t payload = 0;
      if (auto slot = end->slot.lock()) {
        if (auto value = end->segment.lock(); value && slot->value.load(std::memory_order_acquire) == value) {
          size = value->residencyBytes;
          payload = value->ramBytesUsed();
        }
      } else if (auto owner = end->entry.lock()) {
        if (auto value = end->reader.lock(); value && owner->readerValue.load(std::memory_order_acquire) == value) {
          size = value->residencyBytes;
          payload = value->ramBytesUsed();
        }
      }
      // Only a smaller candidate buys a fraction of its final victim. Earlier
      // victims and equal/larger candidates still pay for whole values.
      double fraction = bytes < payload && required < payload ? required / payload : 1;
      victims += end->priority * size * fraction;
      required -= payload;
      if (payload) outranked = end->priority;
    }
    ++end;
  }
  // The sweep evicts in list order, so the value must also rank above what it displaces.
  outranked *= effectiveValueBytes(entry, bytes, residentValueCount(entry) + 1);
  uint64_t sinceRoll = currentEpoch() - lastRefreshEpoch();
  double rate = ledger.rate(sinceRoll, config.recentWorkEpochs);
  if (required <= 0 && rate >= outranked && (rate - victims) * config.recentWorkEpochs > production) {
    for (auto it = residents.begin(); it != end; ++it) it->spokenFor = true;
    return true;
  }
  ledger.refusedVersion = residentsVersion.load(std::memory_order_relaxed);
  ledger.refusedWork = required > 0 ? std::numeric_limits<double>::infinity()
      : std::max(outranked, victims + production / config.recentWorkEpochs)
          * std::max(config.recentWorkEpochs, ledger.recentEpochs + sinceRoll);
  ledger.refusedBytes = bytes;
  return false;
}

void FilterCache::rememberResident(const std::shared_ptr<FilterEntry>& entry,
    const std::shared_ptr<SegmentSlot>& slot,
    const std::shared_ptr<const SegmentValue>& segment,
    const std::shared_ptr<const ReaderValue>& reader) {
  std::lock_guard<std::mutex> lock(residentsMutex);
  if (!residentsReady) return;
  Ledger& ledger = slot ? slot->ledger : entry->ledger;
  std::lock_guard<Ledger> ledgerLock(ledger);
  uint64_t sinceRoll = epoch.load(std::memory_order_relaxed) - refreshEpoch.load(std::memory_order_relaxed);
  Resident value{entry, slot, segment, reader,
      ledger.rate(sinceRoll, config.recentWorkEpochs)
          / (segment ? segment->residencyBytes : reader->residencyBytes),
      segment ? segment->published : reader->published};
  auto it = std::lower_bound(residents.begin(), residents.end(), value);
  if (it == residents.end() || it->published != value.published) residents.insert(it, value);
}

void FilterCache::refreshResidents() {
  uint64_t now = epoch.load(std::memory_order_relaxed);
  uint64_t elapsed = now - refreshEpoch.load(std::memory_order_relaxed);
  bool roll = elapsed >= config.recentWorkEpochs;
  if (residentsReady && !roll) return;
  // Invalidate refusals before any ledger rolls into the new list state.
  residentsVersion.fetch_add(1, std::memory_order_release);
  // Rebuilding identities does not complete an observation interval early.
  residents.clear();
  entries.cvisit_all([&](const auto& item) {
    auto add = [&](Ledger& ledger, const std::shared_ptr<SegmentSlot>& slot,
        const std::shared_ptr<const SegmentValue>& segment,
        const std::shared_ptr<const ReaderValue>& reader) {
      if (segment || reader) {
        ledger.collectCredit(segment ? segment->creditedTotal.load(std::memory_order_relaxed)
            : reader->creditedTotal.load(std::memory_order_relaxed));
      }
      if (roll) {
        ledger.recent = ledger.work;
        ledger.recentEpochs = elapsed;
        ledger.work = 0;
      }
      if (segment || reader) {
        residents.push_back({item.second, slot, segment, reader,
            ledger.rate(roll ? 0 : elapsed, config.recentWorkEpochs)
                / (segment ? segment->residencyBytes : reader->residencyBytes),
            segment ? segment->published : reader->published});
      }
    };
    if (item.second->scope == FilterKeyScope::READER_STABLE) {
      std::lock_guard<Ledger> lock(item.second->ledger);
      add(item.second->ledger, {}, {}, item.second->readerValue.load(std::memory_order_acquire));
    }
    auto slots = item.second->slots.load(std::memory_order_acquire);
    for (const auto& slot : *slots) {
      std::lock_guard<Ledger> lock(slot->ledger);
      add(slot->ledger, slot, slot->value.load(std::memory_order_acquire), {});
    }
  });
  std::sort(residents.begin(), residents.end());
  residentsReady = true;
  if (roll) {
    refreshEpoch.store(now, std::memory_order_relaxed);
  }
}

void FilterCache::sweep() {
  struct SweepGuard {
    std::atomic_flag& flag;
    ~SweepGuard() { flag.clear(std::memory_order_release); }
  };
  for (;;) {
    // Publishers that lose the sweeper hand off a request before returning.
    if (sweeping.test_and_set(std::memory_order_acquire)) return;
    Signal::emit("filterCacheSweepStarted", this);
    sweepRequested.exchange(false, std::memory_order_acq_rel);
    {
      SweepGuard guard{sweeping};
      {
        std::lock_guard<std::mutex> lock(residentsMutex);
        refreshResidents();
        bool rebuilt = false;
        while (residentBytes.load(std::memory_order_relaxed) > budgetBytes()) {
          refreshResidents();
          bool detached = false;
          auto it = residents.begin();
          while (it != residents.end() && residentBytes.load(std::memory_order_relaxed) > budgetBytes()) {
            if (auto slot = it->slot.lock()) {
              if (auto value = it->segment.lock()) {
                if (detachValue(slot->value, slot->ledger, value)) {
                  recordCapacityEviction(value);
                  residentBytes.fetch_sub(value->ramBytesUsed(), std::memory_order_relaxed);
                  counter.evictions.fetch_add(1, std::memory_order_relaxed);
                  detached = true;
                }
              }
            } else if (auto entry = it->entry.lock()) {
              if (auto value = it->reader.lock()) {
                if (detachValue(entry->readerValue, entry->ledger, value)) {
                  recordReaderCapacityEviction(value);
                  residentBytes.fetch_sub(value->ramBytesUsed(), std::memory_order_relaxed);
                  counter.evictions.fetch_add(1, std::memory_order_relaxed);
                  detached = true;
                }
              }
            }
            ++it;
          }
          residents.erase(residents.begin(), it);
          if (detached) {
            // Free remainder can lower a previously refused fractional charge.
            residentsVersion.fetch_add(1, std::memory_order_release);
            continue;
          }
          // A list that no longer names what is resident is rebuilt once.
          if (rebuilt) break;
          residentsReady = false;
          residentsVersion.fetch_add(1, std::memory_order_release);
          rebuilt = true;
        }
      }
      sweepMetadata();
    }
    // Recheck after releasing the guard to close the missed-wakeup window.
    if (!sweepRequested.exchange(false, std::memory_order_acq_rel)) return;
  }
}

void FilterCache::sweepMetadata() {
  size_t bytes = budgetBytes();
  size_t low = lowWatermarkMetadataBytes(bytes);
  if (entries.size() <= config.maxMetadataEntries
      && metadataBytes.load(std::memory_order_relaxed)
          <= maxMetadataBytes(bytes)) {
    return;
  }
  bool removed = false;
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
        > low;
    if (!overCount && !overBytes) break;
    bool canErase = true;
    {
      std::lock_guard<std::mutex> lock(candidate.entry->mutex);
      if (!candidate.entry->resident) continue;
      overCount = entries.size() > config.maxMetadataEntries;
      overBytes = metadataBytes.load(std::memory_order_relaxed)
          > low;
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
          readerValue = detachValue(candidate.entry->readerValue, candidate.entry->ledger);
          if (readerValue != nullptr) {
            recordReaderCapacityEviction(readerValue);
            residentBytes.fetch_sub(readerValue->ramBytesUsed(),
                                    std::memory_order_relaxed);
            counter.evictions.fetch_add(1, std::memory_order_relaxed);
          }
        }
        for (const auto& slot : *slots) {
          slot->active.store(false, std::memory_order_release);
          if (overBytes) {
            auto value = detachValue(slot->value, slot->ledger);
            if (value != nullptr) {
              recordCapacityEviction(value);
              residentBytes.fetch_sub(value->ramBytesUsed(),
                                      std::memory_order_relaxed);
              counter.evictions.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    }
    if (canErase) {
      eraseEntry(candidate.key, candidate.entry);
      removed = true;
    }
  }
  if (removed) {
    std::lock_guard<std::mutex> lock(residentsMutex);
    residents.clear();
    residentsReady = false;
    residentsVersion.fetch_add(1, std::memory_order_release);
  }
}

void FilterCache::detachEntry(const std::shared_ptr<FilterEntry>& entry) {
  std::lock_guard<std::mutex> lock(entry->mutex);
  entry->resident = false;
  auto readerValue = detachValue(entry->readerValue, entry->ledger);
  if (readerValue != nullptr) {
    residentBytes.fetch_sub(readerValue->ramBytesUsed(),
                            std::memory_order_relaxed);
  }
  auto slots = entry->slots.load(std::memory_order_acquire);
  for (const auto& slot : *slots) {
    slot->active.store(false, std::memory_order_release);
    auto value = detachValue(slot->value, slot->ledger);
    if (value != nullptr) {
      residentBytes.fetch_sub(value->ramBytesUsed(),
                              std::memory_order_relaxed);
    }
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
    detachEntry(candidate.entry);
    eraseEntry(candidate.key, candidate.entry);
  }
  std::lock_guard<std::mutex> lock(residentsMutex);
  std::vector<Resident>{}.swap(residents);
  residentsReady = false;
  residentsVersion.fetch_add(1, std::memory_order_release);
}

void FilterCache::resetAdmission() {
  clauseAdmission.reset();
  wholeAdmission.reset();
}

void FilterCache::resetCounters() {
  counter.hits.reset();
  counter.misses.reset();
  counter.buildNanos.store(0, std::memory_order_relaxed);
  counter.estimatedSavedNanos.reset();
  counter.unpaidBuildNanos.store(0, std::memory_order_relaxed);
  counter.admissions.store(0, std::memory_order_relaxed);
  counter.buildAttempts.store(0, std::memory_order_relaxed);
  counter.builds.store(0, std::memory_order_relaxed);
  counter.byproductInserts.store(0, std::memory_order_relaxed);
  counter.publishRejects.store(0, std::memory_order_relaxed);
  counter.evictions.store(0, std::memory_order_relaxed);
  counter.unrepaidEvictions.store(0, std::memory_order_relaxed);
  counter.populationDeferred.store(0, std::memory_order_relaxed);
  counter.capacityDeferred.store(0, std::memory_order_relaxed);
  counter.assumedCharges.reset();
  counter.observedCharges.reset();
  counter.assumedCredits.reset();
  counter.observedCredits.reset();
  counter.purges.store(0, std::memory_order_relaxed);
  counter.oversizedKeyBypasses.store(0, std::memory_order_relaxed);
  counter.readerStableHits.store(0, std::memory_order_relaxed);
  counter.readerStableRefreshes.store(0, std::memory_order_relaxed);
  counter.readerStableRetires.store(0, std::memory_order_relaxed);
}

size_t FilterCache::slotCountForTest(const FilterKey& key) {
  auto entry = findEntry(key);
  return entry == nullptr
      ? 0 : entry->slots.load(std::memory_order_acquire)->size();
}

std::vector<FilterCache::EntryDump> FilterCache::dump(size_t limit,
                                                      size_t* totalResident, bool includeGhosts) {
  std::vector<EntryDump> rows;
  size_t resident = 0;
  entries.cvisit_all([&](const auto& item) {
    const FilterKey& key = item.first;
    const std::shared_ptr<FilterEntry>& entry = item.second;
    EntryDump row;
    size_t valueCount = residentValueCount(*entry) + 1;
    auto addLedger = [&](Ledger& ledger, ValueDump value) {
      std::lock_guard<Ledger> lock(ledger);
      if (!value.resident && !includeGhosts) return;
      value.owed = ledger.owed;
      value.debt = ledger.debt.load(std::memory_order_relaxed);
      double price = productionNanos(ledger, value.maxDoc, ledger.observed, ledger.wholeQuery);
      value.productionNanos = std::max(0.0, price);
      value.productionSource = ledger.measured ? "measured" : price < 0 ? "unknown"
          : ledger.wholeQuery ? "absent" : "prior";
      value.absentNanos = ledger.absent.nanos;
      value.absentCount = ledger.absent.count;
      value.servedNanos = ledger.served.nanos;
      value.servedCount = ledger.served.count;
      value.referenceBytes = ledger.referenceBytes;
      value.rawCardinality = ledger.rawCardinality.load(std::memory_order_relaxed);
      value.recent = ledger.recent;
      value.recentEpochs = ledger.recentEpochs;
      value.work = ledger.work;
      if (!value.resident && ledger.referenceBytes) {
        value.residencyBytes = effectiveValueBytes(*entry, ledger.referenceBytes, valueCount);
      }
      row.values.push_back(value);
    };
    auto readerValue = entry->readerValue.load(std::memory_order_acquire);
    if (entry->scope == FilterKeyScope::READER_STABLE) {
      row.readerValue = true;
      addLedger(entry->ledger, {.readerVersion = readerValue == nullptr ? 0 : readerValue->readerVersion(),
          .resident = readerValue != nullptr,
          .creditedTotal = readerValue == nullptr ? 0 : readerValue->creditedTotal.load(std::memory_order_relaxed),
          .residencyBytes = readerValue == nullptr ? 0 : readerValue->residencyBytes,
          .published = readerValue == nullptr ? 0 : readerValue->published});
    }
    if (readerValue != nullptr) {
      row.readerValue = true;
      row.bytes += readerValue->ramBytesUsed();
      row.buildNanos += readerValue->buildCostNanos;
    }
    auto slots = entry->slots.load(std::memory_order_acquire);
    for (const auto& slot : *slots) {
      auto value = slot->value.load(std::memory_order_acquire);
      addLedger(slot->ledger, {.segId = slot->segId, .maxDoc = slot->maxDoc, .space = slot->space,
          .resident = value != nullptr,
          .creditedTotal = value == nullptr ? 0 : value->creditedTotal.load(std::memory_order_relaxed),
          .residencyBytes = value == nullptr ? 0 : value->residencyBytes,
          .published = value == nullptr ? 0 : value->published});
      if (value == nullptr) continue;
      row.segmentsResident++;
      row.bytes += value->ramBytesUsed();
      row.buildNanos += value->buildCostNanos;
    }
    if (row.bytes != 0) resident++;
    else if (!includeGhosts || row.values.empty()) return;
    row.keyHash = key.hash();
    row.keyBytes = key.bytes().size();
    row.scope = entry->scope;
    row.lastUsed = entry->lastUsed.load(std::memory_order_relaxed);
    // Printable-run extraction: runs of 3+ printable bytes, space-joined,
    // capped. Field names and terms in the serialized key stay readable.
    std::string& text = row.keyText;
    std::string run;
    for (std::byte b : key.bytes()) {
      char c = (char)b;
      if (c >= 0x20 && c < 0x7f) {
        run.push_back(c);
        continue;
      }
      if (run.size() >= 3) {
        if (!text.empty()) text.push_back(' ');
        text.append(run);
      }
      run.clear();
      if (text.size() >= 96) break;
    }
    if (run.size() >= 3 && text.size() < 96) {
      if (!text.empty()) text.push_back(' ');
      text.append(run);
    }
    if (text.size() > 96) text.resize(96);
    rows.push_back(std::move(row));
  });
  if (totalResident != nullptr) *totalResident = resident;
  std::sort(rows.begin(), rows.end(), [](const EntryDump& a, const EntryDump& b) {
    return a.bytes > b.bytes;
  });
  if (rows.size() > limit) rows.resize(limit);
  std::unordered_map<uint64_t, ValueDump*> values;
  for (auto& row : rows) for (auto& value : row.values) {
    if (value.published) values.emplace(value.published, &value);
  }
  std::lock_guard<std::mutex> lock(residentsMutex);
  for (const auto& resident : residents) {
    auto it = values.find(resident.published);
    if (it == values.end()) continue;
    auto& value = *it->second;
    value.rankedRate = resident.priority * value.residencyBytes;
    value.spokenFor = resident.spokenFor;
  }
  return rows;
}

FilterCache::CounterValues FilterCache::counters() const {
  CounterValues result{
      .hits = counter.hits.load(),
      .misses = counter.misses.load(),
      .admissions = counter.admissions.load(std::memory_order_relaxed),
      .buildAttempts = counter.buildAttempts.load(std::memory_order_relaxed),
      .builds = counter.builds.load(std::memory_order_relaxed),
      .buildNanos = counter.buildNanos.load(std::memory_order_relaxed),
      .estimatedSavedNanos = counter.estimatedSavedNanos.load(),
      .unpaidBuildNanos = counter.unpaidBuildNanos.load(std::memory_order_relaxed),
      .byproductInserts = counter.byproductInserts.load(std::memory_order_relaxed),
      .publishRejects = counter.publishRejects.load(std::memory_order_relaxed),
      .evictions = counter.evictions.load(std::memory_order_relaxed),
      .unrepaidEvictions = counter.unrepaidEvictions.load(
          std::memory_order_relaxed),
      .populationDeferred = counter.populationDeferred.load(
          std::memory_order_relaxed),
      .capacityDeferred = counter.capacityDeferred.load(std::memory_order_relaxed),
      .purges = counter.purges.load(std::memory_order_relaxed),
      .oversizedKeyBypasses = counter.oversizedKeyBypasses.load(
          std::memory_order_relaxed),
      .readerStableHits = counter.readerStableHits.load(
          std::memory_order_relaxed),
      .readerStableRefreshes = counter.readerStableRefreshes.load(
          std::memory_order_relaxed),
      .readerStableRetires = counter.readerStableRetires.load(
          std::memory_order_relaxed),
      .assumedCharges = counter.assumedCharges.load(),
      .observedCharges = counter.observedCharges.load(),
      .assumedCredits = counter.assumedCredits.load(),
      .observedCredits = counter.observedCredits.load(),
      .residentBytes = residentBytes.load(std::memory_order_relaxed),
      .metadataBytes = metadataBytes.load(std::memory_order_relaxed)};
  double debtNanos = 0;
  entries.cvisit_all([&](const auto& item) {
    auto add = [&](Ledger& ledger) {
      std::lock_guard<Ledger> lock(ledger);
      debtNanos += ledger.debt.load(std::memory_order_relaxed);
    };
    if (item.second->scope == FilterKeyScope::READER_STABLE) add(item.second->ledger);
    auto slots = item.second->slots.load(std::memory_order_acquire);
    for (const auto& slot : *slots) add(slot->ledger);
  });
  result.outstandingDebtNanos = nanosFromWork(debtNanos);
  return result;
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

  auto validateValue = [](const auto& value) {
    if (!std::isfinite(value.residencyBytes) || value.residencyBytes < value.ramBytesUsed()
        || value.published == 0) {
      throw std::logic_error("FilterCache validation: invalid resident rank");
    }
  };
  size_t expectedResidentBytes = 0;
  size_t expectedMetadataBytes = 0;
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
    if (entry.slotsCharge != slots->size() * SLOT_METADATA_BYTES) {
      throw std::logic_error(
          "FilterCache validation: entry slot charge mismatch");
    }
    expectedMetadataBytes += entry.slotsCharge;
    auto readerValue = entry.readerValue.load(std::memory_order_acquire);
    auto validateLedger = [](Ledger& ledger) {
      std::lock_guard<Ledger> lock(ledger);
      if (!std::isfinite(ledger.work) || ledger.work < 0
          || !std::isfinite(ledger.recent) || ledger.recent < 0
          || !std::isfinite(ledger.owed) || ledger.owed < 0
          || !std::isfinite(ledger.debt.load(std::memory_order_relaxed))
          || ledger.debt.load(std::memory_order_relaxed) < 0) {
        throw std::logic_error("FilterCache validation: invalid ledger");
      }
    };
    if (entry.scope == FilterKeyScope::READER_STABLE) validateLedger(entry.ledger);
    if (entry.scope == FilterKeyScope::READER_STABLE) {
      if (!slots->empty()) {
        throw std::logic_error(
            "FilterCache validation: reader-stable entry owns segment slots");
      }
      if (readerValue != nullptr) {
        validateValue(*readerValue);
        if (readerValue->readerVersion() != currentReaderVersion
            || readerValue->readerVersion() != active->readerVersion) {
          throw std::logic_error(
              "FilterCache validation: stale reader-stable value is resident");
        }
        if (readerValue->segments.size() != active->bundleCount) {
          throw std::logic_error(
              "FilterCache validation: reader-stable segment count mismatch");
        }
        size_t expectedReaderCharge = sizeof(ReaderValue)
            + readerValue->segments.capacity()
                * sizeof(ReaderValue::SegmentDocs);
        for (const auto& segment : readerValue->segments) {
          SegmentIdentity identity{segment.segId, segment.maxDoc, segment.space};
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
    SegmentIdentity previousIdentity{};
    bool first = true;
    for (const auto& slot : *slots) {
      if (!first && !identityLess(previousIdentity, {slot->segId, slot->maxDoc, slot->space})) {
        throw std::logic_error(
            "FilterCache validation: slot segIds are not strictly sorted");
      }
      first = false;
      previousIdentity = {slot->segId, slot->maxDoc, slot->space};
      validateLedger(slot->ledger);
      if (!slot->active.load(std::memory_order_acquire)) {
        throw std::logic_error(
            "FilterCache validation: resident entry retains inactive slot");
      }
      auto value = slot->value.load(std::memory_order_acquire);
      if (value == nullptr) continue;
      validateValue(*value);
      if (entry.scope == FilterKeyScope::READER_STABLE) {
        throw std::logic_error(
            "FilterCache validation: reader-stable entry has raw value");
      }
      #ifndef NDEBUG
      if (value->identityForTest.segId != slot->segId
          || value->identityForTest.maxDoc != slot->maxDoc
          || value->identityForTest.space != slot->space) {
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
