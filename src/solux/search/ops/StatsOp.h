#pragma once
#include <limits>

#include "SearchOp.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/AtomicMerger.h"
#include "solux/util/SegmentMergeDriver.h"
#include "solux/util/NumericUtils.h"
#include "solux/search/ops/DomainIter.h"

namespace solux {
// Single-field numeric stats over a domain: avg, min, or max.  One op instance
// computes one stat; the result is a double (NaN for an empty domain/bucket,
// which the JSON layer renders as null).
class StatsOp : public SearchOp {
public:
  enum Kind { AVG, MIN, MAX };

  const std::string_view fieldName; // the name of the field to compute the stat for
  // INT, FLOAT, or DOUBLE - FLOAT/DOUBLE columns hold sortable bits that
  // must be decoded before any arithmetic (their sum as raw bits is
  // meaningless even though their order is correct).  Because the order IS
  // correct, MIN/MAX compare raw bits directly and decode once at emit.
  const FieldType::Type valType;
  const Kind kind;

  StatsOp(SearchRequest& req, const std::string_view& name, const std::string_view& fieldName,
          FieldType::Type valType, Kind kind)
    : SearchOp(req, name), fieldName(fieldName), valType(valType), kind(kind) {
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

  class MergeableStats : public MergeableData {
  public:
    __int128 sum = 0;  // sum of int values; 128-bit keeps it exact and avoids
                       // the signed int64 overflow UB a full-range sum would hit
    double dsum = 0;   // sum of decoded float/double values in this segment
    int64_t count = 0; // number of values accumulated in this segment
    int64_t rawMin = INT64_MAX; // running extremes in raw (sortable-encoded) form;
    int64_t rawMax = INT64_MIN; // the encoding is order-preserving so raw compare = value compare

    static MergeableStats* merge(MergeableStats* a, MergeableStats* b) {
      a->sum += b->sum;
      a->dsum += b->dsum;
      a->count += b->count;
      a->rawMin = std::min(a->rawMin, b->rawMin);
      a->rawMax = std::max(a->rawMax, b->rawMax);
      return a;
    }
  };

  // The stat this op's kind selects, from fully merged accumulators.  An empty
  // domain gives NaN (for AVG naturally via 0/0).
  double finalValue(const MergeableStats& m) const {
    switch (kind) {
      case AVG:
        // one of sum/dsum is always 0 (a field is either int or floating)
        return ((double)m.sum + m.dsum) / (double)m.count;
      case MIN:
        return m.count ? decodeDouble(m.rawMin) : std::numeric_limits<double>::quiet_NaN();
      default: // MAX
        return m.count ? decodeDouble(m.rawMax) : std::numeric_limits<double>::quiet_NaN();
    }
  }

  class Calc : public Calculator {
    SegmentMergeDriver<MergeableStats> driver;

    void emitResult(double result) {
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
        myVal->kind.emplace<double>(result);
      } else {
        auto& arr = oneofMut<solux::api::ArrDouble>(*myVal);
        const_cast<double*>(arr.v.data())[slot] = result;
      }
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(), [this](std::unique_ptr<MergeableStats> m) {
          emitResult(thisOp().finalValue(*m));
        }) {
    }
    StatsOp& thisOp() {
      return (StatsOp&)getOp();
    }

    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {return nullptr;};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      if (segnum == -1) {
        driver.completeEmpty();
        return;
      }
      driver.contribute([&](MergeableStats& data) {
        SegFieldInfo segFieldInfo;
        auto& postingsReader = thisOp().req.reader->segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(postingsReader);
        if (!fieldReader.seek(thisOp().fieldName)) {
          return; // no value for this field in this segment; still a contribution
        }
        fieldReader.readFieldInfo(segFieldInfo);
        IntColReader intColReader(postingsReader, segFieldInfo);
        int64_t count = 0;
        int64_t missing = 0; // stats ignore docs with no value; not tracked here
        // Domain-driven walk: forEachIntColValue picks the sparse/dense iterator
        // by domain type.  The kind switch stays outside the loop so each case
        // keeps a tight per-value body.
        switch (thisOp().kind) {
          case AVG: {
            __int128 sum = 0;  // 128-bit: exact and overflow-free over full-range int64 values
            double dsum = 0;
            bool floating = thisOp().isFloating();
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
            break;
          }
          case MIN: {
            int64_t raw = INT64_MAX;
            forEachIntColValue(domain, intColReader, maxDoc, missing,
              [&](int32_t docid, int64_t val) SOLUX_INLINE {
                unused(docid);
                raw = std::min(raw, val);
                count++;
              });
            data.rawMin = std::min(data.rawMin, raw);
            break;
          }
          case MAX: {
            int64_t raw = INT64_MIN;
            forEachIntColValue(domain, intColReader, maxDoc, missing,
              [&](int32_t docid, int64_t val) SOLUX_INLINE {
                unused(docid);
                raw = std::max(raw, val);
                count++;
              });
            data.rawMax = std::max(data.rawMax, raw);
            break;
          }
        }
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
      // AVG: val is the running decoded sum (finalize turns it into the average).
      // MIN/MAX: bits is the running extreme in raw sortable-encoded form; the
      // encoding is order-preserving, so compare() works on the int64 and the
      // single decode happens in fillResult.
      union {
        double val;
        int64_t bits;
      };
      // number of field values accumulated.  AVG divides by this (not by the
      // bucket's doc count: docs can be missing the field or carry multiple
      // values); 0 marks an empty bucket, which emits NaN.
      int64_t count;
    };

    std::optional<IntColReader> intColReader;
    std::optional<IntColReader::Iterator> intColIter;

    void accum(Entry& e, int64_t raw) {
      switch (thisOp().kind) {
        case AVG: e.val += thisOp().decodeDouble(raw); break;
        case MIN: e.bits = std::min(e.bits, raw); break;
        case MAX: e.bits = std::max(e.bits, raw); break;
      }
      e.count++;
    }

  public:
    InlineCalc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : InlineCalculator(op, parent, slot, numSlots) {
    }

    ~InlineCalc() override = default;
    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {return nullptr;};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {};

    StatsOp& thisOp() {
      return (StatsOp&)getOp();
    }

    int insert(void* entry, int32_t docid, int space) override {
      if (space < (int)sizeof(Entry)) {
        return -(int)sizeof(Entry); // not enough space to insert
      }
      auto* e = (Entry*)entry;
      switch (thisOp().kind) {
        case AVG: e->val = 0.0; break;
        case MIN: e->bits = INT64_MAX; break;
        case MAX: e->bits = INT64_MIN; break;
      }
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
            accum(*e, intColIter->value());
          } else {
            auto [start, end] = intColReader->getStartEndValueRank(intColIter->rank());
            for (int64_t vrank = start; vrank < end; vrank++) {
              accum(*e, intColIter->values().valueAt(vrank));
            }
          }
        }
      }
      return sizeof(Entry);
    }

