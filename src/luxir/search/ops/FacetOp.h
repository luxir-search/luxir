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
#include "DistinctFacetCounts.h"
#include "SearchOp.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/DocsReader.h"
#include <limits>

#include "luxir/reader/IntColReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/reader/PointsReader.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/OrdMapStr.h"
#include "luxir/search/RangeFacetGrid.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/util/AtomicMerger.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/SegmentMergeDriver.h"
#include "luxir/search/ops/DomainIter.h"

namespace luxir {



class FacetReq : public SearchOp {
public:
  bool needsCompleteBucketInput() const override {
    // Facets open result children after their last segment feed. A nested
    // reset must still be able to recover its ancestors on earlier segments.
    return (needsStructuralScope && !subOps.empty()) || SearchOp::needsCompleteBucketInput();
  }

  const CollapsePlan* countDistinct = nullptr;
  std::shared_ptr<const FacetGroupPlan> groupPlan;
  // RUN requires every group's matching docs to form one run in exactly one
  // segment. Index sorting alone is insufficient: it also needs ownership or
  // a key-ordered merge across segments.
  static constexpr bool isContiguous = false;
  FacetDimension groupDimension() const { return groupPlan->dimension; }

  template <bool Growing = false, typename Visit>
  void collectGroups(DistinctFacetCounts& data, int32_t segnum, DocSet* domain, Visit&& visit) {
    auto pool = MemPool::threadLocalPoolGuard();
    CollapseKeyReader keys(*countDistinct);
    keys.withWalk(segnum, domain, [&](auto read) {
      auto collect = [&](auto add, auto missing) {
        if constexpr (std::is_same_v<std::invoke_result_t<decltype(read), int32_t>, const CollapseKey*>) {
          int32_t previous = -1;
          GroupIdentity saved{GroupIdentity::DROP};
          visit([&](int32_t doc) {
            if (doc != previous) { saved = data.identity(*groupPlan, read(doc)); previous = doc; }
            return saved;
          }, add, missing);
        } else visit(read, add, missing);
      };
      if constexpr (Growing) data.collectValues(collect);
      else data.collect(collect);
    });
  }
  IndexReader& reader;
  std::string_view fieldName;
  int64_t limit;
  int64_t minCount; // minimum count for a facet to be included in the result
  bool missing;

  std::string_view facetName;
  IndexReader::FieldSegments segInfos;

  const SegFieldInfo* fieldInfo(int32_t segnum) const {
    return segInfos.at((size_t)segnum);
  }

  FacetReq(SearchRequest& req, std::string_view fieldName, std::string_view facetName,
    int64_t limit, int64_t minCount, bool missing)
    : SearchOp(req, facetName), reader(*req.reader), fieldName(fieldName), limit(limit), minCount(minCount), missing(missing),
      facetName(facetName), segInfos(fieldName.empty() ? IndexReader::FieldSegments{} : reader.field(fieldName)) {
  }

  virtual ~FacetReq() = default;

  size_t distinctBindingBytes(FacetDimension values) const {
    if (DistinctFacetCounts::select(values, groupDimension(), isContiguous) == DistinctFacetMode::PAGED) {
      uint64_t groups = groupDimension().size;
      uint64_t words = groups / 64 + (groups % 64 != 0);
      return saturatingMultiply((size_t)(values.size + 1),
          (size_t)(words * 8 + (words / 64 + (words % 64 != 0) + 1) * sizeof(uint64_t*)));
    }
    // HASH grows with this bucket's touched pairs, just as a document facet's
    // hash grows with its touched values. Do not price the entire index into
    // every child binding; actual growth is enforced by the request breaker.
    return 0;
  }

  // Distinct missing counts need document identities, not a scalar missing
  // total. The column presence set is independent of bucket/range predicates.
  void forEachMissing(DocSet* domain, int32_t segnum, auto&& accept) {
    auto& postings = reader.segments()[(size_t)segnum].postingsReader();
    auto* info = fieldInfo(segnum);
    if (info == nullptr) { forEachDomainDoc(domain, postings.maxDoc(), accept); return; }
    DocsReader docs(postings, *info);
    if (!docs.hasBitset()) return;
    screaming::BitSet::Iterator values(docs.bitset());
    forEachDomainDoc(domain, postings.maxDoc(), [&](int32_t doc) {
      if (values.val() < doc) values.advance(doc);
      if (values.val() != doc) accept(doc);
    });
  }

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

  // The points format here is the same one-dimensional numeric format used
  // by range counts. Other points shapes must use the column predicate.
  static bool numericIdentityPoints(const SegFieldInfo& info) {
    return info.pointsMetaOff != 0 && (info.type == FieldType::INT
        || info.type == FieldType::FLOAT || info.type == FieldType::DOUBLE
        || info.type == FieldType::DATE);
  }

  // Compare a block's shared column pass with its points fences before
  // constructing one predicate supplier per bucket.
  bool numericIdentitiesLead(size_t segment, const SegFieldInfo& info, DocSet* eligibility,
      size_t count, std::function_ref<PointsReader::ValueRange(size_t)> bounds);

  // Uncached predicate execution owns the indexed representation choice.
  DomainHandle numericPredicateIdentity(size_t segment, int64_t lo, int64_t hi,
                                       DocSet* eligibility);

  // Open the segment column. An absent field adds the
  // whole domain to missing_num; otherwise the visitor owns the column walk.
  bool withIntColumn(DocSet* domain, int32_t segnum, int64_t& missing_num,
                     auto&& visitColumn) {
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto* info = fieldInfo(segnum);
    bool found = info != nullptr;
    if (!found) {
      // field absent in this segment: every in-domain doc is missing.
      missing_num += domain ? domain->card() : maxDoc;
      return false;
    }
    const auto& segFieldInfo = *info;
    IntColReader intColReader(postingsReader, segFieldInfo);
    visitColumn(intColReader, maxDoc);
    return true;
  }

  // utility template method that calls callback with (int32 docid, int64_t value) for each doc in the domain that has
  // a value in the int column field (single or multi-valued).
  // missing is an out parameter that is incremented for every domain doc that does not have the field.
  // The value returned is if the field exists in the segment.
  bool facetSegIntCol(DocSet* domain, int32_t segnum, int64_t& missing_num, auto&& callback) {
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto* info = fieldInfo(segnum);
    bool found = info != nullptr;
    if (!found) {
      // field absent in this segment: every in-domain doc is missing.
      missing_num += domain ? domain->card() : maxDoc;
      return false;
    }
    const auto& segFieldInfo = *info;
    // Walk the domain over the int column.  forEachIntColValue owns the
    // domain-type x single/multi x dense/sparse dispatch (and the one
    // (BitDocSet*)/(ArrDocSet*) cast); callback is per value.
    IntColReader intColReader(postingsReader, segFieldInfo);
    forEachIntColValue(domain, intColReader, maxDoc, missing_num, callback);
    return true;
  }

