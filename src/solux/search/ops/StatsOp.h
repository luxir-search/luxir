#pragma once
#include "SearchOp.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/AtomicMerger.h"
#include "solux/util/SegmentMergeDriver.h"
#include "solux/util/NumericUtils.h"
#include "solux/search/ops/DomainIter.h"

namespace solux {
class AvgOp : public SearchOp {
public:
  const std::string_view fieldName; // the name of the field to calculate the average for
  // INT, FLOAT, or DOUBLE - FLOAT/DOUBLE columns hold sortable bits that
  // must be decoded before any arithmetic (their sum as raw bits is
  // meaningless even though their order is correct).
  const FieldType::Type valType;

  AvgOp(SearchRequest& req, const std::string_view& name, const std::string_view& fieldName,
        FieldType::Type valType = FieldType::INT)
    : SearchOp(req, name), fieldName(fieldName), valType(valType) {
  }

  bool isFloating() const {
    return valType == FieldType::FLOAT || valType == FieldType::DOUBLE;
  }

  double decodeDouble(int64_t raw) const {
    switch (valType) {
      case FieldType::FLOAT: return (double)sortableInt32ToFloat((int32_t)raw);
      case FieldType::DOUBLE: return sortableInt64ToDouble(raw);
      default: return (double)raw;
    }
  }

  class MergeableSum : public MergeableData {
  public:
    __int128 sum = 0;  // sum of int values; 128-bit keeps it exact and avoids
                       // the signed int64 overflow UB a full-range sum would hit
    double dsum = 0;   // sum of decoded float/double values in this segment
    int64_t count = 0; // number of values summed in this segment

    static MergeableSum* merge(MergeableSum* a, MergeableSum* b) {
      a->sum += b->sum;
      a->dsum += b->dsum;
      a->count += b->count;
      return a;
    }
  };
  class Calc : public Calculator {
    SegmentMergeDriver<MergeableSum> driver;

    void emitResult(double avg) {
      auto* myVal = getTarget(nullptr, [&](solux::api::Val& val) {
        if (slot >= 0) {
          // do array creation with mutex held since different buckets could be calculated in parallel
          auto& arr = oneofMut<solux::api::ArrDouble>(val);
          if (arr.v.empty()) {
            build::allocArray(arr.v, numSlots, op.req.lastResponse->mr);
          }
        }
      });
      if (slot == -1) {
        myVal->kind.emplace<double>(avg);
      } else {
        auto& arr = oneofMut<solux::api::ArrDouble>(*myVal);
        const_cast<double*>(arr.v.data())[slot] = avg;
      }
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(), [this](std::unique_ptr<MergeableSum> m) {
          // one of sum/dsum is always 0 (a field is either int or floating); an
          // empty index / no-value field gives 0/0 == NaN, matching the prior
          // empty-index behavior.
          emitResult(((double) m->sum + m->dsum) / (double) m->count);
        }) {
    }
    AvgOp& thisOp() {
      return (AvgOp&)getOp();
    }

    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {return nullptr;};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      //LOG_DEBUG("calc AvgOp: this={} segnum={}, domain={} slot={}", (void*)this, segnum, (void*)domain, slot);
      if (segnum == -1) {
        driver.completeEmpty();
        return;
      }
      driver.contribute([&](MergeableSum& data) {
        SegFieldInfo segFieldInfo;
        bool floating = thisOp().isFloating();
        auto& postingsReader = thisOp().req.reader->segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(poolGuard.pool(), postingsReader);
        if (!fieldReader.seek(thisOp().fieldName)) {
          return; // no value for this field in this segment; still a contribution
        }
        fieldReader.readFieldInfo(segFieldInfo);
        IntColReader intColReader(postingsReader, segFieldInfo);
        __int128 sum = 0;  // 128-bit: exact and overflow-free over full-range int64 values
        double dsum = 0;
        int64_t count = 0;
        int64_t missing = 0; // avg ignores docs with no value; not tracked here
        // Domain-driven walk: forEachIntColValue picks the sparse/dense iterator
        // by domain type instead of the previous full 0..maxDoc scan with a
        // virtual DocSet::get() (a binary search for array domains) per doc.
        forEachIntColValue(domain, intColReader, maxDoc, missing,
          [&](int32_t docid, int64_t val) SOLUX_INLINE {
            unused(docid);
            if (floating) {
              dsum += thisOp().decodeDouble(val);
            } else {
              sum += val;
            }
            count++;
          });
        data.sum += sum;
        data.dsum += dsum;
        data.count += count;
      });
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }

  InlineCalculator* createInlineCalculator(Calculator* parent, int64_t slot, int64_t numSlots) override {
    return new InlineCalc(*this, parent, slot, numSlots);
  }

  bool canInline() override {
    return true; // we can inline this operation
  }

