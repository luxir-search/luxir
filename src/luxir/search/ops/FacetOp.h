// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "luxir/api/luxir_types.hpp"
#include "luxir/search/DocSet.h"
#include "luxir/search/IndexReader.h"
#include "FacetEmit.h"
#include "FacetExecution.h"
#include "SearchOp.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/DocsReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/OrdMapStr.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/util/AtomicMerger.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/SegmentMergeDriver.h"
#include "luxir/search/ops/DomainIter.h"

namespace luxir {



class FacetReq : public SearchOp {
public:
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  int64_t minCount; // minimum count for a facet to be included in the result
  bool missing;

  std::string_view facetName;

  FacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName,
    int64_t limit, int64_t minCount, bool missing)
    : SearchOp(req, facetName), reader(*req.reader), fieldName(fieldName), limit(limit), minCount(minCount), missing(missing),
      facetName(facetName) {
  }

  virtual ~FacetReq() = default;

  void init() override {
    for (const auto& [childName, child] : subOps) {
      if (!child->canEmitAsBucketChild()) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': child op '" + std::string(childName)
            + "' cannot emit per bucket");
      }
    }
    SearchOp::init();
  }

  // utility template method that calls callback with (int32 docid, int64_t value) for each doc in the domain that has
  // a value in the int column field (single or multi-valued).
  // missing is an out parameter that is incremented for every domain doc that does not have the field.
  // segFieldInfo is passed in uninitializsed and filled in if the field exists in the segment.
  // The value returned is if the field exists in the segment.
  bool facetSegIntCol(DocSet* domain, int32_t segnum, int64_t& missing_num, SegFieldInfo& segFieldInfo, auto&& callback) {
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      // field absent in this segment: every in-domain doc is missing.
      missing_num += domain ? domain->card() : maxDoc;
      return false;
    }
    fieldReader.readFieldInfo(segFieldInfo);
    // Walk the domain over the int column.  forEachIntColValue owns the
    // domain-type x single/multi x dense/sparse dispatch (and the one
    // (BitDocSet*)/(ArrDocSet*) cast); callback is per value.
    IntColReader intColReader(postingsReader, segFieldInfo);
    forEachIntColValue(domain, intColReader, maxDoc, missing_num, callback);
    return true;
  }

  bool facetSegOrdCol(DocSet* domain, int32_t segnum, int64_t& missing_num,
                      SegFieldInfo& segFieldInfo, auto&& callback) {
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(postingsReader);
    if (!fieldReader.seek(fieldName)) {
      missing_num += domain ? domain->card() : maxDoc;
      return false;
    }
    fieldReader.readFieldInfo(segFieldInfo);
    OrdColReader ordColReader(postingsReader, segFieldInfo);
    forEachOrdValue(domain, ordColReader, maxDoc, missing_num, callback);
    return true;
  }
};

class FieldFacetReq : public FacetReq {
public:
  const ReqFieldFacet& fieldFacet;
  std::span<const int64_t> selectedInts;
  std::span<const std::string_view> selectedStrings;

  FieldFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet,
    std::string_view fieldName, std::string_view facetName, int64_t limit,
    int64_t minCount, bool missing, std::span<const int64_t> selectedInts,
    std::span<const std::string_view> selectedStrings) :
  FacetReq(req, fieldName, facetName, limit, minCount, missing),
  fieldFacet(fieldFacet), selectedInts(selectedInts),
  selectedStrings(selectedStrings) {}

  virtual ~FieldFacetReq() = default;

};

class IntFacetReq : public FieldFacetReq {
  int64_t globalMin = std::numeric_limits<int64_t>::max();
  int64_t globalMax = std::numeric_limits<int64_t>::min();
  bool useVector = false;
public:
  class MergeableIntFacet : public MergeableData {
  public:
    using IntHash = boost::unordered_flat_map<int64_t, int64_t>;
    using CountVector = std::vector<int64_t>;
    std::variant<std::monostate, IntHash, CountVector> counts;
    int64_t missing_num = 0; // number of missing values in this segment
    int64_t minValue = 0; // minimum value in the range (used when counts is a vector)

