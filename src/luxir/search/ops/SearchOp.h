#pragma once

#include <chrono>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include "luxir/search/DocSet.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/util/MappedAlloc.h"
#include "luxir/util/thread.h"


namespace luxir {
class DocSet;
struct FacetChildContext;
class FacetChildExecutor;

// --- Request-side (non-owning) proto message views ---
// Sub-messages of the request proto (ReqProto = SearchRequest<non_owning_traits>),
// borrowed from the kept-alive request bytes.  Ops hold const refs/views to these;
// the parser hands them in.  Spelled once here so the ops don't repeat the traits.
using ReqTopDocs = luxir::api::TopDocs;
using ReqFusion = luxir::api::Fusion;
using ReqFieldFacet = luxir::api::FieldFacet;
using ReqRangeFacet = luxir::api::RangeFacet;
using ReqQueryFacet = luxir::api::QueryFacet;
using ReqSortList = std::span<const luxir::api::SortSpec>;

struct CollectionRequirements {
  bool needRankedDocs = false;
  bool needExactCount = false;
  bool needExactDomain = false;
};

// --- Response-side (owning) oneof / optional mutators ---
// hpp-proto translation of protobuf's mutable_<oneof_arm>() / mutable_<message>():
// "return the active arm/value, creating a default one if not already present"
// (get-or-create).  Unlike a bare kind.emplace<Arm>(), this never clobbers an arm
// a sibling op/bucket already set - the response tree is assembled concurrently
// under req.mutex and multiple sub-ops share a parent Val's docs/facet arm.
template <typename Arm, typename Msg>
Arm& oneofMut(Msg& msg) {
  if (auto* p = std::get_if<Arm>(&msg.kind)) {
    return *p;
  }
  return msg.kind.template emplace<Arm>();
}
template <typename T>
T& optMut(std::optional<T>& opt) {
  if (!opt.has_value()) {
    opt.emplace();
  }
  return *opt;
}

class SearchOp {
public:
  SearchRequest& req;
  std::string_view name;
  SearchOp* parent = nullptr;  // set by parser after construction.
  boost::unordered_flat_map<std::string_view, SearchOp*> subOps;  // set by parser after construction.
  ExecutionProfileOpState* executionProfile = nullptr;

  // We can't pass both the parent and children to constructors (and have them fully formed)
  // one has to come before the other.  The parser currently sets subOps and parent
  // after construction, so do not inspect these fields in the constructor.
  SearchOp(SearchRequest& req, std::string_view name) : req(req), name(name) {
  }

  virtual void init() {
    for (auto& [name, subOp] : subOps) {
      subOp->init();
    }
  }