  // callback(docid, ord) per value, ord 0-based in `ordSpace`
  // (forEachOrdValue).
  bool facetSegOrdCol(DocSet* domain, int32_t segnum, int64_t& missing_num,
                      const OrdMap::SegToGlobal& ordSpace, auto&& callback) {
    auto& postingsReader = reader.segments()[segnum].postingsReader();
    int32_t maxDoc = postingsReader.maxDoc();
    auto* info = fieldInfo(segnum);
    if (!info) {
      missing_num += domain ? domain->card() : maxDoc;
      return false;
    }
    const auto& segFieldInfo = *info;
    OrdColReader ordColReader(postingsReader, segFieldInfo);
    forEachOrdValue(domain, ordColReader, ordSpace, maxDoc, missing_num,
                    callback);
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
      if (subOp.second->canInlineInBucket()) {
        inlineSubOps.push_back(subOp);
        moved.push_back(subOp.first);
      }
    }
    for (auto& name : moved) subOps.erase(name);
  }

public:
  void init() override {
    FacetReq::init();
    if (countDistinct != nullptr) {
      // Distinct counting feeds result children with ordinary bucket document
      // domains. Metrics fold those documents, never one witness per key.
      if (!fieldFacet.sort.empty()) {
        throw RequestError("facet '" + std::string(facetName)
            + "': metric sort with count_distinct is not supported yet");
      }
      return;
    }
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
        if (!iter->second->canInlineInBucket()) {
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

    template <bool Identity = false>
    DomainHandle materializeTermDomain(
        int32_t segnum, std::string_view term, DocSet* inputDomain) {
      auto& postingsReader =
          fieldOp().reader.segments()[(size_t)segnum].postingsReader();
      int32_t maxDoc = postingsReader.maxDoc();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      std::unique_ptr<DocSet> domain;
      if (auto* info = fieldOp().fieldInfo(segnum)) {
        const auto& segFieldInfo = *info;
        TermsEnum terms(poolGuard.pool(), postingsReader, segFieldInfo);
        if (terms.seek(term)) {
          int32_t docFreq = terms.docFreq();
          DocsOnlyEnum postings(terms);
          if constexpr (Identity) domain = materializeIdentityPostingsIntersection(
              postings, docFreq, inputDomain, maxDoc);
          else domain = materializePostingsIntersection(postings, docFreq, inputDomain, maxDoc);
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

    template <typename Key>
    void identityDomains(size_t segnum, std::span<const SelectedFacetBucket<Key>> buckets,
                         DomainHandle eligibility, FacetBucketFeed feed,
                         const std::shared_ptr<OrdMap>& ordMap = {}) {
      auto& facet = fieldOp();
      auto& postings = facet.reader.segments()[segnum].postingsReader();
      auto* info = facet.fieldInfo((int32_t)segnum);
      bool identityLeads = false;
      if (info) {
        if constexpr (std::is_same_v<Key, std::string_view>) {
          // materializePostingsIntersection chooses postings or its sparse
          // eligibility as the lead, using docFreq and eligibility cardinality.
          identityLeads = (info->flags & FieldType::INDEX_DOCS) != 0;
        } else {
          identityLeads = numericIdentityPoints(*info)
              && facet.numericIdentitiesLead(segnum, *info, eligibility.get(), buckets.size(),
                  [&](size_t i) -> PointsReader::ValueRange {
                    return {buckets[i].key, buckets[i].key};
                  });
        }
      }
      if (identityLeads) {
        for (size_t i = 0; i < buckets.size(); i++) {
          if constexpr (std::is_same_v<Key, std::string_view>) {
            feed(i, materializeTermDomain<true>((int32_t)segnum, buckets[i].key, eligibility.get()));
          } else {
            feed(i, facet.numericPredicateIdentity(
                segnum, buckets[i].key, buckets[i].key, eligibility.get()));
          }
        }
        return;
      }
      feedFacetBucketDomainChunks(postings.maxDoc(), buckets,
          forcedFacetBucketDomainByteBudget ? forcedFacetBucketDomainByteBudget
              : FACET_BUCKET_DOMAIN_BYTES, feed,
          [&](auto chunk) {
            boost::unordered_flat_map<int64_t, int32_t> owner;
            for (size_t i = 0; i < chunk.size(); i++) {
              if constexpr (std::is_same_v<Key, std::string_view>) {
                if (chunk[i].id) owner.emplace(chunk[i].id->value, (int32_t)i);
              } else owner.emplace(chunk[i].key, (int32_t)i);
            }
            return buildFacetBucketDomains(postings.maxDoc(), chunk.size(), [&](auto&& accept) {
              int64_t missing = 0;
              if constexpr (std::is_same_v<Key, std::string_view>) {
                assert(ordMap || !info);
                if (ordMap) facet.facetSegOrdCol(eligibility.get(), (int32_t)segnum, missing,
                    ordMap->getSegToGlobal((int32_t)segnum), accept);
              } else facet.facetSegIntCol(eligibility.get(), (int32_t)segnum, missing, accept);
            }, [&](int64_t value) {
              auto found = owner.find(value);
              return found == owner.end() ? -1 : found->second;
            });
          });
    }

    template <typename Key, typename DomainSource>
    void executeBucketChildren(std::span<const SelectedFacetBucket<Key>> buckets,
                               FacetBucketProducer producer,
                               std::span<const DomainHandle> input, DomainSource&& source,
                               const std::shared_ptr<OrdMap>& ordMap = {}) {
      if (fieldOp().subOps.empty()) return;
      std::vector<SearchOp*> children;
      children.reserve(fieldOp().subOps.size());
      for (auto& [name, child] : fieldOp().subOps) children.push_back(child);
      FacetBucketBlockExecutor::execute<Key>(
          *this, children, buckets, fieldOp().reader, producer, source,
          FacetBucketBlockExecutor::BINDING_BYTES,
          input, [this, &ordMap](size_t segnum, auto block, DomainHandle eligibility,
                               FacetBucketFeed feed) {
            identityDomains(segnum, block, std::move(eligibility), feed, ordMap);
          });
    }

    template <typename Key, typename Buckets, typename KeyAt, typename IdAt>
    static auto selectedBuckets(
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

    template <typename Enumerate, typename Lookup>
    auto finishCountBuckets(Enumerate&& enumerate,
                            std::span<const std::optional<int64_t>> pins, Lookup&& count) {
      FieldBucketFinalizer<int64_t, std::monostate, decltype(countFieldBucketOrder)> finalizer(
          fieldOp().minCount, 0, fieldOp().limit, countFieldBucketOrder);
      enumerate(finalizer);
      auto finalized = finalizer.finish();
      std::vector<PinnedBucketValue<>> pinValues;
      for (auto key : pins) pinValues.push_back({key ? count(*key) : 0, {}});
      mergePinnedBuckets<int64_t, std::monostate>(finalized, pins, pinValues);
      return finalized;
    }

    // Count-only document and distinct facets share labels, output and child
    // routing. Their counter enumeration/lookup and domain producers differ.
    template <typename Label, typename ResolveLabel, typename ExecuteChildren>
    void emitCountPage(const std::vector<FinalizedFacetBucket<int64_t>>& finalized,
                       int64_t missing, ResolveLabel&& label, ExecuteChildren&& executeChildren) {
      std::vector<std::pair<Label, int64_t>> emitted;
      emitted.reserve(finalized.size());
      for (const auto& bucket : finalized) emitted.emplace_back(label(bucket), bucket.count);
      auto& out = *slotArm<luxir::api::FacetResult>(nullptr);
      emitBuckets(out, emitted, op.req.lastResponse->mr);
      if (fieldOp().missing) out.missing = missing;
      using ChildKey = std::conditional_t<std::is_same_v<Label, std::string>, std::string_view, Label>;
      auto buckets = selectedBuckets<ChildKey>(finalized,
          [&](size_t i) -> ChildKey { return emitted[i].first; },
          [&](size_t i) -> std::optional<FacetBucketId> {
            if constexpr (std::is_same_v<Label, int64_t>) return FacetBucketId{emitted[i].first};
            else return finalized[i].key ? std::optional(FacetBucketId{*finalized[i].key}) : std::nullopt;
          });
      executeChildren(buckets);
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

  // A measure-selected count binding, sharing column readers, bucket
  // finalization and child-domain execution with document facets. The ordinary
  // calculators retain their existing loops and additive reduction.
  template <bool Ordinal>
  class DistinctCalc : public Calc {
    std::shared_ptr<OrdMap> ordMap;
    SegmentMergeDriver<DistinctFacetCounts> driver;
    FacetCountResource domainResource;
    std::pmr::vector<DomainHandle> input;

    FacetDimension valueDimension;
    DistinctFacetMode mode = DistinctFacetMode::AUTO;
    void collect(DistinctFacetCounts& data, int32_t segnum, DocSet* domain) {
      auto& facet = this->fieldOp();
      auto pool = MemPool::threadLocalPoolGuard();
      auto& postings = facet.reader.segments()[(size_t)segnum].postingsReader();
      auto* info = facet.fieldInfo(segnum);
      int64_t missing = 0;
      facet.collectGroups(data, segnum, domain, [&](auto group, auto add, auto) {
        if (!info) { missing = domain ? domain->card() : postings.maxDoc(); return; }
        if constexpr (Ordinal) {
          OrdColReader column(postings, *info);
          forEachOrdValue(domain, column, ordMap->getSegToGlobal(segnum), postings.maxDoc(), missing,
              [&](int32_t doc, int64_t value) LUXIR_INLINE { add(value, group(doc)); });
        } else {
          IntColReader column(postings, *info);
          forEachIntColValue(domain, column, postings.maxDoc(), missing,
              [&](int32_t doc, int64_t value) LUXIR_INLINE { add(value, group(doc)); });
        }
      });
      if (facet.missing && missing != 0) {
        facet.collectGroups(data, segnum, domain, [&](auto group, auto, auto missingBucket) {
          facet.forEachMissing(domain, segnum,
              [&](int32_t doc) { missingBucket(group(doc)); });
        });
      }
    }

    void result(std::unique_ptr<DistinctFacetCounts> data) {
      auto& facet = this->fieldOp();
      data->finish();
      auto pool = MemPool::threadLocalPoolGuard();
      std::optional<OrdMapStr> labels;
      if constexpr (Ordinal) labels.emplace(pool.pool(), ordMap.get(), facet.reader, facet.segInfos);
      std::vector<std::optional<int64_t>> pins;
      if constexpr (Ordinal) {
        for (std::string_view selected : facet.selectedStrings) {
          pins.push_back(labels->termStats(selected).globalOrd);
        }
      } else for (int64_t selected : facet.selectedInts) pins.push_back(selected);
      auto finalized = this->finishCountBuckets([&](auto& finalizer) {
        for (auto [value, count] : data->counts) if (count > 0) finalizer.add({value, count, {}});
        if constexpr (Ordinal) if (facet.minCount == 0) {
          for (uint64_t value = 0; value < valueDimension.size; value++) {
            if (data->count((int64_t)value) == 0) finalizer.add({(int64_t)value, 0, {}});
          }
        }
      }, pins, [&](int64_t key) { return data->count(key); });
      int64_t missingCount = data->missing();
      data.reset();
      auto children = [&](auto& buckets) {
        using Key = std::conditional_t<Ordinal, std::string_view, int64_t>;
        this->template executeBucketChildren<Key>(buckets, FacetBucketProducer::JOINT, input,
          [&](size_t segnum, auto block, FacetBucketFeed feed) {
            feedFacetBucketDomainChunks(facet.reader.segments()[segnum].maxDoc(), block,
                FACET_BUCKET_DOMAIN_BYTES, feed, [&](auto chunk) {
                  boost::unordered_flat_map<int64_t, int32_t> bucketOf;
                  for (size_t i = 0; i < chunk.size(); i++) {
                    if (chunk[i].id) bucketOf.emplace(chunk[i].id->value, (int32_t)i);
                  }
                  return buildFacetBucketDomains(facet.reader.segments()[segnum].maxDoc(), chunk.size(),
                      [&](auto&& accept) {
                        int64_t missing = 0;
                        if constexpr (Ordinal) facet.facetSegOrdCol(input[segnum].get(), (int32_t)segnum,
                            missing, ordMap->getSegToGlobal((int32_t)segnum), accept);
                        else facet.facetSegIntCol(input[segnum].get(), (int32_t)segnum, missing, accept);
                      }, [&](int64_t value) {
                        auto it = bucketOf.find(value);
                        return it == bucketOf.end() ? -1 : it->second;
                      });
                });
          }, ordMap);
      };
      if constexpr (Ordinal) this->template emitCountPage<std::string>(finalized, missingCount,
          [&](const auto& bucket) { return std::string(bucket.pinned() ? facet.selectedStrings[bucket.pinIndex]
              : labels->ordToStr(*bucket.key)); }, children);
      else this->template emitCountPage<int64_t>(finalized, missingCount,
          [](const auto& bucket) { return *bucket.key; }, children);
    }

  public:
    DistinctCalc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
                 std::shared_ptr<OrdMap> ordMap = {}, FacetDimension values = {},
                 const StructuralConstraint* structural = nullptr)
        : Calc(op, parent, slot, numSlots, structural), ordMap(std::move(ordMap)),
          driver(op.req.reader->segments().size(),
              [this](std::unique_ptr<DistinctFacetCounts> data) { result(std::move(data)); }),
          domainResource(op.req.memoryTracker, std::format("facet '{}' domains", op.name)), input(&domainResource) {
      driver.setCreator([this]() { return new DistinctFacetCounts(this->op.req.memoryTracker,
          std::format("facet '{}'", this->op.name), mode, valueDimension, this->fieldOp().groupDimension(), FacetReq::isContiguous); });
      if constexpr (Ordinal) valueDimension = {true, 0, (uint64_t)(this->ordMap ? this->ordMap->numOrds() : 0)};
      else valueDimension = values;
      mode = DistinctFacetCounts::select(valueDimension, this->fieldOp().groupDimension(), FacetReq::isContiguous);
      if (!this->fieldOp().subOps.empty()) input.resize(op.req.reader->segments().size());
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DomainHandle domain) override {
      unused(tg);
      if (segnum == -1) { driver.completeEmpty(); return; }
      DocSet* docs = domain.get();
      if (!input.empty()) input[(size_t)segnum] = std::move(domain);
      driver.contribute([&](DistinctFacetCounts& data) { collect(data, segnum, docs); });
    }


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
    using IntHash = BigFlatMap<int64_t, int64_t>;
    using CountVector = BigVector<int64_t>;
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

  IntFacetReq(SearchRequest& req, const ReqFieldFacet& fieldFacet, std::string_view fieldName,
    std::string_view facetName, int64_t limit, int64_t minCount, bool missing,
    std::span<const int64_t> selected) :
  FieldFacetReq(req, fieldFacet, fieldName, facetName, limit, minCount, missing,
                selected, {}) {
    auto range = scanGlobalRange(reader, segInfos);
    globalMin = range.min;
    globalMax = range.max;
    useVector = range.useVector;
  }

  FacetDimension valueDimension() const {
    return {useVector, globalMin, useVector ? (uint64_t)globalMax - (uint64_t)globalMin + 1 : 0};
  }

  bool canEmitAsBucketChild() const override {
    return true;
  }

  // Scan every segment for the column's min/max and decide vector vs map
  // storage based on the resulting range.
  static GlobalRange scanGlobalRange(IndexReader& reader, IndexReader::FieldSegments infos) {
    GlobalRange r;
    for (size_t segnum = 0; segnum < reader.segments().size(); segnum++) {
      auto* info = infos.at(segnum);
      if (!info) continue;
      const auto& segFieldInfo = *info;
      if (segFieldInfo.numValues == 0) continue;

      assert(info->columnBounds);
      r.min = std::min(r.min, info->columnBounds->min);
      r.max = std::max(r.max, info->columnBounds->max);
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
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : FieldFacetReq::Calc(op, parent, slot, numSlots, structural),
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
          thisOp().withIntColumn(domain, segnum, data.missing_num,
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
          facetReq.facetSegIntCol(domain, segnum, data.missing_num,
            [countVec, minVal](int32_t docid, int64_t val) LUXIR_INLINE {
              unused(docid);
              (*countVec)[val - minVal]++;
            });
        } else {
          auto* countMap = &std::get<MergeableIntFacet::IntHash>(data.counts);
          facetReq.facetSegIntCol(domain, segnum, data.missing_num,
            [countMap](int32_t docid, int64_t val) LUXIR_INLINE {
              unused(docid);
              (*countMap)[val]++;
            });
        }
      });
    };

    void facetResult(MergeableIntFacet& merged) {
      auto lookup = [&](int64_t selected) -> int64_t {
        if (auto* counts = std::get_if<MergeableIntFacet::IntHash>(&merged.counts)) {
          auto it = counts->find(selected);
          return it == counts->end() ? 0 : it->second;
        }
        if (auto* counts = std::get_if<MergeableIntFacet::CountVector>(&merged.counts)) {
          uint64_t index = (uint64_t)selected - (uint64_t)merged.minValue;
          return index < counts->size() ? (*counts)[index] : 0;
        }
        return 0;
      };
      std::vector<std::optional<int64_t>> pins;
      for (int64_t selected : thisOp().selectedInts) pins.push_back(selected);
      auto finalized = finishCountBuckets([&](auto& finalizer) {
        if (auto* counts = std::get_if<MergeableIntFacet::IntHash>(&merged.counts)) {
          for (auto [value, count] : *counts) finalizer.add({value, count, {}});
        } else if (auto* counts = std::get_if<MergeableIntFacet::CountVector>(&merged.counts)) {
          for (size_t i = 0; i < counts->size(); i++) if ((*counts)[i] > 0) {
            finalizer.add({(int64_t)((uint64_t)merged.minValue + i), (*counts)[i], {}});
          }
        }
      }, pins, lookup);
      emitCountPage<int64_t>(finalized, merged.missing_num,
          [](const auto& bucket) { return *bucket.key; },
          [this](auto& buckets) { executeResultChildren(buckets); });
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
        size_t segnum, std::span<const SelectedFacetBucket<int64_t>> buckets, DocSet* eligibility) {
      boost::unordered_flat_map<int64_t, int32_t> builderOfValue;
      builderOfValue.reserve(buckets.size());
      for (size_t i = 0; i < buckets.size(); i++) {
        builderOfValue.emplace(buckets[i].key, (int32_t)i);
      }
      return buildFacetBucketDomains(
          thisOp().reader.segments()[segnum].maxDoc(), buckets.size(),
          [&](auto&& accept) {
            int64_t missing = 0;
            thisOp().facetSegIntCol(eligibility, (int32_t)segnum,
                                    missing, accept);
          }, [&](int64_t value) {
            auto it = builderOfValue.find(value);
            return it == builderOfValue.end() ? -1 : it->second;
          });
    }

    void executeResultChildren(std::span<const SelectedFacetBucket<int64_t>> buckets) {
      executeBucketChildren<int64_t>(buckets, FacetBucketProducer::JOINT, input,
          [this](size_t segnum, auto block, FacetBucketFeed feed) {
            feedFacetBucketDomainChunks(
                thisOp().reader.segments()[segnum].maxDoc(), block,
                FACET_BUCKET_DOMAIN_BYTES, feed,
                [&](auto chunk) { return bucketDomains(segnum, chunk, input[segnum].get()); });
          });
    }

  };

  size_t facetBucketResidentBytes() const override {
    if (countDistinct != nullptr) {
      return saturatingAdd(sizeof(DistinctCalc<false>), distinctBindingBytes(valueDimension()));
    }
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

  Calculator* createCalculator(
      Calculator* parent, int64_t slot, int64_t numSlots = -1,
      const StructuralConstraint* structural = nullptr) override {
    if (countDistinct != nullptr) return new DistinctCalc<false>(
        *this, parent, slot, numSlots, {}, valueDimension(), structural);
    return new Calc(*this, parent, slot, numSlots, structural);
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
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : FieldFacetReq::Calc(op, parent, slot, numSlots, structural),
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
        auto& postingsReader = thisOp().reader.segments()[segnum].postingsReader();
        int32_t maxDoc = postingsReader.maxDoc();
        DomainView domainView(domain, maxDoc);
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto* info = thisOp().fieldInfo(segnum);
        if (!info) {
          // field absent in this segment: all in-domain docs are missing
          data.missing_num += domainView.card;
          return;
        }
        const auto& segFieldInfo = *info;
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
        auto* field = thisOp().fieldInfo(segnum);
        if (!field) {
          data.missing_num += view.card;
          return;
        }
        const auto& info = *field;
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
          FacetBucketProducer::INDEPENDENT, input,
          [this](size_t segnum, auto block, FacetBucketFeed feed) {
            for (size_t i = 0; i < block.size(); i++) {
              feed(i, materializeTermDomain((int32_t)segnum, block[i].key,
                                            input[segnum].get()));
            }
          });
    }

    void facetResult(MergeableStrFacet& merged) {
      auto& mr = op.req.lastResponse->mr;  // arena for this leaf result
      auto& facetResultProto = *slotArm<luxir::api::FacetResult>(nullptr);
      auto minCount = thisOp().minCount;
      auto limit = thisOp().limit;
      auto missing = thisOp().missing;

      // TODO: stream into FieldBucketFinalizer instead of collecting every
      // counted value (see finalizeFieldBuckets in FacetEmit.h).
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

  Calculator* createCalculator(
      Calculator* parent, int64_t slot, int64_t numSlots = -1,
      const StructuralConstraint* structural = nullptr) override {
    return new Calc(*this, parent, slot, numSlots, structural);
  }
};

class FixedBucketFacetReq : public FacetReq {
  size_t numBuckets;
  std::span<const size_t> selectedBuckets;

public:
  struct CountView { std::span<const int64_t> counts; int64_t missing_num; };
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
  size_t facetBucketResidentBytes() const override {
    size_t bytes = countDistinct == nullptr
        ? saturatingAdd(sizeof(Calc<false>) + sizeof(MergeableFixedBuckets),
            saturatingMultiply(numBuckets, sizeof(int64_t)))
        : saturatingAdd(sizeof(Calc<true>) + sizeof(DistinctFacetCounts),
            distinctBindingBytes({true, 0, (uint64_t)numBuckets}));
    if (!subOps.empty() || requiresWholeReaderDomain()) {
      bytes = saturatingAdd(bytes, FacetBucketBlockExecutor::inputDomainBytes(reader).wholeReader);
    }
    return bytes;
  }
  std::span<const size_t> selectedBucketIndexes() const {
    return selectedBuckets;
  }

  bool canEmitAsBucketChild() const override { return true; }

  virtual size_t bindingStateChunkBytes() const {
    return FacetBucketBlockExecutor::BINDING_BYTES;
  }
  virtual FacetBucketProducer bucketProducer() const = 0;

  virtual bool requiresWholeReaderDomain() const { return false; }

  virtual std::vector<size_t> emittedBuckets(
      CountView merged,
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
      CountView merged,
      std::span<const size_t> emitted,
      std::pmr::memory_resource& mr) const = 0;

  // Storage and finalized layout are independent of the shared segment and
  // result-child lifecycle. The fixed policy is empty and keeps its old data.
  template <bool Distinct>
  struct FixedStorage {
    using Data = std::conditional_t<Distinct, DistinctFacetCounts, MergeableFixedBuckets>;
    static constexpr bool growing = false;
    explicit FixedStorage(SearchOp&) {}
    static void initialize(FixedBucketFacetReq& op, Data& data) {
      if constexpr (!Distinct) if (data.counts.empty()) data.counts.assign(op.bucketCount(), 0);
    }
    static Data* create(FixedBucketFacetReq& op, DistinctFacetMode mode) {
      if constexpr (Distinct) return new Data(op.req.memoryTracker,
          std::format("facet '{}'", op.name), mode,
          FacetDimension{true, 0, (uint64_t)op.bucketCount()}, op.groupDimension(), FacetReq::isContiguous);
      else return new Data;
    }
    template <typename Emit>
    static void finish(FixedBucketFacetReq& op, Data& data, Emit&& emit) {
      if constexpr (Distinct) {
        data.finish();
        std::vector<int64_t> counts(op.bucketCount());
        for (size_t i = 0; i < counts.size(); i++) counts[i] = data.count((int64_t)i);
        emit(CountView{counts, data.missing()});
      } else emit(CountView{data.counts, data.missing_num});
    }
    static size_t bucketCount(const FixedBucketFacetReq& op) { return op.bucketCount(); }
    static std::vector<uint8_t> pinned(const FixedBucketFacetReq& op) { return std::vector<uint8_t>(op.bucketCount()); }
    static std::vector<size_t> emitted(FixedBucketFacetReq& op, CountView counts, std::span<const uint8_t> pinned) {
      return op.emittedBuckets(counts, pinned);
    }
    static void emit(FixedBucketFacetReq& op, api::FacetResult& result, CountView counts,
                     std::span<const size_t> emitted, std::pmr::memory_resource& mr) {
      op.emitBucketResult(result, counts, emitted, mr);
    }
  };

  virtual void checkEmittedBucketCount(size_t) const {}

  template <bool Distinct = false, typename Storage = FixedStorage<Distinct>>
  class Calc : public Calculator {
  public:
    using Data = typename Storage::Data;
  private:
    SegmentMergeDriver<Data> driver;
    DistinctFacetMode mode = DistinctFacetMode::AUTO;
    std::atomic<int32_t> gatheredDomainsSeen{0};

  protected:
    // Per-segment incoming domains, kept only while something later reads
    // them: the whole-reader gather, and result-stage sub-op execution, where
    // the producer intersects them with each bucket.
    std::vector<DomainHandle> input;

    [[no_unique_address]] Storage storage;

    FixedBucketFacetReq& fixedOp() {
      return (FixedBucketFacetReq&)getOp();
    }

    virtual void collectSegment(
        Data& data, int32_t segnum, DocSet* domain) = 0;
    // The FacetBucketBlockExecutor domain source for result children.
    virtual void feedBucketDomains(
        size_t segnum, std::span<const SelectedFacetBucket<size_t>> block,
        FacetBucketFeed feed) = 0;
    virtual void feedIdentityDomains(size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> block, DomainHandle eligibility,
        FacetBucketFeed feed) = 0;
    virtual Query::Weight* bucketSelection(size_t) { return nullptr; }
    virtual void prepareGatheredDomains(oneapi::tbb::task_group* tg) {
      unused(tg);
    }

    void collectInputSegment(int32_t segnum, DocSet* domain) {
      driver.contribute([&](Data& data) {
        storage.initialize(fixedOp(), data);
        collectSegment(data, segnum, domain);
        if constexpr (Storage::growing) storage.observe(data);
      });
    }

    void prepareAndDispatch(oneapi::tbb::task_group* tg) {
      prepareGatheredDomains(tg);
      task_group_fanout(tg, input.size(), [&](size_t segnum) {
        return [this, segnum, domain = input[segnum]]() {
          collectInputSegment((int32_t)segnum, domain.get());
        };
      });
    }

    void facetResult(CountView merged) {
      auto& mr = op.req.lastResponse->mr;

      auto pinned = storage.pinned(fixedOp());
      for (size_t bucket : fixedOp().selectedBucketIndexes()) {
        pinned[bucket] = 1;
      }
      auto emitted = storage.emitted(fixedOp(), merged, pinned);
      fixedOp().checkEmittedBucketCount(emitted.size());
      auto* result = slotArm<luxir::api::FacetResult>(nullptr);
      storage.emit(fixedOp(), *result, merged, emitted, mr);
      executeResultChildren(merged, emitted, pinned);
    }

    // Post-selection sub-op execution sizes each bucket block from the child
    // plans' retained bytes and feeds every segment; a bucket's bindings close
    // after its last segment. The subclass producer decides how many of a
    // segment's bucket domains are alive at once.
    void executeResultChildren(
        CountView merged,
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
          fixedOp().bucketProducer(),
          [this](size_t segnum, auto block, FacetBucketFeed feed) {
            feedBucketDomains(segnum, block, feed);
          },
          fixedOp().bindingStateChunkBytes(),
          input, [this](size_t segnum, auto block, DomainHandle eligibility, FacetBucketFeed feed) {
            feedIdentityDomains(segnum, block, std::move(eligibility), feed);
          }, [this](const auto& bucket) { return bucketSelection(bucket.key); });
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : Calculator(op, parent, slot, numSlots, structural),
        driver(op.req.reader->segments().size(),
               [this](std::unique_ptr<Data> merged) {
                 if constexpr (Storage::growing) {
                   auto release = scope_guard([&]() noexcept { input.clear(); });
                   storage.finish(fixedOp(), *merged, [this](CountView counts) { facetResult(counts); });
                 } else storage.finish(fixedOp(), *merged, [this](CountView counts) { facetResult(counts); });
               }),
        input(), storage(op) {
      if constexpr (Distinct || Storage::growing) {
        driver.setCreator([this]() { return storage.create(fixedOp(), mode); });
        if constexpr (Distinct && !Storage::growing) {
          mode = DistinctFacetCounts::select({true, 0, (uint64_t)fixedOp().bucketCount()}, fixedOp().groupDimension(), FacetReq::isContiguous);
        }
      }
      if (!fixedOp().subOps.empty() || fixedOp().requiresWholeReaderDomain()) {
        input.resize(op.req.reader->segments().size());
      }
    }

    size_t facetBucketObservedBytes() const override {
      if constexpr (!Storage::growing) return this->op.facetBucketResidentBytes();
      else {
        size_t bytes = sizeof(*this) + storage.observed.load(std::memory_order_relaxed)
            + input.capacity() * sizeof(DomainHandle);
        bytes = saturatingAdd(bytes, storage.retainedInputs.load(std::memory_order_relaxed));
        return bytes;
      }
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
      DocSet* domain = domainHandle.get();
      if (!input.empty()) {
        if constexpr (Storage::growing) {
          if (domainHandle.ownsStorage()) domainHandle = DomainVariantPlan::account(
              std::move(domainHandle), op.req.memoryTracker, "retained range bucket inputs");
          domain = domainHandle.get();
          if (domain && domainHandle.isCharged()) storage.retainedInputs.fetch_add(
              domain->ramBytesUsed(), std::memory_order_relaxed);
        }
        input[(size_t)segnum] = std::move(domainHandle);
      }
      if (!fixedOp().requiresWholeReaderDomain()) {
        collectInputSegment(segnum, domain);
        return;
      }
      int32_t seen = gatheredDomainsSeen.fetch_add(
          1, std::memory_order_acq_rel) + 1;
      assert(seen <= (int32_t)input.size());
      if (seen == (int32_t)input.size()) prepareAndDispatch(tg);
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
      for ([[maybe_unused]] const DomainHandle& domain : input) {
        assert(domain.isDeliverable());
      }
      prepareAndDispatch(tg);
    }
  };
};

class IntFacetRangeReq : public FixedBucketFacetReq {
  std::span<const int64_t> fences;
  int64_t affineGap;
  FieldType::Type valueType;
  bool affine;
  std::optional<RangeFacetGrid> grid;
  std::optional<int64_t> wideningStart, wideningEnd;
  std::span<const RangeFacetRun> runs;

  template <bool Distinct> struct GrowingStorage;
  struct BucketLayout {
    std::span<const int64_t> fences;
    int64_t gap;
    std::span<const RangeFacetRun> runs;
    RangeFacetRunCursor cursor{};
    size_t bucketOf(int64_t value) {
      if (gap) return (size_t)(((uint64_t)value - (uint64_t)fences.front()) / (uint64_t)gap);
      return cursor.bucketOf(value, runs, fences);
    }
  };

  std::vector<DomainHandle> bucketDomains(size_t segnum,
      std::span<const SelectedFacetBucket<size_t>> buckets, DocSet* eligibility,
      BucketLayout layout) {
    std::vector<int32_t> builderOfBucket(layout.fences.size() - 1, -1);
    for (size_t i = 0; i < buckets.size(); i++) builderOfBucket[buckets[i].key] = (int32_t)i;
    int64_t start = layout.fences.front(), end = layout.fences.back();
    return buildFacetBucketDomains(reader.segments()[segnum].maxDoc(), buckets.size(),
        [&](auto&& accept) {
          int64_t missing = 0;
          facetSegIntCol(eligibility, (int32_t)segnum, missing, accept);
        }, [&](int64_t value) {
          return value < start || value >= end ? -1 : builderOfBucket[layout.bucketOf(value)];
        });
  }

  void feedRangeDomains(size_t segnum, std::span<const SelectedFacetBucket<size_t>> block,
      DomainHandle eligibility, FacetBucketFeed feed, BucketLayout layout, bool identity) {
    if (identity) if (auto* info = fieldInfo((int32_t)segnum)) {
      if (numericIdentityPoints(*info)
          && numericIdentitiesLead(segnum, *info, eligibility.get(), block.size(),
              [&](size_t i) -> PointsReader::ValueRange {
                size_t bucket = block[i].key;
                return {layout.fences[bucket], layout.fences[bucket + 1] - 1};
              })) {
        for (size_t i = 0; i < block.size(); i++) {
          size_t bucket = block[i].key;
          feed(i, numericPredicateIdentity(segnum, layout.fences[bucket],
              layout.fences[bucket + 1] - 1, eligibility.get()));
        }
        return;
      }
    }
    size_t budget = identity && forcedFacetBucketDomainByteBudget
        ? forcedFacetBucketDomainByteBudget : bucketDomainByteBudget();
    feedFacetBucketDomainChunks(reader.segments()[segnum].maxDoc(), block, budget, feed,
        [&](auto chunk) { return bucketDomains(segnum, chunk, eligibility.get(), layout); });
  }

  struct DataCounts;
  template <bool Distinct, bool Floating = false> class DataCalc;
  Calculator* createDataCalculator(Calculator* parent, int64_t slot, int64_t numSlots,
                                    const StructuralConstraint* structural);

  // Bucket index for an in-range value (caller checks [front, back)).
  size_t bucketOf(int64_t val) const {
    if (affine) {
      return (size_t)(((uint64_t)val - (uint64_t)fences.front())
                      / (uint64_t)affineGap);
    }
    return (size_t)(std::upper_bound(fences.begin(), fences.end(), val) - fences.begin() - 1);
  }
  LUXIR_INLINE size_t bucketOf(int64_t val, RangeFacetRunCursor& cursor) const {
    if (affine) return bucketOf(val);
    return cursor.bucketOf(val, runs);
  }

public:
  static inline bool disablePointsRangeFacetForTests = false;

  std::span<const int64_t> bucketFences() const { return fences; }

  IntFacetRangeReq(SearchRequest& req,
    std::string_view fieldName, std::string_view facetName,
    std::span<const int64_t> fences, bool affine, int64_t affineGap,
    FieldType::Type valueType, int64_t minCount, bool missing,
    std::span<const size_t> selectedBuckets, std::span<const RangeFacetRun> runs)
  : FixedBucketFacetReq(
        req, fieldName, facetName, minCount, missing,
        fences.size() - 1, selectedBuckets),
    fences(fences), affineGap(affineGap), valueType(valueType),
    affine(affine), runs(runs) {}

  IntFacetRangeReq(SearchRequest& req,
      std::string_view fieldName, std::string_view facetName,
      RangeFacetGrid grid, std::optional<int64_t> start, std::optional<int64_t> end,
      FieldType::Type valueType, int64_t minCount, bool missing)
    : FixedBucketFacetReq(req, fieldName, facetName, minCount, missing, 0, {}),
      affineGap(0), valueType(valueType), affine(false), grid(std::move(grid)),
      wideningStart(start), wideningEnd(end) {}

  size_t facetBucketResidentBytes() const override;
  bool facetBucketGrows() const override { return grid.has_value(); }

  size_t bindingStateChunkBytes() const override {
    return forcedRangeFacetBindingStateChunkBytes != 0
        ? forcedRangeFacetBindingStateChunkBytes
        : FixedBucketFacetReq::bindingStateChunkBytes();
  }

  size_t bucketDomainByteBudget() const {
    return forcedRangeFacetBucketDomainByteBudget != 0
        ? forcedRangeFacetBucketDomainByteBudget
        : FACET_BUCKET_DOMAIN_BYTES;
  }

  FacetBucketProducer bucketProducer() const override {
    return FacetBucketProducer::JOINT;
  }

  void checkEmittedBucketCount(size_t count) const override {
    if (!subOps.empty() && count > 1024) throw RequestError(
        "facet '" + std::string(name) + "': range facets with sub-ops are limited to 1024 buckets");
  }

  std::vector<size_t> emittedBuckets(
      CountView merged,
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
      CountView merged,
      std::span<const size_t> emitted,
      std::pmr::memory_resource& mr) const override {
    emitRangeResult(result, merged, emitted, mr, fences);
  }

  void emitRangeResult(
      luxir::api::FacetResult& result, CountView merged,
      std::span<const size_t> emitted, std::pmr::memory_resource& mr,
      std::span<const int64_t> layout) const {
    size_t n = emitted.size();
    auto emitBounds = [&]<typename Outer>(auto decode) {
      auto& bucketIds = result.bucket_ids.emplace().kind.emplace<Outer>();
      auto* pairs = build::allocArray(bucketIds.v, n, mr);
      int64_t* counts = build::allocArray(result.counts, n, mr);
      for (size_t i = 0; i < n; i++) {
        size_t bucket = emitted[i];
        auto* bounds = build::allocArray(pairs[i].v, 2, mr);
        bounds[0] = decode(layout[bucket]);
        bounds[1] = decode(layout[bucket + 1]);
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

  template <bool Distinct = false>
  class Calc : public FixedBucketFacetReq::Calc<Distinct> {
    using Base = FixedBucketFacetReq::Calc<Distinct>;
    IntFacetRangeReq& rangeOp() {
      return (IntFacetRangeReq&)this->getOp();
    }

    void collectSegment(
        typename Base::Data& data, int32_t segnum,
        DocSet* domain) override {
      if constexpr (Distinct) {
        auto& range = rangeOp();
        int64_t missing = 0;
        if (range.runs.empty()) {
          range.collectGroups(data, segnum, domain, [&](auto group, auto add, auto) {
            range.facetSegIntCol(domain, segnum, missing,
                [&](int32_t doc, int64_t value) LUXIR_INLINE {
                  if (value >= range.fences.front() && value < range.fences.back()) add((int64_t)range.bucketOf(value), group(doc));
                });
          });
        } else {
          RangeFacetRunCursor cursor;
          range.collectGroups(data, segnum, domain, [&](auto group, auto add, auto) {
            range.facetSegIntCol(domain, segnum, missing,
                [&](int32_t doc, int64_t value) LUXIR_INLINE {
                  if (value >= range.fences.front() && value < range.fences.back()) add((int64_t)range.bucketOf(value, cursor), group(doc));
                });
          });
        }
        if (range.missing && missing != 0) range.collectGroups(data, segnum, domain,
            [&](auto group, auto, auto missingBucket) {
              range.forEachMissing(domain, segnum,
                  [&](int32_t doc) { missingBucket(group(doc)); });
            });
      } else {
      int64_t start = rangeOp().fences.front();
      int64_t end = rangeOp().fences.back();
      auto& segment = rangeOp().reader.segments()[segnum];
      bool noSubOps = rangeOp().subOps.empty();
      if (!IntFacetRangeReq::disablePointsRangeFacetForTests
          && domain == nullptr && segment.liveDocs() == nullptr && noSubOps
          && start < end) {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        if (auto* info = rangeOp().fieldInfo(segnum)) {
          const auto& segFieldInfo = *info;
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
      // Select the lookup form once per segment. The plain closure retains
      // the original captures and generated affine loop; only run collectors
      // carry the mutable cursor. Both use the same lifecycle and producer.
      if (rangeOp().runs.empty()) {
        // Capture by value so the counter store cannot alias the captures.
        rangeOp().facetSegIntCol(domain, segnum, data.missing_num,
            [start, end, counts = std::span(data.counts), &range = rangeOp()]
              (int32_t docid, int64_t val) LUXIR_INLINE {
            unused(docid);
            if (val < start || val >= end) return;
            counts[range.bucketOf(val)]++;
          });
      } else {
        rangeOp().facetSegIntCol(domain, segnum, data.missing_num,
            [start, end, counts = std::span(data.counts), &range = rangeOp(), cursor = RangeFacetRunCursor{}]
              (int32_t docid, int64_t val) mutable LUXIR_INLINE {
            unused(docid);
            if (val < start || val >= end) return;
            counts[range.bucketOf(val, cursor)]++;
          });
      }
      }
    }

    void feedBucketDomains(size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> block, FacetBucketFeed feed) override {
      auto& range = rangeOp();
      range.feedRangeDomains(segnum, block, this->input[segnum], feed,
          {range.fences, range.affine ? range.affineGap : 0, range.runs}, false);
    }
    void feedIdentityDomains(size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> block, DomainHandle eligibility,
        FacetBucketFeed feed) override {
      auto& range = rangeOp();
      range.feedRangeDomains(segnum, block, std::move(eligibility), feed,
          {range.fences, range.affine ? range.affineGap : 0, range.runs}, true);
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : Base(op, parent, slot, numSlots, structural) {}
  };

  Calculator* createCalculator(
      Calculator* parent, int64_t slot, int64_t numSlots = -1,
      const StructuralConstraint* structural = nullptr) override {
    if (grid) return createDataCalculator(parent, slot, numSlots, structural);
    if (countDistinct != nullptr) return new Calc<true>(*this, parent, slot, numSlots, structural);
    return new Calc<false>(*this, parent, slot, numSlots, structural);
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

  void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
    if (segmentStateRetained(segment)) return;
    for (auto* weight : bucketWeights) weight->releaseSegmentState(segment);
    SearchOp::releaseSegmentState(segment);
  }

  bool facetBucketSharesSegmentState() const override {
    return std::ranges::any_of(bucketWeights, [](const auto* weight) {
             return weight->hasReleasableSegmentState();
           })
        || SearchOp::facetBucketSharesSegmentState();
  }

  FacetBucketProducer bucketProducer() const override {
    return FacetBucketProducer::INDEPENDENT;
  }

  bool needsCompleteBucketInput() const override {
    return preparedBuckets || FacetReq::needsCompleteBucketInput();
  }

  bool requiresWholeReaderDomain() const override {
    return preparedBuckets;
  }

  void emitBucketResult(
      luxir::api::FacetResult& result,
      CountView merged,
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

  template <bool Distinct = false>
  class Calc : public FixedBucketFacetReq::Calc<Distinct> {
    using Base = FixedBucketFacetReq::Calc<Distinct>;
    std::vector<QueryPrep::PreparedSource> preparedSources;
    std::unique_ptr<DomainVariantPlan::Reservation> preparedCharge;

    QueryFacetReq& queryOp() {
      return (QueryFacetReq&)this->getOp();
    }

    Query::SegmentSource& bucketSource(size_t bucket) {
      return preparedSources.empty()
          ? static_cast<Query::SegmentSource&>(
                *queryOp().bucketWeights[bucket])
          : preparedSources[bucket].segmentSource();
    }

    void releaseBucketState(IndexReader::Segment& segment, size_t bucket) noexcept {
      if (queryOp().segmentStateRetained(segment)) return;
      if (!preparedSources.empty()) preparedSources[bucket].releaseSegmentState(segment);
      queryOp().bucketWeights[bucket]->releaseSegmentState(segment);
    }

    DomainHandle materializeBucket(size_t segnum, size_t bucket, DocSet* domain) {
      auto& segment = queryOp().reader.segments()[segnum];
      return DomainHandle(QueryPrep::materialize(
          bucketSource(bucket), segment, domain,
          Query::SupplierExecutionMode::ORDINARY,
          QueryPrep::MaterializeMode::ORDINARY));
    }

    void prepareGatheredDomains(oneapi::tbb::task_group* tg) override {
      std::vector<DocSet*> domains;
      domains.reserve(this->input.size());
      for (const DomainHandle& domain : this->input) {
        domains.push_back(domain.get());
      }
      Query::Weight::PrepareContext context{
        *queryOp().req.reader,
        std::span<DocSet* const>(domains.data(), domains.size()),
        tg != nullptr, &queryOp().req};
      preparedSources = QueryPrep::prepareSources(
          queryOp().bucketWeights, context);
      assert(preparedSources.size() == queryOp().bucketCount());
      if (this->structural) {
        size_t bytes = preparedSources.capacity() * sizeof(QueryPrep::PreparedSource);
        for (const auto& source : preparedSources) {
          if (source.prepared) bytes += source.prepared->retainedBytes();
        }
        preparedCharge = std::make_unique<DomainVariantPlan::Reservation>(
            this->op.req.memoryTracker, bytes, "query bucket windows");
      }
    }

    Query::Weight* bucketSelection(size_t bucket) override {
      auto* weight = queryOp().bucketWeights[bucket];
      // A prepared predicate is still a predicate, but its execution needs the
      // complete scope. Prepare it at the same bounded stage as a selection;
      // never retain a canonical whole-reader identity table on this Calc.
      return !queryOp().bucketQueries[bucket]->recoverableAsPredicate() || weight->needsPrepare()
          ? weight : nullptr;
    }

    DomainHandle identityDomain(size_t segnum, size_t bucket, DocSet* eligibility) {
      // A mixed block can memoize predicate rows beside staged rows. The
      // latter are applied by StructuralConstraint::finish, never replayed
      // from this placeholder or from the bucket's original selected window.
      if (bucketSelection(bucket)) return DomainHandle(std::make_unique<ArrDocSet>(
          std::vector<int32_t>{}));
      auto& segment = queryOp().reader.segments()[segnum];
      auto release = scope_guard([&]() noexcept { releaseBucketState(segment, bucket); });
      return DomainHandle(QueryPrep::materialize(
          *queryOp().bucketWeights[bucket], nullptr, segment, eligibility));
    }

    void collectSegment(
        typename Base::Data& data, int32_t segnum,
        DocSet* domain) override {
      if constexpr (Distinct) {
        auto& segment = queryOp().reader.segments()[(size_t)segnum];
        for (size_t bucket = 0; bucket < queryOp().bucketCount(); bucket++) {
          auto release = scope_guard([&]() noexcept { releaseBucketState(segment, bucket); });
          DomainHandle matches = materializeBucket((size_t)segnum, bucket, domain);
          queryOp().collectGroups(data, segnum, matches.get(), [&](auto group, auto add, auto) {
            forEachDomainDoc(matches.get(), segment.maxDoc(), [&](int32_t doc) { add((int64_t)bucket, group(doc)); });
          });
        }
      } else {
      auto& segment = queryOp().reader.segments()[(size_t)segnum];
      // The count pass never retains matches (result children re-materialize
      // their selected buckets), so the direct count is eligible whenever the
      // whole segment is the domain.
      bool countOnly = domain == nullptr;
      for (size_t bucket = 0; bucket < queryOp().bucketCount(); bucket++) {
        auto release = scope_guard([&]() noexcept { releaseBucketState(segment, bucket); });
        auto guard = MemPool::threadLocalPoolGuard();
        Query::ScorerSupplier* supplier = nullptr;
        bool supplierBuilt = false;
        auto obtainSupplier = [&]() {
          if (!supplierBuilt) {
            supplier = bucketSource(bucket).scorerSupplier(guard.pool(), segment);
            supplierBuilt = true;
          }
          return supplier;
        };
        if (countOnly && !queryOp().bucketWeights[bucket]->needsPrepare()) {
          auto* weight = queryOp().bucketWeights[bucket];
          auto count = QueryPrep::resolveCount(weight->countShortcut(segment, domain),
              [&](Query::Weight& resolver) {
                return &resolver == weight ? obtainSupplier()
                    : resolver.scorerSupplier(guard.pool(), segment);
              }, Query::COUNT_EXECUTION_LIMIT);
          if (!count) count = QueryPrep::supplierCount(obtainSupplier());
          if (count) {
            data.counts[bucket] += *count;
            continue;
          }
        }
        supplier = obtainSupplier();
        if (supplier == nullptr) continue;
        DomainHandle matches(QueryPrep::materialize(supplier, guard.pool(), segment, domain));
        data.counts[bucket] += matches.get()->card();
      }
      }
    }

    // Bucket queries are independent: one domain is alive at a time, and the
    // bucket's query state is released before its children run.
    void feedBucketDomains(
        size_t segnum, std::span<const SelectedFacetBucket<size_t>> block,
        FacetBucketFeed feed) override {
      auto& segment = queryOp().reader.segments()[segnum];
      for (size_t i = 0; i < block.size(); i++) {
        DomainHandle domain;
        {
          auto release = scope_guard([&]() noexcept {
            releaseBucketState(segment, block[i].key);
          });
          domain = materializeBucket(
              segnum, block[i].key, this->input[segnum].get());
        }
        feed(i, std::move(domain));
      }
    }

    void feedIdentityDomains(size_t segnum,
        std::span<const SelectedFacetBucket<size_t>> block, DomainHandle eligibility,
        FacetBucketFeed feed) override {
      for (size_t i = 0; i < block.size(); i++) {
        if (queryOp().bucketWeights[block[i].key]->matchesAllDocs()) feed(i, eligibility);
        else feed(i, identityDomain(segnum, block[i].key, eligibility.get()));
      }
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : Base(op, parent, slot, numSlots, structural) {}

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      if (queryOp().segmentStateRetained(segment)) return;
      for (const auto& source : preparedSources) source.releaseSegmentState(segment);
      queryOp().releaseSegmentState(segment);
    }
  };

  Calculator* createCalculator(
      Calculator* parent, int64_t slot, int64_t numSlots = -1,
      const StructuralConstraint* structural = nullptr) override {
    if (countDistinct != nullptr) return new Calc<true>(*this, parent, slot, numSlots, structural);
    return new Calc<false>(*this, parent, slot, numSlots, structural);
  }
};

}
