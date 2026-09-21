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

  // Open the segment column under a pool scope. An absent field adds the
  // whole domain to missing_num; otherwise the visitor owns the column walk.
  bool withIntColumn(DocSet* domain, int32_t segnum, int64_t& missing_num,
                     SegFieldInfo& segFieldInfo, auto&& visitColumn) {
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
    IntColReader intColReader(postingsReader, segFieldInfo);
    visitColumn(intColReader, maxDoc);
    return true;
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

protected:
  std::vector<std::pair<const std::string_view, SearchOp*>> inlineSubOps;

  void inlineRemainingSubOps() {
    std::vector<std::string_view> moved;
    for (auto& subOp : subOps) {
      if (subOp.second->canInline()) {
        inlineSubOps.push_back(subOp);
        moved.push_back(subOp.first);
      }
    }
    for (auto& name : moved) subOps.erase(name);
  }

public:
  void init() override {
    FacetReq::init();
    auto sorts = fieldFacet.sort;
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

    bool inlineAll = limit == -1;
    if (forcedFacetSubOpInline == FacetSubOpInlineMode::ALL) inlineAll = true;
    if (forcedFacetSubOpInline == FacetSubOpInlineMode::SORT_KEY_ONLY) inlineAll = false;
    if (inlineAll) inlineRemainingSubOps();
  }

  class MergeableFieldFacetInline : public MergeableData {
  public:
    FacetEntryTable counts;
    int64_t missing_num = 0; // number of missing values in this segment
    std::vector<SearchOp::InlineCalculator*> inlineCalcs;
    static MergeableFieldFacetInline* merge(MergeableFieldFacetInline* a, MergeableFieldFacetInline* b) {
      // Mixed representations always merge into dense. Otherwise merge the
      // smaller touched set into the larger one; dense/dense therefore walks
      // the smaller touched list without scanning the key space.
      if (a->counts.dense() != b->counts.dense()) {
        if (b->counts.dense()) std::swap(a, b);
      } else if (a->counts.size() < b->counts.size()) {
        std::swap(a, b);
      }

      a->counts.merge(b->counts);
      a->missing_num += b->missing_num;
      return a;
    }
    ~MergeableFieldFacetInline() {
      for (auto* calc : inlineCalcs) {
        delete calc; // clean up the inline calculators
      }
    }
  };

  class Calc : public Calculator {
  protected:
    FieldFacetReq& fieldOp() { return (FieldFacetReq&)getOp(); }
    class InlineSegmentScope {
      // Aggregate startSeg binds expression programs in the thread-local pool.
      MemPool::ScopeGuard poolGuard = MemPool::threadLocalPoolGuard();
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

    DomainHandle materializeTermDomain(
        int32_t segnum, std::string_view term, DocSet* inputDomain) {
      SegFieldInfo segFieldInfo;
      auto& postingsReader =
          fieldOp().reader.segments()[(size_t)segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      std::unique_ptr<DocSet> domain;
      if (fieldReader.seek(fieldOp().fieldName)) {
        fieldReader.readFieldInfo(segFieldInfo);
        TermsEnum terms(poolGuard.pool(), postingsReader, segFieldInfo);
        if (terms.seek(term)) {
          int32_t docFreq = terms.docFreq();
          DocsOnlyEnum postings(terms);
          domain = materializePostingsIntersection(
              postings, docFreq, inputDomain, maxDoc);
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

    template <typename Key, typename DomainSource>
    void executeBucketChildren(std::span<const SelectedFacetBucket<Key>> buckets,
                               DomainSource&& source) {
      if (fieldOp().subOps.empty()) return;
      std::vector<SearchOp*> children;
      children.reserve(fieldOp().subOps.size());
      for (auto& [name, child] : fieldOp().subOps) children.push_back(child);
      FacetBucketBlockExecutor::execute<Key>(
          *this, children, buckets, fieldOp().reader, source,
          FacetBucketBlockExecutor::BINDING_BYTES,
          FacetBucketBlockExecutor::DOMAIN_BYTES, []() {});
    }

    template <typename Key, typename Buckets, typename KeyAt, typename IdAt>
    static std::vector<SelectedFacetBucket<Key>> selectedBuckets(
        const Buckets& finalized, KeyAt&& keyAt, IdAt&& idAt) {
      std::vector<SelectedFacetBucket<Key>> buckets;
      buckets.reserve(finalized.size());
      for (size_t i = 0; i < finalized.size(); i++) {
        buckets.push_back({keyAt(i), idAt(i), finalized[i].count,
            FacetOwnerSlot{(int32_t)i}, FacetOutputSlot{(int32_t)i},
            finalized[i].pinned() ? FacetBucketFlags::PINNED : FacetBucketFlags::NONE});
      }
      return buckets;
    }

    template <typename Data = MergeableFieldFacetInline>
    Data* createInlineData(
        std::span<const std::pair<const std::string_view, SearchOp*>> inlineOps,
        int64_t denseKeySpaceSize) {
      auto data = std::make_unique<Data>();
      data->inlineCalcs.reserve(inlineOps.size());
      for (auto& [key, subop] : inlineOps) {
        data->inlineCalcs.push_back(subop->createInlineCalculator(this, -1, -1));
      }
      std::string detail = data->inlineCalcs.size() == 1
          ? std::format("facet '{}' metric '{}'", fieldOp().facetName,
                        data->inlineCalcs.front()->getOp().name)
          : std::format("facet '{}' inline metrics", fieldOp().facetName);
      data->counts.configure(data->inlineCalcs, op.req.memoryTracker,
                             denseKeySpaceSize, std::move(detail),
                             inlineFacetEntryStatsForTests);
      return data.release();
    }

    template <typename Label, typename Data, typename ResolveLabel, typename ExecuteChildren,
              typename KeyLess = std::less<int64_t>>
    void inlineResult(std::unique_ptr<Data> mergedData,
                      std::span<const std::optional<int64_t>> pinKeys,
                      ResolveLabel&& resolveLabel, ExecuteChildren&& executeChildren,
                      KeyLess keyLess = {}) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result (getTarget(nullptr) builds here)
      auto& facetResultProto = *slotArm<luxir::api::FacetResult>(nullptr);
      auto minCount = fieldOp().minCount;
      auto limit = fieldOp().limit;
      auto missing = fieldOp().missing;
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
      if (!fieldOp().fieldFacet.sort.empty()) {
        std::string_view field = fieldOp().fieldFacet.sort[0].expr;
        for (auto* candidate : mergedData->inlineCalcs) {
          if (candidate->getOp().name == field) {
            sortCalc = candidate;
            break;
          }
        }
        assert(sortCalc != nullptr);
        reversed =
            fieldOp().fieldFacet.sort[0].dir == luxir::api::SortSpec_::SortDir::DESC;
      }

      auto better = [sortCalc, reversed, keyLess](const auto& a, const auto& b) {
        if (sortCalc == nullptr) {
          if (a.count != b.count) return a.count > b.count;
          return keyLess(a.key, b.key);
        }
        bool aMissing = sortCalc->isMissing(a.payload.finalizedSlot);
        bool bMissing = sortCalc->isMissing(b.payload.finalizedSlot);
        if (aMissing != bMissing) return !aMissing;
        if (aMissing) return keyLess(a.key, b.key);
        int cmp = sortCalc->compare(a.payload.finalizedSlot,
                                    b.payload.finalizedSlot);
        if (cmp == 0) return keyLess(a.key, b.key);
        return reversed ? cmp > 0 : cmp < 0;
      };

      FieldBucketFinalizer<int64_t, InlinePayload, decltype(better)> finalizer(
          minCount, 0, limit, better);

      // A selected value's metrics live in the entry the one enumeration below
      // already visits, and its finalized slot is that enumeration's index, so
      // both are captured in passing. A facet with nothing selected never
      // enters the branch.
      boost::unordered_flat_map<int64_t, size_t> pinByKey;
      std::vector<PinnedBucketValue<InlinePayload>> pinValues(pinKeys.size());
      for (size_t i = 0; i < pinKeys.size(); i++) {
        if (pinKeys[i]) pinByKey.emplace(*pinKeys[i], i);
      }
      const bool capturePins = !pinByKey.empty();

      size_t finalizedSlot = 0;
      if (finalizer.needsCandidates() || capturePins) {
        counts.forEachEntry([&](int64_t key, char* entry) {
          int64_t count = counts.occurrenceCount(entry);
          finalizer.add({key, count, {entry, finalizedSlot}});
          if (capturePins) {
            auto pin = pinByKey.find(key);
            if (pin != pinByKey.end()) {
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

      // Resolve labels only for selected buckets. Owning strings, if needed,
      // keep borrowed child keys valid throughout synchronous execution.
      std::vector<std::pair<Label, int64_t>> emitted;
      emitted.reserve(finalized.size());
      for (const auto& bucket : finalized) {
        emitted.emplace_back(resolveLabel(bucket), bucket.count);
      }
      emitBuckets(facetResultProto, emitted, mr);
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
        resultCounts.push_back(bucket.payload.has_value()
            ? loadUnaligned<int64_t>(bucket.payload->entry) : 0);
      }
      for (auto calc : mergedData->inlineCalcs) {
        calc->fillResult(results, resultCounts);
      }

      using ChildKey = std::conditional_t<std::is_same_v<Label, std::string>,
                                          std::string_view, Label>;
      auto buckets = selectedBuckets<ChildKey>(finalized,
          [&](size_t i) -> ChildKey { return emitted[i].first; },
          [&](size_t i) -> std::optional<FacetBucketId> {
            if constexpr (std::is_same_v<Label, int64_t>) {
              return FacetBucketId{emitted[i].first};
            } else {
              return finalized[i].key.has_value()
                  ? std::optional(FacetBucketId{*finalized[i].key}) : std::nullopt;
            }
          });
      // Inline results and sort comparisons are complete. Release their
      // per-bucket state before any selected-bucket child bindings allocate
      // their own state.
      mergedData.reset();
      executeChildren(buckets);
    }

  public:
    using Calculator::Calculator;
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
      std::size_t cap = fieldOp().subOps.size() + fieldOp().inlineSubOps.size();
      return build::opsSlot(fr.ops, cap, sub->getOp().name, resp->mr);
    };
  };

public:
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
  struct GlobalRange {
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    bool useVector = false;
  };

  // ProtobufSearchParser computes globalMin/globalMax/useVector via
  // scanGlobalRange and passes them in (see TopDocsReq's ctor comment).
  IntFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing,
    const GlobalRange& range,
    std::span<const int64_t> selected) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing,
                selected, {}),
  globalMin(range.min), globalMax(range.max), useVector(range.useVector) {}

  bool canEmitAsBucketChild() const override {
    return true;
  }

  // Scan every segment for the column's min/max and decide vector vs map
  // storage based on the resulting range.  Runs during parsing so the result
  // can be passed to the IntFacetReq ctor.
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

  class Calc : public FieldFacetReq::Calc {
    SegmentMergeDriver<MergeableIntFacet> driver;
    std::optional<SegmentMergeDriver<MergeableFieldFacetInline>> inlineDriver;
    std::vector<DomainHandle> input;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : FieldFacetReq::Calc(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableIntFacet> m){ facetResult(*m); }) {
      if (!thisOp().subOps.empty()) input.resize(op.req.reader->segments().size());
      if (!thisOp().inlineSubOps.empty()) {
        inlineDriver.emplace(op.req.reader->segments().size(),
            [this](std::unique_ptr<MergeableFieldFacetInline> m) {
              facetInlineResult(std::move(m));
            });
        inlineDriver->setCreator([this]() {
          return createInlineData(thisOp().inlineSubOps, thisOp().useVector
              ? thisOp().globalMax - thisOp().globalMin + 1 : 0);
        });
      }
    }
    IntFacetReq& thisOp() {
      return (IntFacetReq&)getOp();
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();
      if (!thisOp().inlineSubOps.empty()) {
        if (segnum == -1) {
          inlineDriver->completeEmpty();
          return;
        }
        if (!input.empty()) input[(size_t)segnum] = std::move(domainHandle);
        inlineDriver->contribute([&](MergeableFieldFacetInline& data) {
          InlineSegmentScope scope(data.inlineCalcs, segnum);
          int64_t expectedValues = domain ? domain->card()
              : thisOp().reader.segments()[(size_t)segnum].maxDoc();
          data.counts.initialize(expectedValues,
              forcedInlineFacetEntryMode == InlineFacetEntryMode::FORCE_DENSE,
              forcedInlineFacetEntryMode == InlineFacetEntryMode::FORCE_SPARSE);
          int64_t keyBase = thisOp().useVector ? thisOp().globalMin : 0;
          SegFieldInfo info;
          thisOp().withIntColumn(domain, segnum, data.missing_num, info,
              [&](IntColReader& column, int32_t maxDoc) {
                if (!column.multiValued()) {
                  forEachIntColValue(domain, column, maxDoc, data.missing_num,
                      [&](int32_t docid, int64_t value) LUXIR_INLINE {
                        data.counts.add(value - keyBase, docid);
                      });
                } else {
                  // Values retain insertion order, including non-adjacent
                  // repeats. Only the first occurrence feeds the metrics.
                  boost::unordered_flat_map<int64_t, char*> seen;
                  int32_t lastDoc = -1;
                  forEachIntColValue(domain, column, maxDoc, data.missing_num,
                      [&](int32_t docid, int64_t value) LUXIR_INLINE {
                        if (docid != lastDoc) {
                          seen.clear();
                          lastDoc = docid;
                        }
                        int64_t key = value - keyBase;
                        auto [it, first] = seen.try_emplace(key, nullptr);
                        if (first) {
                          it->second = data.counts.resolveEntry(key);
                          data.counts.touchEntry(it->second, docid);
                        } else {
                          data.counts.addOccurrence(it->second);
                        }
                      });
                }
              });
        });
        return;
      }
      if (segnum == -1) {
        driver.completeEmpty();  // empty index -> empty result
        return;
      }
      if (!input.empty()) input[(size_t)segnum] = std::move(domainHandle);
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
          uint64_t index = (uint64_t)selected - (uint64_t)merged.minValue;
          if (index < countVector->size()) {
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
      if (!thisOp().subOps.empty()) {
        auto buckets = selectedBuckets<int64_t>(finalized,
            [&](size_t i) { return *finalized[i].key; },
            [&](size_t i) { return FacetBucketId{*finalized[i].key}; });
        executeResultChildren(buckets);
      }
    }

    void facetInlineResult(std::unique_ptr<MergeableFieldFacetInline> data) {
      int64_t keyBase = thisOp().useVector ? thisOp().globalMin : 0;
      std::vector<std::optional<int64_t>> pins;
      pins.reserve(thisOp().selectedInts.size());
      for (int64_t value : thisOp().selectedInts) {
        if (!thisOp().useVector
            || (value >= thisOp().globalMin && value <= thisOp().globalMax)) {
          pins.emplace_back(value - keyBase);
        } else {
          pins.emplace_back(std::nullopt);
        }
      }
      inlineResult<int64_t>(std::move(data), pins,
          [this, keyBase](const auto& bucket) {
            if (bucket.pinned()) return thisOp().selectedInts[bucket.pinIndex];
            return *bucket.key + keyBase;
          }, [this](auto& buckets) { executeResultChildren(buckets); });
    }

    std::vector<DomainHandle> bucketDomains(
        size_t segnum, std::span<const SelectedFacetBucket<int64_t>> buckets) {
      boost::unordered_flat_map<int64_t, int32_t> builderOfValue;
      builderOfValue.reserve(buckets.size());
      for (size_t i = 0; i < buckets.size(); i++) {
        builderOfValue.emplace(buckets[i].key, (int32_t)i);
      }
      return buildFacetBucketDomains(
          thisOp().reader.segments()[segnum].maxDoc(), buckets.size(),
          [&](auto&& accept) {
            SegFieldInfo info;
            int64_t missing = 0;
            thisOp().facetSegIntCol(input[segnum].get(), (int32_t)segnum,
                                    missing, info, accept);
          }, [&](int64_t value) {
            auto it = builderOfValue.find(value);
            return it == builderOfValue.end() ? -1 : it->second;
          });
    }

    void executeResultChildren(std::span<const SelectedFacetBucket<int64_t>> buckets) {
      executeBucketChildren<int64_t>(buckets,
          [this](size_t segnum, auto chunk) { return bucketDomains(segnum, chunk); });
    }

  };

  size_t facetBucketResidentBytes() const override {
    int64_t width = useVector ? globalMax - globalMin + 1 : 0;
    size_t bytes = sizeof(Calc);
    if (inlineSubOps.empty()) {
      size_t counter = useVector
          ? (size_t)width * sizeof(int64_t)
          : 0;
      bytes = saturatingAdd(bytes, counter);
    } else {
      size_t stride = sizeof(int64_t);
      for (auto& [name, child] : inlineSubOps) {
        stride = saturatingAdd(stride, child->inlineEntryBytes());
      }
      bytes = saturatingAdd(bytes, FacetEntryTable::residentBytes(stride, width));
    }
    if (!subOps.empty()) {
      bytes = saturatingAdd(bytes,
          saturatingMultiply(reader.segments().size(), sizeof(DomainHandle)));
    }
    return bytes;
  }

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

  class MergeableTextInline : public MergeableFieldFacetInline {
    MemPool termPool;
    size_t termBytesCharged = 0;
  public:
    boost::unordered_flat_map<std::string_view, int64_t> termIds;
    std::vector<std::string_view> terms;
    RequestMemTracker* tracker = nullptr;
    std::string chargeDetail;

    int64_t termId(std::string_view term) {
      auto it = termIds.find(term);
      if (it != termIds.end()) return it->second;
      // Reserve pooled term bytes, open-addressed map slots, and amortized
      // reverse-label capacity alongside the entry table's aggregate charge.
      size_t bytes = term.size()
          + 2 * (sizeof(std::pair<std::string_view, int64_t>) + 1)
          + 2 * sizeof(std::string_view);
      tracker->charge(bytes, "facet aggregate state", chargeDetail);
      termBytesCharged += bytes;
      char* text = termPool.alloc(term.size());
      memcpy(text, term.data(), term.size());
      std::string_view stored(text, term.size());
      int64_t id = (int64_t)terms.size();
      terms.push_back(stored);
      termIds.emplace(stored, id);
      return id;
    }

    static MergeableTextInline* merge(MergeableTextInline* a, MergeableTextInline* b) {
      if (a->counts.size() < b->counts.size()) std::swap(a, b);
      a->counts.merge(b->counts, [&](int64_t id) {
        return a->termId(b->terms[(size_t)id]);
      });
      a->missing_num += b->missing_num;
      return a;
    }

    ~MergeableTextInline() {
      if (termBytesCharged != 0) tracker->release(termBytesCharged);
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

  class Calc : public FieldFacetReq::Calc {
    SegmentMergeDriver<MergeableStrFacet> driver;
    std::optional<SegmentMergeDriver<MergeableTextInline>> inlineDriver;
    std::vector<DomainHandle> input;
  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : FieldFacetReq::Calc(op, parent, slot, numSlots),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<MergeableStrFacet> m){ facetResult(*m); }) {
      if (!thisOp().subOps.empty()) input.resize(op.req.reader->segments().size());
      if (!thisOp().inlineSubOps.empty()) {
        inlineDriver.emplace(op.req.reader->segments().size(),
            [this](std::unique_ptr<MergeableTextInline> m) {
              facetInlineResult(std::move(m));
            });
        inlineDriver->setCreator([this]() {
          std::unique_ptr<MergeableTextInline> data(
              createInlineData<MergeableTextInline>(thisOp().inlineSubOps, 0));
          data->tracker = &thisOp().req.memoryTracker;
          data->chargeDetail = std::format("facet '{}' term keys", thisOp().facetName);
          return data.release();
        });
      }
    }
    FullTextFacetReq& thisOp() {
      return (FullTextFacetReq&)getOp();
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();
      if (inlineDriver.has_value()) {
        if (segnum == -1) {
          inlineDriver->completeEmpty();
          return;
        }
        if (!input.empty()) input[(size_t)segnum] = std::move(domainHandle);
        calcInline(segnum, domain);
        return;
      }
      if (segnum == -1) {
        driver.completeEmpty();  // empty index -> empty result
        return;
      }
      if (!input.empty()) input[(size_t)segnum] = std::move(domainHandle);
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

    void calcInline(int32_t segnum, DocSet* domain) {
      inlineDriver->contribute([&](MergeableTextInline& data) {
        InlineSegmentScope scope(data.inlineCalcs, segnum);
        data.counts.initialize(0, false, true);
        auto& postings = thisOp().reader.segments()[(size_t)segnum].postingsReader();
        DomainView view(domain, postings.maxDoc());
        FieldReader field(postings);
        if (!field.seek(thisOp().fieldName)) {
          data.missing_num += view.card;
          return;
        }
        SegFieldInfo info;
        field.readFieldInfo(info);
        TermsEnum terms(MemPool::threadLocal(), postings, info);
        int32_t lastDoc = -1;
        while (terms.nextTerm()) {
          DocsOnlyEnum docs(terms);
          char* entry = nullptr;
          forEachPostingInDomain(docs, terms.docFreq(), domain,
              [&](int32_t docid) LUXIR_INLINE {
                // Resolve on the first matching doc, leaving zero-match terms
                // out of the touched-only inline result and its dictionary.
                if (entry == nullptr) {
                  // A term's postings ascend, so only its first in-domain doc
                  // can step backwards; a term with none never rewinds.
                  if (docid < lastDoc) {
                    for (auto* calc : data.inlineCalcs) calc->rewind();
                  }
                  entry = data.counts.resolveEntry(data.termId((std::string_view)terms.term()));
                }
                data.counts.touchEntry(entry, docid);
                lastDoc = docid;
              });
        }
        if (thisOp().missing) {
          DocsReader docs(postings, info);
          data.missing_num += countMissingInDomain(view, docs);
        }
      });
    }

    void facetInlineResult(std::unique_ptr<MergeableTextInline> data) {
      auto* terms = &data->terms;
      std::vector<std::optional<int64_t>> pins;
      pins.reserve(thisOp().selectedStrings.size());
      for (std::string_view pin : thisOp().selectedStrings) {
        auto it = data->termIds.find(pin);
        pins.push_back(it == data->termIds.end() ? std::nullopt : std::optional(it->second));
      }
      inlineResult<std::string>(std::move(data), pins,
          [this, terms](const auto& bucket) {
            return std::string(bucket.pinned()
                ? thisOp().selectedStrings[bucket.pinIndex] : (*terms)[(size_t)*bucket.key]);
          }, [this](auto& buckets) { executeResultChildren(buckets); },
          [terms](int64_t a, int64_t b) { return (*terms)[(size_t)a] < (*terms)[(size_t)b]; });
    }

    void executeResultChildren(
        std::span<const SelectedFacetBucket<std::string_view>> buckets) {
      executeBucketChildren<std::string_view>(buckets,
          [this](size_t segnum, auto chunk) {
            std::vector<DomainHandle> domains;
            domains.reserve(chunk.size());
            for (const auto& bucket : chunk) {
              domains.push_back(materializeTermDomain((int32_t)segnum, bucket.key,
                                                       input[segnum].get()));
            }
            return domains;
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
      if (!thisOp().subOps.empty()) {
        auto buckets = selectedBuckets<std::string_view>(finalized,
            [&](size_t i) -> std::string_view { return countVec[i].first; },
            [](size_t) { return std::optional<FacetBucketId>(); });
        executeResultChildren(buckets);
      }
    }

  };

  size_t facetBucketResidentBytes() const override {
    return saturatingAdd(sizeof(Calc), subOps.empty() ? 0
        : saturatingMultiply(reader.segments().size(), sizeof(DomainHandle)));
  }

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

class FixedBucketFacetReq : public FacetReq {
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
    return FacetBucketBlockExecutor::BINDING_BYTES;
  }
  virtual size_t bucketDomainByteBudget() const {
    return FacetBucketBlockExecutor::DOMAIN_BYTES;
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

      FacetBucketBlockExecutor::execute<size_t>(
          *this, children, buckets, fixedOp().reader,
          [this](size_t segnum, auto chunk) { return bucketDomains(segnum, chunk); },
          fixedOp().bindingStateChunkBytes(), fixedOp().bucketDomainByteBudget(),
          [this]() { fixedOp().bindingBlockStarted(); });
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
          // Captured by value: through a by-reference capture the counts
          // store may alias the op's fences and the count vector itself, so
          // each value would reload them.
          [start, end, counts = std::span(data.counts), &range = rangeOp()]
          (int32_t docid, int64_t val) LUXIR_INLINE {
            unused(docid);
            if (val < start || val >= end) return;
            counts[range.bucketOf(val)]++;
          });
    }

    // Build one chunk of one segment's bucket domains in a value-column pass.
    // A multi-valued document can put two values in one bucket and
    // DocSetBuilder requires strictly increasing docids, so each builder skips
    // a repeat of the docid it just added.
    std::vector<DomainHandle> bucketDomains(
        size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> buckets) override {
      std::vector<int32_t> builderOfBucket(rangeOp().bucketCount(), -1);
      for (size_t i = 0; i < buckets.size(); i++) {
        builderOfBucket[buckets[i].key] = (int32_t)i;
      }
      int64_t start = rangeOp().fences.front();
      int64_t end = rangeOp().fences.back();
      return buildFacetBucketDomains(
          rangeOp().reader.segments()[segnum].maxDoc(), buckets.size(),
          [&](auto&& accept) {
            SegFieldInfo info;
            int64_t missing = 0;
            rangeOp().facetSegIntCol(input[segnum].get(), (int32_t)segnum,
                                     missing, info, accept);
          }, [&](int64_t value) {
            return value < start || value >= end
                ? -1 : builderOfBucket[rangeOp().bucketOf(value)];
          });
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