    static MergeableIntFacet* merge(MergeableIntFacet* a, MergeableIntFacet* b) {
      // Handle uninitialized cases
      if (std::holds_alternative<std::monostate>(a->counts)) {
        return b;
      }
      if (std::holds_alternative<std::monostate>(b->counts)) {
        return a;
      }
      
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (auto* amap = std::get_if<IntHash>(&a->counts)) {
        if (auto* bmap = std::get_if<IntHash>(&b->counts)) {
          // Both are maps
          if (amap->size() < bmap->size()) {
            std::swap(a, b);
            std::swap(amap, bmap);
          }
          for (auto [val, count] : *bmap) {
            (*amap)[val] += count;
          }
        } else {
          // a is map, b is vector
          auto& bvec = std::get<CountVector>(b->counts);
          for (size_t i = 0; i < bvec.size(); i++) {
            if (bvec[i] > 0) {
              (*amap)[b->minValue + i] += bvec[i];
            }
          }
        }
      } else if (auto* avec = std::get_if<CountVector>(&a->counts)) {
        if (auto* bvec = std::get_if<CountVector>(&b->counts)) {
          assert(a->minValue == b->minValue && avec->size() == bvec->size());
          for (size_t i = 0; i < bvec->size(); i++) {
            (*avec)[i] += (*bvec)[i];
          }
        } else {
          // a is vector, b is map
          auto& bmap = std::get<IntHash>(b->counts);
          for (auto [val, count] : bmap) {
            int64_t idx = val - a->minValue;
            if (idx >= 0 && idx < (int64_t)avec->size()) {
              (*avec)[idx] += count;
            }
          }
        }
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };
public:
  // ProtobufSearchParser computes globalMin/globalMax/useVector via
  // scanGlobalRange and passes them in (see TopDocsReq's ctor comment).
  IntFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing,
    int64_t globalMin, int64_t globalMax, bool useVector,
    std::span<const int64_t> selected) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing,
                selected, {}),
  globalMin(globalMin), globalMax(globalMax), useVector(useVector) {}

  bool canEmitAsBucketChild() const override {
    return true;
  }

  // Scan every segment for the column's min/max and decide vector vs map
  // storage based on the resulting range.  Runs during parsing so the result
  // can be passed to the IntFacetReq ctor.
  struct GlobalRange {
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    bool useVector = false;
  };
  static GlobalRange scanGlobalRange(IndexReader& reader, std::string_view fieldName) {
    GlobalRange r;
    for (size_t segnum = 0; segnum < reader.segments().size(); segnum++) {
      auto& postingsReader = reader.segments()[segnum].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      bool found = fieldReader.seek(fieldName);
      if (!found) continue;

      SegFieldInfo segFieldInfo;
      fieldReader.readFieldInfo(segFieldInfo);
      if (segFieldInfo.numValues == 0) continue;

      IntColReader intColReader(postingsReader, segFieldInfo);
      r.min = std::min(r.min, intColReader.getMin());
      r.max = std::max(r.max, intColReader.getMax());
    }
    if (r.min <= r.max) {
      // width = (#distinct values) - 1, computed unsigned: max-min can exceed
      // int64 for a column spanning near the full range (signed-overflow UB).
      // range <= 100000 is equivalent to width < 100000.
      uint64_t width = (uint64_t)r.max - (uint64_t)r.min;
      // TODO: also consider total number of docs
      if (width < 100000) {
        r.useVector = true;
      }
    }
    return r;
  }

  class Calc : public Calculator {
    SegmentMergeDriver<MergeableIntFacet> driver;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableIntFacet> m){ facetResult(*m); }) {}
    IntFacetReq& thisOp() {
      return (IntFacetReq&)getOp();
    }

    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();
      if (segnum == -1) {
        driver.completeEmpty();  // empty index -> empty result
        return;
      }
      driver.contribute([&](MergeableIntFacet& data) {
        SegFieldInfo segFieldInfo;
        auto& facetReq = (FacetReq&)getOp();
        // Initialize storage based on the pre-determined type (first segment).
        if (std::holds_alternative<std::monostate>(data.counts)) {
          if (thisOp().useVector) {
            int64_t range = thisOp().globalMax - thisOp().globalMin + 1;
            data.counts = MergeableIntFacet::CountVector(range, 0);
            data.minValue = thisOp().globalMin;
          } else {
            data.counts = MergeableIntFacet::IntHash();
          }
        }
        if (auto* countVec = std::get_if<MergeableIntFacet::CountVector>(&data.counts)) {
          int64_t minVal = data.minValue;
          facetReq.facetSegIntCol(domain, segnum, data.missing_num, segFieldInfo,
            [countVec, minVal](int32_t docid, int64_t val) LUXIR_INLINE {
              unused(docid);
              (*countVec)[val - minVal]++;
            });
        } else {
          auto* countMap = &std::get<MergeableIntFacet::IntHash>(data.counts);
          facetReq.facetSegIntCol(domain, segnum, data.missing_num, segFieldInfo,
            [countMap](int32_t docid, int64_t val) LUXIR_INLINE {
              unused(docid);
              (*countMap)[val]++;
            });
        }
      });
    };

    void facetResult(MergeableIntFacet& merged) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result
      auto& facetResultProto = *slotArm<luxir::api::FacetResult>(nullptr);
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      std::vector<FacetCandidate<int64_t>> candidates;
      // Both storage types (monostate -> neither -> empty result, e.g. empty index).
      if (auto* countMap = std::get_if<MergeableIntFacet::IntHash>(&merged.counts)) {
        for (auto [val, count] : *countMap) {
          candidates.push_back({val, count, {}});
        }
      } else if (auto* countVector = std::get_if<MergeableIntFacet::CountVector>(&merged.counts)) {
        for (size_t i = 0; i < countVector->size(); i++) {
          if ((*countVector)[i] > 0) {
            candidates.push_back({merged.minValue + (int64_t)i,
                                  (*countVector)[i], {}});
          }
        }
      }
      // Selected values are reconciled against the finished page, and the
      // count for one the page does not hold is read straight out of the
      // counter rather than carried through selection.
      std::vector<std::optional<int64_t>> pins;
      std::vector<PinnedBucketValue<>> pinValues;
      pins.reserve(thisOp().selectedInts.size());
      pinValues.reserve(thisOp().selectedInts.size());
      for (int64_t selected : thisOp().selectedInts) {
        pins.emplace_back(selected);
        int64_t count = 0;
        if (auto* countMap = std::get_if<MergeableIntFacet::IntHash>(&merged.counts)) {
          auto it = countMap->find(selected);
          if (it != countMap->end()) count = it->second;
        } else if (auto* countVector =
                       std::get_if<MergeableIntFacet::CountVector>(&merged.counts)) {
          int64_t index = selected - merged.minValue;
          if (index >= 0 && (size_t)index < countVector->size()) {
            count = (*countVector)[index];
          }
        }
        pinValues.push_back({count, {}});
      }
      auto finalized = finalizeCountFieldBuckets(
          std::move(candidates), minCount, 0, limit);
      mergePinnedBuckets<int64_t, std::monostate>(finalized, pins, pinValues);
      std::vector<std::pair<int64_t, int64_t>> countVec;
      countVec.reserve(finalized.size());
      for (const auto& bucket : finalized) {
        assert(bucket.key.has_value());
        countVec.emplace_back(*bucket.key, bucket.count);
      }
      emitBuckets(facetResultProto, countVec, mr);
      if (missing) {
        facetResultProto.missing = merged.missing_num;
      }
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  };


};

