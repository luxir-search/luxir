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

  class Calculator;
  virtual Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) = 0;

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
    solux::proto::Val* getTarget(solux::proto::SearchResponse* searchResponse) {
      std::lock_guard<std::mutex> lock(op.req.mutex);
      return parent->getTargetForSub(searchResponse, this);
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




};

}