// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/FacetOp.h"
#include "luxir/search/RangeFacetCountWindow.h"

namespace luxir {

// A contributor owns missing independently of its raw-value cell window.
struct IntFacetRangeReq::DataCounts : MergeableData {
  RangeFacetCountWindow window;
  int64_t missing_num = 0;
  explicit DataCounts(IntFacetRangeReq& op)
      : window(*op.grid, op.req.memoryTracker, std::format("facet '{}'", op.name)) {}
  static DataCounts* merge(DataCounts* a, DataCounts* b) {
    if (a->window.size() < b->window.size()) std::swap(a, b);
    a->window.mergeFrom(b->window);
    a->missing_num += b->missing_num;
    return a;
  }
};

// The growing policy owns only storage and the finalized layout. Segment
// delivery, retained domains, result children and routing use the fixed base.
template <bool Distinct>
struct IntFacetRangeReq::GrowingStorage {
  using Data = std::conditional_t<Distinct, DistinctFacetCounts, DataCounts>;
  static constexpr bool growing = true;
  RequestMemResource resource;
  std::pmr::vector<int64_t> layout;
  int64_t gap = 0;
  std::atomic<size_t> observed{0};
  std::atomic<size_t> retainedInputs{0};
  std::pmr::vector<RangeFacetRun> runs;

  explicit GrowingStorage(SearchOp& op)
      : resource(op.req.memoryTracker, std::format("facet '{}'", op.name), "facet range layout"),
        layout(&resource), runs(&resource) {}
  static void initialize(FixedBucketFacetReq&, Data&) {}
  void observe(const Data& data) {
    size_t bytes = sizeof(Data) + resource.bytes();
    if constexpr (Distinct) bytes += data.residentBytes();
    else bytes += data.window.residentBytes();
    size_t previous = observed.load(std::memory_order_relaxed);
    while (previous < bytes && !observed.compare_exchange_weak(previous, bytes, std::memory_order_relaxed)) {}
  }
  static Data* create(FixedBucketFacetReq& op, DistinctFacetMode) {
    auto& range = (IntFacetRangeReq&)op;
    if constexpr (Distinct) return new Data(range.req.memoryTracker,
        std::format("facet '{}'", range.name), forcedDistinctFacetMode,
        *range.grid, range.groupDimension());
    else return new Data(range);
  }
  template <typename Emit>
  void finish(FixedBucketFacetReq& op, Data& data, Emit&& emit) {
    auto release = scope_guard([&]() noexcept {
      decltype(layout) empty(&resource);
      layout.swap(empty);
      decltype(runs) emptyRuns(&resource);
      runs.swap(emptyRuns);
    });
    observe(data);
    auto& range = (IntFacetRangeReq&)op;
    auto& window = [&]() -> auto& {
      if constexpr (Distinct) return *data.cellWindow();
      else return data.window;
    }();
    if constexpr (Distinct) {
      data.widen(range.wideningStart, range.wideningEnd);
      data.finish();
    } else data.window.widen(range.wideningStart, range.wideningEnd);
    layout.reserve(window.size() + 1);
    for (size_t i = 0; i < window.size(); i++) layout.push_back(window.lowerFence(i));
    if (!window.empty()) layout.push_back(range.grid->next(range.grid->floor(layout.back())).key);
    if (!layout.empty()) for (int64_t civil : range.grid->skippedCalendarCells(layout.front(), layout.back())) {
      range.req.warnOnce("calendar_bucket_skipped",
          "facet '" + std::string(range.name) + "': calendar bucket "
          + formatEpochMillisIso8601(civil).substr(0, 10)
          + " skipped: no such local day in " + range.grid->timeZone().name());
    }
    gap = layout.empty() ? 0 : range.grid->affineGap(layout.front(), layout.back()).value_or(0);
    if (!gap) runs = range.grid->runs(layout, &resource);
    std::pmr::vector<int64_t> distinctCounts(&resource);
    CountView merged;
    if constexpr (Distinct) {
      distinctCounts.reserve(window.size());
      for (size_t i = 0; i < window.size(); i++) distinctCounts.push_back(data.count(layout[i]));
      merged = {distinctCounts, data.missing()};
    } else merged = {window.counts(), data.missing_num};
    observe(data);
    emit(merged);
  }
  size_t bucketCount(const FixedBucketFacetReq&) const { return layout.empty() ? 0 : layout.size() - 1; }
  std::pmr::vector<uint8_t> pinned(const FixedBucketFacetReq& op) {
    return std::pmr::vector<uint8_t>(bucketCount(op), 0, &resource);
  }
  std::pmr::vector<size_t> emitted(FixedBucketFacetReq& op, CountView merged, std::span<const uint8_t>) {
    std::pmr::vector<size_t> result(&resource);
    for (size_t i = 0; i < merged.counts.size(); i++) if (merged.counts[i] >= op.minCount) result.push_back(i);
    return result;
  }
  void emit(FixedBucketFacetReq& op, api::FacetResult& result, CountView merged,
            std::span<const size_t> emitted, std::pmr::memory_resource& mr) {
    ((IntFacetRangeReq&)op).emitRangeResult(result, merged, emitted, mr, layout);
  }
};

template <bool Distinct, bool Floating>
class IntFacetRangeReq::DataCalc : public FixedBucketFacetReq::Calc<Distinct, GrowingStorage<Distinct>> {
  using Base = FixedBucketFacetReq::Calc<Distinct, GrowingStorage<Distinct>>;
  using Data = typename Base::Data;
  IntFacetRangeReq& rangeOp() { return (IntFacetRangeReq&)this->getOp(); }
  FieldType::Type rangeOpType() const { return ((IntFacetRangeReq&)this->op).valueType; }

