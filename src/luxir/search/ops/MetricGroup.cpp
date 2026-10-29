// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TopDocsReq.h"
#include "ExprStatsOp.h"
#include "FacetEmit.h"

namespace luxir {

void resolveMetricGroupOrder(TopDocsReq& op) {
  auto* plan = arenaCreate<MetricGroupPlan>(op.req.arena);
  for (const auto& clause : op.topDocsProto.collapse->sort) {
    auto found = op.groupOps.find(clause.expr);
    if (found == op.groupOps.end() || dynamic_cast<ExprStatsOp*>(found->second) == nullptr) {
      throw RequestError("collapse.sort '" + std::string(clause.expr)
          + "' must name a metric in collapse.ops");
    }
    if (!found->second->canInlineInBucket()) {
      throw RequestError("collapse.sort metric '" + std::string(clause.expr)
          + "' requires an inline fold without a domain override");
    }
    plan->metrics.push_back(found->second);
    plan->descending.push_back(clause.dir == api::SortSpec_::SortDir::DESC);
  }
  op.metricOrder = plan;
}

SearchOp::Calculator* createMetricGroupCalculator(TopDocsReq& op, SearchOp::Calculator* parent,
    int64_t slot, int64_t numSlots, const StructuralConstraint* structural) {
  if (structural && op.domainVariants.hasResetVariants()) {
    return new TopDocsReq::Calc<true, true>(op, parent, slot, numSlots, structural);
  }
  return new TopDocsReq::Calc<false, true>(op, parent, slot, numSlots, structural);
}

api::Val ExprStatsOp::InlineCalc::finalizedValue(size_t slot) const {
  api::Val result;
  if (!valid(slot)) result.kind = google::protobuf::NullValue::NULL_VALUE;
  else if (resultType == BucketValueType::DOUBLE)
    result.kind = std::bit_cast<double>(finalizedValues[slot]);
  else result.kind = std::bit_cast<int64_t>(finalizedValues[slot]);
  return result;
}

namespace {
std::vector<SortClause> memberClauses(const TopDocsReq& op) {
  auto result = op.sortPlan.clauses;
  if (result.empty()) result.emplace_back(SortClause::SCORE, SortField::DESC);
  for (auto& clause : result) {
    if (clause.getKind() != SortClause::COLUMN) continue;
    const auto& field = clause.getSortField();
    auto& type = const_cast<FieldType&>(field.getFieldType());
    if ((type.type() == FieldType::STRING || type.type() == FieldType::ID) && type.indexed()) {
      // Exhaustive folds retain all groups. Global ords bound each slot's
      // storage without retaining a copy of every best member's term.
      clause = SortClause(SortField(field.getFieldName(), type, field.getOrder(),
          field.getMissingValue(), StringSortMode::GLOBAL));
    }
  }
  return result;
}

struct Members {
  RequestMemTracker& tracker;
  RequestMemResource resource;
  FieldSortCollector field;
  std::pmr::vector<std::optional<FieldSortCollector::SortDoc>> heads;
  std::pmr::vector<StrColComparator*> strings;
  size_t slotStride = 0;
  size_t slotBytes = 0;
  size_t stringBytes = 0;
  int32_t segment = -1;
  float score = 0;

  explicit Members(TopDocsReq& op)
      : tracker(op.req.memoryTracker), resource(tracker, "best members", "group aggregate state"),
        field(std::min<int64_t>(op.req.reader->maxDoc(), INT32_MAX), memberClauses(op),
              op.req.reader.get(), op.requestNeedsScores), heads(&resource), strings(&resource) {
    for (auto& clause : field.clauses) {
      if (auto* text = dynamic_cast<StrColComparator*>(clause.comparator.get())) {
        strings.push_back(text);
        slotStride += sizeof(std::string) + sizeof(uint8_t); // value and missing flag
      } else if (clause.comparator) slotStride += sizeof(int64_t);
      else if (clause.expr) slotStride += sizeof(FieldSortCollector::RuntimeClause::ExprSlot);
    }
  }
  ~Members() { if (slotBytes + stringBytes) tracker.release(slotBytes + stringBytes); }

  size_t stringStorage(int32_t slot) const {
    size_t bytes = 0;
    for (const auto* text : strings) bytes += text->retainedSlotBytes(slot);
    return bytes;
  }
  void trackStrings(int32_t slot, size_t before) {
    size_t after = stringStorage(slot);
    if (after > before) {
      tracker.charge(after - before, "group aggregate state", "best-member string values");
      stringBytes += after - before;
    } else {
      tracker.release(before - after);
      stringBytes -= before - after;
    }
  }

