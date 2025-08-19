#pragma once
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "protos/solux_types.pb.h"
#include "solux/search/DocSet.h"
#include "solux/search/IndexReader.h"
#include "SearchOp.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/schema/Schema.h"
#include "solux/search/OrdMapStr.h"
#include "solux/util/AtomicMerger.h"

namespace solux {
class FacetDomain {
public:
  std::vector<BitDocSet> allMatches;
};

class FacetReq : public SearchOp {
public:
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  int64_t minCount; // minimum count for a facet to be included in the result
  bool missing;

  google::protobuf::RepeatedPtrField<proto::SortSpec> sorts;
  std::string_view facetName;
  std::vector<std::pair<const std::string_view, SearchOp*>> inlineSubOps;

  static FacetReq* createFieldFacetReq(SearchRequest& req, std::string_view facetName, const proto::FieldFacet& facetReq,
                                        google::protobuf::Arena& arena);

  FacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing, google::protobuf::RepeatedPtrField<proto::SortSpec> sorts)
    : SearchOp(req, facetName), reader(*req.reader), fieldName(fieldName), limit(limit), minCount(minCount), missing(missing),
      facetName(facetName), sorts(sorts) {
  }

  virtual ~FacetReq() = default;

  void init() override {
    SearchOp::init();
    if (sorts.empty()) {
      for (auto& sort : sorts) {
        auto iter = subOps.find(sort.field());
        if (iter != subOps.end()) {
          if (iter->second->canInline()) {
            inlineSubOps.push_back(*iter);
            subOps.erase(iter);
          } else {
            throw std::runtime_error("Cannot sort by a subop without inline support: " + std::string(iter->second->name));
          }
        }
      }
    }
    //if there's no limit, more efficient to do inline
    if (limit == -1) {
      for (auto& subOp : subOps) {
        if (subOp.second->canInline()) {
          inlineSubOps.push_back(subOp);
          subOps.erase(subOps.find(subOp.first));
        }
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
          auto [start, end] = intColReader.getStartEndRank(intColIter.rank());
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
            auto [start, end] = intColReader.getStartEndRank(iter.rank());
            auto n = end - start;
            for (int64_t vrank = 0; vrank < n; vrank++) {
              auto val = iter.values().valueAt(start + vrank);
              callback(docid, val);
            }
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
public:
  class MergeableIntFacet : public MergeableData {
  public:
    boost::unordered_flat_map<int64_t, int64_t> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableIntFacet* merge(MergeableIntFacet* a, MergeableIntFacet* b) {
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
private:
  AtomicMerger<MergeableIntFacet> countMerger;
public:
  IntFacetReq(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName, std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing){}

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
      std::unique_ptr<MergeableIntFacet> mergeableData(countMerger.obtain());
      boost::unordered_flat_map<int64_t, int64_t>& count = mergeableData->counts;
      SegFieldInfo segFieldInfo;
      auto& facetReq = (FacetReq&)getOp();
      // After facetSegIntCol was upgraded to handle ArrDocSet and BitDocSet, we saw performance degredation of 20-40%.
      // Forcing inline on the callback lambda here resolved the issue.
      facetReq.facetSegIntCol(domain, segnum, mergeableData->missing_num, segFieldInfo, [&](int32_t docid, int64_t val) SOLUX_INLINE {
        unused(docid);
        count[val]++;
      });
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
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
      auto& counts = mergedData->counts;
      std::vector<std::pair<int64_t, int64_t>> countVec;
      for (auto [val, count] : counts) {
        if (count >= minCount) {
          countVec.emplace_back(val, count);
        }
      }
      auto missing_count = mergedData->missing_num;
      delete mergedData;
      std::sort(countVec.begin(), countVec.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second ) {
          return a.second > b.second;
        }
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_i();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        bucketIdsArr.Add(val);
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

class StrFacetReq : public FieldFacetReq {
  std::shared_ptr<OrdMap> ordMap;
  class MergeableStrFacet : public MergeableData {
  public:
    using StrHash = boost::unordered_flat_map<std::string, int64_t>;
    using OrdHash = boost::unordered_flat_map<int64_t, int64_t>;
    using CountVector = std::vector<int64_t>;
    std::variant<StrHash, OrdHash, CountVector> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableStrFacet* merge(MergeableStrFacet* a, MergeableStrFacet* b) {
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (auto* astr = std::get_if<StrHash>(&a->counts)) {
        auto& bstr = std::get<StrHash>(b->counts);
        if (astr->size() < bstr.size()) {
          std::swap(a, b);
          std::swap(*astr, bstr);
        }
        for (auto [val, count] : bstr) {
          (*astr)[val] += count;
        }
      } else if (auto* avec = std::get_if<CountVector>(&a->counts)) {
        if (auto* bvec = std::get_if<CountVector>(&b->counts)) {
          for (int i = 0; i < bvec->size(); i++) {
            (*avec)[i] += (*bvec)[i];
          }
        }  else {
          auto& bord = std::get<OrdHash>(b->counts);
          for (auto [val, count] : bord) {
            (*avec)[val] += count;
          }
        }
      }
      else if (auto* bvec = std::get_if<CountVector>(&b->counts)) {
        auto& aord = std::get<OrdHash>(a->counts);
        for (auto [val, count] : aord) {
          (*bvec)[val] += count;
        }
        std::swap(a, b);
      } else {
        auto* aord = &std::get<OrdHash>(a->counts);
        auto* bord = &std::get<OrdHash>(b->counts);
        if (aord->size() < bord->size()) {
          std::swap(a, b);
          std::swap(aord, bord);
        }
        for (auto [val, count] : *bord) {
          (*aord)[val] += count;
        }
      }
      a->missing_num += b->missing_num;
      return a;
    }
  };

  class MergeableStrFacetInline : public MergeableData {
  public:
    FacetMap<std::string> counts;
    int64_t missing_num = 0; // number of missing values in this segment
    std::vector<SearchOp::InlineCalculator*> inlineCalcs;
    static MergeableStrFacetInline* merge(MergeableStrFacetInline* a, MergeableStrFacetInline* b) {
      // merge the smaller collector into the larger collector, or if both the same size, merge
      // the less competitive collector into the more competitive collector.
      if (a->counts.map.size() < b->counts.map.size()) {
        std::swap(a,b);
      }

      a->counts.merge(b->counts);
      a->missing_num += b->missing_num;
      return a;
    }
    ~MergeableStrFacetInline() {
      for (auto* calc : inlineCalcs) {
        delete calc; // clean up the inline calculators
      }
    }
  };

public:

  StrFacetReq(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing){}

  void init() override {
    FacetReq::init();
    ordMap = req.reader->getOrdMap(fieldName);
  }

  class Calc : public Calculator {
    std::vector<DocSet*> input;
    AtomicMerger<MergeableStrFacet> countMerger;
    AtomicMerger<MergeableStrFacetInline> inlineMerger;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots) : Calculator(op, parent, slot, numSlots) {
      input.resize(op.req.reader->segments().size());




      inlineMerger.creator = [this]() {
        auto* p = new MergeableStrFacetInline;

        for (auto& [key, subop] : thisOp().inlineSubOps) {
          auto* calc = subop->createInlineCalculator(this, -1, -1);
          p->inlineCalcs.push_back(calc);

        }

        p->counts.calcs = p->inlineCalcs;
        return p;
      };
    }

    StrFacetReq& thisOp() {
      return (StrFacetReq&)getOp();
    }


    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(searchResponse, this);
      // the Val should either be unset, or have a DocList
      assert(
        ourVal != nullptr && (ourVal->kind_case() == solux::proto::Val::kFacet
          || ourVal->kind_case() == solux::proto::Val::KIND_NOT_SET));
      return &(*ourVal->mutable_facet()->mutable_ops())[sub->getOp().name];
      //TODO: need to account for slot somehow,  or will subop do that?
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      if (!thisOp().inlineSubOps.empty()) {
        calc2(tg, segnum, domain);
        return;
      } else {
        calcOrdMap(tg, segnum, domain);
        return;
      }
      //write only to different slots, so no need to synchronize
      input[segnum] = domain;
      SegFieldInfo segFieldInfo;
      boost::unordered_flat_map<int64_t, int64_t> count;
      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
        [&](int32_t docid, int64_t val) SOLUX_INLINE {
          unused(docid);
          count[val]++;
        });

      if (count.empty()) {
        // no values found, so we can just return
        if (missing_num > 0) {
          auto* mergeableData = countMerger.obtain();
          mergeableData->missing_num += missing_num;
          countMerger.release(mergeableData);
        }
        return;
      }
      PostingsReader& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();

      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      mergeableData->missing_num += missing_num;
      TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
      auto* strMap = std::get_if<MergeableStrFacet::StrHash>(&mergeableData->counts);
      if (!strMap) {
        mergeableData->counts = MergeableStrFacet::StrHash();
        strMap = &std::get<MergeableStrFacet::StrHash>(mergeableData->counts);
      }
      for (auto [ord, count] : count) {
        tenum.seekOrd(ord - 1);
        std::string_view termView = (std::string_view) tenum.term();
        (*strMap)[std::string(termView)] += count;
      }
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult(tg, std::unique_ptr<MergeableStrFacet>(countMerger.obtain()));
      }
    };

    void calc2(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) {
      std::unique_ptr<MergeableStrFacetInline> mergeableData(inlineMerger.obtain());
      for (auto* calc : mergeableData->inlineCalcs) {
        calc->startSeg(segnum);
      }
      input[segnum] = domain;
      SegFieldInfo segFieldInfo;
      PostingsReader& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();

      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      std::optional<TermsEnum> tenum;
      bool found = fieldReader.seek(thisOp().fieldName);
      if (found) {
        fieldReader.readFieldInfo(segFieldInfo);
        tenum.emplace(poolGuard.pool(), postingsReader, segFieldInfo);
      }
      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
        [&](int32_t docid, int64_t val)SOLUX_INLINE {
          tenum->seekOrd(val - 1);
        std::string_view termView = (std::string_view) tenum->term();
          mergeableData->counts.add((std::string) termView, docid);
        });

      mergeableData->missing_num += missing_num;

      auto merged = inlineMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult2(tg, std::unique_ptr<MergeableStrFacetInline>(inlineMerger.obtain()));
      }
    }

    void calcOrdMap(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) {
      //write only to different slots, so no need to synchronize
      input[segnum] = domain;
      SegFieldInfo segFieldInfo;
      auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      if (!found) {
        if (domain) {
          mergeableData->missing_num += domain->card();
        } else {
          mergeableData->missing_num += maxDoc;
        }
        auto merged = countMerger.release(mergeableData.release());
        if (merged == thisOp().reader.segments().size()) {
          facetResult(tg, std::unique_ptr<MergeableStrFacet>(countMerger.obtain()));
        }
        return; // field not found, nothing to do
      }
      fieldReader.readFieldInfo(segFieldInfo);
      auto* countVec = std::get_if<MergeableStrFacet::CountVector>(&mergeableData->counts);
      auto* countMap = std::get_if<MergeableStrFacet::OrdHash>(&mergeableData->counts);
      int32_t domainSize = domain ? domain->card() : maxDoc;
      if (!countVec && !countMap) {
        if (domainSize >= segFieldInfo.nTerms) {
          mergeableData->counts = MergeableStrFacet::CountVector();
          countVec = &std::get<MergeableStrFacet::CountVector>(mergeableData->counts);
          if (countVec->empty()) {
            if (thisOp().ordMap) {
              countVec->resize(thisOp().ordMap->numOrds());
            } else {
              countVec->resize(segFieldInfo.nTerms);
            }
          }

        } else {
          mergeableData->counts = MergeableStrFacet::OrdHash();
          countMap = &std::get<MergeableStrFacet::OrdHash>(mergeableData->counts);
        }
      }

      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      MonoReader* deltas = nullptr;
      if (thisOp().ordMap) {
        auto segtoGlobal = thisOp().ordMap->getSegToGlobal(segnum);
        deltas = segtoGlobal.deltas;
      }

      if (domainSize >= segFieldInfo.nTerms >> 1) {
        std::vector<int64_t> localCounts(segFieldInfo.nTerms);
        facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
          [&](int32_t docid, int64_t ord) SOLUX_INLINE {
            unused(docid);
            ord--; // ordMap is zero-based, int columns are one-based
            localCounts[ord]++;
          });

        for (int i = 0; i < localCounts.size(); i++) {
          auto count = localCounts[i];
          auto ord = i;
          if (count > 0) {
            if (deltas) {
              ord += deltas->valueAt(ord);
            }
            if (countMap) {
              (*countMap)[ord] += count;
            } else {
              assert(countVec);
              (*countVec)[ord] += count;
            }
          }
        }

      } else if (countMap) {
        facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
          [&](int32_t docid, int64_t ord) SOLUX_INLINE {
            unused(docid);
            ord--; // ordMap is zero-based, int columns are one-based
            if (deltas) {
              ord += deltas->valueAt(ord);
            }
            (*countMap)[ord]++;
          });
      } else {
        assert(countVec);
        facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
          [&](int32_t docid, int64_t ord) SOLUX_INLINE {
            unused(docid);
            ord--; // ordMap is zero-based, int columns are one-based
            if (deltas) {
              ord += deltas->valueAt(ord);
            }
            (*countVec)[ord]++;
          });
      }
      mergeableData->missing_num += missing_num;

      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult(tg, std::unique_ptr<MergeableStrFacet>(countMerger.obtain()));
      }
    }

    void calcVector(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) {
      //write only to different slots, so no need to synchronize
      input[segnum] = domain;
      SegFieldInfo segFieldInfo;
      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      auto* count = std::get_if<MergeableStrFacet::CountVector>(&mergeableData->counts);
      if (!count) {
        mergeableData->counts = MergeableStrFacet::CountVector();
        count = &std::get<MergeableStrFacet::CountVector>(mergeableData->counts);
      }
      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      MonoReader* deltas = nullptr;
      if (thisOp().ordMap) {
        auto segtoGlobal = thisOp().ordMap->getSegToGlobal(segnum);
        deltas = segtoGlobal.deltas;
      }
      facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
        [&](int32_t docid, int64_t ord) SOLUX_INLINE {
          unused(docid);
          ord--; // ordMap is zero-based, int columns are one-based
          if (deltas) {
            ord += deltas->valueAt(ord);
          }
          (*count)[ord]++;
        });
      mergeableData->missing_num += missing_num;

      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
        facetResult(tg, std::unique_ptr<MergeableStrFacet>(countMerger.obtain()));
      }
    }

    void facetResult(oneapi::tbb::task_group* tg, std::unique_ptr<MergeableStrFacet> mergedData) {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto missing_count = -1;

      std::vector<std::pair<std::string, int64_t>> countVec;

      if (auto* strCounts = std::get_if<MergeableStrFacet::StrHash>(&mergedData->counts)) {
        for (auto& [val, count] : *strCounts) {
          if (minCount == -1 || count >= minCount) {
            countVec.emplace_back(val, count);
          }
        }
        missing_count = mergedData->missing_num;
        std::sort(countVec.begin(), countVec.end(), [](auto& a, auto& b) {
          if (a.second != b.second ) {
            return a.second > b.second;
          }
          return a.first < b.first;
        });
        if (limit >= 0 && limit < (int64_t)countVec.size()) {
          countVec.resize(limit);
        }
      } else if (auto* mapCounts = std::get_if<MergeableStrFacet::OrdHash>(&mergedData->counts)) {
        std::vector<std::pair<int64_t, int64_t>> ordCounts;
        for (auto& [val, count] : *mapCounts) {
          if (minCount == -1 || count >= minCount) {
            ordCounts.emplace_back(val, count);
          }
        }
        missing_count = mergedData->missing_num;
        std::sort(ordCounts.begin(), ordCounts.end(), [](auto& a, auto& b) {
          if (a.second != b.second ) {
            return a.second > b.second;
          }
          return a.first < b.first;
        });
        if (limit >= 0 && limit < (int64_t)ordCounts.size()) {
          ordCounts.resize(limit);
        }
        countVec.reserve(ordCounts.size());
        auto poolGuard = MemPool::threadLocalPoolGuard();
        OrdMapStr ordMapStr(poolGuard.pool(), thisOp().ordMap.get(), *thisOp().req.reader, thisOp().fieldName);
        for (auto [ord, count] : ordCounts) {
          auto val = ordMapStr.ordToStr(ord);
          countVec.emplace_back(val, count);
        }
      } else {
        auto* vecCounts = &std::get<MergeableStrFacet::CountVector>(mergedData->counts);
        std::vector<std::pair<int64_t, int64_t>> ordCounts;
        for (int i = 0; i < vecCounts->size(); i++) {
          if (minCount == -1) {
            minCount = 1;
          }
          if ((*vecCounts)[i] >= minCount) {
            ordCounts.emplace_back(i, (*vecCounts)[i]);
          }
        }
        missing_count = mergedData->missing_num;
        std::sort(ordCounts.begin(), ordCounts.end(), [](auto& a, auto& b) {
          if (a.second != b.second ) {
            return a.second > b.second;
          }
          return a.first < b.first;
        });
        if (limit >= 0 && limit < (int64_t)ordCounts.size()) {
          ordCounts.resize(limit);
        }
        countVec.reserve(ordCounts.size());
        auto poolGuard = MemPool::threadLocalPoolGuard();
        OrdMapStr ordMapStr(poolGuard.pool(), thisOp().ordMap.get(), *thisOp().req.reader, thisOp().fieldName);
        for (auto [ord, count] : ordCounts) {
          auto val = ordMapStr.ordToStr(ord);
          countVec.emplace_back(val, count);
        }
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_s();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        auto* strptr = bucketIdsArr.Add();
        *strptr = val; // copy the string
        countsArr.Add(count);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }

      doSubops(thisOp().subOps, countVec);
    }
    void facetResult2(oneapi::tbb::task_group* tg, std::unique_ptr<MergeableStrFacetInline> mergedData) {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;
      mergedData->counts.finalize();

      std::vector<std::pair<std::string, char*>> valVec;
        auto& counts = mergedData->counts.map;
        for (auto& [key, val] : counts) {
          int64_t count = *(int64_t*)val;
          if (minCount == -1 || count >= minCount) {
            valVec.emplace_back(key, val);
          }
        }
        auto missing_count = mergedData->missing_num;
      if (thisOp().fieldFacet.sorts().empty()) {
        std::sort(valVec.begin(), valVec.end(), [](auto& a, auto& b) {
          if (*(int64_t*)a.second != *(int64_t*)b.second ) {
            return *(int64_t*)a.second > *(int64_t*)b.second;
          }
          return a.first < b.first;
        });
      } else if (false) {
      } else if (false) {
      } else {
        std::string_view field = thisOp().fieldFacet.sorts(0).field();
        auto calc = mergedData->inlineCalcs.front();
        assert(field == calc->getOp().name);
        bool reversed = thisOp().fieldFacet.sorts(0).dir() == proto::SortSpec_SortDir_DESC;
        std::sort(valVec.begin(), valVec.end(), [&calc, reversed](auto& a, auto& b) {
          int asize, bsize;
          int  cmp = calc->compare(a.second + sizeof(int64_t), b.second + sizeof(int64_t), asize, bsize);
          if (cmp == 0) {
            return a.first < b.first; // tie-break by bucketid asc
          }
          return reversed ? cmp > 0 : cmp < 0;
        });
      }
      if (limit >= 0 && limit < (int64_t)valVec.size()) {
        valVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_s();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(valVec.size());
      countsArr.Reserve(valVec.size());
      for (auto [key, val] : valVec) {
        auto* strptr = bucketIdsArr.Add();
        *strptr = key; // copy the string
        countsArr.Add(*(int64_t*)val);
      }
      if (missing) {
        facetResultProto.set_missing(missing_count);
      }

      // fill in results from inline calculators
      std::vector<char*> results;
      results.reserve(valVec.size());
      for (auto [key, val] : valVec) {
        results.push_back(val + sizeof(int64_t));
      }
      for (auto calc : mergedData->inlineCalcs) {
        calc->fillResult(results);
      }

      auto opers = thisOp().subOps;
      for (auto& icalc : mergedData->inlineCalcs) {
        opers.erase(icalc->getOp().name);
      }
      doSubops(opers, valVec);
    }

    void doSubops(boost::unordered_flat_map<std::string_view, SearchOp*> &opers, auto& valVec) {
      if (opers.empty()) {
        return; // no post sub ops, nothing to do.
      }
      int64_t slotNum = 0;
      for (auto [key, val] : valVec) {
        std::vector<std::unique_ptr<SearchOp::Calculator>> calculators;
        calculators.reserve(opers.size());
        for (auto& [name, subOp] : opers) {
          auto* subCalc = subOp->createCalculator(this, slotNum, valVec.size());
          calculators.emplace_back(subCalc);
        }
        for (size_t segnum = 0; segnum < input.size(); segnum++) {
          SegFieldInfo segFieldInfo;
          ArrDocSet emptyDomain({});
          DocSet* newDomain = &emptyDomain;  // NOTE - points to stack object
          auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
          int32_t maxDoc = postingsReader.maxDoc();
          auto poolGuard = MemPool::threadLocalPoolGuard();
          FieldReader fieldReader(poolGuard.pool(), postingsReader);
          bool found = fieldReader.seek(thisOp().fieldName);
          RAMBitDocSet output(maxDoc);
          if (found) {
            fieldReader.readFieldInfo(segFieldInfo);
            TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
            if (tenum.seek(key)) {
              newDomain = &output; // we will write to output
              DocsEnum denum(poolGuard.pool(), postingsReader, tenum);
              while (true) {
                auto doc = denum.nextDoc();
                if (doc == DocsEnum::END) {
                  break; // no more docs for this term
                }
                if (input[segnum] && !input[segnum]->get(doc)) {
                  continue; // this doc is not in the domain
                }
                output.mutableBits().set(doc);
              }
            }
          }
          for (auto& subCalc : calculators) {
            //subCalc->calc(tg, segnum, &output);
            // no support for subcalcs launching tasks yet

            subCalc->calc(nullptr, segnum, newDomain);
          }
        }
        slotNum++;
      }
    }
  };
  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots) override {
    return new Calc(*this, parent, slot, numSlots);
  }
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
    StrFacetReq& thisOp() {
      return (StrFacetReq&)getOp();
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      return nullptr;
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      SegFieldInfo segFieldInfo;
      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      BitDocSet* bitDocs = (BitDocSet*) domain;
      auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;
      std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
      boost::unordered_flat_map<std::string, int64_t>& counts = mergeableData->counts;

      auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(poolGuard.pool(), postingsReader);
      bool found = fieldReader.seek(thisOp().fieldName);
      if (!found) {
        if (bitDocs) {
          mergeableData->missing_num += bitDocs->card();
        } else {
          mergeableData->missing_num += maxDoc;
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
          if (domainBits && !domainBits->get(doc)) {
            continue; // this doc is not in the domain
          }
          count++;
        }
        // use heterogeneous lookup in the future to avoid creating string when not needed
        counts[(std::string) (std::string_view) tenum.term()] += count;
      }
      auto merged = countMerger.release(mergeableData.release());
      if (merged == thisOp().reader.segments().size()) {
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
      std::sort(countVec.begin(), countVec.end(), [](auto& a, auto& b) {
        if (a.second != b.second ) {
          return a.second > b.second;
        }
        return a.first < b.first;
      });
      if (limit >= 0 && limit < (int64_t)countVec.size()) {
        countVec.resize(limit);
      }

      // fill in the facet result proto
      auto& bucketIds = *facetResultProto.mutable_bucket_ids()->mutable_col_s();
      auto& bucketIdsArr = *bucketIds.mutable_v();
      auto& countsArr = *facetResultProto.mutable_counts();
      bucketIdsArr.Reserve(countVec.size());
      countsArr.Reserve(countVec.size());
      for (auto [val, count] : countVec) {
        auto* strptr = bucketIdsArr.Add();
        *strptr = val; // copy the string
        countsArr.Add(count);
      }
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
  IntFacetRangeReq(SearchRequest& req, proto::RangeFacet rangeFacet, std::string_view fieldName, std::string_view facetName, int64_t start, int64_t end, int64_t gap, int64_t minCount, bool missing)
  : FacetReq(req, fieldName, facetName, -1, minCount, missing, rangeFacet.sorts()), start(start), end(end), gap(gap) {}

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
      std::unique_ptr<IntFacetReq::MergeableIntFacet> mergeableData(countMerger.obtain());
      SegFieldInfo segFieldInfo;
      boost::unordered_flat_map<int64_t, int64_t>& count = mergeableData->counts;
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
      if (merged == thisOp().reader.segments().size()) {
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
      auto& counts = mergedData->counts;
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

inline FacetReq* FacetReq::createFieldFacetReq(SearchRequest& req, std::string_view facetName, const proto::FieldFacet& facetReq,
                                        google::protobuf::Arena& arena) {
  auto facetField = facetReq.field();
  int64_t limit = 5; // default limit
  if (facetReq.has_limit()) {
    limit = facetReq.limit();
  }
  int64_t minCount = -1;
  if (facetReq.has_mincount()) {
    minCount = facetReq.mincount();
  }
  auto missing = facetReq.missing();
  //arena allocate FacetReq
  FacetReq* facet = nullptr;
  auto& ftype = req.schema->getFieldTypeEx(facetField);

  switch (ftype->type()) {
    case FieldType::Type::INT:
      facet = google::protobuf::Arena::Create<IntFacetReq>(&arena, req, facetReq, facetField, facetName, limit, minCount,  missing);
      break;
    case FieldType::Type::STRING:
      facet = google::protobuf::Arena::Create<StrFacetReq>(&arena, req, facetReq, facetField, facetName, limit, minCount, missing);
      break;
    case FieldType::Type::TEXT:
      facet = google::protobuf::Arena::Create<FullTextFacetReq>(&arena, req, facetReq, facetField, facetName, limit, minCount, missing);
      break;
    default: ;
  }
  if (facet == nullptr) {
    throw std::runtime_error("Unknown facet field type: " + std::string(facetField));
  }
  return facet;
}

}