class FullTextFacetReq : public FieldFacetReq {
  class MergeableStrFacet : public MergeableData {
  public:
    boost::unordered_flat_map<std::string, int64_t> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableStrFacet* merge(MergeableStrFacet* a, MergeableStrFacet* b) {
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (a->counts.size() < b->counts.size()) {
        std::swap(a,b);
      }

      for (auto [val, count] : b->counts) {
        a->counts[val] += count;
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };

public:
  FullTextFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet,
    std::string_view fieldName, std::string_view facetName, int64_t limit,
    int64_t minCount, bool missing,
    std::span<const std::string_view> selected) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing,
                {}, selected) {}

  bool canEmitAsBucketChild() const override {
    return true;
  }

  class Calc : public Calculator {
    SegmentMergeDriver<MergeableStrFacet> driver;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableStrFacet> m){ facetResult(*m); }) {}
    FullTextFacetReq& thisOp() {
      return (FullTextFacetReq&)getOp();
    }

    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();
      if (segnum == -1) {
        driver.completeEmpty();  // empty index -> empty result
        return;
      }
      driver.contribute([&](MergeableStrFacet& data) {
        boost::unordered_flat_map<std::string, int64_t>& counts = data.counts;
        SegFieldInfo segFieldInfo;
        auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        DomainView domainView(domain, maxDoc);
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(postingsReader);
        if (!fieldReader.seek(thisOp().fieldName)) {
          // field absent in this segment: all in-domain docs are missing
          data.missing_num += domainView.card;
          return;
        }
        fieldReader.readFieldInfo(segFieldInfo);
        if (domainView.compCard != 0) {
          domainView.materializeBits(poolGuard.pool());
        }
        TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
        while (tenum.nextTerm()) {
          int32_t docFreq = tenum.docFreq();
          int64_t count;
          if (domainView.compCard == 0) {
            // Avoid even constructing a postings enum for the all-docs case.
            count = docFreq;
          } else {
            DocsOnlyEnum denum(tenum);
            count = countTermInDomain(domainView, denum, docFreq);
          }
          // use heterogeneous lookup in the future to avoid creating string when not needed
          if (count > 0 || thisOp().minCount == 0) {
            counts[(std::string) (std::string_view) tenum.term()] += count;
          }
        }
        if (thisOp().missing) {
          DocsReader docsReader(postingsReader, segFieldInfo);
          data.missing_num += countMissingInDomain(domainView, docsReader);
        }
      });
    }

    void facetResult(MergeableStrFacet& merged) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result
      auto& facetResultProto = *slotArm<luxir::api::FacetResult>(nullptr);
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      std::vector<FacetCandidate<std::string>> candidates;
      for (auto [val, count] : merged.counts) {
        candidates.push_back({std::move(val), count, {}});
      }
      std::vector<std::optional<std::string>> pins;
      std::vector<PinnedBucketValue<>> pinValues;
      pins.reserve(thisOp().selectedStrings.size());
      pinValues.reserve(thisOp().selectedStrings.size());
      for (std::string_view selected : thisOp().selectedStrings) {
        pins.emplace_back(selected);
        int64_t count = 0;
        auto it = merged.counts.find(std::string(selected));
        if (it != merged.counts.end()) count = it->second;
        pinValues.push_back({count, {}});
      }
      auto finalized = finalizeCountFieldBuckets(
          std::move(candidates), minCount, 0, limit);
      mergePinnedBuckets<std::string, std::monostate>(finalized, pins, pinValues);
      std::vector<std::pair<std::string, int64_t>> countVec;
      countVec.reserve(finalized.size());
      for (auto& bucket : finalized) {
        assert(bucket.key.has_value());
        countVec.emplace_back(std::move(*bucket.key), bucket.count);
      }
      emitBuckets(facetResultProto, countVec, mr);
      if (missing) {
        facetResultProto.missing = merged.missing_num;
      }
    }


    };
  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

