#pragma once
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "protos/solux_types.pb.h"
#include "solux/search/DocSet.h"
#include "solux/search/IndexReader.h"
#include "FacetEmit.h"
#include "SearchOp.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/DocsReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/schema/Schema.h"
#include "solux/search/OrdMapStr.h"
#include "solux/util/AtomicMerger.h"

namespace solux {



class FacetReq : public SearchOp {
public:
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  int64_t minCount; // minimum count for a facet to be included in the result
  bool missing;

  // Reference into the request proto (always non-null, lifetime tied to the
  // request).  Storing by value would copy a RepeatedPtrField and could
  // throw during the copy, which mid-ctor would corrupt the arena cleanup
  // list (see TopDocsReq's ctor comment for the hazard).
  const google::protobuf::RepeatedPtrField<proto::SortSpec>& sorts;
  std::string_view facetName;
  std::vector<std::pair<const std::string_view, SearchOp*>> inlineSubOps;

  FacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName,
    int64_t limit, int64_t minCount, bool missing,
    const google::protobuf::RepeatedPtrField<proto::SortSpec>& sorts)
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
        auto iter = subOps.find(sort.field());
        if (iter == subOps.end()) {
          throw std::runtime_error("facet '" + std::string(facetName) + "': unknown sort field '" + sort.field() + "'");
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
    const FixedBitSet* bits = nullptr;
    ArrDocSet* arrDocs = nullptr;
    if (domain) {
      if (domain->type == DocSet::Type::BITSET) {
        BitDocSet* bitDocs = (BitDocSet*) domain;
        bits = &bitDocs->bits();
      } else if (domain->type == DocSet::Type::ARRAY) {
        arrDocs = (ArrDocSet*) domain;
      }
    }

    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), postingsReader);
    bool found = fieldReader.seek(fieldName);
    if (!found) {
      if (domain) {
        missing_num += domain->card();
      } else {
        missing_num += maxDoc;
      }
      return false;
    }
    fieldReader.readFieldInfo(segFieldInfo);
    // this is a int field for now, so we need to read the value for each doc
    // and accumulate counts per value.
    IntColReader intColReader(postingsReader, segFieldInfo);
    IntColReader::Iterator intColIter(intColReader); // TODO: OPT: use sparse iterator if the domain is sparse.

    auto collect = [&](int32_t docid) SOLUX_INLINE {
      if (intColIter.docId() < docid ) {
        intColIter.advance(docid);
      }
      if (intColIter.docId() == docid) {
        if (!intColReader.multiValued()) {
          auto val = intColIter.value();
          callback(docid, val);
        } else {
          // TODO: use bulk iter for dense domain?
          auto [start, end] = intColReader.getStartEndValueRank(intColIter.rank());
          auto n = end - start;
          for (int64_t vrank = 0; vrank < n; vrank++) {
            auto val = intColIter.values().valueAt(start + vrank);
            callback(docid, val);
          }
        }
      } else {
        missing_num++;
      }
    };

    if (arrDocs) {
      IntColReader::SparseIterator iter(intColReader);
      // single valued case
      if (!intColReader.multiValued()) {
        for (auto docid : arrDocs->docs()) {
          if (iter.docId() < docid ) {
            iter.advance(docid);
          }
          if (iter.docId() == docid) {
            auto val = iter.value();
            callback(docid, val);
          } else {
            missing_num++;
          }
        }
      } else {
        // multi-valued case
        for (auto docid : arrDocs->docs()) {
          if (iter.docId() < docid ) {
            iter.advance(docid);
          }
          if (iter.docId() == docid) {
            // TODO: use bulk iter for dense domain?
            auto [start, end] = intColReader.getStartEndValueRank(iter.rank());
            auto n = end - start;
            for (int64_t vrank = 0; vrank < n; vrank++) {
              auto val = iter.values().valueAt(start + vrank);
              callback(docid, val);
            }
          } else {
            missing_num++;
          }
        }
      }
    } else {
      // bit doc set domain
      int32_t docid = -1;
      while (docid + 1 < maxDoc) {
        if (bits) {
          docid = bits->nextSetBit(docid + 1);
          if (docid >= maxDoc) {
            break;
          }
        } else {
          docid++;
        }
        collect(docid);
      }
    }
    return true;
  }
};

class FieldFacetReq : public FacetReq {
public:
  const proto::FieldFacet& fieldFacet;
  FieldFacetReq(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FacetReq(req, fieldName, facetName, limit, minCount, missing, fieldFacet.sorts()), fieldFacet(fieldFacet){}

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
  // Ctor must be nothrow (Arena::Create hazard).  ProtobufSearchParser
  // computes globalMin/globalMax/useVector via scanGlobalRange before
  // allocation and passes them in.
  IntFacetReq(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing,
    int64_t globalMin, int64_t globalMax, bool useVector) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing),
  globalMin(globalMin), globalMax(globalMax), useVector(useVector) {}

