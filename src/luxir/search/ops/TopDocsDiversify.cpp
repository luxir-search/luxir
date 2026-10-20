// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TopDocsReq.h"

namespace luxir {

MergeableCollector::MergeableCollector(size_t topCount, const SortPlan& sortPlan,
    IndexReader* reader, bool needsScores, const DiversifyPlan& diversify)
    : useFieldSort(sortPlan.useFieldSort) {
  if (useFieldSort) {
    diverseFieldCollector = std::make_unique<FieldCollector<true, DiversifyAdmission>>(
        topCount, sortPlan.clauses, reader, needsScores, &diversify);
  } else {
    diverseScoreCollector = std::make_unique<ScoreCollector<true, DiversifyAdmission>>(topCount, &diversify);
  }
}

MergeableCollector* MergeableCollector::mergeDiversified(MergeableCollector* a, MergeableCollector* b) {
  if (a->useFieldSort) {
    if (a->diverseFieldCollector->size() < b->diverseFieldCollector->size()) std::swap(a, b);
    a->diverseFieldCollector->merge(*b->diverseFieldCollector);
  } else {
    if (a->diverseScoreCollector->size() < b->diverseScoreCollector->size()) std::swap(a, b);
    a->diverseScoreCollector->merge(*b->diverseScoreCollector);
  }
  return a;
}

void TopDocsReq::CalcBase::configureDiversify() {
  auto& op = thisOp();
  collectorMerger.creator = [&op]() {
    auto* collector = new MergeableCollector(op.topCount, op.sortPlan, op.req.reader.get(),
                                            op.requestNeedsScores, *op.diversify);
    if (collector->useFieldSort) collector->diverseFieldCollector->primaryValues = op.sortValues;
    return collector;
  };
}

SearchOp::Calculator* TopDocsReq::createDiversifyCalculator(
    Calculator* parent, int64_t slot, int64_t numSlots, const StructuralConstraint* scope) {
  if (scope && domainVariants.hasResetVariants()) {
    return new Calc<true, false, DiversifyAdmission>(*this, parent, slot, numSlots, scope);
  }
  return new Calc<false, false, DiversifyAdmission>(*this, parent, slot, numSlots, scope);
}

void TopDocsReq::fillDiversifyResponse(CalcBase& calc, MergeableCollector& data) {
  auto emit = [&](auto& collector) {
    auto docs = collector.sort();
    auto target = [&](SearchResponse* response) -> api::DocList& {
      auto& list = *calc.slotArm<api::DocList>(response);
      if (topDocsProto.get_number) list.found_groups = collector.groups->totalGroups();
      return list;
    };
    emitDocsResponse(req, target, (int64_t)docs.size(),
        [docs](int64_t i) { return docs[i].doc; },
        [docs](int64_t i) { return docs[i].score; }, collector.totalHits(),
        topDocsProto.fields, topDocsProto.batch_size, topDocsProto.offset,
        topDocsProto.get_number, topDocsProto.get_scores, topDocsProto.document_format,
        calc.underBucketSlot() ? DocEmission::FINAL_RESPONSE : DocEmission::STREAM,
        {}, namedQueries);
  };
  if (data.useFieldSort) emit(*data.diverseFieldCollector);
  else emit(*data.diverseScoreCollector);
}

} // namespace luxir
