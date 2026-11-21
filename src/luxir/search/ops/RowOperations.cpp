// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "TopDocsReq.h"
#include "luxir/reader/OwnershipMapper.h"
#include "luxir/search/RowVectorReplay.h"

namespace luxir {

std::vector<ExtraColumn> SearchOp::rowColumns() {
  std::vector<ExtraColumn> columns;
  for (auto* operation : rowOps) {
    columns.push_back({operation->name, {},
        [operation, source = view](SearchResponse& response,
            std::span<const segdoc> docs, std::span<const uint32_t> rows,
            std::span<api::Val> values, RequestMemResource& memory) {
      auto& op = *operation;
      auto& target = *op.view;
      auto fields = op.rowFields;
      DocBatchScratch scratch;
      ScoreCollector<> scores(op.topCount);
      std::unique_ptr<FieldSortCollector> sorted;
      if (op.sortPlan.useFieldSort) sorted = std::make_unique<FieldSortCollector>(
          op.topCount, op.sortPlan.clauses, &target, op.weight->needsScores());
      MemPool pool;
      auto& segment = target.segments()[docs[rows.front()].segment()];
      bool down = target.path().size() > source->path().size();
      std::optional<OwnershipMapper> mapper;
      std::optional<OwnershipMapper::Cursor> cursor;
      if (target.path() != source->path() && segment.hasSpace()) {
        mapper.emplace(segment.segmentCore(), down ? source->path() : target.path(),
                       down ? target.path() : source->path());
        cursor.emplace(*mapper, pool);
      }
      auto range = [&](int32_t row) -> std::pair<int32_t, int32_t> {
        if (!segment.hasSpace()) return {0, 0};
        if (!cursor) return {row, row + 1};
        auto family = down ? cursor->seek(row) : cursor->owner(row);
        return down ? std::pair{family.first, family.end}
                    : std::pair{family.owner, family.owner + 1};
      };
      std::pmr::vector<std::pair<int32_t, int32_t>> ranges(&memory);
      ranges.reserve(rows.size());
      int64_t candidates = 0;
      for (auto row : rows) {
        auto [first, end] = range(docs[row].docId());
        candidates += end - first;
        ranges.emplace_back(first, end);
      }
      Query::Scorer* scorer = nullptr;
      if (candidates && (op.topCount > 0 || op.requirements.needsCount())) {
        auto* supplier = op.weight->scorerSupplier(pool, segment);
        if (supplier) {
          auto context = supplier->makePlanContext(Query::Demand::fromLeadCost(candidates));
          context.membershipConsumption = Query::MembershipConsumption::EITHER;
          auto* plan = supplier->resolve(pool, context);
          scorer = plan->build(pool);
          skipCount(SkipStats::rowOperationLeafPlans);
        }
      }
      auto uniform = scorer ? scorer->constantContribution() : std::nullopt;
      std::optional<RowVectorReplay::Segment> replay;
      if (op.vectorReplay) replay.emplace(*op.vectorReplay, target, segment);
      bool replayScores = replay && op.vectorReplay->scores && !op.topDocsProto.query;
      std::optional<FieldSortCollector::ExpressionBindings> expressions;
      if (sorted) {
        sorted->setSegment(segment, &pool);
        expressions.emplace(*sorted, pool, segment);
      }
      std::pair<int32_t, int32_t> previous{-1, -1};
      api::Val previousValue;
      std::pmr::vector<segdoc> selected(&memory);
      std::pmr::vector<float> selectedScores(&memory);
      size_t position = 0;
      for (auto row : rows) {
        // Check cancellation between rows, including empty families.
        if (op.req.waitCancellation.get_token().stop_requested()
            || oneapi::tbb::is_current_task_group_canceling()) {
          throw RequestError("request cancelled");
        }
        skipCount(SkipStats::rowOperationRows);
        auto bounds = ranges[position++];
        if (bounds == previous) {
          values[row] = previousValue;
          continue;
        }
        scores.reset();
        if (sorted) sorted->reset();
        if (scorer && bounds.first < bounds.second) {
          if (replay) replay->startFamily();
          auto collect = [&](int32_t doc) {
            float score;
            if (replay) {
              // The replayed kNN restricts to rows it would score, and ranks
              // them unless the operation brings its own query.
              auto similarity = replay->score(doc);
              if (!similarity) return true;
              score = replayScores ? *similarity
                  : uniform ? *uniform : op.weight->needsScores() ? scorer->score() : 0;
            } else score = uniform ? *uniform : op.weight->needsScores() ? scorer->score() : 0;
            if (sorted) sorted->collect(segment.ord, doc, score);
            else scores.collect(segment.ord, doc, score);
            if (!sorted && op.weight->isConstantScoring() && !replayScores
                && !op.requirements.needsCount()
                && scores.hitCount >= op.topCount) {
              return false;
            }
            return true;
          };
          if (op.weight->needsScores() && !uniform) visitMatchesInRange<true>(scorer, nullptr, bounds.first, bounds.second, collect);
          else visitMatchesInRange(scorer, nullptr, bounds.first, bounds.second, collect);
        }
        auto& result = values[row].kind.emplace<api::DocList>();
        int64_t found = sorted ? sorted->totalHits() : scores.totalHits();
        if (op.requirements.needsCount()) {
          result.found = found;
        }
        result.offset = op.topDocsProto.offset;
        selected.clear();
        selectedScores.clear();
        auto select = [&](auto hits) {
          for (size_t i = std::min<size_t>(hits.size(), (size_t)result.offset); i < hits.size(); i++) {
            selected.push_back(hits[i].doc);
            selectedScores.push_back(hits[i].score);
          }
        };
        if (sorted) select(sorted->sort());
        else select(scores.sort());
        result.row_count = (int32_t)selected.size();
        auto placement = op.topDocsProto.document_format;
        if (placement == api::DocFormat::DEFAULT) placement = op.req.docFormatDefault;
        loadDocuments(response, target, selected, fields, result, memory,
            {op.flags, {}, placement, fields.size() + 1, 0, op.topDocsProto.get_scores, &memory},
            [&](int64_t i) { return selectedScores[(size_t)i]; }, scratch);
        previous = bounds;
        previousValue = values[row];
      }
    }});
  }
  return columns;
}

} // namespace luxir
