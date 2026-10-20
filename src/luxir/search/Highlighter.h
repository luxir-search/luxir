// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Search-result highlighting (TopDocs.highlight, Fusion.highlight).  There is
// no offsets index: each returned hit's stored text is re-analyzed with the
// analyzer of every representation the query targeted, tokens matching the
// query are marked by their source byte offsets, and the densest fragments
// are returned.  Only the emitted page is highlighted, inside the batch's
// field loading (one task per segment run).

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_set.hpp>

#include "luxir/api/luxir_types.hpp"
#include "luxir/search/IndexReader.h"
#include "luxir/util/StrRef.h"

namespace luxir {

class Query;
class PhraseQuery;
class SearchRequest;
class segdoc;

// The query leaves that target one physical representation of a highlighted
// field.  Terms are field-native (analyzed and fitted, as indexed).
struct HighlightTarget {
  std::string_view physicalName;
  FieldType* fieldType = nullptr;  // TEXT: analyzed; STRING/ID: one whole-value token
  boost::unordered_flat_set<std::string_view, PackedTermHash, PackedTermEqual> terms;
  std::vector<const PhraseQuery*> phrases;  // exact phrases (slop 0) only
  std::vector<const Query*> multiTerms;     // prefix, wildcard/regex, fuzzy

  bool matchesTerm(std::string_view token, std::string_view fitted) const;
};

struct HighlightField {
  std::string_view outputKey;   // the request spelling (or the matched root)
  std::string_view storedName;  // the stored primary's physical name
  FieldType* stored = nullptr;
  std::vector<HighlightTarget> targets;
  IndexReader::FieldSegments storedInfos;  // the stored resource's segments
};

struct HighlightPlan {
  std::vector<HighlightField> fields;
  std::string preTag;
  std::string postTag;
  int32_t fragmentSize = 100;
  int32_t numberOfFragments = 5;  // 0: each matching value whole
  int32_t maxAnalyzedOffset = 1000000;
  bool html = false;
};

// Resolve `spec` against the query trees whose terms are highlighted (the
// TopDocs query, or every Fusion source query; null entries are skipped).
// Throws RequestError for an unknown field, a field without stored text, or
// an out-of-range option.  Returns null when no requested field is targeted
// by the query, so there is nothing to mark.
HighlightPlan* buildHighlightPlan(SearchRequest& req, const api::Highlight& spec,
                                  std::span<Query* const> queries);

// Highlight the batch rows `rows` (indexes into segDocs, all in one segment)
// into out[row].  Safe to run concurrently for disjoint rows.
void highlightRows(SearchRequest& req, const HighlightPlan& plan,
                   std::span<const segdoc> segDocs, std::span<const uint32_t> rows,
                   api::Map* out, std::pmr::memory_resource& mr);

} // namespace luxir
