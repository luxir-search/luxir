#pragma once

#include "SearchOp.h"
#include "solux/util/thread.h"

namespace solux {

class RootOp : public SearchOp {
public:
  RootOp(SearchRequest& req) : SearchOp(req, "root") {
  }


  class Calc final : public SearchOp::Calculator {
    std::vector<std::unique_ptr<Calculator>> subCalcs;
  public:
    Calc(RootOp& op, SearchOp::Calculator* parent, int64_t slot, int64_t numSlots) : SearchOp::Calculator(op, parent, slot, numSlots) {}

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      searchResponse = searchResponse ? searchResponse : &op.req.lastResponse->proto;
      return &(*searchResponse->mutable_ops())[sub->getOp().name];
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) override {
      // create sub-calculators for each subOp
      subCalcs.reserve(op.subOps.size());
      for (auto [key, subOp] : op.subOps) {
        auto* subCalc = subOp->createCalculator(this, segnum);
        subCalcs.emplace_back(subCalc);
      }

      // Is it better in general to let the sub-calculators launch a new task for each calc() call,
      // or launch them all in parallel here?  If we let them launch their own tasks, then it
      // would be easier for them to use a different nested task group if they wanted to.
      // Future prepared-weight queries that need domains from all segments before
      // launching segment work could also use that child-owned scheduling shape:
      // RootOp would call them synchronously for each segment, and the child would
      // decide when and how to launch follow-up tasks.
      const auto& segs = op.req.reader->segments();
      for (auto& subCalc : subCalcs) {
        for (int32_t i = 0; i < (int32_t)segs.size(); i++) {
          // int32_t segnum = (int32_t)segs.size() - 1 - i; // launch in reverse order to get the first segments done first.
          int32_t segnum = i; // launch in order for later (smaller) segments to start in this thread first.
          auto& seg = segs[segnum];
          // For the domains, start with live docs.  This establishes the
          // PrepareContext domain contract (Query.h): a segment's domain,
          // when present, is live-filtered and is the complete eligibility
          // predicate; null means no deletes and no filters.
          DocSet* domainPtr = seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr;
          task_group_run(tg, [this, segnum, domainPtr, tg,  &subCalc]() {
            // call the calc method on each sub-calculator
              subCalc->calc(tg, segnum, domainPtr);
          });
        }

        // special case empty index reader
        if (segs.size() == 0) {
          // we are done, so we can call the callback
          subCalc->calc(tg, -1, nullptr);
        }
      }
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, nullptr, slot, numSlots);
  }

};

}