  // Scan every segment for the column's min/max and decide vector vs map
  // storage based on the resulting range.  Runs during parsing (before
  // Arena::Create<IntFacetReq>) so any I/O exception propagates out
  // cleanly.
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
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
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
      int64_t range = r.max - r.min + 1;
      // TODO: also consider total number of docs
      if (range > 0 && range <= 100000) {
        r.useVector = true;
      }
    }
    return r;
  }

  class Calc : public Calculator {
    AtomicMerger<MergeableIntFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots) : Calculator(op, parent, slot, numSlots){}
    IntFacetReq& thisOp() {
      return (IntFacetReq&)getOp();
    }

solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
  return nullptr;
};
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      // Handle empty index case
      if (segnum == -1) {
        // Empty index - just generate empty result
        facetResult();
        return;
      }
      
      std::unique_ptr<MergeableIntFacet> mergeableData(countMerger.obtain());
      SegFieldInfo segFieldInfo;
      auto& facetReq = (FacetReq&)getOp();
      
      // Initialize storage based on pre-determined type
      if (std::holds_alternative<std::monostate>(mergeableData->counts)) {
        // First time initialization - use pre-determined storage type
        if (thisOp().useVector) {
          int64_t range = thisOp().globalMax - thisOp().globalMin + 1;
          mergeableData->counts = MergeableIntFacet::CountVector(range, 0);
          mergeableData->minValue = thisOp().globalMin;
        } else {
          mergeableData->counts = MergeableIntFacet::IntHash();
        }
      }
      
      // Now do the actual faceting with simple lambdas
      if (auto* countVec = std::get_if<MergeableIntFacet::CountVector>(&mergeableData->counts)) {
        // Vector storage - simple array indexing
        int64_t minVal = mergeableData->minValue;
        facetReq.facetSegIntCol(domain, segnum, mergeableData->missing_num, segFieldInfo, 
          [countVec, minVal](int32_t docid, int64_t val) SOLUX_INLINE {
            unused(docid);
            (*countVec)[val - minVal]++;
          });
      } else {
        // Map storage - direct value mapping
        auto* countMap = &std::get<MergeableIntFacet::IntHash>(mergeableData->counts);
        facetReq.facetSegIntCol(domain, segnum, mergeableData->missing_num, segFieldInfo, 
          [countMap](int32_t docid, int64_t val) SOLUX_INLINE {
            unused(docid);
            (*countMap)[val]++;
          });
      }
      
      auto merged = countMerger.release(mergeableData.release());
      if ((size_t)merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    };

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto* mergedData = countMerger.obtain();
      std::vector<std::pair<int64_t, int64_t>> countVec;
      
      // Handle both storage types
      if (auto* countMap = std::get_if<MergeableIntFacet::IntHash>(&mergedData->counts)) {
        // Map storage
        for (auto [val, count] : *countMap) {
          if (count >= minCount) {
            countVec.emplace_back(val, count);
          }
        }
      } else if (auto* countVector = std::get_if<MergeableIntFacet::CountVector>(&mergedData->counts)) {
        // Vector storage - convert indices back to values
        for (size_t i = 0; i < countVector->size(); i++) {
          if ((*countVector)[i] > 0 && (*countVector)[i] >= minCount) {
            countVec.emplace_back(mergedData->minValue + i, (*countVector)[i]);
          }
        }
      }
      
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      sortByCountDescAndLimit(countVec, limit);
      emitBuckets(facetResultProto, countVec);
      if (missing) {
        facetResultProto.set_missing(missing_count);
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
  FullTextFacetReq(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing){}

  class Calc : public Calculator {
    AtomicMerger<MergeableStrFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots) : Calculator(op, parent, slot, numSlots){}
    FullTextFacetReq& thisOp() {
      return (FullTextFacetReq&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      // Handle empty index case
      if (segnum == -1) {
        // Empty index - just generate empty result
        facetResult();
        return;
      }
      
      SegFieldInfo segFieldInfo;
      const FixedBitSet* domainBits = nullptr;
      if (domain && domain->type == DocSet::Type::BITSET) {
        domainBits = &static_cast<BitDocSet*>(domain)->bits();
      }
      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      boost::unordered_flat_map<std::string, int64_t>& counts = mergeableData->counts;

      auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      if (!found) {
        if (domain) {
          mergeableData->missing_num += domain->card();
        } else {
          mergeableData->missing_num += maxDoc;
        }
        auto merged = countMerger.release(mergeableData.release());
        if ((size_t)merged == thisOp().reader.segments().size()) {
          facetResult();
        }
        return;
      }
      fieldReader.readFieldInfo(segFieldInfo);
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      while (tenum.nextTerm()) {
        int64_t count = 0;
        DocsEnum denum(poolGuard.pool(), postingsReader, tenum);
        while (true) {
          auto doc = denum.nextDoc();
          if (doc == DocsEnum::END) {
            break; // no more docs for this term
          }
          if (domainBits) {
            if (!domainBits->get(doc)) continue;
          } else if (domain && !domain->get(doc)) {
            continue;
          }
          count++;
        }
        // use heterogeneous lookup in the future to avoid creating string when not needed
        if (count > 0) {
          counts[(std::string) (std::string_view) tenum.term()] += count;
        }
      }
      if (thisOp().missing) {
        // missing = in-domain docs that have no value for this field.  The
        // docs-with-value set is already indexed (the field-length/norms
        // column), so intersect it with the domain rather than rebuilding a
        // per-doc set during the term scan.
        DocsReader docsReader(postingsReader, segFieldInfo);
        int64_t domainCard = domain ? domain->card() : maxDoc;
        int64_t haveField;
        if (!docsReader.hasBitset()) {
          // dense: every doc has the field, so none in the domain are missing.
          haveField = domainCard;
        } else if (!domain) {
          // null domain == all docs, so the intersection is exactly docsWithField.
          haveField = docsReader.numDocs();
        } else {
          haveField = 0;
          screaming::BitSet::Iterator it(docsReader.bitset());
          for (int32_t doc = it.next(); doc != screaming::BitSet::END; doc = it.next()) {
            if (domain->get(doc)) {
              haveField++;
            }
          }
        }
        mergeableData->missing_num += domainCard - haveField;
      }
      auto merged = countMerger.release(mergeableData.release());
      if ((size_t)merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    }

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto* mergedData = countMerger.obtain();
      auto& counts = mergedData->counts;

      std::vector<std::pair<std::string, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (minCount == -1 || count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      sortByCountDescAndLimit(countVec, limit);
      emitBuckets(facetResultProto, countVec);
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }
    }


    };
  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

class IntFacetRangeReq : public FacetReq {
  int64_t start;
  int64_t end;
  int64_t gap;
public:
  // rangeFacet must reference the request proto (not a temporary): FacetReq
  // captures rangeFacet.sorts() by reference.
  IntFacetRangeReq(SearchRequest& req, const proto::RangeFacet& rangeFacet,
    std::string_view fieldName, std::string_view facetName,
    int64_t start, int64_t end, int64_t gap, int64_t minCount, bool missing)
  : FacetReq(req, fieldName, facetName, -1, minCount, missing, rangeFacet.sorts()),
    start(start), end(end), gap(gap) {}

  virtual ~IntFacetRangeReq() = default;

  class Calc : public Calculator {
    AtomicMerger<IntFacetReq::MergeableIntFacet> countMerger;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots) : Calculator(op, parent, slot, numSlots){}
    IntFacetRangeReq& thisOp() {
      return (IntFacetRangeReq&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      // Handle empty index case
      if (segnum == -1) {
        // Empty index - need to initialize merger with empty data
        std::unique_ptr<IntFacetReq::MergeableIntFacet> mergeableData(countMerger.obtain());
        mergeableData->counts = IntFacetReq::MergeableIntFacet::IntHash();
        countMerger.release(mergeableData.release());
        facetResult();
        return;
      }
      
      std::unique_ptr<IntFacetReq::MergeableIntFacet> mergeableData(countMerger.obtain());
      SegFieldInfo segFieldInfo;
      
      // IntFacetRangeReq always uses map storage since ranges can be arbitrary
      if (std::holds_alternative<std::monostate>(mergeableData->counts)) {
        mergeableData->counts = IntFacetReq::MergeableIntFacet::IntHash();
      }
      auto& count = std::get<IntFacetReq::MergeableIntFacet::IntHash>(mergeableData->counts);
      
      auto start = thisOp().start;
      auto end = thisOp().end;
      auto gap = thisOp().gap;
      auto& facetReq = (FacetReq&)getOp();
      facetReq.facetSegIntCol(domain, segnum, mergeableData->missing_num, segFieldInfo, [&](int32_t docid, int64_t val) SOLUX_INLINE {
        unused(docid);
        if (val < start || val >= end) {
          return; // value is out of range
        }
        count[(val-start)/gap]++;
      });
      auto merged = countMerger.release(mergeableData.release());
      if ((size_t)merged == thisOp().reader.segments().size()) {
        facetResult();
      }
    };

    void facetResult() {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;
      auto start = thisOp().start;
      auto end = thisOp().end;
      auto gap = thisOp().gap;

      auto* mergedData = countMerger.obtain();
      // IntFacetRangeReq always uses map storage
      auto& counts = std::get<IntFacetReq::MergeableIntFacet::IntHash>(mergedData->counts);
      std::vector<std::pair<int64_t, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (minCount == -1 || count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      std::sort(countVec.begin(), countVec.end(), [](auto& a, auto& b) {
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_multi_i();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        auto& pair = *bucketIdsArr.Add();
        pair.mutable_v()->Add(start + val * gap);
        int64_t bucketEnd = start + (val + 1) * gap;
        pair.mutable_v()->Add(std::min(bucketEnd, end));
        countsArr.Add(count);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }


    }
  };
  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  };
};

}