class FixedBucketFacetReq : public FacetReq {
  static constexpr size_t BUCKET_BUILDER_FIXED_BYTES = 128;
  static constexpr size_t BUCKET_DOMAIN_BYTE_BUDGET = 64 * 1024 * 1024;
  static constexpr size_t BINDING_STATE_CHUNK_BYTES = 64 * 1024 * 1024;
  size_t numBuckets;
  std::span<const size_t> selectedBuckets;

public:
  class MergeableFixedBuckets : public MergeableData {
  public:
    std::vector<int64_t> counts;
    int64_t missing_num = 0;

    static MergeableFixedBuckets* merge(
        MergeableFixedBuckets* a, MergeableFixedBuckets* b) {
      if (a->counts.empty()) {
        std::swap(a, b);
      } else if (!b->counts.empty()) {
        assert(a->counts.size() == b->counts.size());
        for (size_t i = 0; i < a->counts.size(); i++) {
          a->counts[i] += b->counts[i];
        }
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };

  FixedBucketFacetReq(
      SearchRequest& req, std::string_view fieldName,
      std::string_view facetName, int64_t minCount, bool missing,
      size_t numBuckets,
      std::span<const size_t> selectedBuckets)
    : FacetReq(req, fieldName, facetName, -1, minCount, missing),
      numBuckets(numBuckets), selectedBuckets(selectedBuckets) {}

  size_t bucketCount() const { return numBuckets; }
  std::span<const size_t> selectedBucketIndexes() const {
    return selectedBuckets;
  }

  bool canEmitAsBucketChild() const override { return true; }

  virtual size_t bindingStateChunkBytes() const {
    return BINDING_STATE_CHUNK_BYTES;
  }
  virtual size_t bucketDomainByteBudget() const {
    return BUCKET_DOMAIN_BYTE_BUDGET;
  }
  virtual void bindingBlockStarted() const {}

  virtual bool requiresWholeReaderDomain() const { return false; }

  virtual std::vector<size_t> emittedBuckets(
      const MergeableFixedBuckets& merged,
      std::span<const uint8_t> pinned) const {
    unused(merged);
    unused(pinned);
    std::vector<size_t> emitted;
    emitted.reserve(bucketCount());
    for (size_t i = 0; i < bucketCount(); i++) emitted.push_back(i);
    return emitted;
  }

  virtual void emitBucketResult(
      luxir::api::FacetResult& result,
      const MergeableFixedBuckets& merged,
      std::span<const size_t> emitted,
      std::pmr::memory_resource& mr) const = 0;

  class Calc : public Calculator {
    SegmentMergeDriver<MergeableFixedBuckets> driver;
    std::atomic<int32_t> gatheredDomainsSeen{0};

  protected:
    // Per-segment incoming domains stay retained through result-stage sub-op
    // execution, where the producer intersects them with each bucket.
    std::vector<DomainHandle> input;

    FixedBucketFacetReq& fixedOp() {
      return (FixedBucketFacetReq&)getOp();
    }

    virtual void collectSegment(
        MergeableFixedBuckets& data, int32_t segnum, DocSet* domain) = 0;
    virtual std::vector<DomainHandle> bucketDomains(
        size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> buckets) = 0;
    virtual void prepareGatheredDomains(oneapi::tbb::task_group* tg) {
      unused(tg);
    }

    void collectInputSegment(int32_t segnum) {
      DocSet* domain = input[(size_t)segnum].get();
      driver.contribute([&](MergeableFixedBuckets& data) {
        if (data.counts.empty()) {
          data.counts.assign(fixedOp().bucketCount(), 0);
        }
        collectSegment(data, segnum, domain);
      });
    }

    void prepareAndDispatch(oneapi::tbb::task_group* tg) {
      prepareGatheredDomains(tg);
      for (int32_t segnum = 0; segnum < (int32_t)input.size(); segnum++) {
        task_group_run(tg, [this, segnum]() {
          collectInputSegment(segnum);
        });
      }
    }

    void startGatheredDispatch(oneapi::tbb::task_group* tg) {
      task_group_run(tg, [this, tg]() {
        prepareAndDispatch(tg);
      });
    }

    void facetResult(MergeableFixedBuckets& merged) {
      auto& mr = op.req.lastResponse->mr;
      auto* result = slotArm<luxir::api::FacetResult>(nullptr);

      std::vector<uint8_t> pinned(fixedOp().bucketCount());
      for (size_t bucket : fixedOp().selectedBucketIndexes()) {
        pinned[bucket] = 1;
      }
      std::vector<size_t> emitted = fixedOp().emittedBuckets(merged, pinned);
      fixedOp().emitBucketResult(*result, merged, emitted, mr);
      executeResultChildren(merged, emitted, pinned);
    }

    // Post-selection sub-op execution sizes each bucket block from the child
    // plans' retained bytes, feeds every segment, and destroys the bindings
    // before opening the next block. Within a segment, the domain byte budget
    // may split the binding block further.
    void executeResultChildren(
        const MergeableFixedBuckets& merged,
        std::span<const size_t> emitted,
        std::span<const uint8_t> pinned) {
      if (fixedOp().subOps.empty() || emitted.empty()) return;

      std::vector<SelectedFacetBucket<size_t>> buckets;
      buckets.reserve(emitted.size());
      for (size_t i = 0; i < emitted.size(); i++) {
        buckets.push_back({
            .key = emitted[i],
            .id = FacetBucketId{(int64_t)emitted[i]},
            .count = merged.counts.empty() ? 0 : merged.counts[emitted[i]],
            .owner = FacetOwnerSlot{(int32_t)i},
            .output = FacetOutputSlot{(int32_t)i},
            .flags = pinned[emitted[i]] != 0
                ? FacetBucketFlags::PINNED : FacetBucketFlags::NONE
        });
      }

      std::vector<SearchOp*> children;
      children.reserve(fixedOp().subOps.size());
      for (auto& [name, child] : fixedOp().subOps) {
        unused(name);
        children.push_back(child);
      }

      size_t residentBytesPerBucket = 0;
      for (SearchOp* child : children) {
        residentBytesPerBucket = saturatingAdd(
            residentBytesPerBucket, child->facetBucketResidentBytes());
      }
      size_t bindingBlockSize = std::max<size_t>(
          1, fixedOp().bindingStateChunkBytes()
                 / std::max<size_t>(1, residentBytesPerBucket));

      for (size_t blockBegin = 0; blockBegin < buckets.size();
           blockBegin += bindingBlockSize) {
        fixedOp().bindingBlockStarted();
        size_t blockSize = std::min(
            bindingBlockSize, buckets.size() - blockBegin);
        auto block = std::span<const SelectedFacetBucket<size_t>>(buckets)
                         .subspan(blockBegin, blockSize);

        std::vector<std::unique_ptr<SearchOp::Calculator>> bindings;
        bindings.reserve(blockSize * children.size());
        for (const auto& bucket : block) {
          assert(bucket.output.value >= 0);
          assert(bucket.output.value < (int32_t)buckets.size());
          for (SearchOp* child : children) {
            bindings.emplace_back(child->createCalculator(
                this, bucket.output.value, (int64_t)buckets.size()));
          }
        }

        if (input.empty()) {
          std::span<const DomainHandle> noDomains;
          for (auto& binding : bindings) {
            binding->calcAll(nullptr, noDomains);
          }
          continue;
        }

        for (size_t segnum = 0; segnum < input.size(); segnum++) {
          int32_t maxDoc = fixedOp().reader.segments()[segnum].maxDoc();
          size_t builderBytes =
              (size_t)(((uint64_t)maxDoc + 63) / 64) * 8
              + BUCKET_BUILDER_FIXED_BYTES;
          size_t bucketsPerChunk = std::max<size_t>(
              1, fixedOp().bucketDomainByteBudget() / builderBytes);
          for (size_t chunkBegin = 0; chunkBegin < block.size();
               chunkBegin += bucketsPerChunk) {
            size_t chunkSize = std::min(
                bucketsPerChunk, block.size() - chunkBegin);
            auto chunk = block.subspan(chunkBegin, chunkSize);
            std::vector<DomainHandle> domains = bucketDomains(segnum, chunk);
            for (size_t bucket = 0; bucket < chunkSize; bucket++) {
              for (size_t child = 0; child < children.size(); child++) {
                bindings[(chunkBegin + bucket) * children.size() + child]
                    ->calc(nullptr, (int32_t)segnum, domains[bucket]);
              }
            }
          }
        }
      }
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableFixedBuckets> merged) {
                 facetResult(*merged);
               }) {
      input.resize(op.req.reader->segments().size());
    }

    luxir::api::Val* getTargetForSub(
        SearchResponse* resp, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(resp, this);
      assert(ourVal != nullptr);
      luxir::api::Val* target = nullptr;
      routeTarget<luxir::api::ArrVal>(*ourVal, resp->mr,
          [&](luxir::api::Val& val) { target = &val; });
      auto& result = oneofMut<luxir::api::FacetResult>(*target);
      return build::opsSlot(
          result.ops, fixedOp().subOps.size(), sub->getOp().name, resp->mr);
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) final {
      assert(domainHandle.isDeliverable());
      if (segnum == -1) {
        driver.completeEmpty();
        return;
      }
      input[(size_t)segnum] = std::move(domainHandle);
      if (!fixedOp().requiresWholeReaderDomain()) {
        collectInputSegment(segnum);
        return;
      }
      int32_t seen = gatheredDomainsSeen.fetch_add(
          1, std::memory_order_acq_rel) + 1;
      assert(seen <= (int32_t)input.size());
      if (seen == (int32_t)input.size()) startGatheredDispatch(tg);
    }

    void calcAll(oneapi::tbb::task_group* tg,
                 std::span<const DomainHandle> domains) final {
      if (!fixedOp().requiresWholeReaderDomain()) {
        Calculator::calcAll(tg, domains);
        return;
      }
      assert(domains.size() == input.size());
      if (domains.empty()) {
        driver.completeEmpty();
        return;
      }
      std::copy(domains.begin(), domains.end(), input.begin());
      for (const DomainHandle& domain : input) {
        assert(domain.isDeliverable());
      }
      startGatheredDispatch(tg);
    }
  };
};

class IntFacetRangeReq : public FixedBucketFacetReq {
  std::span<const int64_t> fences;
  int64_t affineGap;
  FieldType::Type valueType;
  bool affine;

