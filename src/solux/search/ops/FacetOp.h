#pragma once
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "solux/api/solux_types.hpp"
#include "solux/search/DocSet.h"
#include "solux/search/IndexReader.h"
#include "FacetEmit.h"
#include "SearchOp.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/DocsReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/reader/PointsReader.h"
#include "solux/reader/SkipStats.h"
#include "solux/reader/TermsEnum.h"
#include "solux/schema/Schema.h"
#include "solux/search/OrdMapStr.h"
#include "solux/util/AtomicMerger.h"
#include "solux/util/NumericUtils.h"
#include "solux/util/SegmentMergeDriver.h"
#include "solux/search/ops/DomainIter.h"

namespace solux {



class FacetReq : public SearchOp {
public:
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  int64_t minCount; // minimum count for a facet to be included in the result
  bool missing;

  // Non-owning view of the request proto's repeated sorts (a trivially
  // copyable span into the kept-alive request bytes, valid for the request
  // lifetime).  Held by value.
  ReqSortList sorts;
  std::string_view facetName;
  std::vector<std::pair<const std::string_view, SearchOp*>> inlineSubOps;

  FacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName,
    int64_t limit, int64_t minCount, bool missing,
    ReqSortList sorts)
    : SearchOp(req, facetName), reader(*req.reader), fieldName(fieldName), limit(limit), minCount(minCount), missing(missing),
      sorts(sorts), facetName(facetName) {
  }

  virtual ~FacetReq() = default;