  class Calculator;
  class InlineCalculator;
  virtual Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) = 0;
  virtual InlineCalculator* createInlineCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) {
    return nullptr;
  }
  virtual bool canInline() {
    return false;
  }

  virtual bool canEmitAsBucketChild() const {
    return false;
  }

  // Resident state retained by one ordinary per-bucket calculator while its
  // segments are fed. Stateless operations keep the default zero estimate.
  virtual size_t facetBucketResidentBytes() const {
    return 0;
  }

  // Result-stage facet children may bind to parent-produced sources. The
  // default operation has no specialized binding and uses BUCKET_DOMAINS.
  // The returned executor is owned by the parent coordinator.
  virtual FacetChildExecutor* bindFacetChild(
      const FacetChildContext& context) {
    unused(context);
    return nullptr;
  }

  virtual ~SearchOp() = default;

  // Opt an operation into request profiling. Other operations can adopt this
  // hook independently without changing request dispatch or response assembly.
  void enableExecutionProfile() {
    executionProfile = req.addExecutionProfileOp(name);
  }

  ExecutionProfileRun* addExecutionProfileRun() {
    return req.addExecutionProfileRun(executionProfile);
  }

  class Calculator {
  protected:
    SearchOp& op;
    Calculator* parent;
    int64_t slot; // the slot this calculator is for, or -1 if not applicable
    int64_t numSlots;


  public:
    class ExecutionProfileScope {
      ExecutionProfilePieceState* state;
      std::chrono::steady_clock::time_point started;

    public:
      ExecutionProfileScope(ExecutionProfilePieceState* state, int32_t segnum) : state(state) {
        if (state == nullptr) return;
        state->wire.kind = "segment";
        state->wire.segment = segnum;
        state->wire.thread_id = (int64_t)::syscall(SYS_gettid);
        started = std::chrono::steady_clock::now();
      }
      ExecutionProfileScope(const ExecutionProfileScope&) = delete;
      ExecutionProfileScope& operator=(const ExecutionProfileScope&) = delete;
      ~ExecutionProfileScope() {
        if (state == nullptr) return;
        state->wire.elapsed_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        state->complete = true;
      }

      // Hand ops the full piece state: typed wire fields plus the owned
      // detail string for composed human-readable text.
      ExecutionProfilePieceState* get() {
        return state;
      }
    };

    Calculator(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots) : op(op), parent(parent), slot(slot), numSlots(numSlots) {}

    // Route an emitter to its ordinary Val or to its bucket slot. Array
    // creation and the visitor both run under the response lock when this is
    // called from getTarget's visitor.
    template <typename ArrayArm, typename Visitor>
    void routeTarget(luxir::api::Val& val, std::pmr::memory_resource& mr,
                     Visitor&& visitor) {
      if (slot == -1) {
        visitor(val);
        return;
      }
      assert(slot >= 0);
      assert(slot < numSlots);
      auto& arr = oneofMut<ArrayArm>(val);
      if (arr.v.empty()) {
        build::allocArray(arr.v, (size_t)numSlots, mr);
      }
      using Element = std::remove_const_t<typename decltype(arr.v)::element_type>;
      visitor(const_cast<Element&>(arr.v[(size_t)slot]));
    }

    ExecutionProfileScope profilePiece(ExecutionProfileRun* run, int32_t segnum) {
      ExecutionProfilePieceState* state =
          run == nullptr || segnum < 0 ? nullptr : &run->pieces[(size_t)segnum];
      return ExecutionProfileScope(state, segnum);
    }

    // --- Result assembly: getTarget() / getTargetForSub() ---
    // (1) BUBBLE-TO-ROOT slot resolution. A sub-op asks its PARENT for its spot
    //     (getTargetForSub bubbles up the Calculator chain to the root op, which owns the
    //     response). Each level builds its piece of the nesting path in the NON-OWNING
    //     response - sets the enclosing Val's variant arm and gets-or-creates the sub's
    //     entry in the ops map - and returns a stable Val* slot for this op to fill. The
    //     `resp` (SearchResponse, carrying both proto + arena `mr`) selects WHICH response
    //     object to build into (see (3)); when null, getTarget resolves it to the request's
    //     current/accumulating response (req.lastResponse) so getTargetForSub is non-null.
    // (2) POINTER STABILITY. The returned Val* must stay valid while the op fills it and
    //     while sibling ops add other slots concurrently. It holds because build::opsSlot
    //     pre-allocates the ops backing array once (sized to the parent op's subOps) and
    //     allocates each Val separately in `resp->mr` - so a Val's address never moves as
    //     siblings fill other slots - and ALL assembly happens under op.req.mutex.
    // (3) INCREMENTAL / STREAMING EMISSION. To emit an intermediate partial result, an op
    //     creates a FRESH SearchResponse, getTarget()s into IT (the same bubble builds the
    //     path into that object, backed by its own mr), fills the slot, the handler
    //     encode's it and respondRaw(..., more=true), then it is dropped (per-emit
    //     eager-drop). The accumulating/final response is untouched; multiple intermediates
    //     may precede the final, which goes out more=false.
    luxir::api::Val* getTarget(SearchResponse* resp, auto&& visitor) {
      std::lock_guard<std::mutex> lock(op.req.mutex);
      resp = resp ? resp : op.req.lastResponse;
      auto* val = parent->getTargetForSub(resp, this);
      visitor(*val); // call the visitor with the target Val with mutex held.
      return val;
    }
    luxir::api::Val* getTarget(SearchResponse* resp) {
      return getTarget(resp, [](luxir::api::Val&){});
    }

    // Called by a subCalculator on us to get the target for the subCalculator to set.
    // `resp` is non-null (getTarget resolves it). Do not call this without op.req.mutex
    // held to protect the response from concurrent modifications.
    virtual luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) = 0;

    SearchOp& getOp() {
      return op;
    }

    int64_t getSlot() const {
      return slot;
    }

    Calculator* getParent() const {
      return parent;
    }

    // If domain.get()==nullptr, then the domain consists of all documents in
    // the segment. Non-null domains crossing this boundary carry their
    // lifetime with them.
    // if segnum == -1, then this is called not for a specific segment, but for the whole index.
    // segnum=-1 is also used for an empty index reader (no segments).
    virtual void calc(oneapi::tbb::task_group* tg, int32_t segnum,
                      DomainHandle domain) = 0;

    // Whole-index delivery. domains[i] is the domain for reader segment i;
    // an empty span represents an empty index. The default implementation
    // fans out the existing per-segment contract, while calculators that need
    // an index-wide view can plan directly from the complete input.
    //
    // The span storage is borrowed only for this call. Async work copies the
    // individual handles, never the span.
    virtual void calcAll(oneapi::tbb::task_group* tg,
                         std::span<const DomainHandle> domains) {
      assert(domains.size() == op.req.reader->segments().size());
      if (domains.empty()) {
        calc(tg, -1, {});
        return;
      }
      // task_group runs locally submitted tasks from a stack. Ascending
      // submission lets later segments, which are typically smaller, begin
      // on this thread first while earlier segments are available to steal.
      for (int32_t segnum = 0; segnum < (int32_t)domains.size(); segnum++) {
        DomainHandle domain = domains[(size_t)segnum];
        assert(domain.isDeliverable());
        task_group_run(tg, [this, tg, segnum, domain]() {
          calc(tg, segnum, domain);
        });
      }
    }

    virtual ~Calculator() = default;
  };


  class InlineCalculator : public Calculator {
  public:
    InlineCalculator(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots) {
    }

    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override { return nullptr; }
    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domain) override {};
    virtual void startSeg(int32_t segnum) {};
    virtual void endSeg(int32_t segnum) {};
    virtual uint32_t fixedEntryBytes() const = 0;
    virtual void insert(void* entry, int32_t docid) = 0;
    virtual void update(void* entry, int32_t docid) = 0;
    virtual void merge(void* target, void* from) = 0;
    // mergeNew is called when entry did not exist for target
    virtual void mergeNew(void* target, void* from) = 0;
    virtual void beginFinalize(size_t entries) = 0;
    virtual void finalize(void* entry, int64_t count) = 0;
    virtual int compare(size_t a, size_t b) = 0;
    virtual bool isMissing(size_t entry) {
      unused(entry);
      return false;
    }
    virtual void fillResult(std::span<char*> entries,
                            std::span<const int64_t> counts) = 0;

  };

};

