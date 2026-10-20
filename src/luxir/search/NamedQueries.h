// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/query/Query.h"
#include "luxir/search/Collector.h"

namespace luxir {

class SearchRequest;

// Named clauses (api::Query.name) of one ranked list, reported per returned
// document as the _matched_queries_ pseudo-field (get_matched_queries).
//
// Each named clause gets its own standalone Weight: inside the request's
// query tree, Boolean normalization dedups and flattens clauses, so the
// executed tree no longer knows which scorer came from which named node.
// Evaluation happens at emission, only for the documents of each batch: per
// segment run (docs ascending), every clause's scorer advances over the run.
// A clause is evaluated on its own over the whole collection, outside its
// enclosing query and filters (a kNN clause therefore selects its own k
// nearest live documents). Reusing a name merges its clauses: the name
// matches when any of them does, with the highest of their scores.
class NamedQueries {
public:
  struct Clause {
    uint32_t name;  // index into names
    Query::Weight* weight;
  };
  struct Hit {
    uint32_t row;   // batch row
    uint32_t name;  // index into names
    float score;    // 0 unless scores
  };

  std::vector<std::string_view> names;  // distinct, in first-parse order
  std::vector<Clause> clauses;
  bool scores;
  // Weights that need a whole-index prepare() pass, prepared once against the
  // live documents on first use (batches of bucket children may race).
  std::vector<std::unique_ptr<Query::Weight::PreparedWeight>> prepared;
  std::once_flag prepareOnce;

  explicit NamedQueries(bool scores) : scores(scores) {}

  // Builds the weights of the collected (name, lowered query) pairs; null
  // when there are none. Serial: runs during request planning.
  static NamedQueries* create(SearchRequest& req, Query::PlanningContext& planning,
                              std::span<const std::pair<std::string_view, Query*>> named,
                              bool scores);

  // The named clauses each row of one batch matches, sorted by row then name,
  // one entry per (row, name). runs are segDocs indexes grouped by segment,
  // docs ascending within a run.
  std::vector<Hit> evaluate(SearchRequest& req, std::span<const segdoc> segDocs,
                            std::span<const std::span<uint32_t>> runs);
};

} // namespace luxir