  int32_t newSlot() {
    if (heads.size() == INT32_MAX) throw RequestError("too many group best-member slots");
    int32_t slot = (int32_t)heads.size();
    if ((int64_t)slot == field.slotCapacity) {
      // Comparator slot vectors grow exactly to this capacity. Reserve their
      // fixed cells before the fold grows them; variable string buffers are
      // accounted separately by their retained capacity.
      size_t capacity = (size_t)std::min<int64_t>(field.topCount,
          std::max<int64_t>(64, field.slotCapacity * 2));
      size_t bytes = saturatingMultiply(capacity, slotStride);
      tracker.charge(bytes - slotBytes, "group aggregate state", "best-member comparator slots");
      slotBytes = bytes;
    }
    heads.emplace_back();
    return slot;
  }
};

struct BestMemberPayload {
  static constexpr size_t BYTES = sizeof(int32_t);
  Members* members = nullptr;

  void insert(char* entry, int32_t doc) {
    int32_t slot = members->newSlot();
    storeUnaligned(entry, slot);
    update(entry, doc);
  }
  void update(char* entry, int32_t doc) {
    int32_t slot = loadUnaligned<int32_t>(entry);
    size_t before = members->stringStorage(slot);
    members->field.fold(slot, members->heads[(size_t)slot],
        segdoc(members->segment, doc), members->score);
    members->trackStrings(slot, before);
  }
  void merge(char* target, char* from, BestMemberPayload& other) {
    int32_t slot = loadUnaligned<int32_t>(target);
    int32_t source = loadUnaligned<int32_t>(from);
    size_t before = members->stringStorage(slot);
    members->field.fold(slot, members->heads[(size_t)slot], other.members->field,
        *other.members->heads[(size_t)source]);
    members->trackStrings(slot, before);
  }
  void mergeNew(char* target, char* from, BestMemberPayload& other) {
    storeUnaligned(target, members->newSlot());
    merge(target, from, other);
  }
};

using GroupTable = KeyedAggregateTable<CollapseKey, CollapseKeyHash, BestMemberPayload>;

// A walk is segment-local and bounded. All values of a document consume one
// shared identity decode; no per-document identity array is retained.
template <typename Read>
struct GroupCollector {
  GroupTable& table;
  Members& members;
  int64_t& hits;
  Read& read;
  bool needsScores;

  void collect(int32_t, int32_t doc, float score) {
    const CollapseKey* key = read(doc);
    if (!key) return;
    members.score = score;
    table.add(*key, doc);
    hits++;
  }
};
}  // namespace

struct MetricGroupData::Impl {
  TopDocsReq& op;
  Members members;
  std::vector<std::unique_ptr<SearchOp::InlineCalculator>> calculators;
  std::vector<SearchOp::InlineCalculator*> calcs;
  GroupTable table;
  int64_t hits = 0;
  bool finalized = false;
  struct Selection {
    const CollapseKey* key;
    size_t metricSlot;
  };
  std::pmr::vector<Selection> selected;
  std::pmr::vector<Rank> ranks;

  Impl(TopDocsReq& op, SearchOp::Calculator* parent)
      : op(op), members(op), table(BestMemberPayload{&members}),
        selected(&members.resource), ranks(&members.resource) {
    for (auto* metric : op.metricOrder->metrics) {
      calculators.emplace_back(metric->createInlineCalculator(parent, -1, -1));
      calcs.push_back(calculators.back().get());
    }
    table.configure(calcs, op.req.memoryTracker, 0,
        "collapse '" + std::string(op.name) + "'", nullptr);
    table.initialize(0, false, true);
  }