// Fixed-stride inline facet entries keyed by global ordinal. Dense entries use
// one flat ordinal-indexed allocation plus touched order; sparse entries use map
// iteration order. Each representation keeps one stable order from finalize
// through selection and result fill.
class OrdinalFacetEntryTable {
  enum class Rep { UNINITIALIZED, SPARSE, DENSE };

  // Initial, refittable crossover constants. The 64 MiB entry-array cap admits
  // the target 2M-ord average (25-byte stride = 50 MiB) per collector. Small
  // tables are dense unconditionally; above that, expected values must cover
  // at least one quarter of the ordinal space before direct indexing repays
  // dense storage. Forced modes keep both sides measurable.
  static constexpr size_t DENSE_ENTRY_BYTES_CAP = 64 * 1024 * 1024;
  static constexpr int64_t DENSE_SMALL_ORDS = 4 * 1024;
  static constexpr uint32_t MAX_SPARSE_ENTRY_BYTES =
      MemPool::BYTE_BLOCK_SIZE - MemPool::HEADER_SIZE;
  // boost::unordered_flat_map's open-addressed slot array needs spare slots;
  // two value slots per touched ordinal is a conservative budget reservation.
  static constexpr size_t SPARSE_HASH_BYTES_PER_ENTRY =
      2 * (sizeof(std::pair<int64_t, char*>) + 1);
  static constexpr std::string_view BREAKER = "facet aggregate state";

