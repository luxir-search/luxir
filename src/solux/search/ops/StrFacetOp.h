#pragma once

#include <variant>
#include <boost/unordered/unordered_flat_map.hpp>
#include "FacetOp.h"
#include "SkinnyCounter.h"

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
public:
  class MergeableStrData : public solux::MergeableData {
  public:
    using OrdHash = boost::unordered_flat_map<int64_t, int64_t>;
    using CountVector = std::vector<int64_t>;
    std::variant<std::monostate, OrdHash, SkinnyCounter8, CountVector> counts;
    int64_t missing_num = 0; // number of missing values in this segment

    static MergeableStrData* merge(MergeableStrData* a, MergeableStrData* b) {
      // start by updating missing_num of both (we will return one or the other)
      a->missing_num += b->missing_num;
      b->missing_num = a->missing_num;

      if (std::holds_alternative<std::monostate>(a->counts)) {
        return b;
      }
      if (std::holds_alternative<std::monostate>(b->counts)) {
        return a;
      }

      // If either variant is a CountVector, merge the other into it.
      auto* avec = std::get_if<CountVector>(&a->counts);
      auto* bvec = std::get_if<CountVector>(&b->counts);

      if (avec || bvec) {
        // normalize so avec has the CountVector
        if (!avec) {
          std::swap(avec, bvec);
          std::swap(a, b);
        }

        if (bvec) {
          for (size_t i = 0u; i < bvec->size(); i++) {
            (*avec)[i] += (*bvec)[i];
          }
        } else if (auto* bord = std::get_if<OrdHash>(&b->counts)) {
          for (auto [val, count] : *bord) {
            if (val >= 0 && val < (int64_t)avec->size()) {
              (*avec)[val] += count;
            }
          }
        } else if (auto* bskinny = std::get_if<SkinnyCounter8>(&b->counts)) {
          for (size_t i = 0u; i < bskinny->counts.size(); i++) {
            (*avec)[i] += bskinny->counts[i];
          }
          for (auto [val, count] : bskinny->overflow) {
            (*avec)[val] += count;
          }
        } else {
          // should not happen
          assert(false);
        }

        return a;
      }

      // Next up: if either is a SkinnyCounter8, merge the other into it.
      // We know that we won't have a CountVector at this point.
      auto* askinny = std::get_if<SkinnyCounter8>(&a->counts);
      auto* bskinny = std::get_if<SkinnyCounter8>(&b->counts);

      if (askinny || bskinny) {
        // normalize so askinny has the SkinnyCounter8
        if (!askinny) {
          std::swap(askinny, bskinny);
          std::swap(a, b);
        }

        // normalize so "a" has larger overflow map.
        if (askinny && bskinny && askinny->overflow.size() < bskinny->overflow.size()) {
          std::swap(askinny, bskinny);
          std::swap(a, b);
        }

        if (bskinny) {
          askinny->merge(*bskinny);
        } else if (auto* bord = std::get_if<OrdHash>(&b->counts)) {
          for (auto [val, count] : *bord) {
            askinny->increment(val, count);
          }
        } else {
          // should not happen
          assert(false);
        }

        return a;
      }

      // should be only option left.
      auto* aord = &std::get<OrdHash>(a->counts);
      auto* bord = &std::get<OrdHash>(b->counts);
      assert(aord && bord);
      if (aord->size() < bord->size()) {
        std::swap(a, b);
        std::swap(aord, bord);
      }
      for (auto [val, count] : *bord) {
        (*aord)[val] += count;
      }

      return a;
    }
  };

