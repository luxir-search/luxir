#pragma once

#include <chrono>
#include <type_traits>
#include <sys/syscall.h>
#include <unistd.h>

#include "luxir/search/DocSet.h"
#include "luxir/search/SearchRequest.h"
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
    // Runs before FacetMap mutates its key map. Implementations may reject a
    // new bucket here; insert/update/merge callbacks must not throw.
    virtual void prepareEntry() {}
    virtual int insert(void* entry, int32_t docid, int space) = 0;
    virtual int update(void* entry, int32_t docid) = 0;
    virtual std::pair<int, int> merge(void* target, void* from) = 0;
    // mergeNew is called when entry did not exist for target
    virtual std::pair<int, int> mergeNew(void* target, void* from, int space) = 0;
    virtual void beginFinalize(size_t entries) = 0;
    virtual int finalize(void* entry, int64_t count) = 0;
    virtual int compare(size_t a, size_t b) = 0;
    virtual bool isMissing(size_t entry) {
      unused(entry);
      return false;
    }
    virtual void fillResult(std::span<char*> entries,
                            std::span<const size_t> finalizedSlots) = 0;

  };

};

// Maps a bucket key to a byte-packed entry carved out of `pool`:
//   [int64 doc count][one variable-size blob per inline calculator]
// Entries start wherever the previous one ended, so nothing in an entry can be
// assumed aligned - go through load/storeUnaligned here, and declare
// calculator entry structs LUXIR_UNALIGNED.
template <typename Key>
class FacetMap {
  using Iterator = typename boost::unordered_flat_map<Key, char*>::iterator;

  std::pair<Iterator, bool> findOrInsert(const Key& key) {
    auto iter = map.find(key);
    if (iter != map.end()) return {iter, false};
    for (auto* calc : calcs) calc->prepareEntry();
    auto [insertedIter, inserted] = map.insert({key, nullptr});
    assert(inserted);
    return {insertedIter, true};
  }

public:
  boost::unordered_flat_map<Key, char*> map;
  std::span<SearchOp::InlineCalculator*> calcs;
  MemPool pool;

  FacetMap(std::span<SearchOp::InlineCalculator*> calcs) : calcs(calcs) {}
  FacetMap() = default;

  void add(const Key& key, int32_t docid) {
    auto [iter, inserted] = findOrInsert(key);

    if (inserted) {
      auto ptr = pool.reserve(sizeof(int64_t));
      int space = (int)pool.spaceLeft();
      auto start = ptr;
      storeUnaligned<int64_t>(ptr, 1);
      space -= sizeof(int64_t);
      ptr += sizeof(int64_t);
      for (auto* calc : calcs) {
        auto calcSpace = calc->insert(ptr, docid, space);
        if (calcSpace < 0) {
          int used = (int)(ptr - start);
          auto newptr = pool.reserve(used + (-calcSpace));
          memcpy(newptr, start, used);
          start = newptr;
          ptr = newptr + used;
          space = (int)pool.spaceLeft() - used;
          calcSpace = calc->insert(ptr, docid, space);
          assert(calcSpace >= 0);
        }
        space -= calcSpace;
        ptr += calcSpace;
      }
      iter->second = start;  // store the pointer to the start of the entry
      auto allocated = pool.alloc(ptr - start);  // allocate the space used by this entry
      assert(allocated == start);
      unused(allocated);
    } else {
      auto ptr = iter->second;
      storeUnaligned<int64_t>(ptr, loadUnaligned<int64_t>(ptr) + 1);
      ptr += sizeof(int64_t);
      for (auto* calc : calcs) {
        auto calcSpace = calc->update(ptr, docid);
        assert(calcSpace >= 0);
        ptr += calcSpace;
      }
    }
  }

  void merge(FacetMap<Key>& other) {
    for (auto& [key, otherPtr] : other.map) {
      auto [iter, inserted] = findOrInsert(key);
      if (inserted) {
        auto ptr = pool.reserve(sizeof(int64_t));
        int space = (int)pool.spaceLeft();
        auto start = ptr;
        storeUnaligned<int64_t>(ptr, loadUnaligned<int64_t>(otherPtr));
        space -= sizeof(int64_t);
        ptr += sizeof(int64_t);
        otherPtr += sizeof(int64_t);
        for (auto* calc : calcs) {
          auto [calcSpace, otherCalcSpace] = calc->mergeNew(ptr, otherPtr, space);
          if (calcSpace < 0) {
            int used = (int)(ptr - start);
            auto newptr = pool.reserve(used + (-calcSpace));
            memcpy(newptr, start, used);
            start = newptr;
            ptr = newptr + used;
            space = (int)pool.spaceLeft() - used;
            std::tie(calcSpace, otherCalcSpace) = calc->mergeNew(ptr, otherPtr, space);
            assert(calcSpace >= 0);
          }
          space -= calcSpace;
          ptr += calcSpace;
          otherPtr += otherCalcSpace;
        }
        iter->second = start;  // store the pointer to the start of the entry
        auto allocated = pool.alloc(ptr - start);  // allocate the space used by this entry
        assert(allocated == start);
        unused(allocated);
      } else {
        auto ptr = iter->second;
        storeUnaligned<int64_t>(ptr, loadUnaligned<int64_t>(ptr) + loadUnaligned<int64_t>(otherPtr));
        ptr += sizeof(int64_t);
        otherPtr += sizeof(int64_t);
        for (auto* calc : calcs) {
          auto [calcSpace, otherCalcSpace] = calc->merge(ptr, otherPtr);
          assert(calcSpace >= 0);
          ptr += calcSpace;
          otherPtr += otherCalcSpace;
        }
      }
    }
  }

  void finalize() {
    for (auto* calc : calcs) calc->beginFinalize(map.size());
    for (auto iter : map) {
      auto ptr = iter.second;
      auto count = loadUnaligned<int64_t>(ptr);
      ptr += sizeof(int64_t);
      for (auto* calc : calcs) {
        auto calcSpace = calc->finalize(ptr, count);
        assert(calcSpace >= 0);
        ptr += calcSpace;
      }
    }
  }

};


}