  // Bucket index for an in-range value (caller checks [front, back)).
  size_t bucketOf(int64_t val) const {
    if (affine) {
      return (size_t)(((uint64_t)val - (uint64_t)fences.front())
                      / (uint64_t)affineGap);
    }
    return (size_t)(std::upper_bound(fences.begin(), fences.end(), val)
                    - fences.begin() - 1);
  }

public:
  static inline bool disablePointsRangeFacetForTests = false;

  std::span<const int64_t> bucketFences() const { return fences; }

  IntFacetRangeReq(SearchRequest& req,
    std::string_view fieldName, std::string_view facetName,
    std::span<const int64_t> fences, bool affine, int64_t affineGap,
    FieldType::Type valueType, int64_t minCount, bool missing,
    std::span<const size_t> selectedBuckets)
  : FixedBucketFacetReq(
        req, fieldName, facetName, minCount, missing,
        fences.size() - 1, selectedBuckets),
    fences(fences), affineGap(affineGap), valueType(valueType),
    affine(affine) {}

  size_t bindingStateChunkBytes() const override {
    return forcedRangeFacetBindingStateChunkBytes != 0
        ? forcedRangeFacetBindingStateChunkBytes
        : FixedBucketFacetReq::bindingStateChunkBytes();
  }

  size_t bucketDomainByteBudget() const override {
    return forcedRangeFacetBucketDomainByteBudget != 0
        ? forcedRangeFacetBucketDomainByteBudget
        : FixedBucketFacetReq::bucketDomainByteBudget();
  }