private:
  std::shared_ptr<OrdMap> ordMap;

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

  // Ctor must be nothrow (Arena::Create hazard).  ProtobufSearchParser
  // resolves the OrdMap before allocation and passes it in.
  StrFacetOp(SearchRequest& req, const proto::FieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing,
    std::shared_ptr<OrdMap> ordMap) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing),
  ordMap(std::move(ordMap)) {}

  class Calc : public Calculator {
    std::vector<DocSet*> input;
    AtomicMerger<MergeableStrData> countMerger;
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
        std::unique_ptr<MergeableStrData> mergeableData(countMerger.obtain());
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
      if ((size_t)merged == thisOp().reader.segments().size()) {
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
      std::unique_ptr<MergeableStrData> mergeableData(countMerger.obtain());
      if (!found) {
        if (domain) {
          mergeableData->missing_num += domain->card();
        } else {
          mergeableData->missing_num += maxDoc;
        }
        auto merged = countMerger.release(mergeableData.release());
        if ((size_t)merged == thisOp().reader.segments().size()) {
          facetResult(tg, std::unique_ptr<MergeableStrData>(countMerger.obtain()));
        }
        return; // field not found, nothing to do
      }
      fieldReader.readFieldInfo(segFieldInfo);

      auto* countVec = std::get_if<MergeableStrData::CountVector>(&mergeableData->counts);
      auto* countMap = std::get_if<MergeableStrData::OrdHash>(&mergeableData->counts);
      auto* countSkinny = std::get_if<SkinnyCounter8>(&mergeableData->counts);

      int32_t domainSize = domain ? domain->card() : maxDoc;
      int64_t globVals = thisOp().ordMap ? thisOp().ordMap->numOrds() : segFieldInfo.nTerms;

      // want a vector of global ords if the domain size is much larger than the number of unique values
      // such that a skinny counter would have many overflows.
      bool wantVec = (domainSize >> 8) >= globVals;

      // if the number of unique values is large compared to the domain size, we want to use a hashmap
      bool wantHash = (globVals >> 6) >= domainSize;

      // skinny is the default in the middle.
      bool wantSkinny = !wantVec && !wantHash;

      // A segment that finished before us may have chosen a different storage type.
      // If they chose a larger storage type (vector or skinny), then we should stick
      // with that.  If they chose a smaller storage type (hashmap or skinny), then we
      // can upgrade.  We do the upgrade by pretending we don't have any storage yet
      // and then merging the old storage into the new storage we create.
      std::unique_ptr<MergeableStrData> oldMergeableData;
      if ((wantVec && (countMap || countSkinny))
        || (wantSkinny && countMap)) {
        std::swap(mergeableData, oldMergeableData);
        mergeableData.reset(countMerger.creator()); // create new empty storage
        mergeableData->releaseCount = oldMergeableData->releaseCount;  // carry over the release count.
        countMap = nullptr;
        countSkinny = nullptr;
      }

      if (!countVec && !countMap && !countSkinny) {
        if (wantVec) {
          mergeableData->counts.emplace<MergeableStrData::CountVector>();
          countVec = &std::get<MergeableStrData::CountVector>(mergeableData->counts);
          if (countVec->empty()) {
            countVec->resize(globVals);
          }
        } else if (wantHash) {
          mergeableData->counts.emplace<MergeableStrData::OrdHash>();
          countMap = &std::get<MergeableStrData::OrdHash>(mergeableData->counts);
        } else if (wantSkinny) {
          mergeableData->counts.emplace<SkinnyCounter8>(globVals);
          countSkinny = &std::get<SkinnyCounter8>(mergeableData->counts);
        }
      }

      if (oldMergeableData) {
        auto* result = MergeableStrData::merge(mergeableData.get(), oldMergeableData.get());
        // result should always be the new one we created.
        assert(result == mergeableData.get());
        oldMergeableData.reset();  // free up memory from the original one.
      }


      int64_t missing_num = 0;
      auto& facetReq = (FacetReq&)getOp();
      MonoReader* deltas = nullptr;
      if (thisOp().ordMap) {
        auto segtoGlobal = thisOp().ordMap->getSegToGlobal(segnum);
        deltas = segtoGlobal.deltas;
      }

      // if we want a vector, then there are enough repeats that we should collect
      // local counts first and then only convert to global ords once.
      if (countVec) {
        // TODO: we have a countVec, but if we wanted a map, that means we should
        // probably not do 2 pass.  Unclear how often this will happen.
        // NOTE: skip 2 phase if the ords for this segment are the same as global ords!

        // for single-valued, we could get away with int32_t
        std::vector<int64_t> localCounts(segFieldInfo.nTerms);
        facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
          [&](int32_t docid, int64_t ord) SOLUX_INLINE {
            unused(docid);
            ord--; // ordMap is zero-based, int columns are one-based
            localCounts[ord]++;
          });

        for (size_t i = 0; i < localCounts.size(); i++) {
          auto count = localCounts[i];
          auto ord = i;
          if (count > 0) {
            if (deltas) {
              ord += deltas->valueAt(ord);
            }
            (*countVec)[ord] += count;
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
        assert(countSkinny);
        // If localords != globalOrds and expected number of repeats per value is > 2, use a local skinny counter first
        // and convert to global ords on overflow.
        if (deltas && domainSize >= (segFieldInfo.nTerms >> 1)) {
          std::vector<uint8_t> localCounts(segFieldInfo.nTerms);
          facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
            [&](int32_t docid, int64_t ord) SOLUX_INLINE {
              unused(docid);
              ord--; // ordMap is zero-based, int columns are one-based
              if (++localCounts[ord] == 0) {
                ord += deltas->valueAt(ord);  // convert to global ord
                countSkinny->increment(ord, std::numeric_limits<uint8_t>::max() + 1);
              }
            });
          for (size_t i = 0; i < localCounts.size(); i++) {
            auto count = localCounts[i];
            if (count > 0) {
              int64_t ord = i + deltas->valueAt(i);  // convert to global ord
              countSkinny->increment(ord, count);
            }
          }
        } else {
          // Not many repeats expected, so just collect global ords directly.
          facetReq.facetSegIntCol(domain, segnum, missing_num, segFieldInfo,
            [&](int32_t docid, int64_t ord) SOLUX_INLINE {
              unused(docid);
              ord--; // ordMap is zero-based, int columns are one-based
              if (deltas) {
                ord += deltas->valueAt(ord);
              }
              countSkinny->increment(ord);
            });
        }
      }
      mergeableData->missing_num += missing_num;

      auto merged = countMerger.release(mergeableData.release());
      if ((size_t)merged == thisOp().reader.segments().size()) {
        facetResult(tg, std::unique_ptr<MergeableStrData>(countMerger.obtain()));
      }
    }

    void facetResult(oneapi::tbb::task_group* tg, std::unique_ptr<MergeableStrData> mergedData) {
      auto* myVal = getTarget(nullptr);
      solux::proto::FacetResult& facetResultProto = *myVal->mutable_facet();
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      auto missing_count = -1;

      std::vector<std::pair<std::string, int64_t>> countVec;

      if (std::holds_alternative<std::monostate>(mergedData->counts)) {
        missing_count = mergedData->missing_num;
      } else {
        auto* mapCounts = std::get_if<MergeableStrData::OrdHash>(&mergedData->counts);
        auto* skinnyCounts = std::get_if<SkinnyCounter8>(&mergedData->counts);
        auto* vecCounts   = std::get_if<MergeableStrData::CountVector>(&mergedData->counts);
        std::vector<std::pair<int64_t, int64_t>> ordCounts;
        auto min = thisOp().minCount == -1 ? 1 : thisOp().minCount;

        if (mapCounts) {
          for (auto& [val, count] : *mapCounts) {
            if (count >= min) {
              ordCounts.emplace_back(val, count);
            }
          }
        } else if (vecCounts) {
          for (size_t i = 0; i < vecCounts->size(); i++) {
            if ((*vecCounts)[i] >= min) {
              ordCounts.emplace_back(i, (*vecCounts)[i]);
            }
          }
        } else {
          assert(skinnyCounts);
          // first go over the overflow counts
          for (auto [ord, count] : skinnyCounts->overflow) {
            count += skinnyCounts->counts[ord];
            skinnyCounts->counts[ord] = 0;
            if (count >= min) {
              ordCounts.emplace_back(ord, count);
            }
          }

          // if we are sorting by count descending and have a limit, we can stop if the overflow
          // counts are enough to fill the limit.
          bool sortingByCountDesc = true;  // FUTURE

          if (sortingByCountDesc && limit != -1 && (int64_t)ordCounts.size() >= limit) {
            // already have enough counts, from overflow, and they are guaranteed to be larger than anything that didn't overflow.
          } else {
            for (size_t ord = 0; ord < skinnyCounts->counts.size(); ord++) {
              auto count = skinnyCounts->counts[ord];
              if (count >= min) {
                ordCounts.emplace_back(ord, count);
              }
            }
          }
        }  // end skinnyCounts

        missing_count = mergedData->missing_num;

        // TODO: use a heap if we are only keeping a small number of results.
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

      mergedData.reset();  // free up memory from the merged data, everything should be in countVec now.


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
      } else {
        // Count and bucket-value sorts are future work; sub-op sort is supported.
        std::string_view field = thisOp().fieldFacet.sorts(0).field();
        SearchOp::InlineCalculator* calc = nullptr;
        for (auto* candidate : mergedData->inlineCalcs) {
          if (candidate->getOp().name == field) {
            calc = candidate;
            break;
          }
        }
        assert(calc != nullptr);
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
          auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
          int32_t maxDoc = postingsReader.maxDoc();
          auto poolGuard = MemPool::threadLocalPoolGuard();
          FieldReader fieldReader(poolGuard.pool(), postingsReader);
          bool found = fieldReader.seek(thisOp().fieldName);
          DocSetBuilder builder(maxDoc);
          std::unique_ptr<DocSet> bucketDomain;
          if (found) {
            fieldReader.readFieldInfo(segFieldInfo);
            TermsEnum tenum(poolGuard.pool(), postingsReader, segFieldInfo);
            if (tenum.seek(key)) {
              DocsEnum denum(poolGuard.pool(), postingsReader, tenum);
              while (true) {
                auto doc = denum.nextDoc();
                if (doc == DocsEnum::END) {
                  break; // no more docs for this term
                }
                if (input[segnum] && !input[segnum]->get(doc)) {
                  continue; // this doc is not in the domain
                }
                builder.add(doc);
              }
              bucketDomain = builder.build();
            }
          }
          for (auto& subCalc : calculators) {
            //subCalc->calc(tg, segnum, &output);
            // no support for subcalcs launching tasks yet

            subCalc->calc(nullptr, segnum, bucketDomain.get());
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