    std::pair<int, int> merge(void* target, void* from) override {
      auto* e = (Entry*)target;
      auto* o = (Entry*)from;
      switch (thisOp().kind) {
        case AVG: e->val += o->val; break;
        case MIN: e->bits = std::min(e->bits, o->bits); break;
        case MAX: e->bits = std::max(e->bits, o->bits); break;
      }
      e->count += o->count;
      return {sizeof(Entry), sizeof(Entry)};
    }

    std::pair<int, int> mergeNew(void* target, void* from, int space) override {
      if (space < (int)sizeof(Entry)) {
        return {-(int)sizeof(Entry), (int)sizeof(Entry)}; // not enough space to insert
      }
      auto* e = (Entry*)target;
      switch (thisOp().kind) {
        case AVG: e->val = 0.0; break;
        case MIN: e->bits = INT64_MAX; break;
        case MAX: e->bits = INT64_MIN; break;
      }
      e->count = 0;
      return merge(target, from);
    }

    // count is the bucket's doc count and is unused: stats work off the number
    // of field values seen (e->count).  AVG resolves its running sum into the
    // average here; an empty bucket becomes NaN (matching the non-inline path's
    // 0/0).  MIN/MAX stay in raw form: compare() runs after finalize and needs
    // the int64 ordering - NaN reaching the sort would break its strict weak
    // ordering - so their decode waits until fillResult.  Always return the
    // entry size - the FacetMap entry walk packs calc entries back to back, so
    // a 0 return would desync any calcs that follow.
    int finalize(void* entry, int64_t count) override {
      unused(count);
      auto* e = (Entry*)entry;
      if (thisOp().kind == AVG) {
        e->val = e->count > 0 ? e->val / e->count
                              : std::numeric_limits<double>::quiet_NaN();
      }
      return sizeof(Entry);
    }

    int compare(void* a, void* b, int& asize, int& bsize) override {
      auto* aentry = (Entry*)a;
      auto* bentry = (Entry*)b;
      asize = sizeof(Entry);
      bsize = sizeof(Entry);
      // Empty buckets order below every non-empty bucket and equal to each
      // other: deterministic, and it keeps NaN entries (AVG) out of the
      // floating-point compare below.
      if (aentry->count == 0 || bentry->count == 0) {
        return (bentry->count == 0) - (aentry->count == 0);
      }
      if (thisOp().kind == AVG) {
        return aentry->val < bentry->val ? -1 : (aentry->val > bentry->val ? 1 : 0);
      }
      return aentry->bits < bentry->bits ? -1 : (aentry->bits > bentry->bits ? 1 : 0);
    }

    void startSeg(int32_t segnum) override {
      intColIter.reset();
      intColReader.reset();
      auto guard = MemPool::threadLocalPoolGuard();
      SegFieldInfo segFieldInfo;
      auto& postingsReader = thisOp().req.reader->segments()[segnum].postingsReader();
      FieldReader fieldReader(postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      if (!found) {
        return; // field not found, nothing to do
      }
      fieldReader.readFieldInfo(segFieldInfo);
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
      Kind kind = thisOp().kind;
      for (auto i = 0u; i < entries.size(); i++) {
        auto e = *(Entry*)entries[i];
        if (kind == AVG) {
          data[i] = e.val;
        } else {
          data[i] = e.count > 0 ? thisOp().decodeDouble(e.bits)
                                : std::numeric_limits<double>::quiet_NaN();
        }
        entries[i] += sizeof(Entry); // move to the next entry part
      }
    }
  };


};
}