  class InlineCalc final : public InlineCalculator {
    // Named Entry (not entry): the InlineCalculator methods take a parameter
    // named "entry" that would shadow the type, silently turning
    // sizeof(Entry) into sizeof(void*).
    struct Entry {
      double val;
      // number of field values summed into val.  The average divides by this
      // (matching the non-inline Calc), not by the bucket's doc count: docs
      // can be missing the field or carry multiple values.
      int64_t count;
    };

    std::optional<IntColReader> intColReader;
    std::optional<IntColReader::Iterator> intColIter;

  public:
    InlineCalc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : InlineCalculator(op, parent, slot, numSlots) {
    }

    ~InlineCalc() override = default;
    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {return nullptr;};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {};

    AvgOp& thisOp() {
      return (AvgOp&)getOp();
    }

    int insert(void* entry, int32_t docid, int space) override {
      if (space < (int)sizeof(Entry)) {
        return -(int)sizeof(Entry); // not enough space to insert
      }
      auto* e = (Entry*)entry;
      e->val = 0.0;
      e->count = 0;
      return update(entry, docid);
    }

    int update(void* entry, int32_t docid) override {
      auto* e = (Entry*)entry;
      if (intColIter) {
        if (intColIter->docId() < docid) {
          intColIter->advance(docid);
        }
        if (intColIter->docId() == docid) {
          if (!intColReader->multiValued()) {
            e->val += thisOp().decodeDouble(intColIter->value());
            e->count++;
          } else {
            auto [start, end] = intColReader->getStartEndValueRank(intColIter->rank());
            for (int64_t vrank = start; vrank < end; vrank++) {
              e->val += thisOp().decodeDouble(intColIter->values().valueAt(vrank));
            }
            e->count += end - start;
          }
        }
      }
      return sizeof(Entry);
    }

    std::pair<int, int> merge(void* target, void* from) override {
      auto* e = (Entry*)target;
      auto* o = (Entry*)from;
      e->val += o->val;
      e->count += o->count;
      return {sizeof(Entry), sizeof(Entry)};
    }

    std::pair<int, int> mergeNew(void* target, void* from, int space) override {
      if (space < (int)sizeof(Entry)) {
        return {-(int)sizeof(Entry), (int)sizeof(Entry)}; // not enough space to insert
      }
      auto* e = (Entry*)target;
      e->val = 0.0;
      e->count = 0;
      return merge(target, from);
    }

    // count is the bucket's doc count and is unused: the average divides by
    // the number of field values seen (e->count).  A bucket with no values
    // reports 0.0.  Always return the entry size - the FacetMap entry walk
    // packs calc entries back to back, so a 0 return would desync any calcs
    // that follow.
    int finalize(void* entry, int64_t count) override {
      unused(count);
      auto* e = (Entry*)entry;
      if (e->count > 0) {
        e->val /= e->count; // calculate the average
      }
      return sizeof(Entry);
    }

    int compare(void* a, void* b, int& asize, int& bsize) override {
      auto* aentry = (Entry*)a;
      auto* bentry = (Entry*)b;
      asize = sizeof(Entry);
      bsize = sizeof(Entry);
      if (aentry->val < bentry->val) {
        return -1;
      } else if (aentry->val > bentry->val) {
        return 1;
      } else {
        return 0;
      }
    }

    void startSeg(int32_t segnum) override {
      intColIter.reset();
      intColReader.reset();
      auto guard = MemPool::threadLocalPoolGuard();
      auto& pool = guard.pool();
      SegFieldInfo segFieldInfo;
      auto& postingsReader = thisOp().req.reader->segments()[segnum].postingsReader();
      FieldReader fieldReader(pool, postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      if (!found) {
        return; // field not found, nothing to do
      }
      fieldReader.readFieldInfo(segFieldInfo);
      // this is a int field for now, so we need to read the value for each doc
      // and accumulate counts per value.
      intColReader.emplace(postingsReader, segFieldInfo);
      intColIter.emplace(*intColReader);
    }

    void endSeg(int32_t segnum) override {
      intColIter.reset();
      intColReader.reset();
    }

    void fillResult(std::span<char*> entries) override {
      auto* myVal = getTarget(nullptr, [&](solux::api::Val& val) {
             // do array creation with mutex held since different buckets could be calculated in parallel
             auto& arr = oneofMut<solux::api::ArrDouble>(val);
             if (arr.v.empty()) {
               build::allocArray(arr.v, entries.size(), op.req.lastResponse->mr);
             }
         });
      auto& arr = oneofMut<solux::api::ArrDouble>(*myVal);
      auto* data = const_cast<double*>(arr.v.data());
      for (auto i = 0u; i < entries.size(); i++) {
        auto e = *(Entry*)entries[i];
        data[i] = e.val;
        entries[i] += sizeof(Entry); // move to the next entry part
      }
    }
  };


};
}
