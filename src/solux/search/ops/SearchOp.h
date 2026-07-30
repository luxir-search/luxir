#pragma once

#include <chrono>
#include <sys/syscall.h>
#include <unistd.h>

#include "solux/search/SearchRequest.h"


namespace solux {
class DocSet;

// --- Request-side (non-owning) proto message views ---
// Sub-messages of the request proto (ReqProto = SearchRequest<non_owning_traits>),
// borrowed from the kept-alive request bytes.  Ops hold const refs/views to these;
// the parser hands them in.  Spelled once here so the ops don't repeat the traits.
using ReqTopDocs = solux::api::TopDocs;
using ReqFusion = solux::api::Fusion;
using ReqFieldFacet = solux::api::FieldFacet;
using ReqRangeFacet = solux::api::RangeFacet;
using ReqSortList = std::span<const solux::api::SortSpec>;

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
    solux::api::Val* getTarget(SearchResponse* resp, auto&& visitor) {
      std::lock_guard<std::mutex> lock(op.req.mutex);
      resp = resp ? resp : op.req.lastResponse;
      auto* val = parent->getTargetForSub(resp, this);
      visitor(*val); // call the visitor with the target Val with mutex held.
      return val;
    }
    solux::api::Val* getTarget(SearchResponse* resp) {
      return getTarget(resp, [](solux::api::Val&){});
    }

    // Called by a subCalculator on us to get the target for the subCalculator to set.
    // `resp` is non-null (getTarget resolves it). Do not call this without op.req.mutex
    // held to protect the response from concurrent modifications.
    virtual solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) = 0;

    SearchOp& getOp() {
      return op;
    }

    int64_t getSlot() const {
      return slot;
    }

    Calculator* getParent() const {
      return parent;
    }

    // If domain==nullptr, then the domain consists of all documents in the segment.
    // if segnum == -1, then this is called not for a specific segment, but for the whole index.
    // segnum=-1 is also used for an empty index reader (no segments).
    virtual void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) = 0;

    virtual ~Calculator() = default;
  };


  class InlineCalculator : public Calculator {
  public:
    InlineCalculator(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots) {
    }

    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override { return nullptr; }
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {};
    virtual void startSeg(int32_t segnum) {};
    virtual void endSeg(int32_t segnum) {};
    virtual int insert(void* entry, int32_t docid, int space) = 0;
    virtual int update(void* entry, int32_t docid) = 0;
    virtual std::pair<int, int> merge(void* target, void* from) = 0;
    // mergeNew is called when entry did not exist for target
    virtual std::pair<int, int> mergeNew(void* target, void* from, int space) = 0;
    virtual int finalize(void* entry, int64_t count) = 0;
    virtual int compare(void* a, void* b, int& asize, int& bsize) = 0;
    virtual void fillResult(std::span<char*>) = 0;

  };

};

// Maps a bucket key to a byte-packed entry carved out of `pool`:
//   [int64 doc count][one variable-size blob per inline calculator]
// Entries start wherever the previous one ended, so nothing in an entry can be
// assumed aligned - go through load/storeUnaligned here, and declare
// calculator entry structs SOLUX_UNALIGNED (see StatsOp::InlineCalc::Entry).
template <typename Key>
class FacetMap {
public:
  boost::unordered_flat_map<Key, char*> map;
  std::span<SearchOp::InlineCalculator*> calcs;
  MemPool pool;

  FacetMap(std::span<SearchOp::InlineCalculator*> calcs) : calcs(calcs) {}
  FacetMap() = default;

  void add(const Key& key, int32_t docid) {
    auto [iter, inserted] = map.insert({key, nullptr});

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
      auto [iter, inserted] = map.insert({key, nullptr});
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