  boost::unordered_flat_map<int64_t, char*> sparseEntries;
  std::vector<int64_t> touchedOrds;
  std::vector<uint32_t> calcEntryBytes;
  MemPool entryPool;
  MappedAlloc denseMapping;
  char* denseMalloc = nullptr;
  char* denseBase = nullptr;
  std::span<SearchOp::InlineCalculator*> calcs;
  RequestMemTracker* tracker = nullptr;
  InlineFacetEntryStats* stats = nullptr;
  std::string chargeDetail;
  size_t sparseBytesCharged = 0;
  size_t touchedCapacityCharged = 0;
  size_t denseStorageBytesCharged = 0;
  size_t stride = sizeof(int64_t);
  int64_t numOrds = 0;
  Rep rep = Rep::UNINITIALIZED;

  void charge(size_t bytes) {
    if (bytes != 0) tracker->charge(bytes, BREAKER, chargeDetail);
  }

  void releaseCharge(size_t bytes) {
    if (bytes != 0) tracker->release(bytes);
  }

  void reserveTouchedCapacity(size_t newCapacity) {
    if (newCapacity <= touchedOrds.capacity()) return;
    size_t oldCapacity = touchedOrds.capacity();
    if (newCapacity > std::numeric_limits<size_t>::max()
                          / sizeof(int64_t)) {
      tracker->chargeOverflow(BREAKER, chargeDetail);
    }
    size_t bytes = (newCapacity - oldCapacity) * sizeof(int64_t);
    charge(bytes);
    try {
      touchedOrds.reserve(newCapacity);
    } catch (...) {
      releaseCharge(bytes);
      throw;
    }
    touchedCapacityCharged += bytes;
  }

  void ensureTouchedCapacity() {
    if (touchedOrds.size() < touchedOrds.capacity()) return;
    reserveTouchedCapacity(std::max<size_t>(8, touchedOrds.capacity() * 2));
  }

  char* denseEntryForIndex(size_t index) {
    return denseBase + index * stride;
  }

  bool tryInitializeDense(size_t logicalBytes, size_t expectedTouched) {
    bool mapped = logicalBytes >= mappedAllocationFloor;
    size_t storageBytes = mapped ? MappedAlloc::roundedSize(logicalBytes)
                                 : logicalBytes;
    if (expectedTouched > std::numeric_limits<size_t>::max()
                              / sizeof(int64_t)) {
      return false;
    }
    size_t touchedBytes = expectedTouched * sizeof(int64_t);
    if (storageBytes > std::numeric_limits<size_t>::max() - touchedBytes) {
      return false;
    }
    size_t reservation = storageBytes + touchedBytes;
    if (!tracker->tryCharge(reservation)) return false;
    try {
      if (expectedTouched != 0) touchedOrds.reserve(expectedTouched);
      if (!mapped) {
        denseMalloc = (char*)std::malloc(logicalBytes);
        if (denseMalloc == nullptr) throw std::bad_alloc();
        denseBase = denseMalloc;
        memset(denseBase, 0, logicalBytes);
      } else {
        denseMapping = MappedAlloc(logicalBytes);
        assert(denseMapping.size() == storageBytes);
        denseBase = (char*)denseMapping.data();
      }
    } catch (...) {
      std::free(denseMalloc);
      denseMalloc = nullptr;
      denseBase = nullptr;
      releaseCharge(reservation);
      std::vector<int64_t>().swap(touchedOrds);
      throw;
    }
    touchedCapacityCharged = touchedBytes;
    denseStorageBytesCharged = storageBytes;
    return true;
  }

  void selectSparseRep() {
    rep = Rep::SPARSE;
    if (stats != nullptr) {
      stats->sparseTables.fetch_add(1, std::memory_order_relaxed);
    }
  }