  void bindingBlockStarted() const override {
    if (rangeFacetBindingBlockCounter != nullptr) {
      (*rangeFacetBindingBlockCounter)++;
    }
  }

  std::vector<size_t> emittedBuckets(
      const MergeableFixedBuckets& merged,
      std::span<const uint8_t> pinned) const override {
    std::vector<size_t> emitted;
    emitted.reserve(bucketCount());
    for (size_t i = 0; i < bucketCount(); i++) {
      int64_t count = merged.counts.empty() ? 0 : merged.counts[i];
      if (pinned[i] != 0 || count >= minCount) emitted.push_back(i);
    }
    return emitted;
  }

  void emitBucketResult(
      luxir::api::FacetResult& result,
      const MergeableFixedBuckets& merged,
      std::span<const size_t> emitted,
      std::pmr::memory_resource& mr) const override {
    size_t n = emitted.size();
    auto emitBounds = [&]<typename Outer>(auto decode) {
      auto& bucketIds = result.bucket_ids.emplace().kind.emplace<Outer>();
      auto* pairs = build::allocArray(bucketIds.v, n, mr);
      int64_t* counts = build::allocArray(result.counts, n, mr);
      for (size_t i = 0; i < n; i++) {
        size_t bucket = emitted[i];
        auto* bounds = build::allocArray(pairs[i].v, 2, mr);
        bounds[0] = decode(fences[bucket]);
        bounds[1] = decode(fences[bucket + 1]);
        counts[i] = merged.counts.empty() ? 0 : merged.counts[bucket];
      }
    };
    switch (valueType) {
      case FieldType::Type::FLOAT:
        emitBounds.template operator()<luxir::api::ArrArrFloat>(
            [](int64_t encoded) {
              return sortableInt32ToFloat((int32_t)encoded);
            });
        break;
      case FieldType::Type::DOUBLE:
        emitBounds.template operator()<luxir::api::ArrArrDouble>(
            [](int64_t encoded) { return sortableInt64ToDouble(encoded); });
        break;
      default:
        emitBounds.template operator()<luxir::api::ArrArrInt>(
            [](int64_t value) { return value; });
        break;
    }
    if (missing) result.missing = merged.missing_num;
  }

