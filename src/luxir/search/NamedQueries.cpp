// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/NamedQueries.h"

#include <algorithm>

#include "luxir/search/SearchRequest.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/proto.h"

namespace luxir {

NamedQueries* NamedQueries::create(SearchRequest& req, Query::PlanningContext& planning,
                                   std::span<const std::pair<std::string_view, Query*>> named,
                                   bool scores) {
  if (named.empty()) return nullptr;
  auto* out = luxir::arenaCreate<NamedQueries>(req.arena, scores);
  // A standalone Context: the clause weights share no execution state with
  // the ranked list's own weights.
  auto* context = Query::Context::create(&req.arena, planning);
  int32_t flags = scores ? Query::NEED_SCORES : 0;
  for (const auto& [name, query] : named) {
    auto found = std::ranges::find(out->names, name);
    auto index = (uint32_t)(found - out->names.begin());
    if (found == out->names.end()) out->names.push_back(name);
    // Usually already visited as part of the enclosing tree; a clause that
    // normalization dropped (a duplicate) is validated here.
    query->validateLogical(planning);
    out->clauses.push_back({index, query->createWeight(*context, flags)});
  }
  out->prepared.resize(out->clauses.size());
  return out;
}

std::vector<NamedQueries::Hit> NamedQueries::evaluate(
    SearchRequest& req, std::span<const segdoc> segDocs,
    std::span<const std::span<uint32_t>> runs) {
  std::call_once(prepareOnce, [&]() {
    std::vector<DocSet*> live;
    for (auto& segment : req.reader->segments()) {
      live.push_back(segment.liveDocs() ? &segment.liveDocs()->docset() : nullptr);
    }
    Query::Weight::PrepareContext context{*req.reader,
        std::span<DocSet* const>(live.data(), live.size()), false, &req};
    for (size_t c = 0; c < clauses.size(); c++) {
      if (clauses[c].weight->needsPrepare()) prepared[c] = clauses[c].weight->prepare(context);
    }
  });

  std::vector<Hit> hits;
  for (auto run : runs) {
    if (run.empty()) continue;
    auto& segment = req.reader->segments()[(size_t)segDocs[run[0]].segment()];
    for (size_t c = 0; c < clauses.size(); c++) {
      Query::SegmentSource& source = prepared[c]
          ? static_cast<Query::SegmentSource&>(*prepared[c]) : *clauses[c].weight;
      MemPool pool;
      auto* supplier = source.scorerSupplier(pool, segment);
      if (supplier == nullptr) continue;
      auto demand = Query::Demand::fromLeadCost((int64_t)run.size());
      auto* scorer = supplier->resolve(pool, supplier->makePlanContext(demand))->build(pool);
      if (scorer == nullptr) continue;  // nothing matches in this segment
      for (uint32_t row : run) {
        int32_t doc = segDocs[row].docId();
        if (scorer->docId() < doc) scorer->advance(doc);
        if (scorer->docId() == doc) {
          hits.push_back({row, clauses[c].name, scores ? scorer->score() : 0.0f});
        }
      }
    }
  }
  std::ranges::sort(hits, [](const Hit& a, const Hit& b) {
    if (a.row != b.row) return a.row < b.row;
    if (a.name != b.name) return a.name < b.name;
    return a.score > b.score;
  });
  // A merged name keeps its first (highest-scoring) entry per row.
  auto dups = std::ranges::unique(hits, [](const Hit& a, const Hit& b) {
    return a.row == b.row && a.name == b.name;
  });
  hits.erase(dups.begin(), dups.end());
  return hits;
}

} // namespace luxir
