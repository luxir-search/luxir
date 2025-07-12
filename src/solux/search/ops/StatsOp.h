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
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots) {
    }
    AvgOp& thisOp() {
      return (AvgOp&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      //LOG_DEBUG("calc AvgOp: this={} segnum={}, domain={} slot={}", (void*)this, segnum, (void*)domain, slot);
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
        auto merged = sumMerger.release(mergeableData.release());
        checkCompletion(merged);
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
      checkCompletion(merged);
    }

    void checkCompletion(int64_t merged) {
      if (merged == thisOp().req.reader->segments().size()) {
        auto* myVal = getTarget(nullptr, [&](solux::proto::Val& val) {
          if (slot >= 0) {
            // do array creation with mutex held since different buckets could be calculated in parallel
            auto& arr = *val.mutable_arr_d();
            if (arr.v_size() == 0) {
              arr.mutable_v()->Resize(numSlots, 0.0);
            }
          }
        });


        std::unique_ptr<MergeableSum> mergeableData(sumMerger.obtain());
        auto sum = mergeableData->sum;
        auto count = mergeableData->count;
        double avg = (double) sum / (double) count;
        if (slot == -1) {
          myVal->set_d(avg);
        } else {
          auto& arr = *myVal->mutable_arr_d();
          arr.set_v(slot, avg);
        }
      }
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  };

  class InlineCalc final : public InlineCalculator {
    struct entry {
      double val;
    };

    MemPool pool;
    MemPool::save_point start;
    std::optional<FieldReader> fieldReader;
    std::optional<SegFieldInfo> segFieldInfo;
    std::optional<IntColReader> intColReader;
    std::optional<IntColReader::Iterator> intColIter;

  public:
    InlineCalc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : InlineCalculator(op, parent, slot, numSlots) {
    }
    AvgOp& thisOp() {
      return (AvgOp&)getOp();
    }

    int insert(void* entry, int32_t docid, int space) override {
      if (space < sizeof(entry)) {
        return -sizeof(entry); // not enough space to insert
      }
      auto* e = (struct entry*)entry;
      e->val = 0.0;
      return update(entry, docid);
    }

    int update(void* entry, int32_t docid) override {
      auto* e = (struct entry*)entry;
      if (intColIter) {
        if (intColIter->docId() < docid) {
          intColIter->advance(docid);
        }
        if (intColIter->docId() == docid) {
          //if (!intColReader->multiValued()) {
          e->val += intColIter->value();
          //} else {
          //auto [start, end] = intColReader->getStartEndRank(intColIter->rank());
          //auto n = end - start;
          //for (int64_t vrank = 0; vrank < n; vrank++) {
          //e->tot += intColIter->values().valueAt(start + vrank);
          //}
          //}
        }
      }
      return sizeof(entry);
    }

    std::pair<int, int> merge(void* target, void* from) override {
      auto* e = (struct entry*)target;
      auto* o = (struct entry*)from;
      e->val += o->val;
      return {sizeof(entry), sizeof(entry)};
    }

    std::pair<int, int> mergeNew(void* target, void* from, int space) override {
      if (space < sizeof(entry)) {
        return {-sizeof(entry), sizeof(entry)}; // not enough space to insert
      }
      auto* e = (struct entry*)target;
      e->val = 0.0;
      return merge(target, from);
    }

    int finalize(void* entry, int64_t count) override {
      auto* e = (struct entry*)entry;
      if (count == 0) {
        return 0; // no values, nothing to do
      }
      e->val /= count; // calculate the average
      return sizeof(entry);
    }

    int compare(void* a, void* b, int& asize, int& bsize) override {
      auto* aentry = (struct entry*)a;
      auto* bentry = (struct entry*)b;
      asize = sizeof(entry);
      bsize = sizeof(entry);
      if (aentry->val < bentry->val) {
        return -1;
      } else if (aentry->val > bentry->val) {
        return 1;
      } else {
        return 0;
      }
    }

    void startSeg(int32_t segnum) override {
      start = pool.getSavePoint();
      auto& postingsReader = thisOp().req.reader->segments()[segnum].postingsReader();
      fieldReader.emplace(pool, postingsReader);
      bool found = fieldReader->seek(thisOp().fieldName);
      if (!found) {
        return; // field not found, nothing to do
      }
      fieldReader->readFieldInfo(*segFieldInfo);
      // this is a int field for now, so we need to read the value for each doc
      // and accumulate counts per value.
      intColReader.emplace(pool, postingsReader, *segFieldInfo);
      intColIter.emplace(*intColReader);
    }

    void endSeg(int32_t segnum) override {
      intColIter.reset();
      intColReader.reset();
      fieldReader.reset();
      segFieldInfo.reset();
      pool.rewind(start);
    };
  };

};
}
