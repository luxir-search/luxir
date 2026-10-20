// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchOp.h"
#include "luxir/search/Collector.h"
#include "luxir/search/NamedQueries.h"
#include "luxir/search/SortField.h"

namespace luxir {

// A ranked list supplies its finalized membership and original scores. The
// source and eligibility handles live through synchronous page execution;
// response columns and ranked storage remain pinned through streaming.
struct CollapsedPageSettings {
  SearchOp& owner;
  const CollapsePlan& collapse;
  const api::Collapse& spec;
  const SortPlan& sort;
  const std::optional<SortPlan>& headSort;
  const boost::unordered_flat_map<std::string_view, SearchOp*>& groupOps;
  std::span<const std::string_view> fields;
  int32_t batchSize;
  int64_t offset;
  bool getNumber;
  bool getScores;
  api::DocFormat documentFormat;
  Query::SegmentSource* members;
  std::span<const DomainHandle> domains;
  api::GroupTable*& pageGroups;
  bool alwaysRank = false;
  void (*releaseSegment)(SearchOp&, IndexReader::Segment&) = nullptr;
  void (*releaseMembers)(SearchOp::Calculator&) = nullptr;
  NamedQueries* namedQueries = nullptr;  // evaluated per returned head
};

void emitCollapsedPage(const CollapsedPageSettings& settings,
                       SearchOp::Calculator& calc, ScoreCollector<true>& collector);

} // namespace luxir