  bool finite(int64_t value) const {
    if constexpr (!Floating) return true;
    if (rangeOpType() == FieldType::Type::FLOAT) {
      return std::isfinite(sortableInt32ToFloat((int32_t)value));
    }
    return std::isfinite(sortableInt64ToDouble(value));
  }

  bool pointsSegment(DataCounts& data, int32_t segnum, DocSet* domain) {
    auto& range = rangeOp();
    auto& segment = range.reader.segments()[(size_t)segnum];
    const auto* info = range.segInfos.at((size_t)segnum);
    if (disablePointsRangeFacetForTests || domain || segment.liveDocs() || !range.subOps.empty()
        || !info || !info->columnBounds || !numericIdentityPoints(*info)) return false;
    auto [lo, hi] = *info->columnBounds;
    auto pool = MemPool::threadLocalPoolGuard();
    IntColReader column(segment.postingsReader(), *info);
    PointsReader points(segment.postingsReader(), *info);
    if (points.pointCount() != (uint64_t)column.numValues()) {
      throw std::runtime_error("IntFacetRangeReq: points/column value count mismatch");
    }
    data.missing_num += segment.maxDoc() - column.docsWithValue();
    auto residual = pool.pool().make_span<uint32_t>(points.maxPointsPerLeaf());
    auto raw = pool.pool().make_span<int64_t>(points.maxPointsPerLeaf());
    if constexpr (Floating) if (!finite(lo) || !finite(hi)) {
      double max = range.valueType == FieldType::FLOAT
          ? (double)std::numeric_limits<float>::max() : std::numeric_limits<double>::max();
      int64_t finiteLo = range.grid->encode(-max), finiteHi = range.grid->encode(max);
      auto span = points.fenceRange(finiteLo, finiteHi);
      if (span.empty) {
        skipCount(SkipStats::rangeFacetPointsArms);
        return true;
      }
      size_t count = points.decodeLeafValuesInto(span.firstLeaf, raw, residual);
      auto first = std::lower_bound(raw.begin(), raw.begin() + count, finiteLo);
      if (first == raw.begin() + count || *first > finiteHi) {
        skipCount(SkipStats::rangeFacetPointsArms);
        return true;
      }
      lo = *first;
      count = points.decodeLeafValuesInto(span.lastLeaf, raw, residual);
      hi = *std::prev(std::upper_bound(raw.begin(), raw.begin() + count, finiteHi));
    }

    // Exact segment extrema are occupied. Extend through the grid once;
    // reuse that canonical fence window for ordinal counting, including
    // when this contributor already owns other segments' cells.
    auto& window = data.window;
    window.observe(lo);
    window.observe(hi);
    size_t first = window.observe(lo), last = window.observe(hi);
    int64_t end = range.grid->next(range.grid->floor(hi)).key;
    uint64_t ordinal = points.ordinalOfFirstAtLeast(window.cellKey(first), residual, raw);
    for (size_t i = first; i <= last; i++) {
      int64_t edge = i == last ? end : window.cellKey(i + 1);
      uint64_t next = points.ordinalOfFirstAtLeast(edge, residual, raw);
      window.entry(i) += (int64_t)(next - ordinal);
      ordinal = next;
    }
    skipCount(SkipStats::rangeFacetPointsArms);
    return true;
  }

