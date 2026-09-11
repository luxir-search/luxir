// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <variant>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include "FacetEmit.h"
#include "FacetExecution.h"
#include "FacetOp.h"
#include "SkinnyCounter.h"
#include "luxir/search/SearchOverrides.h"
#include "SpanCounter.h"
#include "StrFacetPlanning.h"
#include "StrFacetReplay.h"
#include "luxir/util/SegmentMergeDriver.h"
#include "luxir/util/log.h"

namespace luxir {

// How the domain will be walked, for the piece's human-readable detail.
// ARRAY domains point-select ords. Bitset domains adapt for DOCID columns and
// retain fixed bulk decoding for RANK columns; null domains use fixed bulk.
inline const char* domainDesc(DocSet* domain, bool docIdIndexed) {
  if (domain == nullptr) {
    return "all-docs domain, bulk ord loads";
  }
  if (domain->type == DocSet::Type::ARRAY) {
    return "array domain, point ord loads";
  }
  return docIdIndexed ? "bitset domain, adaptive point/bulk ord loads"
                      : "bitset domain, bulk ord loads";
}

inline const char* domainDesc(DocSet* domain) {
  return domain == nullptr ? "all-docs domain"
      : domain->type == DocSet::Type::ARRAY ? "array domain, point ord loads"
                                            : "bitset domain";
}

template<typename Counter>
inline void collectSparseCounts(
    Counter& counter, int64_t min, int64_t limit,
    std::vector<std::pair<int64_t, int64_t>>& ordCounts,
    bool allowEarlyStop = true) {
  // Overflowed ords are guaranteed to outrank every non-overflowed ord. Fold
  // their low bits first and clear those slots so a later scan cannot count
  // them twice.
  counter.foldOverflow([&](int64_t ord, int64_t total) {
    if (total >= min) {
      ordCounts.emplace_back(ord, total);
    }
  });

  bool sortingByCountDesc = true;  // FUTURE
  if (allowEarlyStop && sortingByCountDesc && limit != -1
      && (int64_t)ordCounts.size() >= limit) {
    // The top-K is already in hand, so release the potentially large sparse
    // table instead of scanning it.
    counter.releaseStorage();
    return;
  }

  counter.forEachCount([&](int64_t ord, int64_t count) {
    if (count >= min) {
      ordCounts.emplace_back(ord, count);
    }
  });
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
  // Refittable AUTO crossover. With ten buckets and three metrics, inline-all
  // won at 100K matching docs and lost at 10K; 48K is the initial bracketed
  // boundary.
  static constexpr int64_t INLINE_ALL_MIN_DOMAIN = 48 * 1024;

  std::shared_ptr<OrdMap> ordMap;
  struct PinnedBucket {
    std::string_view key;
    std::optional<int64_t> ord;
    int64_t indexDocFreq;
  };
  std::vector<PinnedBucket> pinnedBuckets;
  std::vector<std::pair<const std::string_view, SearchOp*>> inlineSubOps;
  bool inlineAllCandidate = false;

public:
  class MergeableStrData : public luxir::MergeableData {
  public:
    using OrdHash = boost::unordered_flat_map<int64_t, int64_t>;
    using CountVector = std::vector<int64_t>;
    std::variant<std::monostate, OrdHash, SkinnyCounter8, CountVector,
                 SpanCounter> counts;
    int64_t missing_num = 0; // number of missing values in this segment
    int64_t topTermsSegs = 0;
    // Kept apart from missing_num so a mixed domain can discard the deferred
    // segments' contribution by ignoring it, rather than subtracting it back
    // out before the replay recomputes it.
    int64_t topTermsMissing = 0;

    template<typename Counter>
    static MergeableStrData* mergeSparse(MergeableStrData* a,
                                         MergeableStrData* b) {
      auto* acounter = std::get_if<Counter>(&a->counts);
      auto* bcounter = std::get_if<Counter>(&b->counts);
      if (!acounter) {
        std::swap(a, b);
        std::swap(acounter, bcounter);
      }
      // Forced sparse modes are process-global and homogeneous.
      assert(bcounter != nullptr);
      if (acounter->distinct() < bcounter->distinct()) {
        std::swap(a, b);
        std::swap(acounter, bcounter);
      }
      acounter->merge(*bcounter);
      return a;
    }

    static MergeableStrData* merge(MergeableStrData* a, MergeableStrData* b) {
      // start by updating missing_num of both (we will return one or the other)
      a->missing_num += b->missing_num;
      b->missing_num = a->missing_num;
      a->topTermsSegs += b->topTermsSegs;
      b->topTermsSegs = a->topTermsSegs;
      a->topTermsMissing += b->topTermsMissing;
      b->topTermsMissing = a->topTermsMissing;

      if (std::holds_alternative<std::monostate>(a->counts)) {
        return b;
      }
      if (std::holds_alternative<std::monostate>(b->counts)) {
        return a;
      }

      if (std::holds_alternative<SpanCounter>(a->counts)
          || std::holds_alternative<SpanCounter>(b->counts)) {
        return mergeSparse<SpanCounter>(a, b);
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

    enum class Rep { Vector, Hash, Skinny, Span };

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
          case Rep::Span:
            data.counts.emplace<SpanCounter>((size_t)globVals);
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

  class MergeableStrFacetInline : public MergeableData {
  public:
    // Keyed by GLOBAL term ordinal, which is what makes this mergeable across
    // segments.  It used to key by the term text, because when the inline path
    // was written a segment ord was the only ord there was and text was the
    // only cross-segment-stable key - which cost a dictionary seek and a string
    // hash for every document counted.  Global ords (OrdMap) removed that
    // constraint; term text is now resolved once per emitted bucket, in
    // facetResult2, exactly as the non-inline path does it.
    OrdinalFacetEntryTable counts;
    int64_t missing_num = 0; // number of missing values in this segment
    std::vector<SearchOp::InlineCalculator*> inlineCalcs;
    static MergeableStrFacetInline* merge(MergeableStrFacetInline* a, MergeableStrFacetInline* b) {
      // Mixed representations always merge into dense. Otherwise merge the
      // smaller touched set into the larger one; dense/dense therefore walks
      // the smaller touched list without scanning the ordinal space.
      if (a->counts.dense() != b->counts.dense()) {
        if (b->counts.dense()) std::swap(a, b);
      } else if (a->counts.size() < b->counts.size()) {
        std::swap(a, b);
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
    std::shared_ptr<OrdMap> ordMap,
    std::span<const std::string_view> selected) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing,
                {}, selected),
  ordMap(std::move(ordMap)) {
    if (!selected.empty()) {
      auto poolGuard = MemPool::threadLocalPoolGuard();
      OrdMapStr lookup(poolGuard.pool(), this->ordMap.get(), reader, fieldName);
      pinnedBuckets.reserve(selected.size());
      for (std::string_view key : selected) {
        auto stats = lookup.termStats(key);
        pinnedBuckets.push_back({key, stats.globalOrd, stats.docFreq});
      }
    }
    enableExecutionProfile();
  }

  bool canEmitAsBucketChild() const override {
    return true;
  }

  void init() override {
    FacetReq::init();
    auto sorts = fieldFacet.sorts;
    if (!sorts.empty()) {
      if (sorts.size() > 1) {
        throw std::runtime_error("facet '" + std::string(facetName)
            + "': multiple sort fields are not yet supported");
      }
      for (auto& sort : sorts) {
        auto iter = subOps.find(sort.expr);
        if (iter == subOps.end()) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': unknown sort field '" + std::string(sort.expr) + "'");
        }
        if (!iter->second->canInline()) {
          throw std::runtime_error("facet '" + std::string(facetName)
              + "': cannot sort by a subop without inline support: "
              + std::string(iter->second->name));
        }
        inlineSubOps.push_back(*iter);
        subOps.erase(iter);
      }
    }

    // Whether the REMAINING sub-ops join the count pass or stay behind for the
    // post-selection bucket-domain feed. limit==-1 still inlines all because
    // every bucket is returned. A finite limit that covers the index-wide
    // bucket bound only records a candidate here: calcAll resolves it from the
    // actual matching domain. The ord-column/batch feed made small-domain
    // replay cheap; with ten buckets and three metrics, inline-all won at 100K
    // matching docs and lost at 10K.
    //
    // Restricted to requests that are already inlining something (a sort key,
    // whose value decides which buckets are returned at all). Without one, the
    // count pass would otherwise be free to take a counting strategy the inline
    // path cannot reach - the docFreq-only dictionary walk among them - and
    // that trade has not been measured. LUXIR_FACET_SUBOP_INLINE=all is how to
    // measure it.
    int64_t buckets = ordMap ? ordMap->numOrds() : 0;
    bool inlineAll = limit == -1;
    inlineAllCandidate = limit != -1 && !inlineSubOps.empty()
        && limit >= buckets
        && std::ranges::any_of(subOps, [](const auto& subOp) {
             return subOp.second->canInline();
           });
    if (forcedFacetSubOpInline == FacetSubOpInlineMode::ALL) {
      inlineAll = true;
    } else if (forcedFacetSubOpInline == FacetSubOpInlineMode::SORT_KEY_ONLY) {
      inlineAll = false;
      inlineAllCandidate = false;
    }
    if (inlineAll) {
      inlineAllCandidate = false;
      std::vector<std::string_view> moved;
      for (auto& subOp : subOps) {
        if (subOp.second->canInline()) {
          inlineSubOps.push_back(subOp);
          moved.push_back(subOp.first);
        }
      }
      for (auto& name : moved) subOps.erase(name);
    }
  }

  // pinCounts carries the count each selected value was found to have, read by
  // point lookup from the counter while it was still alive (see facetResult).
  // Selection never sees the pins, so nothing here scales with how many are set.
  std::vector<FinalizedFacetBucket<int64_t>> finalizeOrdCounts(
      std::vector<std::pair<int64_t, int64_t>>& ordCounts,
      bool allowZeroPadding,
      std::span<const int64_t> pinCounts = {}) const {
    // ordCounts holds one entry per counted ord, which for a high-cardinality
    // field is most of the domain's distinct values. Stream it through the
    // finalizer's bounded heap rather than copying it into a candidate vector
    // first: a uniform 2M-value field over a 10M-doc domain would otherwise
    // allocate and fill tens of MB per request to return one page.
    FieldBucketFinalizer<int64_t, std::monostate,
                         decltype(countFieldBucketOrder)>
        finalizer(minCount, 0, limit, countFieldBucketOrder);
    finalizer.addRange(ordCounts.begin(), ordCounts.end(),
        [](const std::pair<int64_t, int64_t>& entry) {
          return FacetCandidate<int64_t>{entry.first, entry.second, {}};
        });

    // Zero-count padding can only be needed when the counted ords do not
    // already fill the page, so sizes decide that before anything is built -
    // and when they do not fill it there are fewer of them than the page.
    if (allowZeroPadding && minCount == 0) {
      int64_t numGlobalOrds = ordMap ? ordMap->numOrds() : 0;
      size_t target = limit < 0 ? (size_t)numGlobalOrds : (size_t)limit;
      if (ordCounts.size() < target) {
        boost::unordered_flat_set<int64_t> present;
        present.reserve(ordCounts.size());
        for (auto [ord, count] : ordCounts) {
          unused(count);
          present.insert(ord);
        }
        size_t emitted = ordCounts.size();
        for (int64_t ord = 0;
             ord < numGlobalOrds && emitted < target; ord++) {
          if (present.contains(ord)) continue;
          finalizer.add({ord, 0, {}});
          emitted++;
        }
      }
    }

    auto page = finalizer.finish();
    if (!pinnedBuckets.empty()) {
      std::vector<std::optional<int64_t>> pinKeys;
      std::vector<PinnedBucketValue<>> pinValues;
      pinKeys.reserve(pinnedBuckets.size());
      pinValues.reserve(pinnedBuckets.size());
      for (size_t i = 0; i < pinnedBuckets.size(); i++) {
        pinKeys.push_back(pinnedBuckets[i].ord);
        pinValues.push_back({i < pinCounts.size() ? pinCounts[i] : 0, {}});
      }
      mergePinnedBuckets<int64_t, std::monostate>(page, pinKeys, pinValues);
    }
    return page;
  }

  class Calc : public Calculator {
    ExecutionProfileRun* profileRun;
    std::vector<DomainHandle> input;
    std::vector<std::pair<const std::string_view, SearchOp*>> inlineOps;
    std::vector<std::pair<const std::string_view, SearchOp*>> feedOps;
    std::vector<uint8_t> topTermsSegments;
    SegmentMergeDriver<MergeableStrData> driver;
    SegmentMergeDriver<MergeableStrFacetInline> inlineDriver;
    std::atomic<int32_t> gatheredDomainsSeen{0};
    int64_t inlineExpectedValues = -1;
    bool inlinePlanResolved = false;

    static int64_t scaleExpectedValues(
        int64_t domainSize, int64_t segmentValues, int32_t maxDoc) {
      if (domainSize == 0 || segmentValues == 0 || maxDoc == 0) return 0;
      __int128 scaled = (__int128)domainSize * segmentValues / maxDoc;
      return scaled > std::numeric_limits<int64_t>::max()
          ? std::numeric_limits<int64_t>::max() : (int64_t)scaled;
    }

    class InlineSegmentScope {
      std::span<SearchOp::InlineCalculator*> calculators;
      int32_t segnum;
      size_t started = 0;

    public:
      InlineSegmentScope(
          std::span<SearchOp::InlineCalculator*> calculators,
          int32_t segnum)
          : calculators(calculators), segnum(segnum) {
        try {
          for (; started < calculators.size(); started++) {
            calculators[started]->startSeg(segnum);
          }
        } catch (...) {
          for (size_t i = 0; i < started; i++) {
            calculators[i]->endSeg(segnum);
          }
          throw;
        }
      }

      ~InlineSegmentScope() {
        for (size_t i = 0; i < started; i++) {
          calculators[i]->endSeg(segnum);
        }
      }
    };
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : Calculator(op, parent, slot, numSlots),
        profileRun(op.addExecutionProfileRun()),
        inlineOps(((StrFacetOp&)op).inlineSubOps),
        feedOps(((StrFacetOp&)op).subOps.begin(),
                ((StrFacetOp&)op).subOps.end()),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableStrData> m){ facetResult(std::move(m)); }),
        inlineDriver(op.req.reader->segments().size(),
                     [this](std::unique_ptr<MergeableStrFacetInline> m){ facetResult2(std::move(m)); }) {
      input.resize(op.req.reader->segments().size());
      topTermsSegments.resize(op.req.reader->segments().size());
      inlineDriver.setCreator([this]() {
        auto* p = new MergeableStrFacetInline;
        for (auto& [key, subop] : inlineOps) {
          auto* calc = subop->createInlineCalculator(this, -1, -1);
          p->inlineCalcs.push_back(calc);
        }
        std::string detail = p->inlineCalcs.size() == 1
            ? std::format("facet '{}' metric '{}'", thisOp().facetName,
                          p->inlineCalcs.front()->getOp().name)
            : std::format("facet '{}' inline metrics", thisOp().facetName);
        p->counts.configure(
            p->inlineCalcs, thisOp().req.memoryTracker,
            thisOp().ordMap ? thisOp().ordMap->numOrds() : 0,
            std::move(detail), inlineFacetEntryStatsForTests);
        return p;
      });
    }

    StrFacetOp& thisOp() {
      return (StrFacetOp&)getOp();
    }

    void promoteRemainingInline() {
      std::vector<std::pair<const std::string_view, SearchOp*>> remaining;
      remaining.reserve(feedOps.size());
      for (auto& subOp : feedOps) {
        if (subOp.second->canInline()) {
          inlineOps.push_back(subOp);
        } else {
          remaining.push_back(subOp);
        }
      }
      feedOps.swap(remaining);
    }

    void resolveInlinePlan(std::span<const DomainHandle> domains) {
      assert(!inlinePlanResolved);
      assert(domains.size() == thisOp().reader.segments().size());
      int64_t totalDomainSize = 0;
      inlineExpectedValues = inlineOps.empty() ? -1 : 0;
      for (size_t segnum = 0; segnum < domains.size(); segnum++) {
        auto& postings = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postings.maxDoc();
        int64_t domainSize = domains[segnum].get() != nullptr
            ? domains[segnum].get()->card() : maxDoc;
        if (domainSize > std::numeric_limits<int64_t>::max()
                             - totalDomainSize) {
          totalDomainSize = std::numeric_limits<int64_t>::max();
        } else {
          totalDomainSize += domainSize;
        }
        if (inlineOps.empty()) continue;
        FieldReader reader(postings);
        if (!reader.seek(thisOp().fieldName)) continue;
        SegFieldInfo info;
        reader.readFieldInfo(info);
        int64_t expected = scaleExpectedValues(
            domainSize, info.numValues, maxDoc);
        if (expected > std::numeric_limits<int64_t>::max()
                           - inlineExpectedValues) {
          inlineExpectedValues = std::numeric_limits<int64_t>::max();
        } else {
          inlineExpectedValues += expected;
        }
      }
      if (thisOp().inlineAllCandidate
          && totalDomainSize >= INLINE_ALL_MIN_DOMAIN) {
        promoteRemainingInline();
      }
      inlinePlanResolved = true;
    }

    void dispatchSegments(oneapi::tbb::task_group* tg,
                          std::span<const DomainHandle> domains) {
      if (domains.empty()) {
        calcSegment(tg, -1, {});
        return;
      }
      for (int32_t segnum = 0; segnum < (int32_t)domains.size(); segnum++) {
        DomainHandle domain = domains[(size_t)segnum];
        assert(domain.isDeliverable());
        task_group_run(tg, [this, tg, segnum, domain]() {
          calcSegment(tg, segnum, domain);
        });
      }
    }


    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(resp, this);
      luxir::api::Val* target = nullptr;
      routeTarget<luxir::api::ArrVal>(*ourVal, resp->mr,
          [&](luxir::api::Val& val) { target = &val; });
      ourVal = target;
      // the Val should either be unset, or have a FacetResult
      assert(
        ourVal != nullptr && (std::holds_alternative<luxir::api::FacetResult>(ourVal->kind)
          || std::holds_alternative<std::monostate>(ourVal->kind)));
      auto& fr = oneofMut<luxir::api::FacetResult>(*ourVal);
      // Cap = max distinct sub-op Vals written into this FacetResult's ops map.
      // Both post-selection sub-ops and inline calculators bubble through
      // here. Their calculator-local partitions are disjoint, so the backing
      // array must be sized for their sum or opsSlot's pre-sized array
      // overflows.
      std::size_t cap = feedOps.size() + inlineOps.size();
      return build::opsSlot(fr.ops, cap, sub->getOp().name, resp->mr);
    };
    void calcAll(oneapi::tbb::task_group* tg,
                 std::span<const DomainHandle> domains) override {
      resolveInlinePlan(domains);
      dispatchSegments(tg, domains);
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      if (thisOp().inlineAllCandidate) {
        if (segnum == -1) {
          std::span<const DomainHandle> noDomains;
          calcAll(tg, noDomains);
          return;
        }
        assert(domainHandle.isDeliverable());
        input[(size_t)segnum] = std::move(domainHandle);
        int32_t seen = gatheredDomainsSeen.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        assert(seen <= (int32_t)input.size());
        if (seen == (int32_t)input.size()) {
          calcAll(tg, input);
        }
        return;
      }
      calcSegment(tg, segnum, std::move(domainHandle));
    }

    void calcSegment(oneapi::tbb::task_group* tg, int32_t segnum,
                     DomainHandle domainHandle) {
      // Result-child execution is synchronous under the bucket-domain stage,
      // so the per-segment parent count needs no task group.
      unused(tg);
      if (segnum == -1) {
        // Empty index - emit an empty (non-inline) result, matching prior behavior.
        driver.completeEmpty();
        return;
      }

      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();
      input[(size_t)segnum] = std::move(domainHandle);

      if (profileRun != nullptr) {
        auto profile = profilePiece(profileRun, segnum);
        if (!inlineOps.empty()) {
          calc2(segnum, domain, profile.get());
        } else {
          calcOrdMap(segnum, domain, profile.get());
        }
        return;
      }

      if (!inlineOps.empty()) {
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
        auto poolGuard = MemPool::threadLocalPoolGuard();
        InlineSegmentScope inlineScope(data.inlineCalcs, segnum);
        SegFieldInfo segFieldInfo;
        PostingsReader& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        if (profile != nullptr) {
          profile->wire.max_doc = maxDoc;
          profile->wire.domain_size = domain ? domain->card() : maxDoc;
          profile->wire.strategy = "inline";
          profile->details.emplace_back(domainDesc(domain));
        }
        FieldReader fieldReader(postingsReader);
        bool found = fieldReader.seek(thisOp().fieldName);
        if (found) {
          fieldReader.readFieldInfo(segFieldInfo);
        }
        if (profile != nullptr) {
          profile->wire.cardinality = found
              ? (thisOp().ordMap ? thisOp().ordMap->numOrds() : segFieldInfo.nTerms)
              : 0;
        }
        // A null OrdMap means the field has no values in any segment, so the
        // callback below never fires; the default identity mapping is right for
        // that case and for a single segment.
        OrdMap::SegToGlobal mapping;
        if (thisOp().ordMap) {
          mapping = thisOp().ordMap->getSegToGlobal(segnum);
        }
        int64_t domainSize = domain ? domain->card() : maxDoc;
        if (found) {
          int64_t expectedValues = inlineExpectedValues >= 0
              ? inlineExpectedValues
              : scaleExpectedValues(
                    domainSize, segFieldInfo.numValues, maxDoc);
          data.counts.initialize(
              expectedValues,
              forcedInlineFacetEntryMode
                  == InlineFacetEntryMode::FORCE_DENSE,
              forcedInlineFacetEntryMode
                  == InlineFacetEntryMode::FORCE_SPARSE);
        }
        int64_t missing_num = 0;
        auto& facetReq = (FacetReq&)getOp();
        if (mapping.bits == 0) {
          // Identity segment (always so for one segment): local ords ARE global
          // ords, so the column value is the key with nothing in between.
          facetReq.facetSegOrdCol(
              domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t val) LUXIR_INLINE {
                data.counts.add((int64_t)val - 1, docid);
              });
        } else {
          // Remapped segment ords arrive in document order, so use the point
          // globalOrd()/deltaAt accessor: it decodes one packed value per random
          // arrival. BulkGlobalOrds is for ordered local-ord drains, where its
          // decoded frame is reused. Local staging may still win with enough
          // repeats; countSegment's skinny repeats>2 crossover is the prior art
          // for that measured follow-up.
          facetReq.facetSegOrdCol(
              domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t val) LUXIR_INLINE {
                data.counts.add(
                    mapping.globalOrd((int64_t)val - 1), docid);
              });
        }

        data.missing_num += missing_num;
      });
    }

    void calcOrdMap(int32_t segnum, DocSet* domain,
                    ExecutionProfilePieceState* profile) {
      driver.contribute([&](MergeableStrData& data) {
        countSegment(data, segnum, domain, profile, true);
      });
    }

    void countSegment(MergeableStrData& data, int32_t segnum, DocSet* domain,
                      ExecutionProfilePieceState* profile,
                      bool allowTopTerms) {
        //write only to different slots, so no need to synchronize
        SegFieldInfo segFieldInfo;
        auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        int32_t domainSize = domain ? domain->card() : maxDoc;
        if (profile != nullptr) {
          profile->wire.max_doc = maxDoc;
          profile->wire.domain_size = domainSize;
        }
        auto poolGuard = MemPool::threadLocalPoolGuard();
        FieldReader fieldReader(postingsReader);
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
        auto& facetReq = (FacetReq&)getOp();
        OrdMap::SegToGlobal mapping;
        if (thisOp().ordMap) {
          mapping = thisOp().ordMap->getSegToGlobal(segnum);
        } else {
          mapping.numOrds = segFieldInfo.nTerms;
        }

        auto forEachMappedOrd = [&](size_t size, auto&& accept) {
          OrdMap::SegToGlobal::BulkGlobalOrds mapped(mapping);
          for (size_t localOrd = 0; localOrd < size; localOrd++) {
            accept(localOrd, mapped.globalOrd((int64_t)localOrd));
          }
        };

        DomainView domainView(domain, maxDoc);
        auto countPlan = StrFacetCountPlan::select(
            StrFacetCountInputs{
                .maxDoc = maxDoc,
                .domainCardinality = domainView.card,
                .complementCardinality = domainView.compCard,
                .segmentTerms = segFieldInfo.nTerms,
                .sumDocFreq = segFieldInfo.sumDocFreq,
                .docsWithField = segFieldInfo.docsWithField,
                .limit = thisOp().limit,
                .topTermsAvailable = thisOp().ordMap == nullptr
                    ? 0 : (int64_t)thisOp().ordMap->topTerms().entries.size(),
                .domainHasBitset = domainView.bits != nullptr,
                .missingRequested = thisOp().missing,
                .allowTopTerms = allowTopTerms,
                .hasGlobalOrdMap = thisOp().ordMap != nullptr,
                .readerHasNoDeletes =
                    thisOp().reader.liveDocs() == thisOp().reader.maxDoc()
            },
            forcedStrFacetStrategy);
        StrFacetCountKind strategy = countPlan.strategy;

        if (strategy == StrFacetCountKind::TOP_TERMS) {
          if (thisOp().missing) {
            data.topTermsMissing +=
                (int64_t)maxDoc - segFieldInfo.docsWithField;
          }
          data.topTermsSegs++;
          topTermsSegments[(size_t)segnum] = 1;
          if (profile != nullptr) {
            profile->wire.cardinality = globVals;
            profile->wire.strategy = "toplist";
            profile->details.emplace_back("parent-count=top-terms");
            profile->details.emplace_back(
                "global docFreq top terms, "
                + std::to_string(thisOp().ordMap->topTerms().entries.size())
                + " listed");
          }
          return;
        }

        bool spanRep = isSparseForcedMode(forcedFacetCounterMode);
        MergeableStrData::Rep rep;
        if (spanRep) {
          rep = MergeableStrData::Rep::Span;
        } else {
          // Measured crossovers (counter-rep grid, single segment): vector wins
          // CPU from R = increments/buckets ~= 16; below R ~= 1/20 the sparse
          // reps reach CPU parity and the choice is memory, where skinny's
          // 1 B/ord stays smallest until well past that point - so the hash
          // threshold sits a step below the CPU crossover (1/32), which also
          // hedges the per-value ord mapping sparse reps pay on multi-segment
          // indexes. R understates on multi-segment indexes (errs toward
          // skinny, the memory-safe side); re-tune with the multi-segment lane.
          //
          // The adds depend on the strategy. The column walk adds 1 once per
          // in-domain doc-value: the R >= 16 vector crossover above applies
          // directly. The postings-side strategies emit pre-aggregated
          // counts - at most one add per ord, magnitude ~= domainSize/G - so
          // their add count caps at the bucket count and the vector-vs-skinny
          // question is instead whether the magnitudes fit skinny's u8:
          // vector from avg count >= 256 (small-G cells with big counts,
          // where skinny's every add would spill to its overflow map), skinny
          // below (big-G cells, where vector's O(G) int64 accumulator costs
          // more than the whole complement walk - measured 2.2x at
          // 99%/1M-terms when the per-doc R=16 rule picked vector here).
          bool postingsSide = strategy != StrFacetCountKind::COLUMN_DOMAIN;
          int64_t adds = postingsSide
              ? std::min((int64_t)domainSize, globVals)
              : (int64_t)domainSize;
          bool wantVec = postingsSide
              ? ((int64_t)domainSize >> 8) >= globVals
              : ((int64_t)domainSize >> 4) >= globVals;
          bool wantHash = (globVals >> 5) >= adds;
          rep =
              forcedFacetCounterMode == FacetCounterMode::FORCE_VECTOR
                  ? MergeableStrData::Rep::Vector
            : forcesSkinnyRep(forcedFacetCounterMode)
                  ? MergeableStrData::Rep::Skinny
            : forcedFacetCounterMode == FacetCounterMode::FORCE_HASH
                  ? MergeableStrData::Rep::Hash
            : wantVec ? MergeableStrData::Rep::Vector
            : wantHash ? MergeableStrData::Rep::Hash
                       : MergeableStrData::Rep::Skinny;
        }

        bool repUpgraded = MergeableStrData::ensureRep(data, rep, globVals);
        auto* countSpan = std::get_if<SpanCounter>(&data.counts);
        auto* countVec = std::get_if<MergeableStrData::CountVector>(&data.counts);
        auto* countMap = std::get_if<MergeableStrData::OrdHash>(&data.counts);
        auto* countSkinny = std::get_if<SkinnyCounter8>(&data.counts);

        const char* wantedRep =
            rep == MergeableStrData::Rep::Vector ? "vector"
          : rep == MergeableStrData::Rep::Hash ? "hash"
          : rep == MergeableStrData::Rep::Skinny ? "skinny"
                                                 : "span";
        const char* usingRep =
            countSpan ? "span"
          : countVec ? "vector"
          : countMap ? "hash"
                     : "skinny";
        if (profile != nullptr) {
          // wire.strategy stays the counter representation.  How the segment was
          // counted is a second, orthogonal dimension, and details is where this
          // proto says it belongs ("terms-index vs column").
          profile->wire.cardinality = globVals;
          profile->wire.strategy = wantedRep;
          profile->details.emplace_back(
              "seg maxOrd=" + std::to_string(segFieldInfo.nTerms)
              + (mapping.bits == 0 ? " ords=identity" : " ords=remapped"));
          if (strategy == StrFacetCountKind::COLUMN_DOMAIN) {
            profile->details.emplace_back("parent-count=column-domain");
            profile->details.emplace_back(domainDesc(
                domain, segFieldInfo.ordIndexing == SegFieldInfo::ORD_DOCID));
          } else if (strategy == StrFacetCountKind::COLUMN_COMPLEMENT) {
            profile->details.emplace_back("parent-count=column-complement");
            profile->details.emplace_back(domainView.compCard == 0
                ? "all-docs domain, docFreq-only dictionary walk"
                : ((int64_t)domainView.compCard >> 4)
                        >= (int64_t)segFieldInfo.nTerms
                ? "inverted domain view, int32 staging"
                : ((int64_t)segFieldInfo.nTerms >> 5)
                        >= (int64_t)domainView.compCard
                ? "inverted domain view, sorted-hit staging"
                : "inverted domain view, u8 staging");
          } else {
            profile->details.emplace_back("parent-count=term-driven");
            profile->details.emplace_back(
                "per-term smallest-side postings intersection");
          }
          if (!countPlan.forcedFallback.empty()) {
            profile->details.emplace_back(countPlan.forcedFallback);
          }
          if (repUpgraded) {
            profile->details.emplace_back(
                std::string("upgraded shared counters to ") + usingRep);
          } else if (wantedRep != std::string_view(usingRep)) {
            profile->details.emplace_back(
                std::string("want=") + wantedRep + ", found=" + usingRep);
          }
        }

        auto addGlobalCount = [&](int64_t globalOrd, int64_t count) {
          assert(count > 0);
          if (countSpan != nullptr) {
            countSpan->increment(globalOrd, count);
          } else if (countVec != nullptr) {
            (*countVec)[(size_t)globalOrd] += count;
          } else if (countMap != nullptr) {
            (*countMap)[globalOrd] += count;
          } else {
            assert(countSkinny != nullptr);
            countSkinny->increment(globalOrd, count);
          }
        };

        if (strategy != StrFacetCountKind::COLUMN_DOMAIN) {
          TermsEnum terms(poolGuard.pool(), postingsReader, segFieldInfo);
          int64_t nTerms = segFieldInfo.nTerms;

          // One fused pass shared by every postings-side strategy: stream
          // docFreqs in local-ord order, apply the strategy's per-ord count,
          // and add only finished positive counts, mapping just the ords
          // actually emitted. SegmentMergeDriver shares one accumulator
          // across segments, so intermediate complement counts must never
          // reach it - and with counts finished inside the stream, nothing
          // here needs a separate drain pass over ord space.
          OrdMap::SegToGlobal::BulkGlobalOrds mapped(mapping);
          auto emitCounts = [&](auto&& countOf) {
            terms.forEachDocFreq(
                [&](int64_t localOrd, int32_t docFreq) LUXIR_INLINE {
              int32_t count = countOf(localOrd, docFreq);
              if (count > 0) {
                addGlobalCount(mapped.globalOrd(localOrd), count);
              }
            });
          };

          if (domainView.compCard == 0) {
            // Shared S2/S3 all-docs path: no ord column or postings enum.
            emitCounts([](int64_t, int32_t docFreq) { return docFreq; });
            if (thisOp().missing) {
              data.missing_num +=
                  (int64_t)maxDoc - segFieldInfo.docsWithField;
            }
          } else if (strategy == StrFacetCountKind::COLUMN_COMPLEMENT) {
            int64_t compMissing = 0;
            OrdColReader ordColReader(postingsReader, segFieldInfo);
            // Local staging is a SkinnyCounter, for skinny's reason: a u8
            // per term with per-ord overflow aggregated in its map, so
            // staging stays O(nTerms) no matter how many doc-value pairs
            // the complement holds. The narrowed instantiation keeps
            // overflow entries at 8 bytes; complement counts fit int32 by
            // maxDoc. Unlike the shared accumulator, the emit stream never
            // probes a map: any side set drains ord-sorted and merges by a
            // sentinel compare as the stream passes.
            //
            // The staging rep is picked by the same math as every other
            // counter, using the walk that fills it: compCard per-doc
            // increments over this segment's nTerms buckets. Wide (int32,
            // branch-free) from R >= 16; u8 in the middle band, where
            // 4*nTerms is worth saving and the walk hides the wrap check
            // (u8's wrap branch measured ~3ns/doc, 10-15% of the
            // walk-dominated low-cardinality cells - never pay it to shrink
            // a few-KB array); hash-aggregated below R = 1/32, where even
            // the u8 array's memset outweighs counting the few touched
            // ords. The staging-specific int32-vs-u8 crossover has not been
            // fit; 16 mirrors the shared vector crossover and may sit high.
            bool stageWide = ((int64_t)domainView.compCard >> 4) >= nTerms;
            bool stageSparse = (nTerms >> 5) >= (int64_t)domainView.compCard;
            // The sorted-side sentinel merge is written out in each arm
            // rather than shared through a helper: a lambda indirection in
            // the emit callback measured 5-15% on the O(nTerms) emit loop.
            if (stageWide) {
              std::vector<int32_t> localCounts((size_t)nTerms);
              forEachComplementOrdValue(
                  domainView, poolGuard.pool(), ordColReader, compMissing,
                  [&](int32_t docid, int32_t value) LUXIR_INLINE {
                    unused(docid);
                    // Column value 0 is missing; value v maps to term ord
                    // v-1. Multi-valued ord columns hold distinct ords per
                    // doc, so this counts the same doc-term pairs as docFreq.
                    localCounts[(size_t)value - 1]++;
                  });
              emitCounts([&](int64_t localOrd, int32_t docFreq) LUXIR_INLINE {
                int32_t complementCount = localCounts[(size_t)localOrd];
                assert(complementCount <= docFreq);
                return docFreq - complementCount;
              });
            } else if (stageSparse) {
              // Collect raw ord hits and aggregate after the walk: the walk
              // callback stays a bare push (a hash probe inlined there
              // measured ~5ms/request at 99%/2Mu), and in this band the
              // sort is small by construction.
              std::vector<int32_t> hits;
              hits.reserve((size_t)domainView.compCard);
              forEachComplementOrdValue(
                  domainView, poolGuard.pool(), ordColReader, compMissing,
                  [&](int32_t docid, int32_t value) LUXIR_INLINE {
                    unused(docid);
                    hits.push_back(value - 1);
                  });
              std::sort(hits.begin(), hits.end());
              std::vector<std::pair<int32_t, int32_t>> side;
              for (size_t i = 0; i < hits.size();) {
                size_t j = i + 1;
                while (j < hits.size() && hits[j] == hits[i]) {
                  j++;
                }
                side.emplace_back(hits[i], (int32_t)(j - i));
                i = j;
              }
              size_t si = 0;
              int64_t next = side.empty()
                  ? std::numeric_limits<int64_t>::max() : side[0].first;
              emitCounts([&](int64_t localOrd, int32_t docFreq) LUXIR_INLINE {
                int32_t complementCount = 0;
                if (localOrd == next) {
                  complementCount = side[si].second;
                  si++;
                  next = si < side.size()
                      ? side[si].first : std::numeric_limits<int64_t>::max();
                }
                // Deleted postings are present in both docFreq and the
                // complement column walk, so the subtraction remains exact.
                assert(complementCount <= docFreq);
                return docFreq - complementCount;
              });
            } else {
              SkinnyCounter<uint8_t, int32_t, int32_t> local((size_t)nTerms);
              forEachComplementOrdValue(
                  domainView, poolGuard.pool(), ordColReader, compMissing,
                  [&](int32_t docid, int32_t value) LUXIR_INLINE {
                    unused(docid);
                    local.increment(value - 1);
                  });
              std::vector<std::pair<int32_t, int32_t>> side(
                  local.overflow.begin(), local.overflow.end());
              std::sort(side.begin(), side.end());
              size_t si = 0;
              int64_t next = side.empty()
                  ? std::numeric_limits<int64_t>::max() : side[0].first;
              emitCounts([&](int64_t localOrd, int32_t docFreq) LUXIR_INLINE {
                int32_t complementCount = local.counts[(size_t)localOrd];
                if (localOrd == next) {
                  complementCount += side[si].second;
                  si++;
                  next = si < side.size()
                      ? side[si].first : std::numeric_limits<int64_t>::max();
                }
                assert(complementCount <= docFreq);
                return docFreq - complementCount;
              });
            }

            if (thisOp().missing) {
              int64_t docsWithValueInComplement =
                  domainView.compCard - compMissing;
              int64_t docsWithValueInDomain =
                  segFieldInfo.docsWithField - docsWithValueInComplement;
              assert(docsWithValueInDomain >= 0
                     && docsWithValueInDomain <= domainView.card);
              data.missing_num +=
                  domainView.card - docsWithValueInDomain;
            }
          } else {
            domainView.materializeBits(poolGuard.pool());
            emitCounts([&](int64_t, int32_t docFreq) {
              DocsOnlyEnum postings(terms);
              return countTermInDomain(domainView, postings, docFreq);
            });
            if (thisOp().missing) {
              DocsReader docsReader(postingsReader, segFieldInfo);
              data.missing_num +=
                  countMissingInDomain(domainView, docsReader);
            }
          }
          return;
        }

        int64_t missing_num = 0;
        if (spanRep) {
          MergeableStrData::ensureRep(data, MergeableStrData::Rep::Span, globVals);
          assert(countSpan != nullptr);
          if (forcedFacetCounterMode == FacetCounterMode::SPAN_GLOBAL) {
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
                unused(docid);
                countSpan->increment(mapping.globalOrd((int64_t)localOrd - 1));
              });
          } else {
            std::vector<uint32_t> localCounts(segFieldInfo.nTerms);
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
                unused(docid);
                localCounts[(size_t)((int64_t)localOrd - 1)]++;
              });
            forEachMappedOrd(localCounts.size(),
                             [&](size_t localOrd, int64_t globalOrd) {
              uint32_t c = localCounts[localOrd];
              if (c > 0) {
                countSpan->increment(globalOrd, (int64_t)c);
              }
            });
          }
          data.missing_num += missing_num;
          return;
        }

        // if we want a vector, then there are enough repeats that we should collect
        // local counts first and then only convert to global ords once.
        if (countVec) {
          if (mapping.bits == 0) {
            // Identity segment: local ords ARE global ords, so count straight
            // into the global vector - no local buffer, no drain copy.
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
                unused(docid);
                (*countVec)[(int64_t)localOrd - 1]++;
              });
          } else {
            // Remapped segment: count dense local ords once, drain to global.
            // TODO: we have a countVec, but if we wanted a map, that means we
            // should probably not do 2 pass.  Unclear how often this happens.
            // for single-valued, we could get away with int32_t
            std::vector<int64_t> localCounts(segFieldInfo.nTerms);
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
                unused(docid);
                localCounts[(int64_t)localOrd - 1]++;
              });
            forEachMappedOrd(localCounts.size(), [&](size_t localOrd, int64_t globalOrd) {
              auto count = localCounts[localOrd];
              if (count > 0) {
                (*countVec)[globalOrd] += count;
              }
            });
          }

        } else if (countMap) {
          facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
            [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
              unused(docid);
              int64_t ord = mapping.globalOrd((int64_t)localOrd - 1);
              (*countMap)[ord]++;
            });
        } else {
          assert(countSkinny);
          // Stage in local ord space first (then drain once per distinct ord)
          // only when the ords are remapped (bits != 0 - identity makes it moot)
          // and enough repeats are expected to amortize. SKINNY_GLOBAL/LOCAL
          // force the strategy for measuring the crossover; AUTO uses the
          // repeats>2 heuristic (domainSize/nTerms > 2).
          FacetCounterMode skinnyMode = forcedFacetCounterMode;
          bool useLocal =
              skinnyMode == FacetCounterMode::SKINNY_GLOBAL ? false
            : skinnyMode == FacetCounterMode::SKINNY_LOCAL  ? (mapping.bits != 0)
            : (mapping.bits != 0 && (domainSize >> 1) >= segFieldInfo.nTerms);
          if (useLocal) {
            std::vector<uint8_t> localCounts(segFieldInfo.nTerms);
            facetReq.facetSegOrdCol(domain, segnum, missing_num, segFieldInfo,
              [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
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
              [&](int32_t docid, int32_t localOrd) LUXIR_INLINE {
                unused(docid);
                countSkinny->increment(mapping.globalOrd((int64_t)localOrd - 1));
              });
          }
        }
        data.missing_num += missing_num;
    }

    void facetResult(std::unique_ptr<MergeableStrData> mergedData) {
      auto limit = thisOp().limit;

      auto missing_count = mergedData->missing_num;
      std::vector<std::pair<int64_t, int64_t>> ordCounts;

      int64_t numSegments = (int64_t)thisOp().reader.segments().size();
      bool allTopTerms =
          numSegments > 0 && mergedData->topTermsSegs == numSegments;
      if (allTopTerms) {
        missing_count += mergedData->topTermsMissing;
      } else if (mergedData->topTermsSegs > 0) {
        // RootOp domains are uniform when there are no deletes, but filtered
        // and nested domains can cover a whole segment and only part of another.
        // Keep this slow replay so deferred segments cannot be dropped.  The
        // deferred segments' topTermsMissing is simply dropped on the floor -
        // the replay recomputes it into missing_num.
        mergedData->topTermsSegs = 0;
        for (int32_t segnum = 0; segnum < numSegments; segnum++) {
          if (topTermsSegments[(size_t)segnum] == 0) continue;
          topTermsSegments[(size_t)segnum] = 0;
          // TOP_TERMS required a full maxDoc domain and a reader without
          // deletes, so null is the same domain and cannot outlive its owner.
          countSegment(*mergedData, segnum, nullptr, nullptr, false);
        }
        missing_count = mergedData->missing_num;
      }

      // Counts for the selected values, filled from the live counter below.
      std::vector<int64_t> pinCounts;
      bool haveOrdCounts =
          allTopTerms
          || !std::holds_alternative<std::monostate>(mergedData->counts);
      if (haveOrdCounts) {
        auto* mapCounts = std::get_if<MergeableStrData::OrdHash>(&mergedData->counts);
        auto* skinnyCounts = std::get_if<SkinnyCounter8>(&mergedData->counts);
        auto* vecCounts = std::get_if<MergeableStrData::CountVector>(&mergedData->counts);
        auto* spanCounts = std::get_if<SpanCounter>(&mergedData->counts);

        // What each selected value counted, read straight from the counter
        // before collection folds its overflow and clears those slots. This is
        // what lets everything below take its early exits: the page no longer
        // has to be collected in full just because a value is pinned.
        if (!thisOp().pinnedBuckets.empty() && allTopTerms) {
          // TOP_TERMS only carries the natural page. Selected values use the
          // exact point stats resolved with their ords, so an unlisted tail
          // does not disable the whole-index shortcut.
          pinCounts.reserve(thisOp().pinnedBuckets.size());
          for (const auto& pin : thisOp().pinnedBuckets) {
            pinCounts.push_back(pin.indexDocFreq);
          }
        } else if (!thisOp().pinnedBuckets.empty()) {
          pinCounts.reserve(thisOp().pinnedBuckets.size());
          for (const auto& pin : thisOp().pinnedBuckets) {
            int64_t count = 0;
            if (pin.ord.has_value()) {
              int64_t ord = *pin.ord;
              if (mapCounts) {
                auto it = mapCounts->find(ord);
                if (it != mapCounts->end()) count = it->second;
              } else if (vecCounts) {
                if (ord >= 0 && (size_t)ord < vecCounts->size()) {
                  count = (*vecCounts)[ord];
                }
              } else if (skinnyCounts) {
                if (ord >= 0 && (size_t)ord < skinnyCounts->max) {
                  count = skinnyCounts->total(ord);
                }
              } else if (spanCounts) {
                if (ord >= 0 && (size_t)ord < spanCounts->maxOrd) {
                  count = spanCounts->total(ord);
                }
              }
            }
            pinCounts.push_back(count);
          }
        }
        // Collect nonzero counts first. Explicit mincount=0 pads zero-count
        // buckets after sorting/truncating the competitive nonzero buckets.
        auto min = std::max<int64_t>(thisOp().minCount, 1);

        if (allTopTerms) {
          assert(thisOp().ordMap != nullptr);
          for (const auto& entry : thisOp().ordMap->topTerms().entries) {
            if (entry.df < min) break;
            if ((int64_t)ordCounts.size() == limit) break;
            ordCounts.emplace_back(entry.ord, entry.df);
          }
        } else if (mapCounts) {
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
        } else if (skinnyCounts) {
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

          if (sortingByCountDesc && limit != -1
              && (int64_t)ordCounts.size() >= limit) {
            // already have enough counts, from overflow, and they are guaranteed to be larger than anything that didn't overflow.
          } else {
            for (size_t ord = 0; ord < skinnyCounts->counts.size(); ord++) {
              auto count = skinnyCounts->counts[ord];
              if (count >= min) {
                ordCounts.emplace_back(ord, count);
              }
            }
          }
        } else if (spanCounts) {
          collectSparseCounts(*spanCounts, min, limit, ordCounts);
        } else {
          assert(false);
        }

        // Bench-only exact per-request counter footprint (set LUXIR_FACET_BYTES).
        // Process RSS is too coarse for the sparse-cell wins this counter targets,
        // so report the chosen rep's heap bytes directly. Cached env read = off-path.
        static const bool logFacetBytes = std::getenv("LUXIR_FACET_BYTES") != nullptr;
        if (logFacetBytes && !allTopTerms) {
          const char* repName = "?";
          size_t bytes = 0;
          if (mapCounts) {
            repName = "hash";
            bytes = mapCounts->bucket_count()
                  * (sizeof(MergeableStrData::OrdHash::value_type) + 1);
          } else if (vecCounts) {
            repName = "vector";
            bytes = vecCounts->capacity() * sizeof(int64_t);
          } else if (skinnyCounts) {
            repName = "skinny";
            bytes = skinnyCounts->counts.capacity()
                  + skinnyCounts->overflow.bucket_count()
                      * (sizeof(std::pair<int64_t, int64_t>) + 1);
          } else if (spanCounts) {
            repName = "span";
            bytes = spanCounts->bytesUsed();
          }
          int64_t numOrds = thisOp().ordMap ? thisOp().ordMap->numOrds() : 0;
          LOG_WARN("FACETBYTES field={} rep={} numOrds={} emitted={} bytes={}",
                   thisOp().fieldName, repName, numOrds, ordCounts.size(), bytes);
        }

      }

      mergedData.reset();
      finishOrdCounts(std::move(ordCounts), missing_count, haveOrdCounts,
                      pinCounts);
    }

    void finishOrdCounts(
        std::vector<std::pair<int64_t, int64_t>> ordCounts,
        int64_t missingCount, bool allowZeroPadding = true,
        std::span<const int64_t> pinCounts = {}) {
      auto finalized = thisOp().finalizeOrdCounts(ordCounts, allowZeroPadding,
                                                  pinCounts);

      std::vector<std::pair<std::string, int64_t>> countVec;
      std::vector<SelectedFacetBucket<std::string_view>> selectedBuckets;
      auto poolGuard = MemPool::threadLocalPoolGuard();
      OrdMapStr ordMapStr(poolGuard.pool(), thisOp().ordMap.get(),
                         *thisOp().req.reader, thisOp().fieldName);
      countVec.reserve(finalized.size());
      selectedBuckets.reserve(finalized.size());
      for (const auto& bucket : finalized) {
        std::string_view key;
        std::optional<FacetBucketId> id;
        FacetBucketFlags flags = FacetBucketFlags::NONE;
        if (bucket.pinned()) {
          const auto& pin = thisOp().pinnedBuckets[bucket.pinIndex];
          key = pin.key;
          if (pin.ord.has_value()) id = FacetBucketId{*pin.ord};
          flags = FacetBucketFlags::PINNED;
        } else {
          assert(bucket.key.has_value());
          key = ordMapStr.ordToStr(*bucket.key);
          id = FacetBucketId{*bucket.key};
        }
        countVec.emplace_back(key, bucket.count);
        int32_t output = (int32_t)selectedBuckets.size();
        selectedBuckets.push_back({
            .key = countVec.back().first,
            .id = id,
            .count = bucket.count,
            .owner = FacetOwnerSlot{output},
            .output = FacetOutputSlot{output},
            .flags = flags,
        });
      }

      auto& mr = op.req.lastResponse->mr;
      luxir::api::FacetResult* result = nullptr;
      getTarget(nullptr, [&](luxir::api::Val& val) {
        routeTarget<luxir::api::ArrVal>(val, mr, [&](luxir::api::Val& target) {
          result = &oneofMut<luxir::api::FacetResult>(target);
        });
      });
      auto& facetResultProto = *result;
      emitBuckets(facetResultProto, countVec, mr);
      if (thisOp().missing) facetResultProto.missing = missingCount;
      executeResultChildren(selectedBuckets);
    }


    void facetResult2(std::unique_ptr<MergeableStrFacetInline> mergedData) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      luxir::api::FacetResult* result = nullptr;
      getTarget(nullptr, [&](luxir::api::Val& val) {
        routeTarget<luxir::api::ArrVal>(val, mr, [&](luxir::api::Val& target) {
          result = &oneofMut<luxir::api::FacetResult>(target);
        });
      });
      auto& facetResultProto = *result;
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;
      mergedData->counts.finalize();

      struct InlinePayload {
        char* entry;
        size_t finalizedSlot;
      };
      auto& counts = mergedData->counts;
      auto missing_count = mergedData->missing_num;

      // Count and bucket-value sorts are future work; sub-op sort is supported.
      SearchOp::InlineCalculator* sortCalc = nullptr;
      bool reversed = false;
      if (!thisOp().fieldFacet.sorts.empty()) {
        std::string_view field = thisOp().fieldFacet.sorts[0].expr;
        for (auto* candidate : mergedData->inlineCalcs) {
          if (candidate->getOp().name == field) {
            sortCalc = candidate;
            break;
          }
        }
        assert(sortCalc != nullptr);
        reversed =
            thisOp().fieldFacet.sorts[0].dir == luxir::api::SortSpec_::SortDir::DESC;
      }

      auto better = [sortCalc, reversed](const auto& a, const auto& b) {
        if (sortCalc == nullptr) {
          if (a.count != b.count) return a.count > b.count;
          return a.key < b.key;
        }
        bool aMissing = sortCalc->isMissing(a.payload.finalizedSlot);
        bool bMissing = sortCalc->isMissing(b.payload.finalizedSlot);
        if (aMissing != bMissing) return !aMissing;
        if (aMissing) return a.key < b.key;
        int cmp = sortCalc->compare(a.payload.finalizedSlot,
                                    b.payload.finalizedSlot);
        if (cmp == 0) return a.key < b.key;
        return reversed ? cmp > 0 : cmp < 0;
      };

      FieldBucketFinalizer<int64_t, InlinePayload, decltype(better)> finalizer(
          minCount, 0, limit, better);

      // A selected value's metrics live in the entry the one enumeration below
      // already visits, and its finalized slot is that enumeration's index, so
      // both are captured in passing. A facet with nothing selected never
      // enters the branch.
      boost::unordered_flat_map<int64_t, size_t> pinByOrd;
      std::vector<std::optional<int64_t>> pinKeys;
      std::vector<PinnedBucketValue<InlinePayload>> pinValues(
          thisOp().pinnedBuckets.size());
      pinKeys.reserve(thisOp().pinnedBuckets.size());
      for (size_t i = 0; i < thisOp().pinnedBuckets.size(); i++) {
        const auto& pin = thisOp().pinnedBuckets[i];
        pinKeys.push_back(pin.ord);
        if (pin.ord.has_value()) pinByOrd.emplace(*pin.ord, i);
      }
      const bool capturePins = !pinByOrd.empty();

      size_t finalizedSlot = 0;
      if (finalizer.needsCandidates()) {
        counts.forEachEntry([&](int64_t key, char* entry) {
          int64_t count = loadUnaligned<int64_t>(entry);
          finalizer.add({key, count, {entry, finalizedSlot}});
          if (capturePins) {
            auto pin = pinByOrd.find(key);
            if (pin != pinByOrd.end()) {
              pinValues[pin->second] =
                  {count, InlinePayload{entry, finalizedSlot}};
            }
          }
          finalizedSlot++;
        });
        assert(finalizedSlot == counts.size());
      }
      auto finalized = finalizer.finish();
      mergePinnedBuckets<int64_t, InlinePayload>(finalized, pinKeys, pinValues);

      // Term text for the buckets that survived selection, and only those -
      // ordToStr seeks the dictionary once per bucket instead of once per
      // counted document.  Its result is valid only until the next call, so
      // each is copied out; the copies back the string_views handed to result
      // children below, so they outlive the pool guard.
      auto poolGuard = MemPool::threadLocalPoolGuard();
      OrdMapStr ordMapStr(poolGuard.pool(), thisOp().ordMap.get(),
                          *thisOp().req.reader, thisOp().fieldName);
      std::vector<std::string> keys;
      keys.reserve(finalized.size());
      for (const auto& bucket : finalized) {
        if (bucket.pinned()) {
          keys.emplace_back(thisOp().pinnedBuckets[bucket.pinIndex].key);
        } else {
          assert(bucket.key.has_value());
          keys.emplace_back(ordMapStr.ordToStr(*bucket.key));
        }
      }

      // fill in the facet result proto (non-owning: size known up front)
      auto& bucketIds = facetResultProto.bucket_ids.emplace().kind.emplace<luxir::api::ColStr>();
      size_t n = finalized.size();
      std::string_view* ids = build::allocArray(bucketIds.v, n, mr);
      int64_t* countArr = build::allocArray(facetResultProto.counts, n, mr);
      for (size_t i = 0; i < n; i++) {
        ids[i] = build::arenaStr(mr, keys[i]);  // copy the (transient) string into the arena
        countArr[i] = finalized[i].count;
      }
      if (missing) {
        facetResultProto.missing = missing_count;
      }

      // fill in results from inline calculators
      std::vector<char*> results;
      std::vector<int64_t> resultCounts;
      results.reserve(finalized.size());
      resultCounts.reserve(finalized.size());
      for (const auto& bucket : finalized) {
        results.push_back(bucket.payload.has_value()
            ? bucket.payload->entry + sizeof(int64_t) : nullptr);
        resultCounts.push_back(bucket.count);
      }
      for (auto calc : mergedData->inlineCalcs) {
        calc->fillResult(results, resultCounts);
      }

      std::vector<SelectedFacetBucket<std::string_view>> selectedBuckets;
      selectedBuckets.reserve(finalized.size());
      for (size_t i = 0; i < finalized.size(); i++) {
        std::optional<FacetBucketId> id;
        FacetBucketFlags flags = FacetBucketFlags::NONE;
        if (finalized[i].key.has_value()) {
          id = FacetBucketId{*finalized[i].key};
        }
        if (finalized[i].pinned()) flags = FacetBucketFlags::PINNED;
        selectedBuckets.push_back({
            .key = keys[i],
            .id = id,
            .count = finalized[i].count,
            .owner = FacetOwnerSlot{(int32_t)i},
            .output = FacetOutputSlot{(int32_t)i},
            .flags = flags,
        });
      }
      // Inline results and sort comparisons are complete. Release their
      // per-bucket state before any selected-bucket child bindings allocate
      // their own state.
      mergedData.reset();
      executeResultChildren(selectedBuckets);
    }

    DomainHandle materializeBucketDomain(
        int32_t segnum,
        const SelectedFacetBucket<std::string_view>& bucket) {
      SegFieldInfo segFieldInfo;
      auto& postingsReader =
          thisOp().reader.segments()[(size_t)segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      std::unique_ptr<DocSet> domain;
      if (fieldReader.seek(thisOp().fieldName)) {
        fieldReader.readFieldInfo(segFieldInfo);
        TermsEnum terms(poolGuard.pool(), postingsReader, segFieldInfo);
        if (terms.seek(bucket.key)) {
          int32_t docFreq = terms.docFreq();
          DocsOnlyEnum postings(terms);
          domain = materializePostingsIntersection(
              postings, docFreq, input[(size_t)segnum].get(), maxDoc);
        }
      }

      // A null domain means all documents, never an empty owner. Preserve an
      // explicit empty set when the field or selected value is absent.
      if (domain == nullptr) {
        DocSetBuilder empty(maxDoc);
        domain = empty.build();
      }
      return DomainHandle(std::move(domain));
    }

    // Build every returned bucket's domain for every segment in one pass over
    // the facet field's ord column, instead of one term seek plus postings
    // intersection per bucket.  Indexed [segment * numBuckets + owner], which is
    // what the bucket-domain executor asks for.
    //
    // A document reaches a builder at most once: ord columns carry each doc's
    // distinct terms, and the segment-to-global mapping is injective, so no two
    // stored ords of one document route to the same owner.  DocSetBuilder's
    // debug monotonicity assert covers the claim.
    std::vector<DomainHandle> ordColumnBucketDomains(
        std::span<const SelectedFacetBucket<std::string_view>> buckets) {
      size_t numBuckets = buckets.size();
      size_t numSegments = input.size();
      std::vector<DomainHandle> domains(numSegments * numBuckets);
      auto selected = StrFacetSelectedOrdMap::selectedBuckets(buckets);

      for (size_t segnum = 0; segnum < numSegments; segnum++) {
        auto& postingsReader =
            thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        auto poolGuard = MemPool::threadLocalPoolGuard();
        // Reserved up front: DocSetBuilder holds a pointer into its own
        // optional bitset, so it must never be reallocated once built into.
        std::vector<DocSetBuilder> builders;
        builders.reserve(numBuckets);
        for (size_t i = 0; i < numBuckets; i++) builders.emplace_back(maxDoc);

        FieldReader fieldReader(postingsReader);
        if (fieldReader.seek(thisOp().fieldName)) {
          SegFieldInfo segFieldInfo;
          fieldReader.readFieldInfo(segFieldInfo);
          OrdColReader ordColReader(postingsReader, segFieldInfo);
          auto mapping = thisOp().ordMap->getSegToGlobal((int32_t)segnum);
          StrFacetSelectedOrdMap selectedMap(mapping, selected);
          int64_t missing_num = 0;
          DocSet* domain = input[segnum].get();
          selectedMap.visit([&](const auto& ownerOfOrd) {
            forEachOrdValue(
                domain, ordColReader, maxDoc, missing_num,
                [&](int32_t docid, int32_t storedOrd) LUXIR_INLINE {
                  int32_t owner = StrFacetSelectedOrdMap::owner(
                      ownerOfOrd, storedOrd);
                  if (owner >= 0) builders[(size_t)owner].add(docid);
                });
          });
        }

        for (size_t i = 0; i < numBuckets; i++) {
          domains[segnum * numBuckets + i] =
              DomainHandle(builders[i].build());
        }
      }
      return domains;
    }

    // Domain size, index size and returned coverage, for the bucket-domain
    // construction choice.
    struct BucketDomainCost {
      int64_t domainDocs = 0;
      int64_t maxDocs = 0;
      int64_t selectedDocs = 0;
    };

    BucketDomainCost bucketDomainCost(
        std::span<const SelectedFacetBucket<std::string_view>> buckets) {
      BucketDomainCost cost;
      for (size_t segnum = 0; segnum < input.size(); segnum++) {
        int32_t segmentMax =
            thisOp().reader.segments()[segnum].postingsReader().maxDoc();
        cost.maxDocs += segmentMax;
        DocSet* domain = input[segnum].get();
        cost.domainDocs += domain != nullptr ? domain->card() : segmentMax;
      }
      for (const auto& bucket : buckets) cost.selectedDocs += bucket.count;
      return cost;
    }

    void executeResultChildren(
        std::span<const SelectedFacetBucket<std::string_view>> buckets) {
      if (feedOps.empty()) return;

      bool tryReplay = thisOp().ordMap != nullptr
          && (forcedFacetFeedStrategy
                  == FacetFeedStrategy::STRING_COLUMN_REPLAY
              || (forcedFacetFeedStrategy == FacetFeedStrategy::AUTO
                  && feedOps.size() == 1));
      if (tryReplay) {
        StringFacetColumnSource source{
            .field = thisOp().fieldName,
            .ordMap = *thisOp().ordMap,
            .domains = input,
            .buckets = buckets
        };
        FacetChildContext context{.parent = *this, .stringColumn = &source};
        std::vector<std::unique_ptr<FacetChildExecutor>> bindings;
        bindings.reserve(feedOps.size());
        for (auto& [name, child] : feedOps) {
          unused(name);
          auto* binding = child->bindFacetChild(context);
          if (binding == nullptr
              || binding->feedKind()
                     != FacetFeedKind::STRING_COLUMN_REPLAY) {
            bindings.clear();
            break;
          }
          bindings.emplace_back(binding);
        }
        bool forced = forcedFacetFeedStrategy
            == FacetFeedStrategy::STRING_COLUMN_REPLAY;
        bool dominant = bindings.size() == feedOps.size()
            && std::ranges::all_of(bindings, [](const auto& binding) {
              return binding->dominatesBucketDomains();
            });
        if (bindings.size() == feedOps.size()
            && (forced || dominant)) {
          if (profileRun != nullptr) {
            for (auto& piece : profileRun->pieces) {
              piece.details.emplace_back(
                  "result-feed=string-column-replay");
            }
          }
          for (auto& binding : bindings) binding->execute();
          return;
        }
      }

      bool ordColumnDomains = false;
      if (thisOp().ordMap != nullptr && !buckets.empty()
          && forcedFacetBucketDomainSource
                 != FacetBucketDomainSource::POSTINGS) {
        auto cost = bucketDomainCost(buckets);
        ordColumnDomains = forcedFacetBucketDomainSource
                == FacetBucketDomainSource::ORD_COLUMN
            || StrFacetBucketDomainPlan::ordColumnBeatsPostings(
                   cost.domainDocs, cost.selectedDocs, cost.maxDocs,
                   (int64_t)buckets.size(), (int64_t)input.size());
      }

      if (profileRun != nullptr) {
        for (auto& piece : profileRun->pieces) {
          // The merge callback proves all piece detail writes are finished,
          // but a final ExecutionProfileScope may not yet have set complete.
          // Do not read that non-atomic lifecycle flag here.
          piece.details.emplace_back("result-feed=bucket-domains");
          piece.details.emplace_back(
              ordColumnDomains ? "bucket-domain-source=ord-column"
                               : "bucket-domain-source=postings");
        }
      }

      std::vector<SearchOp*> children;
      children.reserve(feedOps.size());
      for (auto& [name, child] : feedOps) {
        unused(name);
        children.push_back(child);
      }

      std::vector<DomainHandle> built;
      if (ordColumnDomains) built = ordColumnBucketDomains(buckets);

      FacetBucketDomainExecutor::execute(
          *this, children, buckets, (int32_t)input.size(),
          [&](int32_t segment, const auto& bucket) {
            if (ordColumnDomains) {
              return built[(size_t)segment * buckets.size()
                           + (size_t)bucket.owner.value];
            }
            return materializeBucketDomain(segment, bucket);
          });
    }
  };

  class ReplayResultWriter : public Calculator {
    StrFacetOp& child;

  public:
    ReplayResultWriter(StrFacetOp& child, Calculator& parent,
                       int64_t numSlots)
        : Calculator(child, &parent, -1, numSlots), child(child) {}

    luxir::api::Val* getTargetForSub(
        SearchResponse* resp, Calculator* sub) override {
      unused(resp);
      unused(sub);
      return nullptr;
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domain) override {
      unused(tg);
      unused(segnum);
      unused(domain);
      assert(false);
    }

    void finish(
        StrFacetReplayBank::Rows rows,
        std::span<const int64_t> missingCounts,
        std::span<const SelectedFacetBucket<std::string_view>> buckets) {
      assert(rows.size() == buckets.size());
      assert(missingCounts.size() == buckets.size());
      auto& mr = child.req.lastResponse->mr;
      auto* target = getTarget(nullptr, [&](luxir::api::Val& val) {
        auto& arr = oneofMut<luxir::api::ArrVal>(val);
        if (arr.v.empty()) build::allocArray(arr.v, buckets.size(), mr);
      });
      auto& arr = oneofMut<luxir::api::ArrVal>(*target);

      auto poolGuard = MemPool::threadLocalPoolGuard();
      OrdMapStr ordMapStr(poolGuard.pool(), child.ordMap.get(),
                         *child.req.reader, child.fieldName);
      std::vector<std::pair<std::string, int64_t>> countVec;
      for (const auto& bucket : buckets) {
        int32_t owner = bucket.owner.value;
        int32_t output = bucket.output.value;
        assert(owner >= 0 && owner < (int32_t)rows.size());
        assert(output >= 0 && output < (int32_t)arr.v.size());
        auto& ordCounts = rows[(size_t)owner];
        auto finalized = child.finalizeOrdCounts(ordCounts, true);
        countVec.clear();
        countVec.reserve(finalized.size());
        for (const auto& result : finalized) {
          assert(!result.pinned() && result.key.has_value());
          countVec.emplace_back(
              ordMapStr.ordToStr(*result.key), result.count);
        }
        auto& facetResultProto = oneofMut<luxir::api::FacetResult>(
            const_cast<luxir::api::Val&>(arr.v[(size_t)output]));
        emitBuckets(facetResultProto, countVec, mr);
        if (child.missing) {
          facetResultProto.missing = missingCounts[(size_t)owner];
        }
      }
    }
  };

  class ColumnReplayExecutor : public FacetChildExecutor {
    StrFacetOp& child;
    SearchOp::Calculator& parent;
    StringFacetColumnSource source;
    StrFacetSelectedOrdMap::Selected selectedBuckets;
    ExecutionProfileRun* profileRun;
    int64_t domainDocs = 0;
    int64_t maxDocs = 0;
    int64_t expectedUpdates = 0;

    template <bool ChildMulti, typename Visit>
    static bool visitChildOrds(
        int32_t docid, OrdColReader& childColumn,
        OrdColReader::Iterator& childIterator,
        const OrdMap::SegToGlobal& childMapping,
        Visit&& visit) {
      if (childIterator.docId() < docid) childIterator.advance(docid);
      if (childIterator.docId() != docid) return false;
      if constexpr (!ChildMulti) {
        int32_t storedOrd = childIterator.value();
        if (storedOrd == 0) return false;
        visit(childMapping.globalOrd((int64_t)storedOrd - 1));
        return true;
      } else {
        auto [start, end] = childColumn.getStartEndValueRank(
            childIterator.rank());
        for (int64_t rank = start; rank < end; rank++) {
          int32_t storedOrd = childIterator.values().valueAt(rank);
          visit(childMapping.globalOrd((int64_t)storedOrd - 1));
        }
        return start != end;
      }
    }

    template <bool TrackProfile, bool ChildPresent, bool ChildMulti,
              typename Bank, typename SelectedMap>
    void replayValues(
        Bank& bank, const SelectedMap& selectedMap,
        OrdColReader& parentColumn, int32_t maxDoc, DocSet* domain,
        OrdColReader* childColumn, OrdColReader::Iterator* childIterator,
        const OrdMap::SegToGlobal* childMapping,
        std::vector<int64_t>& missingCounts,
        int64_t& routedDocs, int64_t& routedValues) {
      int64_t numChildOrds = child.ordMap->numOrds();
      forEachSelectedBucketDoc(
          domain, parentColumn, maxDoc, selectedMap,
          [&](int32_t docid,
              std::span<const int32_t> owners) LUXIR_INLINE {
            if constexpr (TrackProfile) routedDocs++;
            bool haveChildValue = false;
            if constexpr (ChildPresent) {
              haveChildValue = visitChildOrds<ChildMulti>(
                  docid, *childColumn, *childIterator, *childMapping,
                  [&](int64_t ord) LUXIR_INLINE {
                    for (int32_t owner : owners) {
                      StrFacetReplayBank::increment(
                          bank, owner, ord, numChildOrds);
                    }
                    if constexpr (TrackProfile) {
                      routedValues += (int64_t)owners.size();
                    }
                  });
            }
            if (!haveChildValue && child.missing) {
              for (int32_t owner : owners) {
                missingCounts[(size_t)owner]++;
              }
            }
          });
    }

    template <bool TrackProfile, typename Bank>
    void replaySegment(Bank& bank,
                       std::vector<int64_t>& missingCounts,
                       int32_t segnum) {
      auto& postingsReader =
          child.reader.segments()[(size_t)segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto profile = parent.profilePiece(
          TrackProfile ? profileRun : nullptr, segnum);
      int64_t routedDocs = 0;
      int64_t routedValues = 0;

      FieldReader parentFieldReader(postingsReader);
      if (!parentFieldReader.seek(source.field)) return;
      SegFieldInfo parentFieldInfo;
      parentFieldReader.readFieldInfo(parentFieldInfo);
      OrdColReader parentColumn(postingsReader, parentFieldInfo);
      auto parentMapping = source.ordMap.getSegToGlobal(segnum);
      StrFacetSelectedOrdMap selected(parentMapping, selectedBuckets);

      FieldReader childFieldReader(postingsReader);
      bool childFieldFound = childFieldReader.seek(child.fieldName);
      std::optional<SegFieldInfo> childFieldInfo;
      std::optional<OrdColReader> childColumn;
      std::optional<OrdColReader::Iterator> childIterator;
      auto childMapping = child.ordMap->getSegToGlobal(segnum);
      if (childFieldFound) {
        childFieldInfo.emplace();
        childFieldReader.readFieldInfo(*childFieldInfo);
        childColumn.emplace(postingsReader, *childFieldInfo);
        childIterator.emplace(*childColumn);
      }

      if (!childFieldFound && !child.missing) {
        if constexpr (TrackProfile) {
          if (auto* piece = profile.get()) {
            piece->details.emplace_back(
                "feed=string-column-replay, child=absent, skipped");
          }
        }
        return;
      }

      selected.visit([&](const auto& selectedMap) {
        DocSet* domain = source.domains[(size_t)segnum].get();
        if (!childFieldFound) {
          replayValues<TrackProfile, false, false>(
              bank, selectedMap, parentColumn, maxDoc, domain,
              nullptr, nullptr, nullptr, missingCounts,
              routedDocs, routedValues);
        } else if (childColumn->multiValued()) {
          replayValues<TrackProfile, true, true>(
              bank, selectedMap, parentColumn, maxDoc, domain,
              &*childColumn, &*childIterator, &childMapping,
              missingCounts, routedDocs, routedValues);
        } else {
          replayValues<TrackProfile, true, false>(
              bank, selectedMap, parentColumn, maxDoc, domain,
              &*childColumn, &*childIterator, &childMapping,
              missingCounts, routedDocs, routedValues);
        }
      });

      if constexpr (TrackProfile) {
        if (auto* piece = profile.get()) {
          piece->details.emplace_back(std::format(
              "feed=string-column-replay, selector={}, parent={}, child={}, "
              "routed-docs={}, routed-values={}",
              selected.dense() ? "dense" : "sparse",
              parentColumn.multiValued() ? "multi" : "single",
              childFieldFound
                  ? (childColumn->multiValued() ? "multi" : "single")
                  : "absent",
              routedDocs, routedValues));
        }
      }
    }

  public:
    ColumnReplayExecutor(StrFacetOp& child,
                         SearchOp::Calculator& parent,
                         const StringFacetColumnSource& source)
        : child(child), parent(parent), source(source),
          selectedBuckets(
              StrFacetSelectedOrdMap::selectedBuckets(source.buckets)),
          profileRun(nullptr) {
      __int128 selectedRoutes = 0;
      for (const auto& bucket : source.buckets) {
        selectedRoutes += bucket.count;
      }
      __int128 childValues = 0;
      for (size_t segnum = 0; segnum < source.domains.size(); segnum++) {
        auto& postingsReader =
            child.reader.segments()[segnum].postingsReader();
        int32_t segmentMax = postingsReader.maxDoc();
        maxDocs += segmentMax;
        DocSet* domain = source.domains[segnum].get();
        domainDocs += domain != nullptr ? domain->card() : segmentMax;
        FieldReader fieldReader(postingsReader);
        if (!fieldReader.seek(child.fieldName)) continue;
        SegFieldInfo fieldInfo;
        fieldReader.readFieldInfo(fieldInfo);
        childValues += fieldInfo.numValues;
      }
      __int128 estimate = maxDocs > 0
          ? (selectedRoutes * childValues + maxDocs - 1) / maxDocs
          : 0;
      expectedUpdates = (int64_t)std::min<__int128>(
          estimate, std::numeric_limits<int64_t>::max());
    }

    FacetFeedKind feedKind() const override {
      return FacetFeedKind::STRING_COLUMN_REPLAY;
    }

    bool dominatesBucketDomains() const override {
      return StrFacetColumnReplayPlan::dominatesBucketDomains(
          source.ordMap.numOrds(), child.ordMap->numOrds(),
          domainDocs, maxDocs, (int64_t)source.buckets.size(),
          expectedUpdates);
    }

    void execute() override {
      int32_t numOwners = (int32_t)source.buckets.size();
      if (numOwners == 0) return;
      profileRun = child.addExecutionProfileRun();

      StrFacetReplayBank bank(
          numOwners, child.ordMap->numOrds(), expectedUpdates);
      if (profileRun != nullptr) {
        for (auto& piece : profileRun->pieces) {
          piece.details.emplace_back(std::format(
              "bank={}, owners={}, child-ords={}, expected-updates={}",
              StrFacetReplayBank::strategyName(bank.effectiveStrategy()),
              numOwners, child.ordMap->numOrds(), expectedUpdates));
        }
      }
      std::vector<int64_t> missingCounts((size_t)numOwners);
      if (profileRun != nullptr) {
        bank.visit([&](auto& concreteBank) {
          for (int32_t segnum = 0;
               segnum < (int32_t)source.domains.size(); segnum++) {
            replaySegment<true>(concreteBank, missingCounts, segnum);
          }
        });
      } else {
        bank.visit([&](auto& concreteBank) {
          for (int32_t segnum = 0;
               segnum < (int32_t)source.domains.size(); segnum++) {
            replaySegment<false>(concreteBank, missingCounts, segnum);
          }
        });
      }

      ReplayResultWriter writer(child, parent, numOwners);
      writer.finish(bank.drain(), missingCounts, source.buckets);
    }
  };

  FacetChildExecutor* bindFacetChild(
      const FacetChildContext& context) override {
    if (context.stringColumn == nullptr || ordMap == nullptr
        || !subOps.empty() || !inlineSubOps.empty() || !fieldFacet.sorts.empty()) {
      return nullptr;
    }
    for (const auto& bucket : context.stringColumn->buckets) {
      if (!bucket.id.has_value()) return nullptr;
    }
    return new ColumnReplayExecutor(*this, context.parent,
                                    *context.stringColumn);
  }

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

} // namespace luxir
