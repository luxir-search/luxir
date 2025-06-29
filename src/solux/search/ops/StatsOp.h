#pragma once
#include "SearchOp.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/AtomicMerger.h"

namespace solux {
class AvgOp : public SearchOp {
public:
  const std::string_view fieldName; // the name of the field to calculate the average for
  AvgOp(SearchRequest& req, const std::string_view& name, const std::string_view& fieldName)
    : SearchOp(req, name), fieldName(fieldName) {
  }
  class MergeableSum : public MergeableData {
  public:
    int64_t sum = 0; // sum of all values in this segment
    int64_t count = 0; // number of values summed in this segment

    static MergeableSum* merge(MergeableSum* a, MergeableSum* b) {
      a->sum += b->sum;
      a->count += b->count;
      return a;
    }
  };
  class Calc : public Calculator {
    AtomicMerger<MergeableSum> sumMerger;
  public:
    Calc(SearchOp& op, Calculator* parent)
      : Calculator(op, parent) {
    }
    AvgOp& thisOp() {
      return (AvgOp&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      std::unique_ptr<MergeableSum> mergeableData(sumMerger.obtain());
      SegFieldInfo segFieldInfo;
      int64_t sum = 0;
      int64_t count = 0;
      BitDocSet* bitDocs = (BitDocSet*) domain;
      auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;

      auto& postingsReader = thisOp().req.reader->segments()[segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      if (!found) {
        return;
      }
      fieldReader.readFieldInfo(segFieldInfo);
      // this is a int field for now, so we need to read the value for each doc
      // and accumulate counts per value.
      IntColReader intColReader(poolGuard.pool(), postingsReader, segFieldInfo);
      IntColReader::Iterator intColIter(intColReader);
      for (int32_t docid = 0; docid < maxDoc; docid++) {
        if (bitDocs && !bitDocs->get(docid)) {
          continue;
        }
        if (intColIter.docId() < docid ) {
          intColIter.advance(docid);
        }
        if (intColIter.docId() == docid) {
          if (!intColReader.multiValued()) {
            auto val = intColIter.value();
            sum += val;
            count++;
          } else {
            auto [start, end] = intColReader.getStartEndRank(intColIter.rank());
            auto n = end - start;
            for (int64_t vrank = 0; vrank < n; vrank++) {
              auto val = intColIter.values().valueAt(start + vrank);
              sum += val;
              count++;
            }
          }
        }
      }
      mergeableData->sum += sum;
      mergeableData->count += count;
      auto merged = sumMerger.release(mergeableData.release());
      if (merged == thisOp().req.reader->segments().size()) {
        auto* myVal = getTarget(nullptr);


        std::unique_ptr<MergeableSum> mergeableData(sumMerger.obtain());
        auto sum = mergeableData->sum;
        auto count = mergeableData->count;
        double avg = (double) sum / (double) count;
        myVal->set_d(avg);
      }
    }

  };

  Calculator* createCalculator(Calculator* parent, int64_t slot) override {
    return new Calc(*this, parent);
  };

};
}
