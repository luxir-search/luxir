#pragma once

#include <variant>
#include <boost/unordered/unordered_flat_map.hpp>
#include "FacetOp.h"

namespace solux {

//
// expected vals = domain cardinality * field sparseness
//   field sparseness = number of docs with the field / maxdoc of the segment
//   TODO: for a multi-valued field, use total number of values in the field instead of numbe of docs with the field.
// 1) If expected vals is <<< unique vals: use hashmap for collector, collect global ords
//    Size for hashmap entry would be 16 bytes.  So we might break even (vs #2) if expected vals
//    is 32 times smaller than unique vals.  But #2 is also expected to have a performance advantage,
//    so maybe change the threshold to 64 times smaller.
// 2) If expected vals is << unique vals: use a skinny counter (vector of uint8_t + map for overflow)
//    if expected vals is still smaller than unique vals, we wouldn't expect many repeats and can
//    collect global ords.
// 3) if expected vals is >>> unique vals: collect segment level ords in a vector, then convert into global ord vector.
//    idea: if unique vals is large, we *could* use a separate skinny counter, and convert to global ords on bucket overflow.
//    but if unique vals is small enough it's not worth bothering?  Unless maybe if there is already a skinny counter
//    on the mergable data?
//
// Q: Is there a way to get a sense of the domain size *before* we do the facet?  One issue is that the smallest
//    segments will likely finish first, and we'll be making initial decisions based on them, while the larger
//    segments will actually be more important.
//

class StrFacetOp : public FieldFacetReq {
  std::shared_ptr<OrdMap> ordMap;
  class MergeableStrFacet : public solux::MergeableData {
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

  StrFacetOp(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName,
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

    StrFacetOp& thisOp() {
      return (StrFacetOp&)getOp();
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
      // Handle empty index case
      if (segnum == -1) {
        // Empty index - just generate empty result
        std::unique_ptr<MergeableStrFacet> mergeableData(countMerger.obtain());
        facetResult(tg, std::move(mergeableData));
        return;
      }

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

} // namespace solux