  class Calc : public FixedBucketFacetReq::Calc {
    IntFacetRangeReq& rangeOp() {
      return (IntFacetRangeReq&)getOp();
    }

    void collectSegment(
        MergeableFixedBuckets& data, int32_t segnum,
        DocSet* domain) override {
      SegFieldInfo segFieldInfo;
      int64_t start = rangeOp().fences.front();
      int64_t end = rangeOp().fences.back();
      auto& segment = rangeOp().reader.segments()[segnum];
      bool noSubOps = rangeOp().subOps.empty();
      if (!IntFacetRangeReq::disablePointsRangeFacetForTests
          && domain == nullptr && segment.liveDocs() == nullptr && noSubOps
          && start < end) {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(segment.postingsReader());
        if (fieldReader.seek(rangeOp().fieldName)) {
          fieldReader.readFieldInfo(segFieldInfo);
          bool oneDimensionalNumeric = segFieldInfo.type == FieldType::INT
                                      || segFieldInfo.type == FieldType::FLOAT
                                      || segFieldInfo.type == FieldType::DOUBLE
                                      || segFieldInfo.type == FieldType::DATE;
          if (segFieldInfo.pointsMetaOff != 0 && oneDimensionalNumeric) {
            IntColReader column(segment.postingsReader(), segFieldInfo);
            PointsReader points(segment.postingsReader(), segFieldInfo);
            if (points.pointCount() != (uint64_t)column.numValues()) {
              throw std::runtime_error(
                  "IntFacetRangeReq: points/column value count mismatch");
            }
            data.missing_num += segment.maxDoc() - column.docsWithValue();
            auto residualScratch = poolGuard.pool().make_span<uint32_t>(
                points.maxPointsPerLeaf());
            auto rawScratch = poolGuard.pool().make_span<int64_t>(
                points.maxPointsPerLeaf());
            uint64_t ordinal = points.ordinalOfFirstAtLeast(
                start, residualScratch, rawScratch);
            for (size_t bucket = 0; bucket < rangeOp().bucketCount(); bucket++) {
              int64_t edge = rangeOp().fences[bucket + 1];
              uint64_t nextOrdinal = points.ordinalOfFirstAtLeast(
                  edge, residualScratch, rawScratch);
              data.counts[bucket] += (int64_t)(nextOrdinal - ordinal);
              ordinal = nextOrdinal;
            }
            skipCount(SkipStats::rangeFacetPointsArms);
            return;
          }
        }
      }
      rangeOp().facetSegIntCol(
          domain, segnum, data.missing_num, segFieldInfo,
          [&](int32_t docid, int64_t val) LUXIR_INLINE {
            unused(docid);
            if (val < start || val >= end) return;
            data.counts[rangeOp().bucketOf(val)]++;
          });
    }

    // Build one chunk of one segment's bucket domains in a value-column pass.
    // A multi-valued document can put two values in one bucket and
    // DocSetBuilder requires strictly increasing docids, so each builder skips
    // a repeat of the docid it just added.
    std::vector<DomainHandle> bucketDomains(
        size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> buckets) override {
      size_t numBuckets = buckets.size();
      std::vector<DomainHandle> domains(numBuckets);
      std::vector<int32_t> builderOfBucket(rangeOp().bucketCount(), -1);
      for (size_t i = 0; i < buckets.size(); i++) {
        builderOfBucket[buckets[i].key] = (int32_t)i;
      }
      int64_t start = rangeOp().fences.front();
      int64_t end = rangeOp().fences.back();
      int32_t maxDoc = rangeOp().reader.segments()[segnum].maxDoc();
      std::vector<DocSetBuilder> builders;
      builders.reserve(numBuckets);
      for (size_t i = 0; i < numBuckets; i++) builders.emplace_back(maxDoc);
      std::vector<int32_t> lastAdded(numBuckets, -1);

      SegFieldInfo segFieldInfo;
      int64_t missing_num = 0;
      rangeOp().facetSegIntCol(
          input[segnum].get(), (int32_t)segnum, missing_num, segFieldInfo,
          [&](int32_t docid, int64_t val) LUXIR_INLINE {
            if (val < start || val >= end) return;
            int32_t builder = builderOfBucket[rangeOp().bucketOf(val)];
            if (builder < 0 || lastAdded[(size_t)builder] == docid) return;
            lastAdded[(size_t)builder] = docid;
            builders[(size_t)builder].add(docid);
          });

      for (size_t i = 0; i < numBuckets; i++) {
        domains[i] = DomainHandle(builders[i].build());
      }
      return domains;
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : FixedBucketFacetReq::Calc(op, parent, slot, numSlots) {}
  };