  void finish() {
    if (finalized) return;
    finalized = true;
    table.finalize();
    struct Payload { size_t metricSlot; int32_t memberSlot; };
    auto better = [&](const auto& a, const auto& b) {
      size_t left = a.payload.metricSlot, right = b.payload.metricSlot;
      for (size_t i = 0; i < calcs.size(); i++) {
        bool am = calcs[i]->isMissing(left), bm = calcs[i]->isMissing(right);
        if (am != bm) return !am;
        if (am) continue;
        int cmp = calcs[i]->compare(left, right);
        if (cmp) return op.metricOrder->descending[i] ? cmp > 0 : cmp < 0;
      }
      return members.field.compare(*members.heads[(size_t)a.payload.memberSlot],
          *members.heads[(size_t)b.payload.memberSlot]) < 0;
    };
    FieldBucketFinalizer<const CollapseKey*, Payload, decltype(better)> finalizer(
        1, 0, op.topCount, better);
    size_t slot = 0;
    table.forEachEntry([&](const CollapseKey& key, char* entry) {
      finalizer.add({&key, loadUnaligned<int64_t>(entry),
          {slot++, loadUnaligned<int32_t>(entry + sizeof(int64_t))}});
    });
    auto page = finalizer.finish();
    ranks.reserve(page.size());
    selected.reserve(page.size());
    for (const auto& row : page) {
      auto head = *members.heads[(size_t)row.payload->memberSlot];
      ranks.push_back({head.doc, head.score, selected.size()});
      selected.push_back({*row.key, row.payload->metricSlot});
    }
  }
};

MetricGroupData::MetricGroupData(TopDocsReq& op, SearchOp::Calculator* parent)
    : impl(std::make_unique<Impl>(op, parent)) {}
MetricGroupData::~MetricGroupData() = default;

void MetricGroupData::collect(int32_t segment, Query::Scorer* scorer, DocSet* filter,
    DocSetBuilder* builder, DomainBuildMode mode, DocSet* rawDomain) {
  auto& data = *impl;
  auto& seg = data.op.req.reader->segments()[(size_t)segment];
  data.members.segment = segment;
  data.members.field.setSegment(seg, &MemPool::threadLocal());
  FieldSortCollector::ExpressionBindings expressions(data.members.field, MemPool::threadLocal(), seg);
  size_t started = 0;
  auto end = scope_guard([&]() noexcept {
    for (size_t i = 0; i < started; i++) data.calcs[i]->endSeg(segment);
  });
  for (; started < data.calcs.size(); started++) data.calcs[started]->startSeg(segment);
  CollapseKeyReader keys(*data.op.collapse);
  keys.withKeyWalk(segment, filter, [&](auto&& read) {
    GroupCollector collector{data.table, data.members, data.hits, read, data.op.requestNeedsScores};
    collectTopK(segment, scorer, filter, builder, collector, false, nullptr, mode, rawDomain);
  });
}

MetricGroupData* MetricGroupData::merge(MetricGroupData* a, MetricGroupData* b) {
  if (a->impl->table.size() < b->impl->table.size()) std::swap(a, b);
  a->impl->table.merge(b->impl->table);
  a->impl->hits += b->impl->hits;
  return a;
}
std::span<MetricGroupData::Rank> MetricGroupData::sort() { impl->finish(); return impl->ranks; }
const CollapseKey& MetricGroupData::key(size_t slot) const { return *impl->selected[slot].key; }
int64_t MetricGroupData::totalHits() const { return impl->hits; }
int64_t MetricGroupData::totalGroups() const { return (int64_t)impl->table.size(); }
void MetricGroupData::renderRanks(std::span<const api::ArrVal> ranks, uint64_t flags,
                                 std::pmr::memory_resource& mr) const {
  if (RequestFlags::dateMode(flags) == RequestFlags::DateMode::NATIVE) return;
  for (size_t i = 0; i < impl->calcs.size(); i++) {
    auto& metric = (ExprStatsOp&)impl->calcs[i]->getOp();
    metric.withOutput(flags, mr, [&](auto output) {
      if constexpr (!std::is_same_v<decltype(output), NativeDateOutput>) {
        for (const auto& rank : ranks) renderDateValue(const_cast<api::Val&>(rank.v[i]), output);
      }
    });
  }
}

void MetricGroupData::emitRank(api::ArrVal& out, size_t slot, std::pmr::memory_resource& mr) const {
  auto* values = build::allocArray(out.v, impl->calcs.size(), mr);
  for (size_t i = 0; i < impl->calcs.size(); i++) values[i] =
      static_cast<ExprStatsOp::InlineCalc*>(impl->calcs[i])->finalizedValue(impl->selected[slot].metricSlot);
}
MetricGroupState::MetricGroupState(TopDocsReq& op, SearchOp::Calculator* parent)
    : merger([&op, parent] { return new MetricGroupData(op, parent); },
             [](MetricGroupData* data) { delete data; }) {}
}  // namespace luxir
