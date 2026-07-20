#pragma once

#include <variant>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include "FacetEmit.h"
#include "FacetOp.h"
#include "SkinnyCounter.h"
#include "solux/util/SegmentMergeDriver.h"

namespace solux {

// How the domain will be walked, for the piece's human-readable detail.
// ARRAY domains point-select ords; bitset/null domains scan bulk frames for
// DOCID columns or presence ranks for RANK columns.
inline const char* domainDesc(DocSet* domain) {
  return domain == nullptr ? "all-docs domain, bulk column scan"
      : domain->type == DocSet::Type::ARRAY ? "array domain, point ord loads"
                                            : "bitset domain, bulk column scan";
}

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

    enum class Rep { Vector, Hash, Skinny };

    // Make `data.counts` the requested representation, sized for globVals,
    // folding any existing (smaller) counts in.  This is the storage-upgrade a
    // later segment performs when it wants a larger counter than the
    // merged-so-far it obtained.  Rules (matching the original calcOrdMap inline
    // logic): upgrade map/skinny -> vector and map -> skinny by snapshotting the
    // old counts, rebuilding as the larger rep, and folding the old back in via
    // merge; otherwise keep whatever data already holds (never downgrade), and
    // build fresh from monostate.  The driver owns `data` across the merge, so
    // releaseCount is never touched; merge equalizes missing_num so data's is
    // already correct. Returns whether storage was upgraded. Exposed for
    // direct unit testing of the upgrade.
    static bool ensureRep(MergeableStrData& data, Rep rep, int64_t globVals) {
      bool isMap = std::holds_alternative<OrdHash>(data.counts);
      bool isSkinny = std::holds_alternative<SkinnyCounter8>(data.counts);
      bool needUpgrade = (rep == Rep::Vector && (isMap || isSkinny))
                      || (rep == Rep::Skinny && isMap);

      std::optional<MergeableStrData> oldData;
      if (needUpgrade) {
        oldData.emplace();
        oldData->counts = std::move(data.counts);
        data.counts.emplace<std::monostate>();
      }

      // Only (re)build when there is no usable storage yet (fresh, or just
      // cleared for an upgrade); an existing equal/larger rep is kept as-is.
      if (std::holds_alternative<std::monostate>(data.counts)) {
        switch (rep) {
          case Rep::Vector: {
            data.counts.emplace<CountVector>();
            auto& vec = std::get<CountVector>(data.counts);
            if (vec.empty()) {
              vec.resize(globVals);
            }
            break;
          }
          case Rep::Hash:
            data.counts.emplace<OrdHash>();
            break;
          case Rep::Skinny:
            data.counts.emplace<SkinnyCounter8>(globVals);
            break;
        }
      }

      if (oldData) {
        // Fold the old (smaller) rep into data's new (larger) one.  The
        // upgrade-to-larger invariant means merge returns data; defend against
        // a future variant breaking that so we never silently drop counts in a
        // release build (assert off).
        auto* result = MergeableStrData::merge(&data, &*oldData);
        assert(result == &data);
        if (result != &data) {
          data.counts = std::move(result->counts);
        }
      }
      return needUpgrade;
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

  // ProtobufSearchParser resolves the OrdMap and passes it in
  // (see TopDocsReq's ctor comment).
  StrFacetOp(SearchRequest& req, const ReqFieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing,
    std::shared_ptr<OrdMap> ordMap) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing),
  ordMap(std::move(ordMap)) {
    enableExecutionProfile();
  }