  char* sparseFirstTouch(int64_t ord) {
    size_t bytes = stride + SPARSE_HASH_BYTES_PER_ENTRY;
    charge(bytes);
    sparseBytesCharged += bytes;
    char* entry = entryPool.alloc(stride);
    auto [iter, inserted] = sparseEntries.emplace(ord, entry);
    unused(iter);
    assert(inserted);
    return entry;
  }

  char* findEntry(int64_t ord) {
    if (rep == Rep::DENSE) {
      assert(ord >= 0 && ord < numOrds);
      char* entry = denseEntryForIndex((size_t)ord);
      // Zero count is the absence sentinel. Inline mincount=0 deliberately
      // remains touched-only; untouched global ords are not synthesized.
      return loadUnaligned<int64_t>(entry) == 0 ? nullptr : entry;
    }
    auto iter = sparseEntries.find(ord);
    return iter == sparseEntries.end() ? nullptr : iter->second;
  }

  char* firstTouch(int64_t ord) {
    if (rep == Rep::DENSE) {
      assert(ord >= 0 && ord < numOrds);
      ensureTouchedCapacity();
      touchedOrds.push_back(ord);
      return denseEntryForIndex((size_t)ord);
    }
    return sparseFirstTouch(ord);
  }

  void insertEntry(char* entry, int32_t docid) {
    storeUnaligned<int64_t>(entry, 1);
    char* ptr = entry + sizeof(int64_t);
    if (calcs.size() == 1) {
      calcs.front()->insert(ptr, docid);
      assert(sizeof(int64_t) + calcEntryBytes.front() == stride);
      return;
    }
    for (size_t i = 0; i < calcs.size(); i++) {
      calcs[i]->insert(ptr, docid);
      ptr += calcEntryBytes[i];
    }
    assert((size_t)(ptr - entry) == stride);
  }

  void updateEntry(char* entry, int32_t docid) {
    storeUnaligned<int64_t>(entry, loadUnaligned<int64_t>(entry) + 1);
    char* ptr = entry + sizeof(int64_t);
    if (calcs.size() == 1) {
      calcs.front()->update(ptr, docid);
      return;
    }
    for (size_t i = 0; i < calcs.size(); i++) {
      calcs[i]->update(ptr, docid);
      ptr += calcEntryBytes[i];
    }
  }

  void mergeNewEntry(char* target, char* from) {
    storeUnaligned<int64_t>(target, loadUnaligned<int64_t>(from));
    target += sizeof(int64_t);
    from += sizeof(int64_t);
    for (size_t i = 0; i < calcs.size(); i++) {
      calcs[i]->mergeNew(target, from);
      target += calcEntryBytes[i];
      from += calcEntryBytes[i];
    }
  }

  void mergeEntry(char* target, char* from) {
    storeUnaligned<int64_t>(
        target, loadUnaligned<int64_t>(target) + loadUnaligned<int64_t>(from));
    target += sizeof(int64_t);
    from += sizeof(int64_t);
    for (size_t i = 0; i < calcs.size(); i++) {
      calcs[i]->merge(target, from);
      target += calcEntryBytes[i];
      from += calcEntryBytes[i];
    }
  }

public:
  OrdinalFacetEntryTable() = default;
  OrdinalFacetEntryTable(const OrdinalFacetEntryTable&) = delete;
  OrdinalFacetEntryTable& operator=(const OrdinalFacetEntryTable&) = delete;

  ~OrdinalFacetEntryTable() {
    std::free(denseMalloc);
    if (tracker != nullptr) {
      releaseCharge(denseStorageBytesCharged + sparseBytesCharged
                    + touchedCapacityCharged);
    }
  }