  void init() override {
    SearchOp::init();
    if (!sorts.empty()) {
      if (sorts.size() > 1) {
        throw std::runtime_error("facet '" + std::string(facetName) + "': multiple sort fields are not yet supported");
      }
      for (auto& sort : sorts) {
        auto iter = subOps.find(sort.expr);
        if (iter == subOps.end()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': unknown sort field '" + std::string(sort.expr) + "'");
        }
        if (iter->second->canInline()) {
          inlineSubOps.push_back(*iter);
          subOps.erase(iter);
        } else {
          throw std::runtime_error("facet '" + std::string(facetName) + "': cannot sort by a subop without inline support: " + std::string(iter->second->name));
        }
      }
    }
    //if there's no limit, more efficient to do inline
    if (limit == -1) {
      for (auto& subOp : subOps) {
        if (subOp.second->canInline()) {
          inlineSubOps.push_back(subOp);
        }
      }
      for (auto& subOp : inlineSubOps) {
        subOps.erase(subOp.first);
      }
    }
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
  FieldFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FacetReq(req, fieldName, facetName, limit, minCount, missing, fieldFacet.sorts), fieldFacet(fieldFacet){}

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
    int64_t globalMin, int64_t globalMax, bool useVector) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing),
  globalMin(globalMin), globalMax(globalMax), useVector(useVector) {}

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

    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
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
            [countVec, minVal](int32_t docid, int64_t val) SOLUX_INLINE {
              unused(docid);
              (*countVec)[val - minVal]++;
            });
        } else {
          auto* countMap = &std::get<MergeableIntFacet::IntHash>(data.counts);
          facetReq.facetSegIntCol(domain, segnum, data.missing_num, segFieldInfo,
            [countMap](int32_t docid, int64_t val) SOLUX_INLINE {
              unused(docid);
              (*countMap)[val]++;
            });
        }
      });
    };

    void facetResult(MergeableIntFacet& merged) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      auto* myVal = getTarget(nullptr);
      solux::api::FacetResult& facetResultProto = oneofMut<solux::api::FacetResult>(*myVal);
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      std::vector<std::pair<int64_t, int64_t>> countVec;
      // Both storage types (monostate -> neither -> empty result, e.g. empty index).
      if (auto* countMap = std::get_if<MergeableIntFacet::IntHash>(&merged.counts)) {
        for (auto [val, count] : *countMap) {
          if (count >= minCount) {
            countVec.emplace_back(val, count);
          }
        }
      } else if (auto* countVector = std::get_if<MergeableIntFacet::CountVector>(&merged.counts)) {
        for (size_t i = 0; i < countVector->size(); i++) {
          if ((*countVector)[i] > 0 && (*countVector)[i] >= minCount) {
            countVec.emplace_back(merged.minValue + i, (*countVector)[i]);
          }
        }
      }
      sortByCountDescAndLimit(countVec, limit);
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
  FullTextFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing){}

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

    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
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
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      auto* myVal = getTarget(nullptr);
      solux::api::FacetResult& facetResultProto = oneofMut<solux::api::FacetResult>(*myVal);
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      std::vector<std::pair<std::string, int64_t>> countVec;
      for (auto [val, count] : merged.counts) {
        if (minCount == -1 || count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      sortByCountDescAndLimit(countVec, limit);
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

class IntFacetRangeReq : public FacetReq {
  std::span<const int64_t> fences;
  int64_t affineGap;
  FieldType::Type valueType;
  bool affine;

  size_t bucketCount() const { return fences.size() - 1; }

public:
  static inline bool disablePointsRangeFacetForTests = false;

  // rangeFacet must reference the request proto (not a temporary): FacetReq
  // captures a span over rangeFacet.sorts that points into the request bytes.
  IntFacetRangeReq(SearchRequest& req, const ReqRangeFacet& rangeFacet,
    std::string_view fieldName, std::string_view facetName,
    std::span<const int64_t> fences, bool affine, int64_t affineGap,
    FieldType::Type valueType, int64_t minCount, bool missing)
  : FacetReq(req, fieldName, facetName, -1, minCount, missing, rangeFacet.sorts),
    fences(fences), affineGap(affineGap), valueType(valueType), affine(affine) {}

  virtual ~IntFacetRangeReq() = default;

  class MergeableRangeFacet : public MergeableData {
  public:
    std::vector<int64_t> counts;
    int64_t missing_num = 0;

    static MergeableRangeFacet* merge(
        MergeableRangeFacet* a, MergeableRangeFacet* b) {
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

  class Calc : public Calculator {
    SegmentMergeDriver<MergeableRangeFacet> driver;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableRangeFacet> m){ facetResult(*m); }) {}
    IntFacetRangeReq& thisOp() {
      return (IntFacetRangeReq&)getOp();
    }

    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();
      if (segnum == -1) {
        driver.completeEmpty();
        return;
      }
      driver.contribute([&](MergeableRangeFacet& data) {
        SegFieldInfo segFieldInfo;
        if (data.counts.empty()) {
          data.counts.assign(thisOp().bucketCount(), 0);
        }
        auto start = thisOp().fences.front();
        auto end = thisOp().fences.back();
        auto& facetReq = (FacetReq&)getOp();
        auto& segment = facetReq.reader.segments()[segnum];
        bool noSubOps = thisOp().subOps.empty()
                     && thisOp().inlineSubOps.empty()
                     && thisOp().sorts.empty();
        if (!IntFacetRangeReq::disablePointsRangeFacetForTests
            && domain == nullptr && segment.liveDocs() == nullptr && noSubOps
            && start < end) {
          auto poolGuard = MemPool::threadLocalPoolGuard();
          FieldReader fieldReader(segment.postingsReader());
          if (fieldReader.seek(thisOp().fieldName)) {
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
              for (size_t bucket = 0; bucket < thisOp().bucketCount(); bucket++) {
                int64_t edge = thisOp().fences[bucket + 1];
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
        facetReq.facetSegIntCol(domain, segnum, data.missing_num, segFieldInfo, [&](int32_t docid, int64_t val) SOLUX_INLINE {
          unused(docid);
          if (val < start || val >= end) {
            return; // value is out of range
          }
          size_t bucket;
          if (thisOp().affine) {
            bucket = (size_t)(((uint64_t)val - (uint64_t)start)
                              / (uint64_t)thisOp().affineGap);
          } else {
            bucket = (size_t)(std::upper_bound(
                thisOp().fences.begin(), thisOp().fences.end(), val)
                - thisOp().fences.begin() - 1);
          }
          data.counts[bucket]++;
        });
      });
    };

    void facetResult(MergeableRangeFacet& merged) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      auto* myVal = getTarget(nullptr);
      solux::api::FacetResult& facetResultProto = oneofMut<solux::api::FacetResult>(*myVal);
      auto minCount = thisOp().minCount;
      auto missing = thisOp().missing;

      std::vector<size_t> emitted;
      emitted.reserve(thisOp().bucketCount());
      for (size_t i = 0; i < thisOp().bucketCount(); i++) {
        int64_t count = merged.counts.empty() ? 0 : merged.counts[i];
        if (count >= minCount) emitted.push_back(i);
      }

      size_t n = emitted.size();
      auto emitBounds = [&]<typename Outer>(auto decode) {
        auto& bucketIds = facetResultProto.bucket_ids.emplace().kind.emplace<Outer>();
        auto* pairs = build::allocArray(bucketIds.v, n, mr);
        int64_t* counts = build::allocArray(facetResultProto.counts, n, mr);
        for (size_t i = 0; i < n; i++) {
          size_t bucket = emitted[i];
          auto* bounds = build::allocArray(pairs[i].v, 2, mr);
          bounds[0] = decode(thisOp().fences[bucket]);
          bounds[1] = decode(thisOp().fences[bucket + 1]);
          counts[i] = merged.counts.empty() ? 0 : merged.counts[bucket];
        }
      };
      switch (thisOp().valueType) {
        case FieldType::Type::FLOAT:
          emitBounds.template operator()<solux::api::ArrArrFloat>(
              [](int64_t encoded) {
                return sortableInt32ToFloat((int32_t)encoded);
              });
          break;
        case FieldType::Type::DOUBLE:
          emitBounds.template operator()<solux::api::ArrArrDouble>(
              [](int64_t encoded) { return sortableInt64ToDouble(encoded); });
          break;
        default:
          emitBounds.template operator()<solux::api::ArrArrInt>(
              [](int64_t value) { return value; });
          break;
      }
      if (missing) {
        facetResultProto.missing = merged.missing_num;
      }
    }
  };
  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  };
};

}