  class Calc : public Calculator {
    ExecutionProfileRun* profileRun;
    std::vector<DocSet*> input;
    SegmentMergeDriver<MergeableStrData> driver;
    SegmentMergeDriver<MergeableStrFacetInline> inlineDriver;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        profileRun(op.addExecutionProfileRun()),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableStrData> m){ facetResult(std::move(m)); }),
        inlineDriver(op.req.reader->segments().size(),
                     [this](std::unique_ptr<MergeableStrFacetInline> m){ facetResult2(std::move(m)); }) {
      input.resize(op.req.reader->segments().size());
      inlineDriver.setCreator([this]() {
        auto* p = new MergeableStrFacetInline;
        for (auto& [key, subop] : thisOp().inlineSubOps) {
          auto* calc = subop->createInlineCalculator(this, -1, -1);
          p->inlineCalcs.push_back(calc);
        }
        p->counts.calcs = p->inlineCalcs;
        return p;
      });
    }

    StrFacetOp& thisOp() {
      return (StrFacetOp&)getOp();
    }


    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(resp, this);
      // the Val should either be unset, or have a FacetResult
      assert(
        ourVal != nullptr && (std::holds_alternative<solux::api::FacetResult>(ourVal->kind)
          || std::holds_alternative<std::monostate>(ourVal->kind)));
      auto& fr = oneofMut<solux::api::FacetResult>(*ourVal);
      // Cap = max distinct sub-op Vals written into this FacetResult's ops map.
      // Both post sub-ops (doSubops, from subOps) and inline calculators
      // (fillResult, from inlineSubOps) bubble through here, and the two sets are
      // disjoint (init() moves inline ops out of subOps), so the backing array
      // must be sized for their sum or opsSlot's pre-sized array overflows.
      std::size_t cap = thisOp().subOps.size() + thisOp().inlineSubOps.size();
      return build::opsSlot(fr.ops, cap, sub->getOp().name, resp->mr);
      //TODO: need to account for slot somehow,  or will subop do that?
    };
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DocSet* domain) override {
      // Sub-op task launching is not wired up yet (doSubops runs sub-calcs
      // inline with a null tg), so the per-segment work needs no task group.
      unused(tg);
      if (segnum == -1) {
        // Empty index - emit an empty (non-inline) result, matching prior behavior.
        driver.completeEmpty();
        return;
      }

      if (profileRun != nullptr) {
        auto profile = profilePiece(profileRun, segnum);
        if (!thisOp().inlineSubOps.empty()) {
          calc2(segnum, domain, profile.get());
        } else {
          calcOrdMap(segnum, domain, profile.get());
        }
        return;
      }

      if (!thisOp().inlineSubOps.empty()) {
        calc2(segnum, domain, nullptr);
        return;
      } else {
        calcOrdMap(segnum, domain, nullptr);
        return;
      }
    };

    void calc2(int32_t segnum, DocSet* domain,
               ExecutionProfilePieceState* profile) {
      inlineDriver.contribute([&](MergeableStrFacetInline& data) {
        for (auto* calc : data.inlineCalcs) {
          calc->startSeg(segnum);
        }
        input[segnum] = domain;
        SegFieldInfo segFieldInfo;
        PostingsReader& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        if (profile != nullptr) {
          profile->wire.max_doc = maxDoc;
          profile->wire.domain_size = domain ? domain->card() : maxDoc;
          profile->wire.strategy = "inline";
          profile->details.emplace_back(domainDesc(domain));
        }
        auto poolGuard = MemPool::threadLocalPoolGuard();

        FieldReader fieldReader(poolGuard.pool(), postingsReader);
        std::optional<TermsEnum> tenum;
        bool found = fieldReader.seek(thisOp().fieldName);
        if (found) {
          fieldReader.readFieldInfo(segFieldInfo);
          tenum.emplace(poolGuard.pool(), postingsReader, segFieldInfo);
        }
        if (profile != nullptr) {
          profile->wire.cardinality = found
              ? (thisOp().ordMap ? thisOp().ordMap->numOrds() : segFieldInfo.nTerms)
              : 0;
        }
        int64_t missing_num = 0;
        auto& facetReq = (FacetReq&)getOp();
        facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
          [&](int32_t docid, int32_t val)SOLUX_INLINE {
            tenum->seekOrd(val - 1);
            std::string_view termView = (std::string_view) tenum->term();
            data.counts.add((std::string) termView, docid);
          });

        data.missing_num += missing_num;
      });
    }

    void calcOrdMap(int32_t segnum, DocSet* domain,
                    ExecutionProfilePieceState* profile) {
      driver.contribute([&](MergeableStrData& data) {
        //write only to different slots, so no need to synchronize
        input[segnum] = domain;
        SegFieldInfo segFieldInfo;
        auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        int32_t domainSize = domain ? domain->card() : maxDoc;
        if (profile != nullptr) {
          profile->wire.max_doc = maxDoc;
          profile->wire.domain_size = domainSize;
        }
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(poolGuard.pool(), postingsReader);
        bool found = fieldReader.seek(thisOp().fieldName);
        if (!found) {
          data.missing_num += domainSize;
          if (profile != nullptr) {
            profile->wire.cardinality = 0;
            profile->wire.strategy = "none";
            profile->details.emplace_back("field not present in segment");
          }
          return; // field not found, but still a contribution (driver releases)
        }
        fieldReader.readFieldInfo(segFieldInfo);

        int64_t globVals = thisOp().ordMap ? thisOp().ordMap->numOrds() : segFieldInfo.nTerms;

        // want a vector of global ords if the domain size is much larger than the number of unique values
        // such that a skinny counter would have many overflows.
        bool wantVec = (domainSize >> 8) >= globVals;

        // if the number of unique values is large compared to the domain size, we want to use a hashmap
        bool wantHash = (globVals >> 6) >= domainSize;

        // A segment that finished before us may have chosen a smaller storage
        // type; ensureRep upgrades data.counts in place to the larger rep we
        // want (folding the old counts in), or keeps/creates it as needed.  Then
        // re-read the active alternative for the scan below.
        MergeableStrData::Rep rep = wantVec ? MergeableStrData::Rep::Vector
                                  : wantHash ? MergeableStrData::Rep::Hash
                                             : MergeableStrData::Rep::Skinny;
        bool repUpgraded = MergeableStrData::ensureRep(data, rep, globVals);
        auto* countVec = std::get_if<MergeableStrData::CountVector>(&data.counts);
        auto* countMap = std::get_if<MergeableStrData::OrdHash>(&data.counts);
        auto* countSkinny = std::get_if<SkinnyCounter8>(&data.counts);
        if (profile != nullptr) {
          const char* want = rep == MergeableStrData::Rep::Vector ? "vector"
                           : rep == MergeableStrData::Rep::Hash ? "hash"
                                                                : "skinny";
          const char* using_ = countVec ? "vector" : countMap ? "hash" : "skinny";
          profile->wire.cardinality = globVals;
          profile->wire.strategy = want;
          profile->details.emplace_back(domainDesc(domain));
          if (repUpgraded) {
            // This piece grew the shared accumulator (and paid the fold).
            profile->details.emplace_back(std::string("upgraded shared counters to ") + using_);
          } else if (want != std::string_view(using_)) {
            // Accumulator already outgrew our pick (no-downgrade rule).
            profile->details.emplace_back(std::string("want=") + want + ", found=" + using_);
          }
        }

        int64_t missing_num = 0;
        auto& facetReq = (FacetReq&)getOp();
        OrdMap::SegToGlobal mapping;
        if (thisOp().ordMap) {
          mapping = thisOp().ordMap->getSegToGlobal(segnum);
        } else {
          mapping.numOrds = segFieldInfo.nTerms;
        }

        auto forEachMappedOrd = [&](size_t size, auto&& accept) {
          uint64_t deltaFrame[128];
          for (size_t base = 0; base < size; base += 128) {
            uint32_t count = (uint32_t)std::min<size_t>(128, size - base);
            if (mapping.bits != 0) {
              mapping.unpackDeltas(base, count, deltaFrame);
            }
            for (uint32_t i = 0; i < count; i++) {
              int64_t localOrd = (int64_t)base + i;
              int64_t globalOrd = mapping.bits == 0
                  ? localOrd : localOrd + (int64_t)deltaFrame[i];
              accept((size_t)localOrd, globalOrd);
            }
          }
        };

        // if we want a vector, then there are enough repeats that we should collect
        // local counts first and then only convert to global ords once.
        if (countVec) {
          // TODO: we have a countVec, but if we wanted a map, that means we should
          // probably not do 2 pass.  Unclear how often this will happen.
          // NOTE: skip 2 phase if the ords for this segment are the same as global ords!

          // for single-valued, we could get away with int32_t
          std::vector<int64_t> localCounts(segFieldInfo.nTerms);
          facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
            [&](int32_t docid, int32_t localOrd) SOLUX_INLINE {
              unused(docid);
              int64_t ord = (int64_t)localOrd - 1;
              localCounts[ord]++;
            });

          forEachMappedOrd(localCounts.size(), [&](size_t localOrd, int64_t globalOrd) {
            auto count = localCounts[localOrd];
            if (count > 0) {
              (*countVec)[globalOrd] += count;
            }
          });

        } else if (countMap) {
          facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
            [&](int32_t docid, int32_t localOrd) SOLUX_INLINE {
              unused(docid);
              int64_t ord = mapping.globalOrd((int64_t)localOrd - 1);
              (*countMap)[ord]++;
            });
        } else {
          assert(countSkinny);
          // If localords != globalOrds and expected number of repeats per value is > 2, use a local skinny counter first
          // and convert to global ords on overflow.
          if (mapping.bits != 0 && (domainSize >> 1) >= segFieldInfo.nTerms) {
            std::vector<uint8_t> localCounts(segFieldInfo.nTerms);
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) SOLUX_INLINE {
              unused(docid);
              int64_t ord = (int64_t)localOrd - 1;
              if (++localCounts[ord] == 0) {
                countSkinny->increment(mapping.globalOrd(ord),
                                       std::numeric_limits<uint8_t>::max() + 1);
              }
            });
            forEachMappedOrd(localCounts.size(), [&](size_t localOrd, int64_t globalOrd) {
              auto count = localCounts[localOrd];
              if (count > 0) {
                countSkinny->increment(globalOrd, count);
              }
            });
          } else {
            // Not many repeats expected, so just collect global ords directly.
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) SOLUX_INLINE {
                unused(docid);
                countSkinny->increment(mapping.globalOrd((int64_t)localOrd - 1));
              });
          }
        }
        data.missing_num += missing_num;
      });
    }

    void facetResult(std::unique_ptr<MergeableStrData> mergedData) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      auto* myVal = getTarget(nullptr, [&](solux::api::Val& val) {
        if (slot >= 0) {
          // sub-facet: this Val is shared by all parent buckets, so index by
          // slot into a per-bucket array (parallel to the parent bucket_ids),
          // allocated once at numSlots under the mutex like StatsOp's arr_d.
          auto& arr = oneofMut<solux::api::ArrVal>(val);
          if (arr.v.empty()) build::allocArray(arr.v, numSlots, mr);
        }
      });
      // For a sub-facet, our FacetResult goes into the slot-th element of the
      // shared ArrVal (each slot is a distinct, stably-addressed Val).
      solux::api::Val* targetVal = myVal;
      if (slot >= 0) {
        auto& arr = oneofMut<solux::api::ArrVal>(*myVal);
        targetVal = &const_cast<solux::api::Val*>(arr.v.data())[slot];
      }
      solux::api::FacetResult& facetResultProto = oneofMut<solux::api::FacetResult>(*targetVal);
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
        // Collect nonzero counts first. Explicit mincount=0 pads zero-count
        // buckets after sorting/truncating the competitive nonzero buckets.
        auto min = std::max<int64_t>(thisOp().minCount, 1);

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

        sortByCountDescAndLimit(ordCounts, limit);
        bool showZeros = (thisOp().minCount == 0);
        if (showZeros) {
          int64_t numOrds = thisOp().ordMap ? thisOp().ordMap->numOrds() : 0;
          int64_t target = (limit < 0) ? numOrds : limit;
          if ((int64_t)ordCounts.size() < target) {
            boost::unordered_flat_set<int64_t> present;
            present.reserve(ordCounts.size());
            for (auto& [ord, count] : ordCounts) {
              unused(count);
              present.insert(ord);
            }
            for (int64_t ord = 0; ord < numOrds && (int64_t)ordCounts.size() < target; ord++) {
              if (!present.contains(ord)) {
                ordCounts.emplace_back(ord, 0);
              }
            }
          }
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


      emitBuckets(facetResultProto, countVec, mr);
      if (missing) {
        facetResultProto.missing = missing_count;
      }

      doSubops(thisOp().subOps, countVec);
    }


    void facetResult2(std::unique_ptr<MergeableStrFacetInline> mergedData) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      auto* myVal = getTarget(nullptr, [&](solux::api::Val& val) {
        if (slot >= 0) {
          auto& arr = oneofMut<solux::api::ArrVal>(val);
          if (arr.v.empty()) build::allocArray(arr.v, numSlots, mr);
        }
      });
      solux::api::Val* targetVal = myVal;
      if (slot >= 0) {
        auto& arr = oneofMut<solux::api::ArrVal>(*myVal);
        targetVal = &const_cast<solux::api::Val*>(arr.v.data())[slot];
      }
      solux::api::FacetResult& facetResultProto = oneofMut<solux::api::FacetResult>(*targetVal);
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
      if (thisOp().fieldFacet.sorts.empty()) {
        std::sort(valVec.begin(), valVec.end(), [](auto& a, auto& b) {
          if (*(int64_t*)a.second != *(int64_t*)b.second ) {
            return *(int64_t*)a.second > *(int64_t*)b.second;
          }
          return a.first < b.first;
        });
      } else {
        // Count and bucket-value sorts are future work; sub-op sort is supported.
        std::string_view field = thisOp().fieldFacet.sorts[0].field;
        SearchOp::InlineCalculator* calc = nullptr;
        for (auto* candidate : mergedData->inlineCalcs) {
          if (candidate->getOp().name == field) {
            calc = candidate;
            break;
          }
        }
        assert(calc != nullptr);
        bool reversed = thisOp().fieldFacet.sorts[0].dir == solux::api::SortSpec_::SortDir::DESC;
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

      // fill in the facet result proto (non-owning: size known from valVec)
      auto& bucketIds = facetResultProto.bucket_ids.emplace().kind.emplace<solux::api::ColStr>();
      size_t n = valVec.size();
      std::string_view* ids = build::allocArray(bucketIds.v, n, mr);
      int64_t* countArr = build::allocArray(facetResultProto.counts, n, mr);
      for (size_t i = 0; i < n; i++) {
        ids[i] = build::arenaStr(mr, valVec[i].first);  // copy the (transient) string into the arena
        countArr[i] = *(int64_t*)valVec[i].second;
      }
      if (missing) {
        facetResultProto.missing = missing_count;
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
            }
          }
          // Always pass a (possibly empty) bucket domain.  A null domain means
          // "all docs" to a sub-op (e.g. StatsOp), so when the field or value is
          // absent in this segment the bucket would wrongly absorb every doc in
          // the segment.  The bucket has no docs here, so the domain is empty.
          std::unique_ptr<DocSet> bucketDomain = builder.build();
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