  void collectSegment(Data& data, int32_t segnum, DocSet* domain) override {
    auto& range = rangeOp();
    if constexpr (Distinct) {
      int64_t missing = 0;
      range.template collectGroups<true>(data, segnum, domain, [&](auto group, auto add, auto) {
        range.facetSegIntCol(domain, segnum, missing,
            [&](int32_t doc, int64_t value) LUXIR_INLINE {
              if constexpr (Floating) if (!finite(value)) return;
              add(value, group(doc));
            });
      });
      if (range.missing && missing != 0) range.template collectGroups<true>(data, segnum, domain,
          [&](auto group, auto, auto missingBucket) {
            range.forEachMissing(domain, segnum,
                [&](int32_t doc) { missingBucket(group(doc)); });
          });
    } else {
      if (pointsSegment(data, segnum, domain)) return;
      range.facetSegIntCol(domain, segnum, data.missing_num,
          [&](int32_t, int64_t value) LUXIR_INLINE {
            if constexpr (Floating) if (!finite(value)) return;
            data.window.add(value);
          });
    }
  }

  void feedBucketDomains(size_t segnum,
      std::span<const SelectedFacetBucket<size_t>> block, FacetBucketFeed feed) override {
    rangeOp().feedRangeDomains(segnum, block, this->input[segnum], feed,
        {this->storage.layout, this->storage.gap, this->storage.runs}, false);
  }
  void feedIdentityDomains(size_t segnum,
      std::span<const SelectedFacetBucket<size_t>> block, DomainHandle eligibility,
      FacetBucketFeed feed) override {
    rangeOp().feedRangeDomains(segnum, block, std::move(eligibility), feed,
        {this->storage.layout, this->storage.gap, this->storage.runs}, true);
  }
public:
  using Base::Base;
};

size_t IntFacetRangeReq::facetBucketResidentBytes() const {
  if (!grid) return FixedBucketFacetReq::facetBucketResidentBytes();
  // Unknown extents are priced by their initial directory, never by the
  // field-wide span or the cap. Actual allocations use the request breaker.
  size_t bytes = countDistinct ? sizeof(DataCalc<true>) + sizeof(DistinctFacetCounts)
      + distinctBindingBytes({true, 0, 8})
      : sizeof(DataCalc<false>) + sizeof(DataCounts) + 8 * sizeof(int64_t);
  if (!subOps.empty()) bytes = saturatingAdd(bytes, reader.segments().size() * sizeof(DomainHandle));
  return bytes;
}

SearchOp::Calculator* IntFacetRangeReq::createDataCalculator(
    Calculator* parent, int64_t slot, int64_t numSlots,
    const StructuralConstraint* structural) {
  bool floating = valueType == FieldType::Type::FLOAT || valueType == FieldType::Type::DOUBLE;
  if (countDistinct) {
    if (floating) return new DataCalc<true, true>(*this, parent, slot, numSlots, structural);
    return new DataCalc<true>(*this, parent, slot, numSlots, structural);
  }
  if (floating) return new DataCalc<false, true>(*this, parent, slot, numSlots, structural);
  return new DataCalc<false>(*this, parent, slot, numSlots, structural);
}

} // namespace luxir
