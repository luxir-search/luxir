#pragma once

#include "solux/search/SearchRequest.h"


namespace solux {
class DocSet;

class SearchOp {
public:
  SearchRequest& req;
  std::string_view name;
  SearchOp* parent = nullptr;  // set by parser after construction.
  boost::unordered_flat_map<std::string_view, SearchOp*> subOps;  // set by parser after construction.

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

  class Calculator {
  protected:
    SearchOp& op;
    Calculator* parent;
    int64_t slot; // the slot this calculator is for, or -1 if not applicable
    int64_t numSlots;


  public:
    Calculator(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots) : op(op), parent(parent), slot(slot), numSlots(numSlots) {}

    // Return the target Val for this calculator.
    // If searchResponse is not null, then the subOp wants the path created in the given searchResponse.
    solux::proto::Val* getTarget(solux::proto::SearchResponse* searchResponse, auto&& visitor) {
      std::lock_guard<std::mutex> lock(op.req.mutex);
      auto* val = parent->getTargetForSub(searchResponse, this);
      visitor(*val); // call the visitor with the target Val with mutex held.
      return val;
    }
    solux::proto::Val* getTarget(solux::proto::SearchResponse* searchResponse) {
      return getTarget(searchResponse, [](solux::proto::Val& val){});
    }

    // Called by a subCalculator on us to get the target for the subCalculator to set.
    // Do not call this without a lock held to protect the response from concurrent modifications.
    virtual solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) = 0;

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

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override { return nullptr; }
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
      *(int64_t*)ptr = 1;
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
      (*(int64_t*)ptr)++;
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
        *(int64_t*)ptr = *(int64_t*)otherPtr;
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
        (*(int64_t*)ptr) += *(int64_t*)otherPtr;
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
      auto count = (*(int64_t*)ptr);
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