  Calculator* createCalculator(
      Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

class QueryFacetReq : public FixedBucketFacetReq {
  const ReqQueryFacet& queryFacet;
  std::span<Query*> bucketQueries;
  std::span<Query::Weight*> bucketWeights;
  bool preparedBuckets;

public:
  QueryFacetReq(
      SearchRequest& req, const ReqQueryFacet& queryFacet,
      std::string_view facetName, std::span<Query*> bucketQueries,
      std::span<Query::Weight*> bucketWeights)
    : FixedBucketFacetReq(
          req, {}, facetName, 0, false, queryFacet.buckets.size(), {}),
      queryFacet(queryFacet), bucketQueries(bucketQueries),
      bucketWeights(bucketWeights),
      preparedBuckets(QueryPrep::anyNeedsPrepare(bucketWeights)) {
    assert(bucketQueries.size() == queryFacet.buckets.size());
    assert(bucketWeights.size() == queryFacet.buckets.size());
  }

  bool requiresWholeReaderDomain() const override {
    return preparedBuckets;
  }

  void emitBucketResult(
      luxir::api::FacetResult& result,
      const MergeableFixedBuckets& merged,
      std::span<const size_t> emitted,
      std::pmr::memory_resource& mr) const override {
    auto& ids = result.bucket_ids.emplace().kind.emplace<luxir::api::ColStr>();
    std::string_view* names = build::allocArray(ids.v, emitted.size(), mr);
    int64_t* counts = build::allocArray(result.counts, emitted.size(), mr);
    for (size_t i = 0; i < emitted.size(); i++) {
      size_t bucket = emitted[i];
      names[i] = build::arenaStr(mr, queryFacet.buckets[bucket].name);
      counts[i] = merged.counts.empty() ? 0 : merged.counts[bucket];
    }
  }

  class Calc : public FixedBucketFacetReq::Calc {
    std::vector<QueryPrep::PreparedSource> preparedSources;

    QueryFacetReq& queryOp() {
      return (QueryFacetReq&)getOp();
    }

    Query::SegmentSource& bucketSource(size_t bucket) {
      return preparedSources.empty()
          ? static_cast<Query::SegmentSource&>(
                *queryOp().bucketWeights[bucket])
          : preparedSources[bucket].segmentSource();
    }

    DomainHandle materializeBucket(size_t segnum, size_t bucket) {
      auto& segment = queryOp().reader.segments()[segnum];
      DocSet* domain = input[segnum].get();
      return DomainHandle(QueryPrep::materialize(
          bucketSource(bucket), segment, domain,
          Query::SupplierExecutionMode::ORDINARY,
          QueryPrep::MaterializeMode::ORDINARY));
    }

    void prepareGatheredDomains(oneapi::tbb::task_group* tg) override {
      std::vector<DocSet*> domains;
      domains.reserve(input.size());
      for (const DomainHandle& domain : input) {
        domains.push_back(domain.get());
      }
      Query::Weight::PrepareContext context{
        *queryOp().req.reader,
        std::span<DocSet* const>(domains.data(), domains.size()),
        tg != nullptr
      };
      preparedSources = QueryPrep::prepareSources(
          queryOp().bucketWeights, context);
      assert(preparedSources.size() == queryOp().bucketCount());
    }

    void collectSegment(
        MergeableFixedBuckets& data, int32_t segnum,
        DocSet* domain) override {
      auto& segment = queryOp().reader.segments()[(size_t)segnum];
      // The count pass never retains matches (result children re-materialize
      // their selected buckets), so the direct count is eligible whenever the
      // whole segment is the domain.
      bool countOnly = domain == nullptr;
      for (size_t bucket = 0; bucket < queryOp().bucketCount(); bucket++) {
        if (countOnly && !queryOp().bucketWeights[bucket]->needsPrepare()) {
          int64_t count = queryOp().bucketWeights[bucket]->count(segment);
          if (count >= 0) {
            data.counts[bucket] += count;
            continue;
          }
        }
        DomainHandle matches = materializeBucket((size_t)segnum, bucket);
        data.counts[bucket] += matches.get()->card();
      }
    }

    std::vector<DomainHandle> bucketDomains(
        size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> buckets) override {
      std::vector<DomainHandle> domains;
      domains.reserve(buckets.size());
      for (const auto& bucket : buckets) {
        domains.push_back(materializeBucket(segnum, bucket.key));
      }
      return domains;
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : FixedBucketFacetReq::Calc(op, parent, slot, numSlots) {}
  };

  Calculator* createCalculator(
      Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

}