  void configure(std::span<SearchOp::InlineCalculator*> calculators,
                 RequestMemTracker& memoryTracker, int64_t globalOrds,
                 std::string detail, InlineFacetEntryStats* entryStats) {
    assert(rep == Rep::UNINITIALIZED);
    calcs = calculators;
    tracker = &memoryTracker;
    stats = entryStats;
    assert(globalOrds >= 0);
    numOrds = globalOrds;
    chargeDetail = std::move(detail);
    // Preserve the existing packed layout. Calculator state already uses
    // unaligned accessors, so padding 17-byte average state to 24/32 bytes
    // would spend bandwidth without buying legal aligned access.
    calcEntryBytes.reserve(calcs.size());
    for (auto* calc : calcs) {
      uint32_t bytes = calc->fixedEntryBytes();
      calcEntryBytes.push_back(bytes);
      if (bytes > std::numeric_limits<size_t>::max() - stride) {
        tracker->chargeOverflow(BREAKER, chargeDetail);
      }
      stride += bytes;
    }
    if (stride > MAX_SPARSE_ENTRY_BYTES) {
      throw std::length_error("inline facet entry exceeds MemPool chunk size");
    }
    if (stats != nullptr) {
      stats->entryStride.store(stride, std::memory_order_relaxed);
    }
  }

  void initialize(int64_t expectedValues, bool forceDense,
                  bool forceSparse) {
    if (rep != Rep::UNINITIALIZED) return;
    assert(!(forceDense && forceSparse));
    expectedValues = std::max<int64_t>(expectedValues, 0);
    size_t expectedTouched =
        (size_t)std::min<int64_t>(expectedValues, numOrds);
    bool denseSizeValid = numOrds >= 0 && stride != 0
        && (size_t)numOrds <= std::numeric_limits<size_t>::max() / stride;
    size_t denseBytes = denseSizeValid ? (size_t)numOrds * stride : 0;
    bool denseFitsCap = denseSizeValid
        && denseBytes <= DENSE_ENTRY_BYTES_CAP;
    int64_t quarterOrds = numOrds / 4 + (numOrds % 4 != 0);
    bool likelyDense = numOrds <= DENSE_SMALL_ORDS
        || expectedValues >= quarterOrds;
    bool wantDense = !forceSparse
        && (forceDense || (denseFitsCap && likelyDense));
    if (wantDense && numOrds > 0) {
      if (denseSizeValid && tryInitializeDense(denseBytes, expectedTouched)) {
        rep = Rep::DENSE;
        if (stats != nullptr) {
          stats->denseTables.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }
      if (stats != nullptr) {
        stats->denseFallbacks.fetch_add(1, std::memory_order_relaxed);
      }
    }
    selectSparseRep();
  }

  void add(int64_t ord, int32_t docid) {
    assert(rep != Rep::UNINITIALIZED);
    char* entry = findEntry(ord);
    if (entry == nullptr) {
      insertEntry(firstTouch(ord), docid);
    } else {
      updateEntry(entry, docid);
    }
  }

  void merge(OrdinalFacetEntryTable& other) {
    assert(rep != Rep::UNINITIALIZED || other.empty());
    if (rep == Rep::DENSE && !other.empty() && stats != nullptr) {
      stats->denseMerges.fetch_add(1, std::memory_order_relaxed);
      if (!other.dense()) {
        stats->mixedMerges.fetch_add(1, std::memory_order_relaxed);
      }
    }
    other.forEachEntry([&](int64_t ord, char* from) {
      char* target = findEntry(ord);
      if (target == nullptr) {
        mergeNewEntry(firstTouch(ord), from);
      } else {
        mergeEntry(target, from);
      }
    });
  }

  void finalize() {
    for (auto* calc : calcs) calc->beginFinalize(size());
    forEachEntry([&](int64_t ord, char* entry) {
      unused(ord);
      int64_t count = loadUnaligned<int64_t>(entry);
      char* ptr = entry + sizeof(int64_t);
      for (size_t i = 0; i < calcs.size(); i++) {
        calcs[i]->finalize(ptr, count);
        ptr += calcEntryBytes[i];
      }
    });
  }

  template<typename Accept>
  void forEachEntry(Accept&& accept) {
    if (rep == Rep::DENSE) {
      for (int64_t ord : touchedOrds) {
        size_t index = (size_t)ord;
        accept(ord, denseBase + index * stride);
      }
      return;
    }
    for (auto& [ord, entry] : sparseEntries) accept(ord, entry);
  }

  size_t size() const {
    return rep == Rep::SPARSE ? sparseEntries.size() : touchedOrds.size();
  }
  bool empty() const { return size() == 0; }
  bool dense() const { return rep == Rep::DENSE; }
};


}
