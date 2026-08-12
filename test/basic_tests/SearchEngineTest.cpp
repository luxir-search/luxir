
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "solux/query/BooleanQuery.h"
#include "solux/reader/Postings.h"
#include "solux/reader/SkipStats.h"
#include "solux/reader/DocsEnum.h"
#include "solux/search/SearchOverrides.h"
#include "solux/search/ops/TopDocsReq.h"
#include "solux/server/GRPCServer.h"

using namespace solux;
using namespace solux::test;

namespace {
class TopDocsFilterFoldGuard {
  bool saved;

public:
  explicit TopDocsFilterFoldGuard(bool disabled)
    : saved(disableTopDocsFilterFold) {
    disableTopDocsFilterFold = disabled;
  }
  ~TopDocsFilterFoldGuard() {
    disableTopDocsFilterFold = saved;
  }
};

class MultiTermDenseFillGuard {
  bool saved;

public:
  explicit MultiTermDenseFillGuard(bool disabled)
    : saved(MultiTermQuery::disableDenseFillForTests) {
    MultiTermQuery::disableDenseFillForTests = disabled;
  }
  ~MultiTermDenseFillGuard() {
    MultiTermQuery::disableDenseFillForTests = saved;
  }
};

class PhraseShapeGuard {
  bool saved;

public:
  explicit PhraseShapeGuard(bool disabled)
    : saved(PhraseQuery::disableShapesForTests) {
    PhraseQuery::disableShapesForTests = disabled;
  }
  ~PhraseShapeGuard() {
    PhraseQuery::disableShapesForTests = saved;
  }
};

class NumericRangeShapeGuard {
  bool saved;

public:
  explicit NumericRangeShapeGuard(bool disabled)
    : saved(NumericRangeQuery::disableShapesForTests) {
    NumericRangeQuery::disableShapesForTests = disabled;
  }
  ~NumericRangeShapeGuard() {
    NumericRangeQuery::disableShapesForTests = saved;
  }
};

class WindowFillSpanScaleGuard {
  int64_t saved;

public:
  explicit WindowFillSpanScaleGuard(int64_t scale)
    : saved(BooleanQuery::windowFillSpanScaleForTests) {
    BooleanQuery::windowFillSpanScaleForTests = scale;
  }
  ~WindowFillSpanScaleGuard() {
    BooleanQuery::windowFillSpanScaleForTests = saved;
  }
};

class CountDenseThresholdGuard {
  int32_t savedDense;
  int32_t savedTermTail;

public:
  explicit CountDenseThresholdGuard(int32_t inverse)
    : savedDense(BooleanQuery::ConjunctionBulkScorer::
                     denseThresholdInverseForTests),
      savedTermTail(BooleanQuery::ConjunctionBulkScorer::
                        termTailDenseThresholdInverseForTests) {
    BooleanQuery::ConjunctionBulkScorer::denseThresholdInverseForTests =
        inverse;
    BooleanQuery::ConjunctionBulkScorer::termTailDenseThresholdInverseForTests =
        inverse;
  }
  ~CountDenseThresholdGuard() {
    BooleanQuery::ConjunctionBulkScorer::denseThresholdInverseForTests =
        savedDense;
    BooleanQuery::ConjunctionBulkScorer::termTailDenseThresholdInverseForTests =
        savedTermTail;
  }
};

class FilterClauseCountGuard {
  bool saved;

public:
  explicit FilterClauseCountGuard(bool disabled)
    : saved(BooleanQuery::disableFilterClauseCountForTests) {
    BooleanQuery::disableFilterClauseCountForTests = disabled;
  }
  ~FilterClauseCountGuard() {
    BooleanQuery::disableFilterClauseCountForTests = saved;
  }
};

class FilteredDisjunctionBatchGuard {
  bool saved;

public:
  explicit FilteredDisjunctionBatchGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredDisjunctionBatchForTests) {
    BooleanQuery::disableFilteredDisjunctionBatchForTests = disabled;
  }
  ~FilteredDisjunctionBatchGuard() {
    BooleanQuery::disableFilteredDisjunctionBatchForTests = saved;
  }
};

class FilteredConjunctionBatchGuard {
  bool saved;

public:
  explicit FilteredConjunctionBatchGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredConjunctionBatchForTests) {
    BooleanQuery::disableFilteredConjunctionBatchForTests = disabled;
  }
  ~FilteredConjunctionBatchGuard() {
    BooleanQuery::disableFilteredConjunctionBatchForTests = saved;
  }
};

class IntegratedFilteredCountGuard {
  bool saved;

public:
  explicit IntegratedFilteredCountGuard(bool disabled)
    : saved(BooleanQuery::disableIntegratedFilteredCountForTests) {
    BooleanQuery::disableIntegratedFilteredCountForTests = disabled;
  }
  ~IntegratedFilteredCountGuard() {
    BooleanQuery::disableIntegratedFilteredCountForTests = saved;
  }
};

class ExactTermCountGuard {
  bool saved;

public:
  explicit ExactTermCountGuard(bool disabled)
    : saved(BooleanQuery::disableExactTermCountForTests) {
    BooleanQuery::disableExactTermCountForTests = disabled;
  }
  ~ExactTermCountGuard() {
    BooleanQuery::disableExactTermCountForTests = saved;
  }
};

class FilteredDisjunctionCountCompactionGuard {
  bool saved;

public:
  explicit FilteredDisjunctionCountCompactionGuard(bool disabled)
    : saved(BooleanQuery::
        disableFilteredDisjunctionCountCompactionForTests) {
    BooleanQuery::disableFilteredDisjunctionCountCompactionForTests = disabled;
  }
  ~FilteredDisjunctionCountCompactionGuard() {
    BooleanQuery::disableFilteredDisjunctionCountCompactionForTests = saved;
  }
};

class TopKCountCompositionGuard {
  bool saved;
  int32_t savedDensityInverse;

public:
  TopKCountCompositionGuard(bool disabled, int32_t densityInverse)
    : saved(disableTopKCountComposition),
      savedDensityInverse(
          TopDocsReq::exactCountTopKMinCandidateDensityInverseForTests) {
    disableTopKCountComposition = disabled;
    TopDocsReq::exactCountTopKMinCandidateDensityInverseForTests =
        densityInverse;
  }
  ~TopKCountCompositionGuard() {
    disableTopKCountComposition = saved;
    TopDocsReq::exactCountTopKMinCandidateDensityInverseForTests =
        savedDensityInverse;
  }
};

class SparseExactCountSinglePassGuard {
  bool savedDisabled;
  int32_t savedDensityInverse;

public:
  SparseExactCountSinglePassGuard(
      bool disabled, int32_t densityInverse =
          TopDocsReq::
              kExactCountTopKSparseFilterSinglePassDensityInverse)
    : savedDisabled(
          TopDocsReq::
              disableExactCountTopKSparseFilterSinglePassForTests),
      savedDensityInverse(
          TopDocsReq::
              exactCountTopKSparseFilterSinglePassDensityInverseForTests) {
    TopDocsReq::disableExactCountTopKSparseFilterSinglePassForTests =
        disabled;
    TopDocsReq::
        exactCountTopKSparseFilterSinglePassDensityInverseForTests =
            densityInverse;
  }

  ~SparseExactCountSinglePassGuard() {
    TopDocsReq::disableExactCountTopKSparseFilterSinglePassForTests =
        savedDisabled;
    TopDocsReq::
        exactCountTopKSparseFilterSinglePassDensityInverseForTests =
            savedDensityInverse;
  }
};

class SparseFilteredTopKRerouteGuard {
  bool savedDisabled;
  bool savedUnionDisabled;
  std::array<int32_t, 3> savedDensityInverse;
  std::array<int32_t, 3> savedUnionDensityInverse;

public:
  explicit SparseFilteredTopKRerouteGuard(
      bool disabled, bool unionDisabled = false,
      std::array<int32_t, 3> densityInverse =
          TopDocsReq::kSparseFilteredTopKDensityInverse,
      std::array<int32_t, 3> unionDensityInverse =
          TopDocsReq::kSparseFilteredTopKUnionDensityInverse)
    : savedDisabled(TopDocsReq::disableSparseFilteredTopKRerouteForTests),
      savedUnionDisabled(
          TopDocsReq::disableSparseFilteredTopKUnionForTests),
      savedDensityInverse(
          TopDocsReq::sparseFilteredTopKDensityInverseForTests),
      savedUnionDensityInverse(
          TopDocsReq::sparseFilteredTopKUnionDensityInverseForTests) {
    TopDocsReq::disableSparseFilteredTopKRerouteForTests = disabled;
    TopDocsReq::disableSparseFilteredTopKUnionForTests = unionDisabled;
    TopDocsReq::sparseFilteredTopKDensityInverseForTests = densityInverse;
    TopDocsReq::sparseFilteredTopKUnionDensityInverseForTests =
        unionDensityInverse;
  }

  ~SparseFilteredTopKRerouteGuard() {
    TopDocsReq::disableSparseFilteredTopKRerouteForTests =
        savedDisabled;
    TopDocsReq::disableSparseFilteredTopKUnionForTests =
        savedUnionDisabled;
    TopDocsReq::sparseFilteredTopKDensityInverseForTests =
        savedDensityInverse;
    TopDocsReq::sparseFilteredTopKUnionDensityInverseForTests =
        savedUnionDensityInverse;
  }
};

std::vector<std::string> resultIds(const LocalReq& req, std::string_view opName) {
  std::vector<std::string> out;
  const auto* docs = req.docList(opName);
  if (docs == nullptr) return out;
  const auto* column = docs->columns.find("id");
  if (column == nullptr) return out;
  const auto* ids = std::get_if<api::ColStr>(&column->kind);
  if (ids == nullptr) return out;
  for (auto id : ids->v) out.emplace_back(id);
  return out;
}

void appendRawFilter(OpCursor& cursor, std::string_view name,
                     const api::Query& query) {
  auto& topDocs = std::get<api::TopDocs>(cursor.rawOp().kind);
  auto old = topDocs.filter;
  api::NamedQuery* filters =
      api::build::allocArray(topDocs.filter, old.size() + 1, cursor.mr());
  std::copy(old.begin(), old.end(), filters);
  filters[old.size()].name = api::build::arenaStr(cursor.mr(), name);
  auto* stored = (api::Query*)cursor.mr().allocate(
      sizeof(api::Query), alignof(api::Query));
  new (stored) api::Query(query);
  filters[old.size()].query = stored;
}

std::map<std::string, float> resultScoreMap(const LocalReq& req,
                                             std::string_view opName) {
  std::map<std::string, float> out;
  const auto* docs = req.docList(opName);
  if (docs == nullptr) return out;
  const auto* idColumn = docs->columns.find("id");
  const auto* scoreColumn = docs->columns.find("_score_");
  if (idColumn == nullptr || scoreColumn == nullptr) return out;
  const auto* ids = std::get_if<api::ColStr>(&idColumn->kind);
  const auto* scores = std::get_if<api::ColFloat>(&scoreColumn->kind);
  if (ids == nullptr || scores == nullptr || ids->v.size() != scores->v.size()) return out;
  for (size_t i = 0; i < ids->v.size(); i++) {
    out.emplace(std::string(ids->v[i]), scores->v[i]);
  }
  return out;
}

std::map<std::string, int64_t> resultFacetMap(const LocalReq& req,
                                               std::string_view opName,
                                               std::string_view facetName) {
  std::map<std::string, int64_t> out;
  const auto* docs = req.docList(opName);
  if (docs == nullptr) return out;
  const auto* value = docs->ops.find(facetName);
  if (value == nullptr) return out;
  const auto* facet = std::get_if<api::FacetResult>(&(*value)->kind);
  if (facet == nullptr || !facet->bucket_ids.has_value()) return out;
  const auto* ids = std::get_if<api::ColStr>(&facet->bucket_ids->kind);
  if (ids == nullptr || ids->v.size() != facet->counts.size()) return out;
  for (size_t i = 0; i < ids->v.size(); i++) {
    out.emplace(std::string(ids->v[i]), facet->counts[i]);
  }
  return out;
}

void expectSameScoreMap(const std::map<std::string, float>& expected,
                        const std::map<std::string, float>& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (const auto& [id, expectedScore] : expected) {
    auto found = actual.find(id);
    ASSERT_NE(found, actual.end()) << id;
    float scale = std::max({std::fabs(expectedScore), std::fabs(found->second), 1.0f});
    EXPECT_NEAR(expectedScore, found->second, 1e-6f * scale) << id;
  }
}

// FieldFacet.missing has no fluent OpCursor setter; reach through the raw op.
OpCursor& facetMissing(OpCursor& cur) {
  std::get<solux::api::FieldFacet>(cur.rawOp().kind).missing = true;
  return cur;
}

enum class FilteredCountShape {
  TERM,
  INTERSECTION,
  UNION,
  PHRASE,
};

std::string_view filteredCountShapeName(FilteredCountShape shape) {
  switch (shape) {
    case FilteredCountShape::TERM: return "term";
    case FilteredCountShape::INTERSECTION: return "intersection";
    case FilteredCountShape::UNION: return "union";
    case FilteredCountShape::PHRASE: return "phrase";
  }
  std::unreachable();
}

api::Query filteredCountBody(std::pmr::memory_resource& mr,
                             FilteredCountShape shape) {
  switch (shape) {
    case FilteredCountShape::TERM:
      return qb::match(mr, "body_w", "alpha");
    case FilteredCountShape::INTERSECTION:
      return qb::boolean(mr,
          {qb::match(mr, "body_w", "alpha"),
           qb::match(mr, "body_w", "beta")});
    case FilteredCountShape::UNION:
      return qb::boolean(mr, {},
          {qb::match(mr, "body_w", "beta"),
           qb::match(mr, "body_w", "gamma")});
    case FilteredCountShape::PHRASE:
      return qb::phraseWords(mr, "body_w", {"quick", "fox"});
  }
  std::unreachable();
}

class SkipStatsGuard {
  bool saved;

public:
  SkipStatsGuard() : saved(SkipStats::enabled) {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
  }
};

struct SparseFilteredTopKResult {
  std::vector<std::string> ids;
  std::map<std::string, float> scores;
  int64_t reroutes;
  int64_t unionReroutes;
  int64_t densityRejects;
  int64_t shapeRejects;
  int64_t disjunctionBatchScoreWindows;
};

void indexSparseFilteredTopKDocs(CollectionHelper& helper) {
  constexpr int32_t nDocs = 1024;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string filter;
    if ((doc % 128) == 0) filter += "sparse ";
    if ((doc % 48) == 0) filter += "between ";
    if ((doc & 1) == 0) filter += "dense";
    std::string body = "alpha beta quick fox";
    if ((doc % 3) == 0) body += " gamma";
    docs.push_back(flatdoc(
        "id", "reroute_" + std::to_string(doc),
        "body_w", body,
        "filter_w", filter));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
}

SparseFilteredTopKResult runSparseFilteredTopK(
    SearchEngine& engine, std::string_view collection,
    FilteredCountShape shape, std::string_view filter, int64_t topCount,
    bool exact = false) {
  auto req = localReq(engine);
  req->collection(collection);
  auto& cur = req->topDocs("q").getScores().fields({"id"}).limit(topCount);
  if (exact) cur.getNumber();
  cur.rawQuery() = filteredCountBody(cur.mr(), shape);
  if (!filter.empty()) {
    cur.matchFilter("filter", "filter_w", filter);
  }
  SkipStatsGuard statsGuard;
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return {
    resultIds(*req, "q"),
    resultScoreMap(*req, "q"),
    SkipStats::sparseFilteredTopKReroutes,
    SkipStats::sparseFilteredTopKUnionReroutes,
    SkipStats::sparseFilteredTopKDensityRejects,
    SkipStats::sparseFilteredTopKShapeRejects,
    SkipStats::filteredDisjBatchScoreWindows,
  };
}

struct FilteredCountResult {
  int64_t count;
  int64_t denseWindows;
  int64_t disjGroupWindows;
  int64_t sparseFallbacks;
  int64_t bulkFillCalls;
  int64_t docsOnlyWordProbeAdvances;
  int64_t tfreqBlocksDecoded;
  int64_t filteredDisjBatchEngagements;
  int64_t filteredDisjBatchPostingsFeedEngagements;
  int64_t filteredDisjBatchCountWindows;
  int64_t filteredDisjBatchScoreWindows;
  int64_t filteredConjBatchCountWindows;
  int64_t filteredCountCandidateAdmits;
  int64_t filteredCountCandidateDenseLatchBacks;
  int64_t ownedFilterMaterializations;
  int64_t ownedFilterServes;
  int64_t exactTermCountBatches;
};

enum class FilteredCountPath {
  FOLDED,
  PLAIN_INTERSECTION,
  MATERIALIZED,
};

FilteredCountResult runFilteredCount(SearchEngine& engine,
                                     FilteredCountShape shape,
                                     std::string_view filterTerm,
                                     FilteredCountPath path,
                                     std::string_view collection = "main") {
  auto req = localReq(engine);
  req->collection(collection);
  auto& cur = req->topDocs("q").getNumber().limit(0);
  api::Query body = filteredCountBody(cur.mr(), shape);
  if (path == FilteredCountPath::PLAIN_INTERSECTION) {
    cur.rawQuery() = qb::boolean(cur.mr(),
        {body, qb::match(cur.mr(), "filter_w", filterTerm)});
  } else {
    cur.rawQuery() = body;
    cur.matchFilter("filter", "filter_w", filterTerm);
  }

  SkipStatsGuard statsGuard;
  {
    TopDocsFilterFoldGuard foldGuard(path == FilteredCountPath::MATERIALIZED);
    req->execute(false);
  }
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return {
    req->getMatchCount("q"),
    SkipStats::conjDenseCountWindows,
    SkipStats::conjDisjGroupCountWindows,
    SkipStats::conjCountFallbacks,
    SkipStats::countBulkFillCalls,
    SkipStats::docsOnlyWordProbeAdvances,
    SkipStats::tfreqBlocksDecoded,
    SkipStats::filteredDisjBatchEngagements,
    SkipStats::filteredDisjBatchPostingsFeedEngagements,
    SkipStats::filteredDisjBatchCountWindows,
    SkipStats::filteredDisjBatchScoreWindows,
    SkipStats::filteredConjBatchCountWindows,
    SkipStats::filteredCountCandidateAdmits,
    SkipStats::filteredCountCandidateDenseLatchBacks,
    SkipStats::ownedFilterMaterializations,
    SkipStats::ownedFilterServes,
    SkipStats::exactTermCountBatches,
  };
}

FilteredCountResult runUnfilteredCount(SearchEngine& engine,
                                       FilteredCountShape shape,
                                       std::string_view collection) {
  auto req = localReq(engine);
  req->collection(collection);
  auto& cur = req->topDocs("q").getNumber().limit(0);
  cur.rawQuery() = filteredCountBody(cur.mr(), shape);

  SkipStatsGuard statsGuard;
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return {
    req->getMatchCount("q"),
    SkipStats::conjDenseCountWindows,
    SkipStats::conjDisjGroupCountWindows,
    SkipStats::conjCountFallbacks,
    SkipStats::countBulkFillCalls,
    SkipStats::docsOnlyWordProbeAdvances,
    SkipStats::tfreqBlocksDecoded,
    SkipStats::filteredDisjBatchEngagements,
    SkipStats::filteredDisjBatchPostingsFeedEngagements,
    SkipStats::filteredDisjBatchCountWindows,
    SkipStats::filteredDisjBatchScoreWindows,
    SkipStats::filteredConjBatchCountWindows,
    SkipStats::filteredCountCandidateAdmits,
    SkipStats::filteredCountCandidateDenseLatchBacks,
    SkipStats::ownedFilterMaterializations,
    SkipStats::ownedFilterServes,
    SkipStats::exactTermCountBatches,
  };
}

void indexFilteredCountDocs(CollectionHelper& helper, bool multiSegment) {
  constexpr int32_t segmentDocs = DocsEnumMeta::L1_DOCS + 193;
  int32_t segmentCount = multiSegment ? 2 : 1;
  for (int32_t segment = 0; segment < segmentCount; segment++) {
    std::vector<Doc> docs;
    docs.reserve(segmentDocs);
    for (int32_t local = 0; local < segmentDocs; local++) {
      int32_t doc = segment * segmentDocs + local;
      std::string body;
      if ((doc & 1) == 0) body += "alpha ";
      if ((doc % 3) != 0) body += "beta ";
      if ((doc % 5) != 0) body += "gamma ";
      body += (doc % 4) == 0 ? "quick fox" : "quick noise fox";

      std::string filter = (doc % 8) == 0 ? "other" : "fat";
      if ((doc % 8) != 0) {
        filter += " fat_term_single fat_intersection_single fat_union_single fat_phrase_single"
                  " fat_term_multi fat_intersection_multi fat_union_multi fat_phrase_multi";
      }
      if ((doc % 1021) == 0) {
        filter += " rare rare_term_single rare_intersection_single rare_union_single rare_phrase_single"
                  " rare_term_multi rare_intersection_multi rare_union_multi rare_phrase_multi";
      }
      docs.push_back(flatdoc(
          "id", "fc_" + std::to_string(doc),
          "body_w", body, "filter_w", filter));
    }
    auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
    ASSERT_TRUE(result.success) << result.error_message;
  }
}

void expectFilteredCountEquivalence(SearchEngine& engine, bool multiSegment) {
  CollectionHelper helper;
  indexFilteredCountDocs(helper, multiSegment);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), multiSegment ? 2u : 1u);

  constexpr std::array<FilteredCountShape, 4> shapes = {
    FilteredCountShape::TERM,
    FilteredCountShape::INTERSECTION,
    FilteredCountShape::UNION,
    FilteredCountShape::PHRASE,
  };
  constexpr std::array<std::string_view, 4> fatSingle = {
    "fat_term_single", "fat_intersection_single", "fat_union_single", "fat_phrase_single"};
  constexpr std::array<std::string_view, 4> fatMulti = {
    "fat_term_multi", "fat_intersection_multi", "fat_union_multi", "fat_phrase_multi"};
  constexpr std::array<std::string_view, 4> rareSingle = {
    "rare_term_single", "rare_intersection_single", "rare_union_single", "rare_phrase_single"};
  constexpr std::array<std::string_view, 4> rareMulti = {
    "rare_term_multi", "rare_intersection_multi", "rare_union_multi", "rare_phrase_multi"};
  const auto& fatFilters = multiSegment ? fatMulti : fatSingle;
  const auto& rareFilters = multiSegment ? rareMulti : rareSingle;
  for (size_t shapeOrd = 0; shapeOrd < shapes.size(); shapeOrd++) {
    FilteredCountShape shape = shapes[shapeOrd];
    std::array<std::string_view, 2> filters{
        fatFilters[shapeOrd], rareFilters[shapeOrd]};
    int64_t fatCount = 0;
    for (size_t filterOrd = 0; filterOrd < filters.size(); filterOrd++) {
      std::string_view filter = filters[filterOrd];
      auto folded = runFilteredCount(
          engine, shape, filter, FilteredCountPath::FOLDED);
      auto plain = runFilteredCount(
          engine, shape, filter, FilteredCountPath::PLAIN_INTERSECTION);
      auto materialized = runFilteredCount(
          engine, shape, filter, FilteredCountPath::MATERIALIZED);
      SCOPED_TRACE(std::string(filteredCountShapeName(shape)) + "/"
                   + std::string(filter));
      EXPECT_GT(plain.count, 0);
      EXPECT_EQ(folded.count, plain.count);
      EXPECT_EQ(folded.count, materialized.count);

      if (filterOrd == 0) {
        fatCount = folded.count;
        if (shape == FilteredCountShape::PHRASE) {
          EXPECT_EQ(folded.denseWindows, 0);
          EXPECT_EQ(folded.disjGroupWindows, 0);
          EXPECT_EQ(folded.sparseFallbacks, 0);
        } else if (shape == FilteredCountShape::INTERSECTION) {
          EXPECT_GT(folded.exactTermCountBatches, 0);
          EXPECT_EQ(folded.denseWindows, 0);
          EXPECT_EQ(folded.disjGroupWindows, 0);
        } else {
          EXPECT_TRUE(folded.denseWindows > 0
                      || folded.disjGroupWindows > 0);
        }
        if (shape == FilteredCountShape::UNION) {
          EXPECT_GT(folded.disjGroupWindows, 0);
        }
      } else {
        EXPECT_LT(folded.count, fatCount);
        EXPECT_EQ(folded.denseWindows, 0);
        EXPECT_EQ(folded.disjGroupWindows, 0);
        EXPECT_EQ(folded.sparseFallbacks, 0);
        if (shape == FilteredCountShape::INTERSECTION) {
          EXPECT_GT(folded.exactTermCountBatches, 0);
        }
      }
    }
  }
}

void appendRepeatedTerm(std::string& body, std::string_view term, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(term);
  }
}

void addPrunableFacetDocs(CollectionHelper& helper, int32_t segmentCount) {
  const int32_t segmentDocs = 3 * Postings::DOCS_BLOCK_SIZE + 40;
  for (int32_t segment = 0; segment < segmentCount; segment++) {
    std::vector<Doc> docs;
    docs.reserve((size_t) segmentDocs);
    for (int32_t local = 0; local < segmentDocs; local++) {
      int32_t doc = segment * segmentDocs + local;
      std::string body = "common";
      if (doc < 6) {
        int32_t rareTf = 8 - doc;
        appendRepeatedTerm(body, "rare", rareTf);
        appendRepeatedTerm(body, "pad", 8 - rareTf);
        body += " medium";
      } else {
        if ((doc % 9) == 0) body += " medium";
        appendRepeatedTerm(body, "filler", 60);
      }
      docs.push_back(flatdoc(
          "id", "p" + std::to_string(doc),
          "body_w", body,
          "group_s", "g" + std::to_string(doc % 4)));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
}

void setPrunableDisjunction(OpCursor& cur) {
  cur.rawQuery() = qb::boolean(
      cur.mr(), /*required=*/{},
      /*optional=*/{qb::match(cur.mr(), "body_w", "common"),
                    qb::match(cur.mr(), "body_w", "medium"),
                    qb::match(cur.mr(), "body_w", "rare")});
}

void expectPrunedFacetTwoPassMatchesExhaustive(CollectionHelper& helper,
                                                int32_t segmentCount) {
  addPrunableFacetDocs(helper, segmentCount);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), (size_t) segmentCount);
  const int64_t topK = 3;

  auto baseline = localReq(helper.getSearchEngine());
  baseline->collection("main");
  auto& baselineRank = baseline->topDocs("rank").getNumber().getScores()
      .fields({"id"}).limit(topK);
  setPrunableDisjunction(baselineRank);
  auto& baselineDomain = baseline->topDocs("domain").getNumber().limit(0);
  setPrunableDisjunction(baselineDomain);
  baselineDomain.facet("groups", "group_s").limit(-1);
  baseline->execute(false);
  ASSERT_OK(baseline);

  auto actual = localReq(helper.getSearchEngine());
  actual->collection("main");
  auto& actualRank = actual->topDocs("rank").getNumber().getScores()
      .fields({"id"}).limit(topK);
  setPrunableDisjunction(actualRank);
  actualRank.facet("groups", "group_s").limit(-1);

  int64_t pruningEvents = 0;
  int64_t domainWindows = 0;
  {
    SkipStatsGuard stats;
    actual->execute(false);
    pruningEvents = SkipStats::maxScoreBufferCompactions
        + SkipStats::maxScoreDeadOuterJumps;
    domainWindows = SkipStats::bulkDomainWindowsFed;
  }
  ASSERT_OK(actual);

  EXPECT_EQ(resultIds(*baseline, "rank"), resultIds(*actual, "rank"));
  EXPECT_EQ(resultScoreMap(*baseline, "rank"), resultScoreMap(*actual, "rank"));
  EXPECT_EQ(resultFacetMap(*baseline, "domain", "groups"),
            resultFacetMap(*actual, "rank", "groups"));
  EXPECT_EQ(baseline->getMatchCount("rank"), actual->getMatchCount("rank"));
  EXPECT_EQ(baseline->getMatchCount("domain"), actual->getMatchCount("rank"));
  EXPECT_GT(actual->getMatchCount("rank"), topK);
  EXPECT_GT(domainWindows, 0);
  EXPECT_GT(pruningEvents, 0);
}

void indexSparseConstantDispatchDocs(CollectionHelper& helper) {
  // These tests assert collector-path counters; filter admission/materialization
  // is independent work that would otherwise contaminate those counters.
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  std::vector<Doc> docs;
  docs.reserve(1024);
  for (int32_t doc = 0; doc < 1024; doc++) {
    docs.push_back(flatdoc(
        "id", "sc_" + std::to_string(doc),
        "limit_s", doc < 32 ? "yes" : "no",
        "over_s", doc < 33 ? "yes" : "no",
        "all_s", "yes",
        "group_s", "g" + std::to_string(doc % 4)));
  }
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(result.success) << result.error_message;
}

struct SparseConstantDispatchResult {
  std::vector<std::string> ids;
  std::map<std::string, int64_t> facets;
  int64_t found = 0;
  int64_t pullCollections = 0;
  int64_t domainWindows = 0;
  int64_t bulkFillCalls = 0;
};

SparseConstantDispatchResult runSparseConstantDispatch(
    SearchEngine& engine, std::string_view filterField,
    bool passive, bool secondFilter = false) {
  auto req = localReq(engine);
  req->collection("main");
  auto& topDocs = req->topDocs("q").allQuery().getNumber()
      .fields({"id"}).limit(10);
  if (!filterField.empty()) {
    topDocs.matchFilter("filter", filterField, "yes");
  }
  if (secondFilter) {
    topDocs.matchFilter("all", "all_s", "yes");
  }
  topDocs.facet("groups", "group_s").limit(-1);

  SparseConstantDispatchResult result;
  {
    SkipStatsGuard stats;
    TopDocsFilterFoldGuard fold(passive);
    req->execute(false);
    result.pullCollections = SkipStats::constantPullDomainCollections;
    result.domainWindows = SkipStats::bulkDomainWindowsFed;
    result.bulkFillCalls = SkipStats::countBulkFillCalls;
  }
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  result.ids = resultIds(*req, "q");
  result.facets = resultFacetMap(*req, "q", "groups");
  result.found = req->getMatchCount("q");
  return result;
}
}  // namespace

class SearchEngineTest : public SoluxTest {
public:
};

TEST_F(SearchEngineTest, prunedTopDocsFacetTwoPassMatchesExhaustive) {
  CollectionHelper helper;
  expectPrunedFacetTwoPassMatchesExhaustive(helper, 1);
}

TEST_F(SearchEngineTest, prunedTopDocsFacetTwoPassMatchesExhaustiveMultiSegment) {
  CollectionHelper helper;
  expectPrunedFacetTwoPassMatchesExhaustive(helper, 3);
}

TEST_F(SearchEngineTest, statsOpsEmptyIndexEmitNan) {
  CollectionHelper helper;

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->requestId("test_stats_ops_empty_index_emit_nan");

  auto& topDocs = req->topDocs("q").allQuery().getNumber();
  topDocs.avg("nested_avg", "foo_i");
  topDocs.sum("nested_sum", "foo_i");

  req->avg("root_avg", "foo_i");
  req->sum("root_sum", "foo_i");
  req->min("root_min", "foo_i");
  req->max("root_max", "foo_i");

  req->execute();

  ASSERT_EQ(1u, req->responses.size()) << req->toString();
  const auto& response = req->responses[0]->proto;
  ASSERT_FALSE(hasError(response)) << req->toString();
  ASSERT_TRUE(response.ops.contains("root_avg")) << req->toString();
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_avg")));
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_sum")));
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_min")));
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_max")));

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(0, docs->found.value_or(0));
  ASSERT_TRUE(docs->ops.contains("nested_avg")) << req->toString();
  EXPECT_TRUE(std::isnan(std::get<double>(docs->ops.at("nested_avg")->kind)));
  EXPECT_TRUE(std::isnan(std::get<double>(docs->ops.at("nested_sum")->kind)));
}

// Root-level min/max across 2 segments over int, float, double, and
// multi-valued int fields.  Negative values matter: the sortable encodings
// must keep ordering across the sign, since min/max compare raw encoded bits.
TEST_F(SearchEngineTest, minMaxOps) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("foo_i", -8, "foo_f", -2.5, "foo_d", 3.5,
                       "prices_is", vec_i(20, 35, 45)), UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_i", 23, "foo_f", 1.25, "foo_d", -1e100,
                       "prices_is", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_i", 5), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().getNumber();
  req->min("min_i", "foo_i");
  req->max("max_i", "foo_i");
  req->min("min_f", "foo_f");
  req->max("max_f", "foo_f");
  req->min("min_d", "foo_d");
  req->max("max_d", "foo_d");
  req->min("min_is", "prices_is");
  req->max("max_is", "prices_is");

  req->execute();

  ASSERT_EQ(1u, req->responses.size()) << req->toString();
  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
  EXPECT_EQ(-8.0, req->scalar<double>("min_i"));
  EXPECT_EQ(23.0, req->scalar<double>("max_i"));
  EXPECT_EQ(-2.5, req->scalar<double>("min_f"));
  EXPECT_EQ(1.25, req->scalar<double>("max_f"));
  EXPECT_EQ(-1e100, req->scalar<double>("min_d"));
  EXPECT_EQ(3.5, req->scalar<double>("max_d"));
  EXPECT_EQ(3.0, req->scalar<double>("min_is"));
  EXPECT_EQ(45.0, req->scalar<double>("max_is"));
}

TEST_F(SearchEngineTest, sumOps) {
  CollectionHelper helper;
  helper.clear();
  constexpr int64_t twoTo53 = int64_t{1} << 53;
  helper.index(flatdoc("foo_i", twoTo53, "foo_f", 1.5,
                       "foo_d", 3.5, "prices_is", vec_i(20, 35, 45)),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_i", 1, "foo_f", -2.25,
                       "foo_d", -1.25, "prices_is", 3),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_i", -twoTo53, "foo_f", 0.75),
               UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  auto& topDocs = req->topDocs("q").allQuery().getNumber();
  topDocs.sum("nested_i", "foo_i");
  req->sum("sum_i", "foo_i");
  req->sum("sum_f", "foo_f");
  req->sum("sum_d", "foo_d");
  req->sum("sum_is", "prices_is");

  req->execute();

  ASSERT_OK(req);
  EXPECT_EQ(1.0, req->scalar<double>("sum_i"));
  EXPECT_EQ(0.0, req->scalar<double>("sum_f"));
  EXPECT_EQ(2.25, req->scalar<double>("sum_d"));
  EXPECT_EQ(103.0, req->scalar<double>("sum_is"));
  EXPECT_EQ(1.0, std::get<double>(req->docList("q")->ops.at("nested_i")->kind));
}

TEST_F(SearchEngineTest, sumRejectsDateFields) {
  CollectionHelper helper;
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->sum("bad", "when_dt");

  req->execute();

  ASSERT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("cannot sum DATE field 'when_dt'"));
}

// limit 0 ("count/aggregate only, no docs") must return an accurate count and any
// sub-op results without collecting or ranking documents - and must never crash the
// top-K collector (which used to assert topCount > 0 / build a zero-capacity heap).
TEST_F(SearchEngineTest, limitZeroCountsWithoutDocs) {
  CollectionHelper helper;
  helper.index(flatdoc("foo_w", "brown cow", "foo_i", 17, "color_s", "red"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w", "charlie brown", "foo_i", 23, "color_s", "blue"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w", "brown", "foo_i", 5, "color_s", "brown"), UpdateMessage::COMMIT);

  // Score path: match query, limit 0 + get_number.  Accurate count, zero docs.
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("q").matchQuery("foo_w", "brown").getNumber().limit(0);
    req->execute();

    ASSERT_OK(req);
    EXPECT_EQ(3, req->getMatchCount("q"));
    EXPECT_TRUE(req->getDocs("q").empty());
  }

  // A facet sub-op under a limit-0 topDocs still sees the full matching domain.
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& q = req->topDocs("q").matchQuery("foo_w", "brown").getNumber().limit(0);
    q.facet("colors", "color_s").limit(-1);
    req->execute();

    ASSERT_OK(req);
    EXPECT_EQ(3, req->getMatchCount("q"));
    EXPECT_TRUE(req->getDocs("q").empty());

    const auto& docs = *req->docList("q");
    const auto& facet = std::get<solux::api::FacetResult>(docs.ops.at("colors")->kind);
    int64_t facetTotal = 0;
    for (auto c : facet.counts) facetTotal += c;
    EXPECT_EQ(3, facetTotal);
  }

  // Field-sort path: sort by a column, limit 0 + get_number.  Accurate count, zero docs.
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& q = req->topDocs("q").matchQuery("foo_w", "brown").getNumber().limit(0);
    qb::sort(q, "foo_i", qb::ASC);
    req->execute();

    ASSERT_OK(req);
    EXPECT_EQ(3, req->getMatchCount("q"));
    EXPECT_TRUE(req->getDocs("q").empty());
  }
}

TEST_F(SearchEngineTest, singleTermLimitZeroFacetFallsBackOnCacheMiss) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  docs.reserve(256);
  for (int32_t i = 0; i < 256; i++) {
    std::string body = (i % 3) == 0 ? "needle filler" : "filler";
    std::string color = (i % 2) == 0 ? "red" : "blue";
    docs.push_back(flatdoc("id", "bdf_" + std::to_string(i),
                           "foo_w", body, "color_s", color));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  auto& q = req->topDocs("q").matchQuery("foo_w", "needle").getNumber().limit(0);
  q.facet("colors", "color_s").limit(-1);
  req->execute(false);

  ASSERT_OK(req);
  EXPECT_EQ(86, req->getMatchCount("q"));
  const auto& docsOut = *req->docList("q");
  const auto& facet = std::get<solux::api::FacetResult>(docsOut.ops.at("colors")->kind);
  int64_t facetTotal = 0;
  for (auto c : facet.counts) facetTotal += c;
  EXPECT_EQ(86, facetTotal);
  EXPECT_EQ(0, SkipStats::exactDomainDocSetCollections);
  EXPECT_GT(SkipStats::exactDomainStreamFallbacks, 0);
  EXPECT_GT(SkipStats::bulkDomainWindowsFed, 0);

  SkipStats::enabled = savedStats;
}

TEST_F(SearchEngineTest, basic) {
  bool para = true;

  CollectionHelper helper;
  helper.index(flatdoc("foo_w","how now brown cow", "foo_i", 17, "color_s","red", "colors_ss", "red", "prices_is", vec_i(20, 35, 45)),UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w","charlie brown", "foo_i", 23, "color_s","blue", "prices_is", 35),UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w","brown", "foo_i", 5, "color_s","brown", "colors_ss",vecs("red","black")),UpdateMessage::COMMIT);
  // should be 2 segments now.

  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");

    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s", "colors_ss", "prices_is"});
    size_t ncols = 4 + 1; // 4 requested fields + _score_

    req->facet("f", "foo_i");
    facetMissing(req->facet("f2", "prices_is")); // include missing values in the facet
    facetMissing(req->facet("f3", "noexist_i")); // include missing values in the facet
    auto& f4 = req->facet("f4", "color_s").limit(-1);
    f4.avg("avgsub", "foo_i");
    qb::sort(f4, "avgsub", qb::ASC); // sort by avg ascending
    req->facet("f5", "colors_ss");
    req->facet("f6", "prices_is").mincount(2);
    req->facet("f7", "colors_ss").mincount(2);
    req->rangeFacet("f8", "foo_i").range(-5, 34, 20);
    req->facet("f9", "foo_w");
    req->avg("avg", "foo_i");

    req->execute(para);
    // LOG_DEBUG("ENGINE REQ: {}", req->toString());

    const auto& resp = req->responses[0]->proto;
    ASSERT_EQ(req->proto.request_id, resp.request_id);
    const auto& docs = *req->docList("q");
    ASSERT_EQ(3, docs.found.value_or(0));
    ASSERT_EQ(ncols, docs.columns.size());

    // Accessors: column arms (columns is a plain map) and facet arms (ops is indirect).
    auto colI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(docs.columns.at(n).kind);
    };
    auto colS = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(docs.columns.at(n).kind);
    };
    auto multiS = [&](const char* n) -> const solux::api::ArrArrStr& {
      return std::get<solux::api::ArrArrStr>(docs.columns.at(n).kind);
    };
    auto multiI = [&](const char* n) -> const solux::api::ArrArrInt& {
      return std::get<solux::api::ArrArrInt>(docs.columns.at(n).kind);
    };
    auto facetOf = [&](const char* n) -> const solux::api::FacetResult& {
      return std::get<solux::api::FacetResult>(resp.ops.at(n)->kind);
    };
    auto fBidsI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(facetOf(n).bucket_ids->kind);
    };
    auto fBidsS = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(facetOf(n).bucket_ids->kind);
    };
    auto fBidsMultiI = [&](const char* n) -> const solux::api::ArrArrInt& {
      return std::get<solux::api::ArrArrInt>(facetOf(n).bucket_ids->kind);
    };

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(5, colI("foo_i").v[0]);
    ASSERT_EQ("brown", colS("color_s").v[0]);
    ASSERT_EQ(23, colI("foo_i").v[1]);
    ASSERT_EQ("blue", colS("color_s").v[1]);
    ASSERT_EQ(17, colI("foo_i").v[2]);
    ASSERT_EQ("red", colS("color_s").v[2]);

    // check the multi-valued strings
    ASSERT_EQ(2, multiS("colors_ss").v[0].v.size());
    ASSERT_EQ("black", multiS("colors_ss").v[0].v[0]);
    ASSERT_EQ(0, multiS("colors_ss").v[1].v.size()); // missing for this doc
    ASSERT_EQ(1, multiS("colors_ss").v[2].v.size()); // single-valued for this doc
    ASSERT_EQ("red", multiS("colors_ss").v[2].v[0]);

    // check the multi-valued integers
    ASSERT_EQ(3, multiI("prices_is").v[2].v.size());
    ASSERT_EQ(20, multiI("prices_is").v[2].v[0]);
    ASSERT_EQ(35, multiI("prices_is").v[2].v[1]);
    ASSERT_EQ(45, multiI("prices_is").v[2].v[2]);
    ASSERT_EQ(1, multiI("prices_is").v[1].v.size()); // single-valued for this doc
    ASSERT_EQ(35, multiI("prices_is").v[1].v[0]);
    ASSERT_EQ(0, multiI("prices_is").v[0].v.size()); // missing for this doc

    // check the facet
    ASSERT_EQ(3, fBidsI("f").v.size());
    ASSERT_EQ(5, fBidsI("f").v[0]);
    ASSERT_EQ(17, fBidsI("f").v[1]);
    ASSERT_EQ(23, fBidsI("f").v[2]);
    ASSERT_EQ(3, facetOf("f").counts.size());
    ASSERT_EQ(1, facetOf("f").counts.at(0));
    ASSERT_EQ(1, facetOf("f").counts.at(1));
    ASSERT_EQ(1, facetOf("f").counts.at(2));
    //check for the abscence of missing
    ASSERT_FALSE(facetOf("f").missing.has_value());

    // check the second facet
    ASSERT_EQ(3, fBidsI("f2").v.size());
    ASSERT_EQ(35, fBidsI("f2").v[0]);
    ASSERT_EQ(20, fBidsI("f2").v[1]);
    ASSERT_EQ(45, fBidsI("f2").v[2]);
    ASSERT_EQ(3, facetOf("f2").counts.size());
    ASSERT_EQ(2, facetOf("f2").counts.at(0));
    ASSERT_EQ(1, facetOf("f2").counts.at(1));
    ASSERT_EQ(1, facetOf("f2").counts.at(2));
    ASSERT_EQ(1, facetOf("f2").missing.value());

    //check the third facet
    ASSERT_EQ(0, fBidsI("f3").v.size());
    ASSERT_EQ(3, facetOf("f3").missing.value());

    // check the fourth facet
    ASSERT_EQ(3, fBidsS("f4").v.size());
    ASSERT_EQ("brown", fBidsS("f4").v[0]);
    ASSERT_EQ("red", fBidsS("f4").v[1]);
    ASSERT_EQ("blue", fBidsS("f4").v[2]);
    ASSERT_EQ(3, facetOf("f4").counts.size());
    ASSERT_EQ(1, facetOf("f4").counts.at(0));
    ASSERT_EQ(1, facetOf("f4").counts.at(1));
    ASSERT_EQ(1, facetOf("f4").counts.at(2));
    // check the sub-op avg
    ASSERT_EQ(3, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v.size());
    ASSERT_EQ(5, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v[0]);
    ASSERT_EQ(17, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v[1]);
    ASSERT_EQ(23, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v[2]);

    // check the fifth facet
    ASSERT_EQ(2, fBidsS("f5").v.size());
    ASSERT_EQ("red", fBidsS("f5").v[0]);
    ASSERT_EQ("black", fBidsS("f5").v[1]);
    ASSERT_EQ(2, facetOf("f5").counts.size());
    ASSERT_EQ(2, facetOf("f5").counts.at(0));
    ASSERT_EQ(1, facetOf("f5").counts.at(1));

    //check the sixth facet
    ASSERT_EQ(1, fBidsI("f6").v.size());
    ASSERT_EQ(35, fBidsI("f6").v[0]);
    ASSERT_EQ(1, facetOf("f6").counts.size());
    ASSERT_EQ(2, facetOf("f6").counts.at(0));

    //check the seventh facet
    ASSERT_EQ(1, fBidsS("f7").v.size());
    ASSERT_EQ("red", fBidsS("f7").v[0]);
    ASSERT_EQ(1, facetOf("f7").counts.size());
    ASSERT_EQ(2, facetOf("f7").counts.at(0));

    //check the eighth facet
    ASSERT_EQ(2, fBidsMultiI("f8").v.size());
    ASSERT_EQ(-5, fBidsMultiI("f8").v[0].v[0]);
    ASSERT_EQ(15, fBidsMultiI("f8").v[0].v[1]);
    ASSERT_EQ(15, fBidsMultiI("f8").v[1].v[0]);
    ASSERT_EQ(34, fBidsMultiI("f8").v[1].v[1]);
    ASSERT_EQ(2, facetOf("f8").counts.size());
    ASSERT_EQ(1, facetOf("f8").counts.at(0));
    ASSERT_EQ(2, facetOf("f8").counts.at(1));

    //check the ninth facet
    ASSERT_EQ(5, fBidsS("f9").v.size());
    ASSERT_EQ("brown", fBidsS("f9").v[0]);
    ASSERT_EQ("charlie", fBidsS("f9").v[1]);
    ASSERT_EQ("cow", fBidsS("f9").v[2]);
    ASSERT_EQ("how", fBidsS("f9").v[3]);
    ASSERT_EQ("now", fBidsS("f9").v[4]);
    ASSERT_EQ(5, facetOf("f9").counts.size());
    ASSERT_EQ(3, facetOf("f9").counts.at(0));
    ASSERT_EQ(1, facetOf("f9").counts.at(1));
    ASSERT_EQ(1, facetOf("f9").counts.at(2));
    ASSERT_EQ(1, facetOf("f9").counts.at(3));
    ASSERT_EQ(1, facetOf("f9").counts.at(4));

    // check the avg
    ASSERT_EQ(std::get<double>(resp.ops.at("avg")->kind), 15);
  }


#ifdef REMOVED
  // FIXME
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");

    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s", "colors_ss"});

    req->facet("f", "foo_i").limit(2);
    req->facet("f2", "foo_i").limit(1);

    req->execute(para);
    // LOG_DEBUG("ENGINE REQ: {}", req->toString());

    const auto& resp = req->responses[0]->proto;
    auto fBidsI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(std::get<solux::api::FacetResult>(resp.ops.at(n)->kind).bucket_ids->kind);
    };
    auto fCounts = [&](const char* n) -> const auto& {
      return std::get<solux::api::FacetResult>(resp.ops.at(n)->kind).counts;
    };

    // check the facet
    ASSERT_EQ(2, fBidsI("f").v.size());
    ASSERT_EQ(5, fBidsI("f").v[0]);
    ASSERT_EQ(17, fBidsI("f").v[1]);
    ASSERT_EQ(2, fCounts("f").size());
    ASSERT_EQ(1, fCounts("f").at(0));
    ASSERT_EQ(1, fCounts("f").at(1));

    //check the second facet
    ASSERT_EQ(1, fBidsI("f2").v.size());
    ASSERT_EQ(5, fBidsI("f2").v[0]);
    ASSERT_EQ(1, fCounts("f2").size());
    ASSERT_EQ(1, fCounts("f2").at(0));
  }
#endif


  // now lets do the same request, but try to get multiple responses.
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");
    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s"}).batchSize(2);

    req->execute(para);
// LOG_DEBUG("ENGINE REQ: {}", req->toString());

    ASSERT_EQ(req->proto.request_id, req->responses[0]->proto.request_id);
    const auto& docs = *req->docList("q");
    auto colI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(docs.columns.at(n).kind);
    };
    auto colS = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(docs.columns.at(n).kind);
    };
    ASSERT_EQ(3, docs.found.value_or(0));
    ASSERT_EQ(3, docs.columns.size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(2, colI("foo_i").v.size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(2, colS("color_s").v.size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(5, colI("foo_i").v[0]);
    ASSERT_EQ("brown", colS("color_s").v[0]);
    ASSERT_EQ(23, colI("foo_i").v[1]);
    ASSERT_EQ("blue", colS("color_s").v[1]);
    // check that "more" flags are set both at DocList level and at Response level
    ASSERT_TRUE(docs.more);
    ASSERT_TRUE(req->responses[0]->proto.more);


    ASSERT_EQ(req->proto.request_id, req->responses[1]->proto.request_id);
    const auto& docs2 = std::get<solux::api::DocList>(req->responses[1]->proto.ops.at("q")->kind);
    auto colI2 = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(docs2.columns.at(n).kind);
    };
    auto colS2 = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(docs2.columns.at(n).kind);
    };
    ASSERT_EQ(3, docs2.found.value_or(0));
    ASSERT_EQ(2, docs2.offset);
    ASSERT_EQ(3, docs2.columns.size());
    ASSERT_EQ(1, colI2("foo_i").v.size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(1, colS2("color_s").v.size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(17, colI2("foo_i").v[0]);
    ASSERT_EQ("red", colS2("color_s").v[0]);
    // check that more flags are false at DocList level and at Response level
    ASSERT_FALSE(docs2.more);
    ASSERT_FALSE(req->responses[1]->proto.more);
  }

  {
    // new let's try for 3 responses

    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");
    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s"}).batchSize(1).limit(7);

    req->execute(para);
    ASSERT_EQ(3, req->responses.size());
    // check offsets are correct
    ASSERT_EQ(0, std::get<solux::api::DocList>(req->responses[0]->proto.ops.at("q")->kind).offset);
    ASSERT_EQ(1, std::get<solux::api::DocList>(req->responses[1]->proto.ops.at("q")->kind).offset);
    ASSERT_EQ(2, std::get<solux::api::DocList>(req->responses[2]->proto.ops.at("q")->kind).offset);
  }
}

TEST_F(SearchEngineTest, forcePrepareWrapperMatchesChild) {
  CollectionHelper helper;
  helper.index(flatdoc("foo_w", "how now brown cow", "foo_i", 17, "color_s", "red"), UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w", "charlie brown", "foo_i", 23, "color_s", "blue"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w", "brown", "foo_i", 5, "color_s", "brown"), UpdateMessage::COMMIT);

  auto addBrownTopDocs = [](OpCursor& cur) {
    cur.withStats().fields({"foo_i", "color_s"}).matchQuery("foo_w", "brown");
  };

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  addBrownTopDocs(req->topDocs("normal"));
  req->execute();
  ASSERT_EQ(1, req->responses.size()) << req->toString();
  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();

  // Same query through the prepared path: the engine test seam wraps each
  // top-docs root in ForcePrepareQuery (the wrapper is not on the wire).
  auto forcedReq = localReq(soluxNode->getSearchEngine());
  forcedReq->testForcePrepare = true;
  forcedReq->collection("main");
  auto& forced = forcedReq->topDocs("forced");
  addBrownTopDocs(forced);
  forced.facet("colors", "color_s").limit(-1);
  forcedReq->execute();
  ASSERT_EQ(1, forcedReq->responses.size()) << forcedReq->toString();
  ASSERT_FALSE(hasError(forcedReq->responses[0]->proto)) << forcedReq->toString();

  const auto& normalDocs = *req->docList("normal");
  const auto& forcedDocs = *forcedReq->docList("forced");
  ASSERT_EQ(normalDocs.found.value_or(0), forcedDocs.found.value_or(0));

  const auto& normalFoo = std::get<solux::api::ColInt>(normalDocs.columns.at("foo_i").kind).v;
  const auto& forcedFoo = std::get<solux::api::ColInt>(forcedDocs.columns.at("foo_i").kind).v;
  ASSERT_EQ(normalFoo.size(), forcedFoo.size());
  for (size_t i = 0; i < normalFoo.size(); i++) {
    EXPECT_EQ(normalFoo[i], forcedFoo[i]);
  }

  const auto& normalColor = std::get<solux::api::ColStr>(normalDocs.columns.at("color_s").kind).v;
  const auto& forcedColor = std::get<solux::api::ColStr>(forcedDocs.columns.at("color_s").kind).v;
  ASSERT_EQ(normalColor.size(), forcedColor.size());
  for (size_t i = 0; i < normalColor.size(); i++) {
    EXPECT_EQ(normalColor[i], forcedColor[i]);
  }

  const auto& normalScore = std::get<solux::api::ColFloat>(normalDocs.columns.at("_score_").kind).v;
  const auto& forcedScore = std::get<solux::api::ColFloat>(forcedDocs.columns.at("_score_").kind).v;
  ASSERT_EQ(normalScore.size(), forcedScore.size());
  for (size_t i = 0; i < normalScore.size(); i++) {
    EXPECT_FLOAT_EQ(normalScore[i], forcedScore[i]);
  }

  const auto& facet = std::get<solux::api::FacetResult>(forcedDocs.ops.at("colors")->kind);
  const auto& facetBids = std::get<solux::api::ColStr>(facet.bucket_ids->kind);
  ASSERT_EQ(3, facetBids.v.size());
  std::map<std::string, int64_t> facetCounts;
  for (size_t i = 0; i < facetBids.v.size(); i++) {
    facetCounts[std::string(facetBids.v[i])] = facet.counts.at(i);
  }
  EXPECT_EQ(1, facetCounts["blue"]);
  EXPECT_EQ(1, facetCounts["brown"]);
  EXPECT_EQ(1, facetCounts["red"]);
}

TEST_F(SearchEngineTest, constantScoreWrapperSetsScore) {
  CollectionHelper helper;
  helper.index(flatdoc("foo_w", "how now brown cow", "foo_i", 17, "color_s", "red"), UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w", "charlie brown", "foo_i", 23, "color_s", "blue"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w", "brown", "foo_i", 5, "color_s", "brown"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  // The prepared path must not disturb the constant score: the test seam
  // wraps the root, so this runs ForcePrepare(ConstantScore(match)).  (The
  // reverse composition, ConstantScore over a preparing child, is covered at
  // the engine level in ScorerCostTest.)
  req->testForcePrepare = true;
  req->collection("main");

  auto& cur = req->topDocs("constant").withStats().fields({"foo_i", "color_s"});
  cur.rawQuery() = qb::constantScore(cur.mr(), qb::match(cur.mr(), "foo_w", "brown"), 7.5f);
  cur.facet("colors", "color_s").limit(-1);

  req->execute();
  ASSERT_EQ(1, req->responses.size()) << req->toString();
  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();

  const auto& docs = *req->docList("constant");
  ASSERT_EQ(3, docs.found.value_or(0));

  const auto& foo = std::get<solux::api::ColInt>(docs.columns.at("foo_i").kind).v;
  ASSERT_EQ(3, foo.size());
  std::map<int64_t, bool> seenFoo;
  for (size_t i = 0; i < foo.size(); i++) {
    seenFoo[foo[i]] = true;
  }
  EXPECT_TRUE(seenFoo[5]);
  EXPECT_TRUE(seenFoo[17]);
  EXPECT_TRUE(seenFoo[23]);

  const auto& scores = std::get<solux::api::ColFloat>(docs.columns.at("_score_").kind).v;
  ASSERT_EQ(3, scores.size());
  for (size_t i = 0; i < scores.size(); i++) {
    EXPECT_FLOAT_EQ(7.5f, scores[i]);
  }

  const auto& facetResult = std::get<solux::api::FacetResult>(docs.ops.at("colors")->kind);
  const auto& facetBids = std::get<solux::api::ColStr>(facetResult.bucket_ids->kind);
  ASSERT_EQ(3, facetBids.v.size());
  std::map<std::string, int64_t> facetCounts;
  for (size_t i = 0; i < facetBids.v.size(); i++) {
    facetCounts[std::string(facetBids.v[i])] = facetResult.counts.at(i);
  }
  EXPECT_EQ(1, facetCounts["blue"]);
  EXPECT_EQ(1, facetCounts["brown"]);
  EXPECT_EQ(1, facetCounts["red"]);
}

// Op and filter names are path-safe ([A-Za-z0-9_-]+): they appear in path-based
// debug/warning addressing, URL overlays, and Domain include/exclude references.
TEST_F(SearchEngineTest, opAndFilterNameCharset) {
  CollectionHelper helper;
  helper.index(flatdoc("foo_w", "hello"), UpdateMessage::COMMIT);

  {  // unusual but legal name
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("My-Op_2").matchQuery("foo_w", "hello");
    req->execute();
    ASSERT_EQ(1, req->responses.size()) << req->toString();
    ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
  }
  {  // path-unsafe op name is rejected with the teaching message
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("bad.name!").matchQuery("foo_w", "hello");
    ExpectLog quiet("Search request failed:");
    req->execute();
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(req->errorMsg().find("restricted to"), std::string::npos) << req->errorMsg();
  }
  {  // filter names use the same rule
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").matchQuery("foo_w", "hello");
    auto& td = std::get<solux::api::TopDocs>(cur.rawOp().kind);
    auto* f = solux::api::build::allocArray(td.filter, 1, cur.mr());
    f[0].name = "bad name";
    auto* q = (solux::api::Query*)cur.mr().allocate(sizeof(solux::api::Query),
                                                    alignof(solux::api::Query));
    new (q) solux::api::Query(qb::match(cur.mr(), "foo_w", "hello"));
    f[0].query = q;
    ExpectLog quiet("Search request failed:");
    req->execute();
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(req->errorMsg().find("restricted to"), std::string::npos) << req->errorMsg();
  }
}

TEST_F(SearchEngineTest, topDocsFilters) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "foo_w", "hello", "cat_s", "a", "size_s", "big"),
    flatdoc("id", "2", "foo_w", "hello", "cat_s", "a", "size_s", "small"),
    flatdoc("id", "3", "foo_w", "hello", "cat_s", "b", "size_s", "big"),
    flatdoc("id", "4", "foo_w", "other", "cat_s", "a", "size_s", "big"),
  }, UpdateMessage::COMMIT);

  {  // single filter narrows the query domain
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("q").matchQuery("foo_w", "hello").getNumber().fields({"id"})
        .matchFilter("f", "cat_s", "a");
    req->execute();
    ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    auto& docs = std::get<api::DocList>(req->responses[0]->proto.ops.at("q")->kind);
    EXPECT_EQ(2, docs.found);
  }
  {  // two filters intersect
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("q").matchQuery("foo_w", "hello").getNumber().fields({"id"})
        .matchFilter("f1", "cat_s", "a").matchFilter("f2", "size_s", "big");
    req->execute();
    ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    auto& docs = std::get<api::DocList>(req->responses[0]->proto.ops.at("q")->kind);
    EXPECT_EQ(1, docs.found);
  }
  {  // a nested facet counts over the filtered domain
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& td = req->topDocs("q");
    td.matchQuery("foo_w", "hello").getNumber().fields({"id"})
        .matchFilter("f", "size_s", "big");
    td.facet("cats", "cat_s").limit(-1);
    req->execute();
    ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    auto& docs = std::get<api::DocList>(req->responses[0]->proto.ops.at("q")->kind);
    EXPECT_EQ(2, docs.found);
    auto& facet = std::get<api::FacetResult>(docs.ops.at("cats")->kind);
    ASSERT_EQ(2u, facet.counts.size());
    EXPECT_EQ(1, facet.counts[0]);  // one "hello"+"big" doc in each of a and b
    EXPECT_EQ(1, facet.counts[1]);
  }
}

TEST_F(SearchEngineTest, topDocsFilterFoldMatchesExplicitBoolean) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "apple apple", "keep_s", "yes"),
    flatdoc("id", "d2", "body_w", "apple", "keep_s", "yes"),
    flatdoc("id", "d3", "body_w", "apple apple apple", "keep_s", "no"),
    flatdoc("id", "d4", "body_w", "banana", "keep_s", "yes"),
  }, UpdateMessage::COMMIT);

  auto folded = localReq(soluxNode->getSearchEngine());
  folded->collection("main");
  folded->topDocs("q").matchQuery("body_w", "apple").withStats().fields({"id"})
      .limit(-1).matchFilter("keep", "keep_s", "yes");
  folded->execute();
  ASSERT_OK(folded);

  auto explicitFilter = localReq(soluxNode->getSearchEngine());
  explicitFilter->collection("main");
  auto& cur = explicitFilter->topDocs("q").withStats().fields({"id"}).limit(-1);
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "body_w", "apple")},
      /*optional=*/{}, /*prohibited=*/{},
      /*filter=*/{qb::match(cur.mr(), "keep_s", "yes")});
  explicitFilter->execute();
  ASSERT_OK(explicitFilter);

  EXPECT_EQ(resultIds(*folded, "q"), resultIds(*explicitFilter, "q"));
  expectSameScoreMap(resultScoreMap(*folded, "q"),
                     resultScoreMap(*explicitFilter, "q"));
}

TEST_F(SearchEngineTest, topDocsFilterFoldMatchesPassivePath) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "apple apple apple", "keep_s", "yes", "group_s", "x"),
    flatdoc("id", "d2", "body_w", "apple apple", "keep_s", "no", "group_s", "x"),
    flatdoc("id", "d3", "body_w", "apple", "keep_s", "yes", "group_s", "y"),
    flatdoc("id", "d4", "body_w", "apple banana", "keep_s", "yes", "group_s", "y"),
    flatdoc("id", "d5", "body_w", "banana", "keep_s", "yes", "group_s", "x"),
  }, UpdateMessage::COMMIT);

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    std::map<std::string, int64_t> facets;
    int64_t count = 0;
  };

  auto run = [&](bool passive) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("ranked").matchQuery("body_w", "apple").withStats().fields({"id"})
        .limit(2).matchFilter("keep", "keep_s", "yes");
    auto& count = req->topDocs("count").matchQuery("body_w", "apple")
        .getNumber().limit(0).matchFilter("keep", "keep_s", "yes");
    count.facet("groups", "group_s").limit(-1);
    {
      TopDocsFilterFoldGuard guard(passive);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "ranked"),
      resultScoreMap(*req, "ranked"),
      resultFacetMap(*req, "count", "groups"),
      req->getMatchCount("count")
    };
  };

  auto folded = run(false);
  auto passive = run(true);
  ASSERT_EQ(2u, folded.ids.size());
  EXPECT_EQ(3, folded.count);
  EXPECT_EQ((std::map<std::string, int64_t>{{"x", 1}, {"y", 2}}), folded.facets);
  EXPECT_EQ(folded.ids, passive.ids);
  EXPECT_EQ(folded.count, passive.count);
  EXPECT_EQ(folded.facets, passive.facets);
  expectSameScoreMap(folded.scores, passive.scores);
}

TEST_F(SearchEngineTest, filteredCountBulkIntersectionSingleSegment) {
  expectFilteredCountEquivalence(soluxNode->getSearchEngine(), false);
}

TEST_F(SearchEngineTest, filteredCountBulkIntersectionMultiSegment) {
  expectFilteredCountEquivalence(soluxNode->getSearchEngine(), true);
}

TEST_F(SearchEngineTest,
       uncachedFilteredCountSelectsAndSamplesEitherPostingsLead) {
  constexpr std::string_view collection =
      "uncached_filtered_count_candidate";
  constexpr int32_t nDocs = 3 * DocsEnumMeta::L1_DOCS + 123;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});

  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  int32_t selectedCount = 0;
  int32_t overlapCount = 0;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    bool selected = (doc % 8) == 0;
    bool overlap = (doc % 256) == 0;
    bool independentTail = (doc % 8) != 0 && (doc % 6) == 0;
    selectedCount += (int32_t) selected;
    overlapCount += (int32_t) overlap;

    std::string body = "dense_a dense_b";
    if (independentTail || overlap) body += " filter_tail";
    if (selected) body += " required_lead";

    std::string filter;
    if (selected) filter += "selected ";
    if (independentTail || overlap) filter += "required_tail";
    docs.push_back(flatdoc(
        "id", "candidate_" + std::to_string(doc),
        "body_w", body, "filter_w", filter));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_GT(selectedCount,
            BooleanQuery::ConjunctionBulkScorer::
                kFilteredConjunctionBatchSize);

  struct ExactRun {
    int64_t count = 0;
    int64_t engagements = 0;
    int64_t batches = 0;
    int64_t candidateWindows = 0;
    int64_t denseWindows = 0;
    int64_t tfreqBlocksDecoded = 0;
  };
  auto runExact = [&](bool disabled) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().limit(0);
    cur.rawQuery() = qb::boolean(
        cur.mr(),
        {qb::match(cur.mr(), "body_w", "required_lead"),
         qb::match(cur.mr(), "body_w", "dense_a")});
    cur.matchFilter("selection", "filter_w", "required_tail");
    ExactTermCountGuard routeGuard(disabled);
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return ExactRun{
      req->getMatchCount("q"),
      SkipStats::exactTermCountEngagements,
      SkipStats::exactTermCountBatches,
      SkipStats::filteredConjBatchCountWindows,
      SkipStats::conjDenseCountWindows,
      SkipStats::tfreqBlocksDecoded,
    };
  };

  ExactRun exact = runExact(false);
  ExactRun exactDisabled = runExact(true);
  EXPECT_EQ(overlapCount, exact.count);
  EXPECT_EQ(exactDisabled.count, exact.count);
  EXPECT_GT(exact.engagements, 0);
  EXPECT_GT(exact.batches, 1);
  EXPECT_EQ(0, exact.candidateWindows);
  EXPECT_EQ(0, exact.denseWindows);
  EXPECT_EQ(0, exact.tfreqBlocksDecoded);
  EXPECT_EQ(0, exactDisabled.engagements);

  struct Run {
    int64_t count = 0;
    int64_t candidateWindows = 0;
    int64_t candidateAdmits = 0;
    int64_t denseLatchBacks = 0;
    int64_t denseWindows = 0;
    int64_t tfreqBlocksDecoded = 0;
  };
  auto run = [&](std::string_view first, std::string_view second,
                 std::string_view filter, bool disabled = false) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().limit(0);
    cur.rawQuery() = qb::boolean(
        cur.mr(),
        {qb::match(cur.mr(), "body_w", first),
         qb::match(cur.mr(), "body_w", second)});
    cur.matchFilter("selection", "filter_w", filter);
    IntegratedFilteredCountGuard routeGuard(disabled);
    ExactTermCountGuard exactGuard(true);
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::filteredConjBatchCountWindows,
      SkipStats::filteredCountCandidateAdmits,
      SkipStats::filteredCountCandidateDenseLatchBacks,
      SkipStats::conjDenseCountWindows,
      SkipStats::tfreqBlocksDecoded,
    };
  };

  // The filter is cheapest in the first shape; the required term is cheapest
  // in the second. Both feeds exceed one batch and retain only the 1/32
  // overlap after the first tail probe.
  Run filterLead = run("filter_tail", "dense_a", "selected");
  Run requiredLead = run("required_lead", "dense_a", "required_tail");
  int64_t expectedCandidateWindows = 1
      + (selectedCount
         - BooleanQuery::ConjunctionBulkScorer::kCandidateCountSampleSize
         + BooleanQuery::ConjunctionBulkScorer::kFilteredConjunctionBatchSize
         - 1)
          / BooleanQuery::ConjunctionBulkScorer::
              kFilteredConjunctionBatchSize;
  for (const Run* result : {&filterLead, &requiredLead}) {
    EXPECT_EQ(overlapCount, result->count);
    EXPECT_EQ(expectedCandidateWindows, result->candidateWindows);
    EXPECT_GT(result->candidateAdmits, 0);
    EXPECT_EQ(0, result->denseLatchBacks);
    EXPECT_EQ(0, result->denseWindows);
    EXPECT_EQ(0, result->tfreqBlocksDecoded);
  }

  // A non-selective first tail rejects the candidate arm after one batch and
  // resumes the dense route without losing the remaining filter postings.
  Run latched = run("dense_a", "dense_b", "selected");
  EXPECT_EQ(selectedCount, latched.count);
  EXPECT_EQ(1, latched.candidateWindows);
  EXPECT_EQ(0, latched.candidateAdmits);
  EXPECT_GT(latched.denseLatchBacks, 0);
  EXPECT_GT(latched.denseWindows, 0);
  EXPECT_EQ(0, latched.tfreqBlocksDecoded);

  auto runSingleTerm = [&] {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().limit(0);
    cur.rawQuery() = qb::match(cur.mr(), "body_w", "dense_a");
    cur.matchFilter("selection", "filter_w", "selected");
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::filteredConjBatchCountWindows,
      SkipStats::filteredCountCandidateAdmits,
      SkipStats::filteredCountCandidateDenseLatchBacks,
      SkipStats::conjDenseCountWindows,
      SkipStats::tfreqBlocksDecoded,
    };
  };
  Run singleTerm = runSingleTerm();
  EXPECT_EQ(selectedCount, singleTerm.count);
  EXPECT_EQ(0, singleTerm.candidateWindows);
  EXPECT_EQ(0, singleTerm.candidateAdmits);
  EXPECT_EQ(0, singleTerm.denseLatchBacks);
  EXPECT_GT(singleTerm.denseWindows, 0);

  Run disabled = run("filter_tail", "dense_a", "selected", true);
  EXPECT_EQ(filterLead.count, disabled.count);
  EXPECT_EQ(0, disabled.candidateWindows);
  EXPECT_EQ(0, disabled.candidateAdmits);
  EXPECT_EQ(0, disabled.denseLatchBacks);
  EXPECT_GT(disabled.denseWindows, 0);
}

TEST_F(SearchEngineTest, cachedFilterHitKeepsDenseCountPath) {
  constexpr std::string_view collection = "cached_dense_count";
  CollectionHelper helper(collection);
  indexFilteredCountDocs(helper, false);
  auto cache = helper.getIndexWriter()->getFilterCache();

  auto first = runFilteredCount(
      soluxNode->getSearchEngine(), FilteredCountShape::TERM,
      "fat_term_single", FilteredCountPath::FOLDED, collection);
  auto second = runFilteredCount(
      soluxNode->getSearchEngine(), FilteredCountShape::TERM,
      "fat_term_single", FilteredCountPath::FOLDED, collection);
  auto beforeHit = cache->counters();
  auto hit = runFilteredCount(
      soluxNode->getSearchEngine(), FilteredCountShape::TERM,
      "fat_term_single", FilteredCountPath::FOLDED, collection);

  EXPECT_EQ(first.count, second.count);
  EXPECT_EQ(first.count, hit.count);
  EXPECT_GT(hit.denseWindows, 0);
  EXPECT_GT(cache->counters().hits, beforeHit.hits);
}

TEST_F(SearchEngineTest, cachedNumericFilterHitIgnoresShapeToggle) {
  constexpr std::string_view collection = "cached_numeric_dense_count";
  constexpr int32_t N = DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0});
  SchemaBuilder schema;
  auto& range = schema.field("range_i");
  range.type = api::FieldDef::FieldClass::INT;
  range.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());
  std::vector<Doc> docs;
  docs.reserve(N);
  for (int32_t doc = 0; doc < N; doc++) {
    docs.push_back(flatdoc(
        "id", "cached_numeric_" + std::to_string(doc),
        "body_w", "alpha", "range_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto cache = helper.getIndexWriter()->getFilterCache();

  struct Run {
    int64_t found;
    int64_t island;
    int64_t numericGeoIsland;
    int64_t denseWindows;
    int64_t numericArms;
  };
  auto run = [&](bool disableShapes) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").matchQuery("body_w", "alpha")
        .getNumber().limit(0);
    appendRawFilter(topDocs, "range", qb::range(
        topDocs.mr(), "range_i", qb::valI64(topDocs.mr(), 0), nullptr,
        qb::valI64(topDocs.mr(), N / 2 - 1), nullptr));
    SkipStatsGuard stats;
    {
      NumericRangeShapeGuard shapeGuard(disableShapes);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::conjPlanUnknownIsland,
      SkipStats::conjPlanUnknownIslandNumericGeo,
      SkipStats::conjDenseCountWindows,
      SkipStats::numericRangePointsArms
          + SkipStats::numericRangeComplementArms
          + SkipStats::numericRangeZoneArms
          + SkipStats::numericRangeSparseVerifyArms,
    };
  };

  Run first = run(false);
  Run second = run(false);
  auto beforeHit = cache->counters();
  Run hitWithShapesDisabled = run(true);
  EXPECT_EQ(first.found, second.found);
  EXPECT_EQ(first.found, hitWithShapesDisabled.found);
  EXPECT_EQ(N / 2, hitWithShapesDisabled.found);
  EXPECT_GT(cache->counters().hits, beforeHit.hits);
  EXPECT_EQ(0, hitWithShapesDisabled.island);
  EXPECT_EQ(0, hitWithShapesDisabled.numericGeoIsland);
  EXPECT_GT(hitWithShapesDisabled.denseWindows, 0);
  EXPECT_EQ(0, hitWithShapesDisabled.numericArms);
}

TEST_F(SearchEngineTest,
       conjunctionPlanReadyRoutesMatchBuiltClassification) {
  constexpr std::string_view collection = "conjunction_plan_ready_routes";
  constexpr int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string filter;
    if ((doc % 4) == 0) filter += "exact ";
    if ((doc & 1) == 0) filter += "dense ";
    if ((doc % 2048) == 0) filter += "sparse";
    docs.push_back(flatdoc(
        "id", "plan_" + std::to_string(doc),
        "body_w", "alpha beta", "filter_w", filter));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  FilteredCountResult exact = runFilteredCount(
      soluxNode->getSearchEngine(), FilteredCountShape::INTERSECTION,
      "exact", FilteredCountPath::FOLDED, collection);
  EXPECT_EQ((nDocs + 3) / 4, exact.count);
  EXPECT_GT(exact.exactTermCountBatches, 0);

  FilteredCountResult dense;
  {
    ExactTermCountGuard exactGuard(true);
    IntegratedFilteredCountGuard sampleGuard(true);
    dense = runFilteredCount(
        soluxNode->getSearchEngine(), FilteredCountShape::INTERSECTION,
        "dense", FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ((nDocs + 1) / 2, dense.count);
  EXPECT_GT(dense.denseWindows, 0);

  runFilteredCount(
      soluxNode->getSearchEngine(), FilteredCountShape::INTERSECTION,
      "sparse", FilteredCountPath::FOLDED, collection);
  runFilteredCount(
      soluxNode->getSearchEngine(), FilteredCountShape::INTERSECTION,
      "sparse", FilteredCountPath::FOLDED, collection);
  FilteredCountResult sparse;
  {
    ExactTermCountGuard exactGuard(true);
    sparse = runFilteredCount(
        soluxNode->getSearchEngine(), FilteredCountShape::INTERSECTION,
        "sparse", FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ((nDocs + 2047) / 2048, sparse.count);
  EXPECT_EQ(0, sparse.denseWindows);
  EXPECT_GT(sparse.filteredConjBatchCountWindows, 0);
}

TEST_F(SearchEngineTest, cachedSparseFilterLeadsDenseCountWorkByCardinality) {
  constexpr std::string_view collection = "cached_sparse_count_clause";
  constexpr int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  int32_t filterCard = 0;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    bool selected = (doc % 100) == 0;
    filterCard += (int32_t) selected;
    docs.push_back(flatdoc(
        "id", "clause_" + std::to_string(doc),
        "body_w", "alpha beta",
        "filter_w", selected ? "selected" : "other"));
  }
  ASSERT_GT(filterCard, nDocs / BooleanQuery::ConjunctionBulkScorer::
      kTermTailDenseThresholdInverse);
  ASSERT_LE(filterCard, DocSetBuilder::arrayLimitFor(nDocs));
  auto indexed = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success) << indexed.error_message;
  auto cache = helper.getIndexWriter()->getFilterCache();

  // Default admission is two sightings: warm through publication, then
  // measure a true hit whose DocSet is a costed conjunction clause.
  {
    FilterClauseCountGuard enabled(false);
    runFilteredCount(soluxNode->getSearchEngine(),
                     FilteredCountShape::INTERSECTION, "selected",
                     FilteredCountPath::FOLDED, collection);
    runFilteredCount(soluxNode->getSearchEngine(),
                     FilteredCountShape::INTERSECTION, "selected",
                     FilteredCountPath::FOLDED, collection);
  }
  auto beforeHit = cache->counters();
  FilteredCountResult routed;
  {
    FilterClauseCountGuard enabled(false);
    routed = runFilteredCount(soluxNode->getSearchEngine(),
                              FilteredCountShape::INTERSECTION, "selected",
                              FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(filterCard, routed.count);
  EXPECT_GT(routed.denseWindows, 0);
  EXPECT_LE(routed.denseWindows, filterCard);
  // Every ratcheted window contains a lead-filter doc. The DocSet fill itself
  // does no postings fill, and the first term fill drops below the crossover,
  // so full clause fills are bounded by those nonempty filter windows.
  EXPECT_LE(routed.bulkFillCalls, routed.denseWindows);
  EXPECT_GT(cache->counters().hits, beforeHit.hits);

  FilteredCountResult legacy;
  {
    FilterClauseCountGuard disabled(true);
    legacy = runFilteredCount(soluxNode->getSearchEngine(),
                              FilteredCountShape::INTERSECTION, "selected",
                              FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(routed.count, legacy.count);
  EXPECT_LE(routed.denseWindows, legacy.denseWindows);
  // Negative control: the disabled route leaves the cached DocSet as a
  // call-time domain, so the query clauses perform extra full window fills
  // before the filter can reject anything.
  EXPECT_GT(legacy.bulkFillCalls, legacy.denseWindows);
}

TEST_F(SearchEngineTest, cachedDocSetSparseLeadKeepsTermWordProbes) {
  constexpr std::string_view collection = "cached_docset_sparse_lead";
  constexpr int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  int32_t filterCard = 0;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    bool selected = (doc % 2048) == 0;
    filterCard += (int32_t) selected;
    docs.push_back(flatdoc(
        "id", "sparse_lead_" + std::to_string(doc),
        "body_w", "alpha beta",
        "filter_w", selected ? "selected" : "other"));
  }
  ASSERT_LT(filterCard, nDocs / BooleanQuery::ConjunctionBulkScorer::
      kTermTailDenseThresholdInverse);
  auto indexed = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success) << indexed.error_message;

  {
    FilterClauseCountGuard enabled(false);
    runFilteredCount(soluxNode->getSearchEngine(),
                     FilteredCountShape::INTERSECTION, "selected",
                     FilteredCountPath::FOLDED, collection);
    runFilteredCount(soluxNode->getSearchEngine(),
                     FilteredCountShape::INTERSECTION, "selected",
                     FilteredCountPath::FOLDED, collection);
  }
  FilteredCountResult routed;
  {
    FilterClauseCountGuard enabled(false);
    routed = runFilteredCount(soluxNode->getSearchEngine(),
                              FilteredCountShape::INTERSECTION, "selected",
                              FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(filterCard, routed.count);
  EXPECT_EQ(0, routed.denseWindows);
  EXPECT_EQ(0, routed.sparseFallbacks);
  EXPECT_GT(routed.filteredConjBatchCountWindows, 0);
  EXPECT_GT(routed.docsOnlyWordProbeAdvances, 0);
  EXPECT_EQ(0, routed.tfreqBlocksDecoded);

  FilteredCountResult legacy;
  {
    FilterClauseCountGuard disabled(true);
    legacy = runFilteredCount(soluxNode->getSearchEngine(),
                              FilteredCountShape::INTERSECTION, "selected",
                              FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(routed.count, legacy.count);
  EXPECT_EQ(0, legacy.sparseFallbacks);
  EXPECT_EQ(0, legacy.filteredConjBatchCountWindows);

  FilteredCountResult unbatched;
  {
    FilterClauseCountGuard enabled(false);
    FilteredConjunctionBatchGuard disabled(true);
    unbatched = runFilteredCount(soluxNode->getSearchEngine(),
                                 FilteredCountShape::INTERSECTION, "selected",
                                 FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(routed.count, unbatched.count);
  EXPECT_GT(unbatched.sparseFallbacks, 0);
  EXPECT_EQ(0, unbatched.filteredConjBatchCountWindows);
}

TEST_F(SearchEngineTest, cachedDocSetBatchesExactScoredTerm) {
  constexpr std::string_view collection = "cached_docset_scored_term";
  constexpr int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  int32_t filterCard = 0;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    bool selected = (doc % 100) == 0;
    filterCard += (int32_t) selected;
    docs.push_back(flatdoc(
        "id", "scored_term_" + std::to_string(doc),
        "body_w", "alpha beta",
        "filter_w", selected ? "selected" : "other"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t count;
    int64_t engagements;
  };
  auto run = [&](bool disabled) {
    FilteredConjunctionBatchGuard guard(disabled);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().withStats()
        .fields({"id"}).limit(100);
    cur.rawQuery() = filteredCountBody(cur.mr(), FilteredCountShape::TERM);
    cur.matchFilter("filter", "filter_w", "selected");
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      req->getMatchCount("q"),
      SkipStats::filteredConjBatchEngagements,
    };
  };

  run(false);
  run(false);
  Result batch = run(false);
  Result legacy = run(true);
  EXPECT_EQ(filterCard, batch.count);
  EXPECT_EQ(legacy.count, batch.count);
  EXPECT_EQ(legacy.ids, batch.ids);
  expectSameScoreMap(legacy.scores, batch.scores);
  EXPECT_GT(batch.engagements, 0);
  EXPECT_EQ(0, legacy.engagements);
}

TEST_F(SearchEngineTest, sparseFilteredTopKRerouteAdmitsConjunctionShapes) {
  constexpr std::string_view collection = "sparse_filtered_topk_reroute";
  CollectionHelper helper(collection);
  indexSparseFilteredTopKDocs(helper);
  auto& engine = soluxNode->getSearchEngine();

  for (FilteredCountShape shape :
       {FilteredCountShape::TERM, FilteredCountShape::INTERSECTION,
        FilteredCountShape::PHRASE}) {
    SparseFilteredTopKResult pruned;
    {
      SparseFilteredTopKRerouteGuard guard(true);
      pruned = runSparseFilteredTopK(
          engine, collection, shape, "sparse", 10);
    }
    SparseFilteredTopKResult routed;
    {
      SparseFilteredTopKRerouteGuard guard(false);
      routed = runSparseFilteredTopK(
          engine, collection, shape, "sparse", 10);
    }
    EXPECT_EQ(pruned.ids, routed.ids);
    expectSameScoreMap(pruned.scores, routed.scores);
    EXPECT_EQ(0, pruned.reroutes);
    EXPECT_EQ(1, routed.reroutes);
  }

  SparseFilteredTopKRerouteGuard guard(false);
  auto dense = runSparseFilteredTopK(
      engine, collection, FilteredCountShape::TERM, "dense", 10);
  EXPECT_EQ(0, dense.reroutes);
  EXPECT_EQ(1, dense.densityRejects);

  auto exact = runSparseFilteredTopK(
      engine, collection, FilteredCountShape::TERM, "sparse", 10, true);
  EXPECT_EQ(0, exact.reroutes);

  auto unfiltered = runSparseFilteredTopK(
      engine, collection, FilteredCountShape::TERM, "", 10);
  EXPECT_EQ(0, unfiltered.reroutes);
}

TEST_F(SearchEngineTest,
       sparseFilteredTermUnionTopKReroutePreservesResults) {
  constexpr std::string_view collection = "sparse_filtered_union_topk";
  CollectionHelper helper(collection);
  indexSparseFilteredTopKDocs(helper);
  auto& engine = soluxNode->getSearchEngine();

  SparseFilteredTopKResult routed;
  {
    SparseFilteredTopKRerouteGuard guard(false);
    routed = runSparseFilteredTopK(
        engine, collection, FilteredCountShape::UNION, "sparse", 5);
  }
  SparseFilteredTopKResult pruned;
  {
    SparseFilteredTopKRerouteGuard guard(true);
    pruned = runSparseFilteredTopK(
        engine, collection, FilteredCountShape::UNION, "sparse", 5);
  }

  EXPECT_EQ(pruned.ids, routed.ids);
  expectSameScoreMap(pruned.scores, routed.scores);
  EXPECT_EQ(1, routed.reroutes);
  EXPECT_EQ(1, routed.unionReroutes);
  EXPECT_GT(routed.disjunctionBatchScoreWindows, 0);
  EXPECT_EQ(0, pruned.reroutes);
}

TEST_F(SearchEngineTest, sparseFilteredTopKRerouteUsesFamilyDensityKnees) {
  constexpr std::string_view collection = "sparse_filtered_topk_knees";
  CollectionHelper helper(collection);
  indexSparseFilteredTopKDocs(helper);
  SparseFilteredTopKRerouteGuard guard(
      false, false, {32, 32, 32}, {64, 64, 64});
  auto& engine = soluxNode->getSearchEngine();

  auto conjunction = runSparseFilteredTopK(
      engine, collection, FilteredCountShape::INTERSECTION, "between", 100);
  auto termUnion = runSparseFilteredTopK(
      engine, collection, FilteredCountShape::UNION, "between", 100);

  EXPECT_EQ(1, conjunction.reroutes);
  EXPECT_EQ(0, conjunction.unionReroutes);
  EXPECT_EQ(0, termUnion.reroutes);
  EXPECT_EQ(1, termUnion.densityRejects);
}

TEST_F(SearchEngineTest, sparseFilteredTermUnionDisableRestoresPrunedPath) {
  constexpr std::string_view collection =
      "sparse_filtered_union_topk_disable";
  CollectionHelper helper(collection);
  indexSparseFilteredTopKDocs(helper);
  auto& engine = soluxNode->getSearchEngine();

  SparseFilteredTopKResult routed;
  {
    SparseFilteredTopKRerouteGuard guard(false);
    routed = runSparseFilteredTopK(
        engine, collection, FilteredCountShape::UNION, "sparse", 5);
  }
  SparseFilteredTopKResult pruned;
  {
    SparseFilteredTopKRerouteGuard guard(false, true);
    pruned = runSparseFilteredTopK(
        engine, collection, FilteredCountShape::UNION, "sparse", 5);
  }

  EXPECT_EQ(routed.ids, pruned.ids);
  expectSameScoreMap(routed.scores, pruned.scores);
  EXPECT_EQ(1, routed.unionReroutes);
  EXPECT_GT(routed.disjunctionBatchScoreWindows, 0);
  EXPECT_EQ(0, pruned.reroutes);
  EXPECT_EQ(0, pruned.disjunctionBatchScoreWindows);
}

TEST_F(SearchEngineTest, cachedSparseFilterDrivesDisjunctionBatch) {
  constexpr std::string_view collection = "cached_sparse_disjunction_batch";
  constexpr int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  int32_t filterCard = 0;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    bool selected = (doc % 1000) == 0;
    filterCard += (int32_t) selected;
    std::string body = (doc & 1) == 0 ? "beta" : "gamma";
    if ((doc % 7) == 0) {
      body += " beta gamma";
    }
    docs.push_back(flatdoc(
      "id", "disj_batch_" + std::to_string(doc),
        "body_w", body,
        "filter_w", selected ? "selected" : "other",
        "group_s", (doc & 1) == 0 ? "even" : "odd"));
  }
  ASSERT_LE(filterCard, nDocs
      / BooleanQuery::kFilteredDisjunctionBatchDensityInverse);
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  {
    FilteredDisjunctionBatchGuard enabled(false);
    auto cold = runFilteredCount(
        soluxNode->getSearchEngine(), FilteredCountShape::UNION, "selected",
        FilteredCountPath::FOLDED, collection);
    EXPECT_EQ(0, cold.ownedFilterMaterializations);
    EXPECT_EQ(0, cold.ownedFilterServes);
    EXPECT_GT(cold.filteredDisjBatchEngagements, 0);
    EXPECT_GT(
        cold.filteredDisjBatchPostingsFeedEngagements, 0);
    runFilteredCount(soluxNode->getSearchEngine(),
                     FilteredCountShape::UNION, "selected",
                     FilteredCountPath::FOLDED, collection);
  }
  FilteredCountResult batchCount;
  {
    FilteredDisjunctionBatchGuard enabled(false);
    batchCount = runFilteredCount(
        soluxNode->getSearchEngine(), FilteredCountShape::UNION,
        "selected", FilteredCountPath::FOLDED, collection);
  }
  FilteredCountResult pullCount;
  {
    FilteredDisjunctionBatchGuard disabled(true);
    pullCount = runFilteredCount(
        soluxNode->getSearchEngine(), FilteredCountShape::UNION,
        "selected", FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(filterCard, batchCount.count);
  EXPECT_EQ(batchCount.count, pullCount.count);
  EXPECT_GT(batchCount.filteredDisjBatchEngagements, 0);
  EXPECT_GT(batchCount.filteredDisjBatchCountWindows, 0);
  EXPECT_LE(batchCount.filteredDisjBatchCountWindows, filterCard);
  EXPECT_EQ(0, pullCount.filteredDisjBatchEngagements);

  FilteredCountResult uncompactedCount;
  {
    FilteredDisjunctionCountCompactionGuard disabled(true);
    uncompactedCount = runFilteredCount(
        soluxNode->getSearchEngine(), FilteredCountShape::UNION,
        "selected", FilteredCountPath::FOLDED, collection);
  }
  EXPECT_EQ(batchCount.count, uncompactedCount.count);
  EXPECT_GT(uncompactedCount.filteredDisjBatchCountWindows, 0);

  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().limit(0);
    cur.rawQuery() = filteredCountBody(cur.mr(), FilteredCountShape::UNION);
    cur.matchFilter("filter", "filter_w", "selected");
    cur.facet("groups", "group_s").limit(-1);
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_EQ(filterCard, req->getMatchCount("q"));
    EXPECT_EQ((std::map<std::string, int64_t>{{"even", filterCard}}),
              resultFacetMap(*req, "q", "groups"));
    EXPECT_EQ(0, SkipStats::exactDomainDocSetCollections);
    EXPECT_GT(SkipStats::exactDomainStreamFallbacks, 0);
    EXPECT_GT(SkipStats::filteredDisjBatchCountWindows, 0);
  }

  struct TopResult {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t count;
    int64_t scoreWindows;
  };
  auto runTopCount = [&](bool disabled) {
    FilteredDisjunctionBatchGuard guard(disabled);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().withStats()
        .fields({"id"}).limit(100);
    cur.rawQuery() = filteredCountBody(cur.mr(), FilteredCountShape::UNION);
    cur.matchFilter("filter", "filter_w", "selected");
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return TopResult{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      req->getMatchCount("q"),
      SkipStats::filteredDisjBatchScoreWindows,
    };
  };
  TopResult batchTop = runTopCount(false);
  TopResult pullTop = runTopCount(true);
  EXPECT_EQ(filterCard, batchTop.count);
  EXPECT_EQ(batchTop.count, pullTop.count);
  EXPECT_EQ(batchTop.ids, pullTop.ids);
  expectSameScoreMap(pullTop.scores, batchTop.scores);
  EXPECT_GT(batchTop.scoreWindows, 0);
  EXPECT_EQ(0, pullTop.scoreWindows);
}

TEST_F(SearchEngineTest, cachedSparseFilterLeadsPhraseDisjunctionPull) {
  constexpr std::string_view collection = "cached_phrase_disjunction_pull";
  constexpr int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = (doc % 3) == 0
        ? "to be or not to be" : "to not be or to be";
    if ((doc % 11) == 0) body += " hamlet";
    docs.push_back(flatdoc(
        "id", "phrase_pull_" + std::to_string(doc),
        "body_w", body,
        "filter_w", (doc % 1000) == 0 ? "selected" : "other"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t count;
    int64_t engagements;
    int64_t phraseVerifies;
    int64_t ownedMaterializations;
  };
  auto run = [&](bool disabled) {
    FilteredDisjunctionBatchGuard guard(disabled);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().withStats()
        .fields({"id"}).limit(100);
    cur.rawQuery() = qb::boolean(cur.mr(), {},
        {qb::phraseWords(
             cur.mr(), "body_w", {"to", "be", "or", "not", "to", "be"}),
         qb::match(cur.mr(), "body_w", "hamlet")});
    cur.matchFilter("filter", "filter_w", "selected");
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      req->getMatchCount("q"),
      SkipStats::filteredDisjBatchEngagements,
      SkipStats::phraseVerifies,
      SkipStats::ownedFilterMaterializations,
    };
  };

  Result cold = run(false);
  EXPECT_EQ(0, cold.ownedMaterializations);
  run(false);
  Result routed = run(false);
  Result bodyBulk = run(true);
  EXPECT_EQ(bodyBulk.count, routed.count);
  EXPECT_EQ(bodyBulk.ids, routed.ids);
  expectSameScoreMap(bodyBulk.scores, routed.scores);
  EXPECT_EQ(0, routed.engagements);
  EXPECT_EQ(0, bodyBulk.engagements);
  EXPECT_LE(routed.phraseVerifies, 9);
}

TEST_F(SearchEngineTest, exactCountTopKRoutesAtFilterUnionCostBoundary) {
  constexpr std::string_view collection = "exact_count_topk_composition";
  constexpr int32_t nDocs = DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "filler";
    if (doc < 30) body += " alpha";
    if (doc >= 30 && doc < 60) body += " beta";
    std::string filter;
    if (doc < 40) filter += "sparse ";
    if (doc < 60) filter += "equal";
    docs.push_back(flatdoc(
        "id", "compose_" + std::to_string(doc),
        "body_w", body,
        "filter_w", filter.empty() ? "other" : filter));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  ASSERT_GE(40, nDocs
      / TopDocsReq::kExactCountTopKMinCandidateDensityInverse);
  ASSERT_LE(40, nDocs
      / TopDocsReq::
          kExactCountTopKSparseFilterSinglePassDensityInverse);

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t count;
    int64_t compositions;
    int64_t sparseSinglePassRejects;
  };
  auto run = [&](std::string_view filter, bool disableSparseDecision) {
    TopKCountCompositionGuard compositionGuard(
        false, TopDocsReq::kExactCountTopKMinCandidateDensityInverse);
    SparseExactCountSinglePassGuard sparseGuard(
        disableSparseDecision);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().withStats()
        .fields({"id"}).limit(100);
    cur.rawQuery() = qb::boolean(cur.mr(), {},
        {qb::match(cur.mr(), "body_w", "alpha"),
         qb::match(cur.mr(), "body_w", "beta")});
    cur.matchFilter("filter", "filter_w", filter);
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      req->getMatchCount("q"),
      SkipStats::exactCountTopKCompositions,
      SkipStats::exactCountTopKSparseFilterSinglePassRejects,
    };
  };

  Result singlePass = run("sparse", false);
  Result forcedComposition = run("sparse", true);
  EXPECT_EQ(forcedComposition.count, singlePass.count);
  EXPECT_EQ(forcedComposition.ids, singlePass.ids);
  EXPECT_EQ(forcedComposition.scores, singlePass.scores);
  EXPECT_EQ(0, singlePass.compositions);
  EXPECT_GT(singlePass.sparseSinglePassRejects, 0);
  EXPECT_GT(forcedComposition.compositions, 0);
  EXPECT_EQ(0, forcedComposition.sparseSinglePassRejects);

  Result equalCosts = run("equal", false);
  EXPECT_EQ(60, equalCosts.count);
  EXPECT_GT(equalCosts.compositions, 0);
  EXPECT_EQ(0, equalCosts.sparseSinglePassRejects);
}

TEST_F(SearchEngineTest,
       exactCompositionIneligibleConstantRankingDeclinesBeforeBuild) {
  constexpr std::string_view collection =
      "exact_composition_constant_ranking";
  constexpr int32_t nDocs = DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "filler";
    if (doc < 30) body += " alpha";
    if (doc >= 30 && doc < 60) body += " beta";
    docs.push_back(flatdoc(
        "id", "constant_rank_" + std::to_string(doc),
        "body_w", body,
        "filter_w", doc < 60 ? "keep" : "other"));
  }
  auto indexed = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success) << indexed.error_message;

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t count;
    int64_t fallbacks;
    int64_t exactCompositionRejects;
    int64_t profitabilityRejects;
    int64_t sparseRejects;
    int64_t compositions;
  };
  auto run = [&](bool disableComposition) {
    TopKCountCompositionGuard compositionGuard(
        disableComposition,
        TopDocsReq::kExactCountTopKMinCandidateDensityInverse);
    SparseFilteredTopKRerouteGuard rerouteGuard(true);
    FilteredDisjunctionBatchGuard batchGuard(true);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().withStats()
        .fields({"id"}).limit(100);
    cur.rawQuery() = qb::boolean(cur.mr(), {},
        {qb::match(cur.mr(), "body_w", "alpha"),
         qb::match(cur.mr(), "body_w", "beta")});
    cur.matchFilter("filter", "filter_w", "keep");
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      req->getMatchCount("q"),
      SkipStats::exactCountTopKBulkFallbacks,
      SkipStats::bulkBuiltThenRejectedExactComposition,
      SkipStats::exactCountTopKProfitabilityRejects,
      SkipStats::exactCountTopKSparseFilterSinglePassRejects,
      SkipStats::exactCountTopKCompositions,
    };
  };

  Result planned = run(false);
  Result oldShape = run(true);
  EXPECT_EQ(oldShape.count, planned.count);
  EXPECT_EQ(oldShape.ids, planned.ids);
  EXPECT_EQ(oldShape.scores, planned.scores);
  ASSERT_FALSE(planned.scores.empty());
  float score = planned.scores.begin()->second;
  for (const auto& [id, candidateScore] : planned.scores) {
    unused(id);
    EXPECT_EQ(score, candidateScore);
  }
  EXPECT_GT(planned.fallbacks, 0)
      << planned.profitabilityRejects << " " << planned.sparseRejects
      << " " << planned.compositions;
  EXPECT_EQ(0, planned.exactCompositionRejects);
}

TEST_F(SearchEngineTest, unfilteredCountDoesNotConstructFilterClause) {
  constexpr std::string_view collection = "unfiltered_count_clause_guard";
  CollectionHelper helper(collection);
  indexFilteredCountDocs(helper, false);

  FilteredCountResult enabled;
  {
    FilterClauseCountGuard guard(false);
    enabled = runUnfilteredCount(soluxNode->getSearchEngine(),
                                 FilteredCountShape::INTERSECTION, collection);
  }
  FilteredCountResult disabled;
  {
    FilterClauseCountGuard guard(true);
    disabled = runUnfilteredCount(soluxNode->getSearchEngine(),
                                  FilteredCountShape::INTERSECTION, collection);
  }
  EXPECT_EQ(enabled.count, disabled.count);
  EXPECT_EQ(enabled.denseWindows, disabled.denseWindows);
  EXPECT_EQ(enabled.disjGroupWindows, disabled.disjGroupWindows);
  EXPECT_EQ(enabled.sparseFallbacks, disabled.sparseFallbacks);
  EXPECT_EQ(enabled.bulkFillCalls, disabled.bulkFillCalls);
  EXPECT_EQ(enabled.docsOnlyWordProbeAdvances,
            disabled.docsOnlyWordProbeAdvances);
  EXPECT_EQ(enabled.tfreqBlocksDecoded, disabled.tfreqBlocksDecoded);
}

TEST_F(SearchEngineTest, sparseConstantPullDispatchUsesInclusiveArrayThreshold) {
  CollectionHelper helper;
  indexSparseConstantDispatchDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 1u);
  ASSERT_EQ(reader->segments()[0].maxDoc(), 1024);
  ASSERT_EQ(DocSetBuilder::arrayLimitFor(1024), 32);

  auto atLimitBaseline = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), "limit_s", true);
  auto atLimit = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), "limit_s", false);
  EXPECT_EQ(atLimit.pullCollections, 1);
  EXPECT_EQ(atLimit.domainWindows, 0);
  EXPECT_EQ(atLimit.bulkFillCalls, 0);
  EXPECT_EQ(atLimit.found, 32);
  EXPECT_EQ(atLimit.ids, atLimitBaseline.ids);
  EXPECT_EQ(atLimit.found, atLimitBaseline.found);
  EXPECT_EQ(atLimit.facets, atLimitBaseline.facets);

  auto overLimitBaseline = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), "over_s", true);
  auto overLimit = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), "over_s", false);
  EXPECT_EQ(overLimit.pullCollections, 0);
  EXPECT_GT(overLimit.domainWindows, 0);
  EXPECT_EQ(overLimit.found, 33);
  EXPECT_EQ(overLimit.ids, overLimitBaseline.ids);
  EXPECT_EQ(overLimit.found, overLimitBaseline.found);
  EXPECT_EQ(overLimit.facets, overLimitBaseline.facets);
}

TEST_F(SearchEngineTest, sparseConstantPullDispatchExcludesMultiFilterPlans) {
  CollectionHelper helper;
  indexSparseConstantDispatchDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 1u);
  ASSERT_EQ(reader->segments()[0].maxDoc(), 1024);

  auto baseline = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), "limit_s", true, true);
  auto actual = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), "limit_s", false, true);
  EXPECT_EQ(actual.pullCollections, 0);
  EXPECT_GT(actual.domainWindows, 0);
  EXPECT_EQ(actual.ids, baseline.ids);
  EXPECT_EQ(actual.found, baseline.found);
  EXPECT_EQ(actual.facets, baseline.facets);
}

TEST_F(SearchEngineTest, sparseConstantPullDispatchLeavesMatchAllShortcutUntouched) {
  CollectionHelper helper;
  indexSparseConstantDispatchDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 1u);
  ASSERT_EQ(reader->segments()[0].maxDoc(), 1024);

  auto actual = runSparseConstantDispatch(
      soluxNode->getSearchEngine(), {}, false);
  EXPECT_EQ(actual.pullCollections, 0);
  EXPECT_EQ(actual.found, 1024);
  int64_t facetTotal = 0;
  for (const auto& [group, count] : actual.facets) {
    unused(group);
    facetTotal += count;
  }
  EXPECT_EQ(facetTotal, actual.found);
}

TEST_F(SearchEngineTest, filterOnlyBulkAndConstantTopKMatchPassivePath) {
  CollectionHelper helper;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int32_t local = 0; local < 24; local++) {
      int32_t doc = segment * 24 + local;
      docs.push_back(flatdoc(
          "id", "f" + std::to_string(doc),
          "keep_s", (doc % 4) == 1 ? "no" : "yes",
          "size_s", (doc % 3) == 0 ? "small" : "big",
          "group_s", "g" + std::to_string(doc % 3)));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  std::vector<std::string> deleted = {"f2", "f25", "f38"};
  helper.deleteByIds(deleted, UpdateMessage::COMMIT);
  ASSERT_GT(helper.getIndexWriter()->getIndexReader()->segments().size(), 1u);

  struct Result {
    std::vector<std::string> ids;
    std::optional<int64_t> found;
    std::map<std::string, int64_t> facets;
  };

  auto run = [&](bool passive, bool twoFilters, int64_t limit,
                 bool getNumber, bool withFacet) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q").allQuery().fields({"id"}).limit(limit);
    if (getNumber) topDocs.getNumber();
    topDocs.matchFilter("keep", "keep_s", "yes");
    if (twoFilters) topDocs.matchFilter("size", "size_s", "big");
    if (withFacet) topDocs.facet("groups", "group_s").limit(-1);
    {
      TopDocsFilterFoldGuard guard(passive);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->docList("q");
    EXPECT_NE(docs, nullptr);
    return Result{
      resultIds(*req, "q"),
      docs == nullptr ? std::optional<int64_t>{} : docs->found,
      withFacet ? resultFacetMap(*req, "q", "groups")
                : std::map<std::string, int64_t>{}
    };
  };

  for (bool twoFilters : {false, true}) {
    for (int64_t limit : {0, 10}) {
      for (bool getNumber : {false, true}) {
        bool withFacet = limit > 0 && getNumber;
        auto bulk = run(false, twoFilters, limit, getNumber, withFacet);
        auto pull = run(true, twoFilters, limit, getNumber, withFacet);
        EXPECT_EQ(bulk.ids, pull.ids);
        EXPECT_EQ(bulk.found, pull.found);
        EXPECT_EQ(bulk.facets, pull.facets);
        EXPECT_EQ(bulk.found.has_value(), getNumber);
        if (limit == 0) {
          EXPECT_TRUE(bulk.ids.empty());
        } else {
          EXPECT_EQ(bulk.ids.size(), 10u);
          EXPECT_EQ(std::find(bulk.ids.begin(), bulk.ids.end(), "f2"),
                    bulk.ids.end());
        }
        if (withFacet) {
          int64_t facetDomain = 0;
          for (const auto& [group, count] : bulk.facets) {
            unused(group);
            facetDomain += count;
          }
          ASSERT_TRUE(bulk.found.has_value());
          EXPECT_EQ(facetDomain, *bulk.found);
          EXPECT_GT(*bulk.found, (int64_t) bulk.ids.size());
        }
      }
    }
  }
}

TEST_F(SearchEngineTest, cachedFilterOnlyDocSetIsTopDocsFacetDomain) {
  constexpr std::string_view collection = "filter_docset_identity";
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0});
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 16; doc++) {
    std::string selected = doc % 3 == 0 ? "no" : "yes";
    docs.push_back(flatdoc(
        "id", "d" + std::to_string(doc),
        "keep_zero_s", selected,
        "keep_top_s", selected,
        "body_w", doc % 2 == 0 ? "apple" : "pear",
        "group_s", "g" + std::to_string(doc % 2),
        "sub_s", "s" + std::to_string(doc % 4),
        "value_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto cache = helper.getIndexWriter()->getFilterCache();

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, int64_t> facets;
    std::map<std::string, double> avgs;
    std::map<std::string, std::map<std::string, int64_t>> subFacets;
    int64_t found = 0;
    int64_t identities = 0;
    int64_t docSetDomains = 0;
    int64_t domainWindows = 0;
  };
  auto run = [&](std::string_view filterField, int64_t limit,
                 bool passive, bool nested) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").allQuery().getNumber()
        .fields({"id"}).limit(limit)
        .matchFilter("keep", filterField, "yes");
    auto& facet = topDocs.facet("groups", "group_s").limit(-1);
    if (nested) {
      facet.avg("avg", "value_i");
      facet.facet("subs", "sub_s").limit(-1);
    }

    Result result;
    {
      SkipStatsGuard stats;
      TopDocsFilterFoldGuard fold(passive);
      req->execute(false);
      result.identities = SkipStats::filterDocSetIdentityCollections;
      result.docSetDomains = SkipStats::exactDomainDocSetCollections;
      result.domainWindows = SkipStats::bulkDomainWindowsFed;
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    result.ids = resultIds(*req, "q");
    result.facets = resultFacetMap(*req, "q", "groups");
    result.found = req->getMatchCount("q");
    if (nested) {
      const auto* facetResult =
          req->docList("q")->ops.at("groups")->facetResult();
      const auto& groupIds =
          std::get<api::ColStr>(facetResult->bucket_ids->kind);
      const auto& avgs =
          std::get<api::ArrDouble>(facetResult->ops.at("avg")->kind);
      const auto& subResults =
          std::get<api::ArrVal>(facetResult->ops.at("subs")->kind);
      for (size_t i = 0; i < groupIds.v.size(); i++) {
        std::string group(groupIds.v[i]);
        result.avgs[group] = avgs.v[i];
        const auto& subFacet =
            std::get<api::FacetResult>(subResults.v[i].kind);
        const auto& subIds =
            std::get<api::ColStr>(subFacet.bucket_ids->kind);
        for (size_t j = 0; j < subIds.v.size(); j++) {
          result.subFacets[group][std::string(subIds.v[j])] =
              subFacet.counts[j];
        }
      }
    }
    return result;
  };

  const std::map<std::string, int64_t> expectedFacets{
      {"g0", 5}, {"g1", 5}};
  auto missZero = run("keep_zero_s", 0, false, false);
  EXPECT_EQ(0, missZero.identities);
  EXPECT_EQ(0, missZero.docSetDomains);
  EXPECT_GT(missZero.domainWindows, 0);
  auto buildZero = run("keep_zero_s", 0, false, false);
  EXPECT_EQ(0, buildZero.identities);
  EXPECT_GT(buildZero.docSetDomains, 0);
  auto beforeZeroHit = cache->counters();
  auto hitZero = run("keep_zero_s", 0, false, false);
  EXPECT_EQ(0, hitZero.identities);
  EXPECT_GT(hitZero.docSetDomains, 0);
  EXPECT_EQ(0, hitZero.domainWindows);
  EXPECT_GT(cache->counters().hits, beforeZeroHit.hits);
  EXPECT_EQ(10, hitZero.found);
  EXPECT_TRUE(hitZero.ids.empty());
  EXPECT_EQ(expectedFacets, hitZero.facets);
  EXPECT_EQ(missZero.found, hitZero.found);
  EXPECT_EQ(missZero.facets, hitZero.facets);

  auto missTop = run("keep_top_s", 4, false, true);
  EXPECT_EQ(0, missTop.identities);
  EXPECT_GT(missTop.domainWindows, 0);
  run("keep_top_s", 4, false, true);
  auto beforeTopHit = cache->counters();
  auto hitTop = run("keep_top_s", 4, false, true);
  EXPECT_GT(hitTop.identities, 0);
  EXPECT_EQ(0, hitTop.domainWindows);
  EXPECT_GT(cache->counters().hits, beforeTopHit.hits);
  EXPECT_EQ(10, hitTop.found);
  EXPECT_EQ((std::vector<std::string>{"d1", "d2", "d4", "d5"}),
            hitTop.ids);
  EXPECT_EQ(expectedFacets, hitTop.facets);
  EXPECT_DOUBLE_EQ(7.6, hitTop.avgs.at("g0"));
  EXPECT_DOUBLE_EQ(7.4, hitTop.avgs.at("g1"));
  EXPECT_EQ((std::map<std::string, int64_t>{{"s0", 2}, {"s2", 3}}),
            hitTop.subFacets.at("g0"));
  EXPECT_EQ((std::map<std::string, int64_t>{{"s1", 3}, {"s3", 2}}),
            hitTop.subFacets.at("g1"));
  EXPECT_EQ(missTop.ids, hitTop.ids);
  EXPECT_EQ(missTop.facets, hitTop.facets);
  EXPECT_EQ(missTop.avgs, hitTop.avgs);
  EXPECT_EQ(missTop.subFacets, hitTop.subFacets);

  auto passive = run("keep_top_s", 4, true, true);
  EXPECT_EQ(0, passive.identities);
  EXPECT_EQ(hitTop.ids, passive.ids);
  EXPECT_EQ(hitTop.found, passive.found);
  EXPECT_EQ(hitTop.facets, passive.facets);
  EXPECT_EQ(hitTop.avgs, passive.avgs);
  EXPECT_EQ(hitTop.subFacets, passive.subFacets);

  ASSERT_TRUE(helper.deleteById("d4", UpdateMessage::COMMIT).success);
  auto deleted = run("keep_top_s", 4, false, true);
  EXPECT_EQ(0, deleted.identities);
  EXPECT_EQ(9, deleted.found);
  EXPECT_EQ((std::vector<std::string>{"d1", "d2", "d5", "d7"}),
            deleted.ids);
  EXPECT_EQ((std::map<std::string, int64_t>{{"g0", 4}, {"g1", 5}}),
            deleted.facets);
}

TEST_F(SearchEngineTest, filterDocSetIdentityRejectsNonIdentityPlans) {
  constexpr std::string_view collection = "filter_docset_identity_guards";
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0});
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 12; doc++) {
    docs.push_back(flatdoc(
        "id", "g" + std::to_string(doc),
        "keep_s", doc % 3 == 0 ? "no" : "yes",
        "even_s", doc % 2 == 0 ? "yes" : "no",
        "body_w", doc % 2 == 0 ? "apple" : "pear",
        "group_s", "only",
        "sort_i", 100 - doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto warm = [&](std::string_view field) {
    for (int i = 0; i < 3; i++) {
      auto req = localReq(soluxNode->getSearchEngine());
      req->collection(collection);
      req->topDocs("q").allQuery().getNumber().limit(0)
          .matchFilter("filter", field, "yes");
      req->execute(false);
      EXPECT_TRUE(req->ok()) << req->errorMsg();
    }
  };
  warm("keep_s");
  warm("even_s");
  auto cache = helper.getIndexWriter()->getFilterCache();

  auto run = [&](bool twoFilters, bool scoredQuery, bool fieldSort,
                 bool passive, bool disableFilterClause) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").getNumber().fields({"id"})
        .limit(fieldSort || scoredQuery ? 4 : 0);
    if (scoredQuery) {
      topDocs.matchQuery("body_w", "apple");
    } else {
      topDocs.allQuery();
    }
    topDocs.matchFilter("keep", "keep_s", "yes");
    if (twoFilters) {
      topDocs.matchFilter("even", "even_s", "yes");
    }
    if (fieldSort) {
      qb::sort(topDocs, "sort_i", qb::ASC);
    }
    topDocs.facet("groups", "group_s").limit(-1);

    int64_t identities;
    int64_t docSetDomains;
    int64_t domainWindows;
    {
      SkipStatsGuard stats;
      TopDocsFilterFoldGuard fold(passive);
      FilterClauseCountGuard filterClause(disableFilterClause);
      req->execute(false);
      identities = SkipStats::filterDocSetIdentityCollections;
      docSetDomains = SkipStats::exactDomainDocSetCollections;
      domainWindows = SkipStats::bulkDomainWindowsFed;
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return std::tuple{
        req->getMatchCount("q"), resultIds(*req, "q"),
        identities, docSetDomains, domainWindows};
  };

  auto beforeTwoFilter = cache->counters();
  auto [twoFilterCount, twoFilterIds, twoFilterIdentities, twoFilterDomains,
        twoFilterWindows] = run(true, false, false, false, false);
  EXPECT_EQ(4, twoFilterCount);
  EXPECT_TRUE(twoFilterIds.empty());
  EXPECT_EQ(0, twoFilterIdentities);
  EXPECT_GT(twoFilterDomains, 0);
  EXPECT_EQ(0, twoFilterWindows);
  EXPECT_GE(cache->counters().hits - beforeTwoFilter.hits, 2);

  auto [queryCount, queryIds, queryIdentities, queryDomains, queryWindows] =
      run(false, true, false, false, false);
  EXPECT_EQ(4, queryCount);
  EXPECT_EQ((std::vector<std::string>{"g2", "g4", "g8", "g10"}),
            queryIds);
  EXPECT_EQ(0, queryIdentities);
  EXPECT_EQ(0, queryDomains);
  EXPECT_GT(queryWindows, 0);

  auto [sortedCount, sortedIds, sortedIdentities, sortedDomains,
        sortedWindows] =
      run(false, false, true, false, false);
  EXPECT_EQ(8, sortedCount);
  EXPECT_EQ((std::vector<std::string>{"g11", "g10", "g8", "g7"}),
            sortedIds);
  EXPECT_EQ(0, sortedIdentities);
  EXPECT_EQ(0, sortedDomains);
  unused(sortedWindows);

  auto [disabledCount, disabledIds, disabledIdentities, disabledDomains,
        disabledWindows] =
      run(false, false, false, false, true);
  EXPECT_EQ(8, disabledCount);
  EXPECT_TRUE(disabledIds.empty());
  EXPECT_EQ(0, disabledIdentities);
  EXPECT_GT(disabledDomains, 0);
  EXPECT_EQ(0, disabledWindows);

  auto [passiveCount, passiveIds, passiveIdentities, passiveDomains,
        passiveWindows] =
      run(false, false, false, true, false);
  EXPECT_EQ(8, passiveCount);
  EXPECT_TRUE(passiveIds.empty());
  EXPECT_EQ(0, passiveIdentities);
  EXPECT_GT(passiveDomains, 0);
  EXPECT_EQ(0, passiveWindows);

}

TEST_F(SearchEngineTest, limitZeroSubOpsIntersectQueryAndFilterDocSets) {
  constexpr std::string_view collection = "query_filter_docset_domain";
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.minSegmentDocs = 0});
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 12; doc++) {
    docs.push_back(flatdoc(
        "id", "d" + std::to_string(doc),
        "body_w", doc % 2 == 0 ? "apple" : "pear",
        "keep_s", doc % 3 == 0 ? "no" : "yes",
        "low_s", doc < 9 ? "yes" : "no",
        "group_s", "g" + std::to_string(doc % 3)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto run = [&]() {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").matchQuery("body_w", "apple")
        .getNumber().limit(0)
        .matchFilter("keep", "keep_s", "yes")
        .matchFilter("low", "low_s", "yes");
    topDocs.facet("groups", "group_s").limit(-1);
    int64_t docSetDomains;
    int64_t domainWindows;
    {
      SkipStatsGuard stats;
      req->execute(false);
      docSetDomains = SkipStats::exactDomainDocSetCollections;
      domainWindows = SkipStats::bulkDomainWindowsFed;
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return std::tuple{
        req->getMatchCount("q"), resultFacetMap(*req, "q", "groups"),
        docSetDomains, domainWindows};
  };

  auto first = run();
  auto second = run();
  auto beforeHit = helper.getIndexWriter()->getFilterCache()->counters();
  auto [found, facets, docSetDomains, domainWindows] = run();
  EXPECT_EQ(3, found);
  EXPECT_EQ((std::map<std::string, int64_t>{{"g1", 1}, {"g2", 2}}),
            facets);
  EXPECT_GT(docSetDomains, 0);
  EXPECT_EQ(0, domainWindows);
  EXPECT_GE(helper.getIndexWriter()->getFilterCache()->counters().hits
                - beforeHit.hits,
            3);
  EXPECT_EQ(std::get<0>(first), found);
  EXPECT_EQ(std::get<1>(first), facets);
  EXPECT_EQ(std::get<0>(second), found);
  EXPECT_EQ(std::get<1>(second), facets);

  auto runPrepared = [&](std::string_view lowValue) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->testForcePrepare = true;
    req->collection(collection);
    auto& topDocs = req->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() = qb::boolean(
        topDocs.mr(), {}, {}, {},
        {qb::match(topDocs.mr(), "body_w", "apple")});
    topDocs.matchFilter("low", "low_s", lowValue);
    topDocs.facet("groups", "group_s").limit(-1);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount("q");
  };

  runPrepared("yes");
  runPrepared("yes");
  runPrepared("yes");
  EXPECT_EQ(1, runPrepared("no"));

}

TEST_F(SearchEngineTest, topDocsFilterFoldAllPrepareAndDeletes) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "apple", "keep_s", "yes", "group_s", "x"),
    flatdoc("id", "d2", "body_w", "apple", "keep_s", "no", "group_s", "x"),
    flatdoc("id", "d3", "body_w", "banana", "keep_s", "yes", "group_s", "y"),
    flatdoc("id", "d4", "body_w", "apple", "keep_s", "yes", "group_s", "y"),
  }, UpdateMessage::COMMIT);

  auto runAll = [&](bool filtered) {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").allQuery().getNumber().fields({"id"}).limit(-1);
    if (filtered) cur.matchFilter("keep", "keep_s", "yes");
    cur.facet("groups", "group_s").limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return std::pair{req->getMatchCount("q"), resultFacetMap(*req, "q", "groups")};
  };

  auto filteredAll = runAll(true);
  EXPECT_EQ(3, filteredAll.first);
  EXPECT_EQ((std::map<std::string, int64_t>{{"x", 1}, {"y", 2}}), filteredAll.second);
  auto unfilteredAll = runAll(false);
  EXPECT_EQ(4, unfilteredAll.first);
  EXPECT_EQ((std::map<std::string, int64_t>{{"x", 2}, {"y", 2}}), unfilteredAll.second);

  auto prepared = localReq(soluxNode->getSearchEngine());
  prepared->testForcePrepare = true;
  prepared->collection("main");
  auto& preparedCur = prepared->topDocs("q").matchQuery("body_w", "apple")
      .getNumber().fields({"id"}).limit(0).matchFilter("keep", "keep_s", "yes");
  preparedCur.facet("groups", "group_s").limit(-1);
  prepared->execute();
  ASSERT_OK(prepared);
  EXPECT_EQ(2, prepared->getMatchCount("q"));
  EXPECT_EQ((std::map<std::string, int64_t>{{"x", 1}, {"y", 1}}),
            resultFacetMap(*prepared, "q", "groups"));

  std::vector<std::string> deleted{"d1"};
  helper.deleteByIds(deleted, UpdateMessage::COMMIT);
  auto afterDelete = runAll(true);
  EXPECT_EQ(2, afterDelete.first);
  EXPECT_EQ((std::map<std::string, int64_t>{{"y", 2}}), afterDelete.second);
}

// Missing collection targets error cleanly and do not auto-create on reads.
TEST_F(SearchEngineTest, missingCollectionErrors) {
  std::string name = "search_engine_missing_collection";
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection(name);
  req->topDocs("q").allQuery();
  ExpectLog quiet("Search request failed:");
  req->execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_NE(req->errorMsg().find("collection '" + name + "' does not exist"),
            std::string::npos) << req->errorMsg();
  EXPECT_THROW(soluxNode->getCollection(name), CollectionResolutionError);
}

TEST_F(SearchEngineTest, unsafeCollectionNameErrors) {
  // Search-time resolution is lookup-only: an invalid name reads as
  // not-found, with no validation on the request path.
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("../bad");
  req->topDocs("q").allQuery();
  ExpectLog quiet("Search request failed:");
  req->execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_NE(req->errorMsg().find("collection '../bad' does not exist"), std::string::npos)
      << req->errorMsg();
}

TEST_F(SearchEngineTest, concurrentCreateCollectionExactlyOnce) {
  const std::string name = "concurrent_create_once_" + std::to_string(SoluxTest::rng_seed);
  constexpr int numThreads = 32;

  std::vector<std::shared_ptr<Collection>> collections(numThreads);
  std::vector<std::thread> threads;
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};

  threads.reserve(numThreads);
  for (int i = 0; i < numThreads; i++) {
    threads.emplace_back([&, i]() {
      ready.fetch_add(1);
      while (!start.load()) {
        std::this_thread::yield();
      }
      collections[i] = soluxNode->getOrCreateCollection(name);
    });
  }

  while (ready.load() != numThreads) {
    std::this_thread::yield();
  }
  start.store(true);

  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_NE(collections[0], nullptr);
  for (const auto& collection : collections) {
    EXPECT_EQ(collections[0], collection);
  }
  EXPECT_EQ(collections[0], soluxNode->getCollection(name));
}

namespace {

// Forces the old two-arrangement constant top-k shape (independent capture
// scorer + count-only bulk pass) for parity runs.
class OldConstantShapeGuard {
  bool saved;

public:
  explicit OldConstantShapeGuard(bool old)
    : saved(TopDocsReq::disableConstantWindowCaptureForTests) {
    TopDocsReq::disableConstantWindowCaptureForTests = old;
  }
  ~OldConstantShapeGuard() {
    TopDocsReq::disableConstantWindowCaptureForTests = saved;
  }
};

// qax* matches i % 2 == 0, qbx* matches i % 3 == 0; the conjunction is
// i % 6 == 0. keep_s excludes every 30th doc from the filtered variants.
void indexConstantConjDocs(CollectionHelper& helper, int32_t segments) {
  constexpr int32_t kDocs = 240;
  int32_t perSegment = kDocs / segments;
  for (int32_t seg = 0; seg < segments; seg++) {
    std::vector<Doc> docs;
    for (int32_t i = seg * perSegment; i < (seg + 1) * perSegment; i++) {
      std::string body;
      if (i % 2 == 0) body += "qax" + std::to_string(i);
      if (i % 3 == 0) {
        body += (body.empty() ? "" : " ") + std::string("qbx") + std::to_string(i);
      }
      if (body.empty()) body = "filler";
      docs.push_back(flatdoc(
          "id", "d" + std::to_string(1000 + i),  // fixed width: id order == doc order
          "body_w", body,
          "keep_s", i % 30 == 0 ? "no" : "yes",
          "group_s", "g" + std::to_string(i % 3)));
    }
    auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
    ASSERT_TRUE(result.success) << result.error_message;
  }
}

void indexMultiTermDenseRouteDocs(CollectionHelper& helper) {
  std::vector<Doc> docs;
  docs.reserve(DocsEnumMeta::L1_DOCS);
  for (int32_t i = 0; i < DocsEnumMeta::L1_DOCS; i++) {
    docs.push_back(flatdoc(
        "id", "d" + std::to_string(10000 + i),
        "body_w", (i & 1) == 0 ? "qax qcommon" : "other qcommon",
        "dense_s", i % 17 == 0 ? "no" : "yes",
        "sparse_s", i % 1024 == 0 ? "yes" : "no",
        "sort_i", (int64_t) ((i * 7919) % DocsEnumMeta::L1_DOCS)));
  }
  auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(result.success) << result.error_message;
}

struct ConstantTopKRun {
  std::vector<std::string> ids;
  std::map<std::string, float> scores;
  std::map<std::string, int64_t> facets;
  int64_t found = 0;
  int64_t captures = 0;
};

ConstantTopKRun runConstantConjTopK(SearchEngine& engine, int64_t limit,
                                    bool oldShape, bool withScores,
                                    bool withFilter, bool withFacet) {
  auto req = localReq(engine);
  req->collection("main");
  auto& topDocs = req->topDocs("q")
      .exprQuery("body_w:qax* AND body_w:qbx*")
      .getNumber().fields({"id"}).limit(limit);
  if (withScores) topDocs.getScores();
  if (withFilter) topDocs.matchFilter("keep", "keep_s", "yes");
  if (withFacet) topDocs.facet("groups", "group_s").limit(-1);

  ConstantTopKRun run;
  {
    SkipStatsGuard stats;
    OldConstantShapeGuard shape(oldShape);
    req->execute(false);
    run.captures = SkipStats::constantWindowCaptures;
  }
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  run.ids = resultIds(*req, "q");
  if (withScores) run.scores = resultScoreMap(*req, "q");
  if (withFacet) run.facets = resultFacetMap(*req, "q", "groups");
  run.found = req->getMatchCount("q");
  return run;
}

// Doc-order expectations computed from the corpus definition.
std::vector<std::string> expectedConstantFirstK(int64_t k, bool withFilter) {
  std::vector<std::string> ids;
  for (int32_t i = 0; i < 240 && (int64_t) ids.size() < k; i += 6) {
    if (withFilter && i % 30 == 0) continue;
    ids.push_back("d" + std::to_string(1000 + i));
  }
  return ids;
}

int64_t expectedConstantMatches(bool withFilter) {
  int64_t count = 0;
  for (int32_t i = 0; i < 240; i += 6) {
    if (withFilter && i % 30 == 0) continue;
    count++;
  }
  return count;
}

void expectConstantTopKShapes(SearchEngine& engine) {
  struct Shape {
    int64_t limit;
    bool withScores, withFilter, withFacet;
  };
  // limit 4: the capture window overshoots K. limit 1000: matches run out
  // before K, so the count phase is a no-op.
  for (auto [limit, withScores, withFilter, withFacet] :
       {Shape{4, false, false, false}, Shape{4, true, false, false},
        Shape{4, false, true, false}, Shape{4, true, false, true},
        Shape{4, false, true, true}, Shape{1000, false, false, false},
        Shape{1000, true, true, true}}) {
    SCOPED_TRACE("limit=" + std::to_string(limit)
                 + " scores=" + std::to_string(withScores)
                 + " filter=" + std::to_string(withFilter)
                 + " facet=" + std::to_string(withFacet));
    auto fresh = runConstantConjTopK(engine, limit, false,
                                     withScores, withFilter, withFacet);
    auto old = runConstantConjTopK(engine, limit, true,
                                   withScores, withFilter, withFacet);
    // Filtered requests route through the filter-specific collection paths,
    // not the constant window-capture branch (before this change too).
    if (!withFilter) {
      EXPECT_GT(fresh.captures, 0);
    }
    EXPECT_EQ(0, old.captures);
    EXPECT_EQ(old.ids, fresh.ids);
    EXPECT_EQ(old.facets, fresh.facets);
    EXPECT_EQ(old.found, fresh.found);
    expectSameScoreMap(old.scores, fresh.scores);

    EXPECT_EQ(expectedConstantMatches(withFilter), fresh.found);
    EXPECT_EQ(expectedConstantFirstK(limit, withFilter), fresh.ids);
    if (withScores) {
      ASSERT_FALSE(fresh.scores.empty());
      float constant = fresh.scores.begin()->second;
      for (auto& [id, score] : fresh.scores) {
        EXPECT_EQ(constant, score) << id;
      }
    }
  }
}

}  // namespace

TEST_F(SearchEngineTest, constantTopKWindowCaptureSingleSegment) {
  CollectionHelper helper;
  indexConstantConjDocs(helper, 1);
  expectConstantTopKShapes(helper.getSearchEngine());
}

TEST_F(SearchEngineTest, constantTopKWindowCaptureMultiSegment) {
  CollectionHelper helper;
  indexConstantConjDocs(helper, 3);
  expectConstantTopKShapes(helper.getSearchEngine());
}

TEST_F(SearchEngineTest, filteredMultiTermCountExpandsOncePerSegment) {
  CollectionHelper helper;
  constexpr int32_t kSegments = 3;
  indexConstantConjDocs(helper, kSegments);

  struct Run {
    int64_t found;
    int64_t rejected;
    int64_t wrapperRoute;
    int64_t unknownIsland;
    int64_t expansions;
  };
  auto run = [&](bool filtered) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q").exprQuery("body_w:qax*")
        .getNumber().limit(0);
    if (filtered) {
      topDocs.matchFilter("keep", "keep_s", "yes");
    }
    SkipStatsGuard stats;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::bulkBuiltThenRejected,
      SkipStats::bulkBuiltThenRejectedWrapperRoute,
      SkipStats::conjPlanUnknownIsland,
      SkipStats::multitermExpansions,
    };
  };

  Run filtered = run(true);
  EXPECT_EQ(112, filtered.found);
  EXPECT_EQ(0, filtered.rejected);
  EXPECT_EQ(0, filtered.wrapperRoute);
  EXPECT_EQ(0, filtered.unknownIsland);
  EXPECT_EQ(kSegments, filtered.expansions);

  Run unfiltered = run(false);
  EXPECT_EQ(120, unfiltered.found);
  EXPECT_EQ(0, unfiltered.rejected);
  EXPECT_EQ(0, unfiltered.wrapperRoute);
  EXPECT_EQ(0, unfiltered.unknownIsland);
  EXPECT_EQ(kSegments, unfiltered.expansions);
}

TEST_F(SearchEngineTest, filteredMultiTermDenseCountUsesWindowFill) {
  CollectionHelper helper;
  indexMultiTermDenseRouteDocs(helper);

  struct CountRun {
    int64_t found;
    int64_t rejected;
    int64_t denseWindows;
    int64_t countFallbacks;
  };
  auto runCount = [&](std::string_view filterField, bool disableDenseFill) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q").exprQuery("body_w:qax*")
        .getNumber().limit(0);
    topDocs.matchFilter("filter", filterField, "yes");
    SkipStatsGuard stats;
    {
      MultiTermDenseFillGuard denseFillGuard(disableDenseFill);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return CountRun{
      req->getMatchCount("q"),
      SkipStats::bulkBuiltThenRejected,
      SkipStats::conjDenseCountWindows,
      SkipStats::conjCountFallbacks,
    };
  };

  CountRun dense = runCount("dense_s", false);
  CountRun denseOracle = runCount("dense_s", true);
  EXPECT_EQ(denseOracle.found, dense.found);
  EXPECT_EQ(0, dense.rejected);
  EXPECT_GT(dense.denseWindows, 0);
  EXPECT_EQ(0, dense.countFallbacks);
  EXPECT_EQ(0, denseOracle.denseWindows);

  CountRun sparse = runCount("sparse_s", false);
  CountRun sparseOracle = runCount("sparse_s", true);
  EXPECT_EQ(sparseOracle.found, sparse.found);
  EXPECT_EQ(0, sparse.rejected);
  EXPECT_EQ(0, sparse.denseWindows);
}

TEST_F(SearchEngineTest, filteredMultiTermFieldSortUsesWindowBulk) {
  CollectionHelper helper;
  indexMultiTermDenseRouteDocs(helper);

  struct SortRun {
    std::vector<std::string> ids;
    int64_t bulkCollections;
    int64_t builtThenRejected;
    int64_t sortMatchWindowRejects;
  };
  auto runSort = [&](bool disableDenseFill) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q").exprQuery("body_w:qax*")
        .fields({"id"}).limit(25);
    topDocs.matchFilter("filter", "dense_s", "yes");
    qb::sort(topDocs, "sort_i", qb::ASC);
    SkipStatsGuard stats;
    {
      MultiTermDenseFillGuard denseFillGuard(disableDenseFill);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return SortRun{
      resultIds(*req, "q"),
      SkipStats::fieldSortBulkCollections,
      SkipStats::bulkBuiltThenRejected,
      SkipStats::bulkBuiltThenRejectedSortMatchWindow,
    };
  };

  SortRun sorted = runSort(false);
  SortRun sortedOracle = runSort(true);
  EXPECT_EQ(sortedOracle.ids, sorted.ids);
  EXPECT_EQ(1, sorted.bulkCollections);
  EXPECT_EQ(0, sorted.builtThenRejected);
  EXPECT_EQ(0, sorted.sortMatchWindowRejects);
  EXPECT_EQ(1, sorted.bulkCollections + sorted.sortMatchWindowRejects);
  EXPECT_EQ(0, sortedOracle.bulkCollections);
  EXPECT_EQ(0, sortedOracle.builtThenRejected);
  EXPECT_EQ(0, sortedOracle.sortMatchWindowRejects);
}

TEST_F(SearchEngineTest, conjunctionPlanFillsOptionalMultiTermMemo) {
  CollectionHelper helper;
  indexConstantConjDocs(helper, 1);

  struct Run {
    int64_t found;
    int64_t unknownIsland;
    int64_t expansions;
  };
  auto run = [&](bool disableFilterFold) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() = qb::boolean(
        topDocs.mr(), {},
        {qb::match(topDocs.mr(), "body_w", "filler"),
         qb::prefix(topDocs.mr(), "body_w", "qax")},
        {}, {}, 1);
    topDocs.matchFilter("keep", "keep_s", "yes");
    SkipStatsGuard stats;
    {
      TopDocsFilterFoldGuard foldGuard(disableFilterFold);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::conjPlanUnknownIsland,
      SkipStats::multitermExpansions,
    };
  };

  Run planned = run(false);
  Run disabledIdentity = run(true);
  EXPECT_EQ(disabledIdentity.found, planned.found);
  EXPECT_EQ(0, planned.unknownIsland);
  EXPECT_EQ(0, disabledIdentity.unknownIsland);
  EXPECT_EQ(1, planned.expansions);
  EXPECT_EQ(1, disabledIdentity.expansions);
}

TEST_F(SearchEngineTest, filteredPhraseCountRetiresPhraseIsland) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll(
      {flatdoc("id", "1", "body_w", "quick fox", "keep_s", "yes"),
       flatdoc("id", "2", "body_w", "quick brown fox", "keep_s", "yes"),
       flatdoc("id", "3", "body_w", "quick fox", "keep_s", "no")},
      UpdateMessage::COMMIT).success);

  struct Run {
    int64_t found;
    int64_t island;
    int64_t phraseIsland;
    int64_t multiTermIsland;
    int64_t numericGeoIsland;
    int64_t otherIsland;
  };
  auto run = [&](bool disableShapes) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() =
        qb::phraseWords(topDocs.mr(), "body_w", {"quick", "fox"});
    topDocs.matchFilter("keep", "keep_s", "yes");
    SkipStatsGuard stats;
    {
      PhraseShapeGuard shapeGuard(disableShapes);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::conjPlanUnknownIsland,
      SkipStats::conjPlanUnknownIslandPhrase,
      SkipStats::conjPlanUnknownIslandMultiTerm,
      SkipStats::conjPlanUnknownIslandNumericGeo,
      SkipStats::conjPlanUnknownIslandOther,
    };
  };

  Run oracle = run(true);
  Run planned = run(false);
  EXPECT_EQ(oracle.found, planned.found);
  EXPECT_EQ(1, planned.found);
  EXPECT_EQ(0, oracle.island);
  EXPECT_EQ(0, oracle.phraseIsland);
  EXPECT_EQ(0, planned.island);
  EXPECT_EQ(0, planned.phraseIsland);
  EXPECT_EQ(0, planned.multiTermIsland);
  EXPECT_EQ(0, planned.numericGeoIsland);
  EXPECT_EQ(0, planned.otherIsland);
}

TEST_F(SearchEngineTest, filteredNumericCountRetiresNumericIsland) {
  constexpr std::string_view collection = "numeric_shape_island";
  constexpr int32_t N = 2 * DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  SchemaBuilder schema;
  auto& range = schema.field("range_i");
  range.type = api::FieldDef::FieldClass::INT;
  range.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());

  std::vector<Doc> docs;
  docs.reserve(N);
  for (int32_t doc = 0; doc < N; doc++) {
    docs.push_back(flatdoc(
        "id", "numeric_shape_" + std::to_string(doc),
        "body_w", "alpha", "range_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Run {
    int64_t found;
    int64_t island;
    int64_t numericGeoIsland;
    int64_t denseWindows;
    int64_t pointsArms;
    int64_t complementArms;
    int64_t zoneArms;
    int64_t sparseVerifyArms;
    std::array<int64_t, 11> bulkRejects;
  };
  auto run = [&](bool disableShapes) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").matchQuery("body_w", "alpha")
        .getNumber().limit(0);
    appendRawFilter(topDocs, "range", qb::range(
        topDocs.mr(), "range_i", qb::valI64(topDocs.mr(), 0), nullptr,
        qb::valI64(topDocs.mr(), N / 2 - 1), nullptr));
    SkipStatsGuard stats;
    {
      NumericRangeShapeGuard shapeGuard(disableShapes);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::conjPlanUnknownIsland,
      SkipStats::conjPlanUnknownIslandNumericGeo,
      SkipStats::conjDenseCountWindows,
      SkipStats::numericRangePointsArms,
      SkipStats::numericRangeComplementArms,
      SkipStats::numericRangeZoneArms,
      SkipStats::numericRangeSparseVerifyArms,
      {
        SkipStats::bulkBuiltThenRejected,
        SkipStats::bulkBuiltThenRejectedWrapperRoute,
        SkipStats::bulkBuiltThenRejectedMandOptTwoPhase,
        SkipStats::bulkBuiltThenRejectedMaxScoreProhibited,
        SkipStats::bulkBuiltThenRejectedFilterAttach,
        SkipStats::bulkBuiltThenRejectedFilteredDisj,
        SkipStats::bulkBuiltThenRejectedExactMandOpt,
        SkipStats::bulkBuiltThenRejectedFilteredScored,
        SkipStats::bulkBuiltThenRejectedFilterOnly,
        SkipStats::bulkBuiltThenRejectedSortMatchWindow,
        SkipStats::bulkBuiltThenRejectedExactComposition,
      },
    };
  };

  Run oracle = run(true);
  Run planned = run(false);
  EXPECT_EQ(oracle.found, planned.found);
  EXPECT_EQ(N / 2, planned.found);
  EXPECT_EQ(0, oracle.island);
  EXPECT_EQ(0, oracle.numericGeoIsland);
  EXPECT_EQ(0, planned.island);
  EXPECT_EQ(0, planned.numericGeoIsland);
  EXPECT_GT(planned.denseWindows, 0);
  EXPECT_GT(planned.pointsArms, 0);
  EXPECT_EQ(0, planned.complementArms);
  EXPECT_EQ(0, planned.zoneArms);
  EXPECT_EQ(0, planned.sparseVerifyArms);
  EXPECT_EQ(oracle.pointsArms, planned.pointsArms);
  EXPECT_EQ(oracle.complementArms, planned.complementArms);
  EXPECT_EQ(oracle.zoneArms, planned.zoneArms);
  EXPECT_EQ(oracle.sparseVerifyArms, planned.sparseVerifyArms);
  for (size_t i = 0; i < planned.bulkRejects.size(); i++) {
    EXPECT_LE(planned.bulkRejects[i], oracle.bulkRejects[i])
        << "counter=" << i;
  }
}

TEST_F(SearchEngineTest,
       exhaustiveNumericCountBuildsPointsWhileFieldSortKeepsSparseVerify) {
  constexpr std::string_view collection = "numeric_count_consumption";
  constexpr int32_t N = 2 * DocsEnumMeta::L1_DOCS + 257;
  constexpr int64_t hi = (int64_t) N * 4 / 5 - 1;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  SchemaBuilder schema;
  auto& range = schema.field("range_i");
  range.type = api::FieldDef::FieldClass::INT;
  range.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());

  std::vector<Doc> docs;
  docs.reserve(N);
  int64_t expected = 0;
  for (int32_t doc = 0; doc < N; doc++) {
    bool alpha = (doc & 1) == 0;
    expected += alpha && doc <= hi;
    docs.push_back(flatdoc(
        "id", "numeric_use_" + std::to_string(doc),
        "body_w", alpha ? "alpha" : "other", "range_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Run {
    int64_t found;
    std::vector<std::string> ids;
    int64_t island;
    int64_t pointsArms;
    int64_t complementArms;
    int64_t sparseVerifyArms;
    int64_t fieldSortBulkCollections;
  };
  auto run = [&](bool fieldSort, bool disableShapes) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").matchQuery("body_w", "alpha")
        .getNumber();
    if (fieldSort) {
      topDocs.fields({"id"}).limit(25);
      qb::sort(topDocs, "range_i", qb::ASC);
    } else {
      topDocs.limit(0);
    }
    appendRawFilter(topDocs, "range", qb::range(
        topDocs.mr(), "range_i", qb::valI64(topDocs.mr(), 0), nullptr,
        qb::valI64(topDocs.mr(), hi), nullptr));
    SkipStatsGuard stats;
    {
      NumericRangeShapeGuard shapeGuard(disableShapes);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"), resultIds(*req, "q"),
      SkipStats::conjPlanUnknownIslandNumericGeo,
      SkipStats::numericRangePointsArms,
      SkipStats::numericRangeComplementArms,
      SkipStats::numericRangeSparseVerifyArms,
      SkipStats::fieldSortBulkCollections,
    };
  };

  Run count = run(false, false);
  Run countShapesDisabled = run(false, true);
  EXPECT_EQ(expected, count.found);
  EXPECT_EQ(count.found, countShapesDisabled.found);
  EXPECT_GT(count.pointsArms + count.complementArms, 0);
  EXPECT_EQ(0, count.sparseVerifyArms);
  EXPECT_EQ(0, count.island);
  EXPECT_EQ(0, countShapesDisabled.pointsArms
                   + countShapesDisabled.complementArms);
  EXPECT_GT(countShapesDisabled.sparseVerifyArms, 0);
  EXPECT_EQ(0, countShapesDisabled.island);

  Run sorted = run(true, false);
  Run sortedShapesDisabled = run(true, true);
  EXPECT_EQ(expected, sorted.found);
  EXPECT_EQ(sorted.found, sortedShapesDisabled.found);
  EXPECT_EQ(sorted.ids, sortedShapesDisabled.ids);
  EXPECT_EQ(25u, sorted.ids.size());
  EXPECT_EQ(0, sorted.pointsArms + sorted.complementArms);
  EXPECT_GT(sorted.sparseVerifyArms, 0);
  EXPECT_GT(sorted.fieldSortBulkCollections, 0);
}

TEST_F(SearchEngineTest,
       exhaustiveNumericCountPricesMaterializationFromDemandSpan) {
  constexpr std::string_view collection = "numeric_count_lead_exposure";
  constexpr int32_t N = 2 * DocsEnumMeta::L1_DOCS + 257;
  constexpr int64_t hi = (int64_t) N * 4 / 5 - 1;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  SchemaBuilder schema;
  auto& range = schema.field("range_i");
  range.type = api::FieldDef::FieldClass::INT;
  range.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());

  std::vector<Doc> docs;
  docs.reserve(N);
  int64_t expected = 0;
  for (int32_t doc = 0; doc < N; doc++) {
    // One survivor per required term keeps the default
    // minOther * WINDOW_SIZE span below the fat range fence. The scoped
    // dense-threshold override only keeps this provably-tiny fixture on the
    // refresh path so both build choices remain observable.
    bool first = doc % 10000 == 0;
    bool second = doc % 10001 == 0;
    expected += first && second && doc <= hi;
    std::string body;
    if (first) body += "moderate_a ";
    if (second) body += "moderate_b";
    if (body.empty()) body = "other";
    docs.push_back(flatdoc(
        "id", "numeric_exposure_" + std::to_string(doc),
        "body_w", body, "range_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Run {
    int64_t found;
    int64_t denseWindows;
    int64_t pointsArms;
    int64_t complementArms;
    int64_t sparseVerifyArms;
  };
  auto run = [&](int64_t spanScale) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() = qb::boolean(
        topDocs.mr(),
        {qb::match(topDocs.mr(), "body_w", "moderate_a"),
         qb::match(topDocs.mr(), "body_w", "moderate_b")});
    appendRawFilter(topDocs, "range", qb::range(
        topDocs.mr(), "range_i", qb::valI64(topDocs.mr(), 0), nullptr,
        qb::valI64(topDocs.mr(), hi), nullptr));
    WindowFillSpanScaleGuard spanScaleGuard(spanScale);
    CountDenseThresholdGuard denseThresholdGuard(N);
    SkipStatsGuard stats;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::conjDenseCountWindows,
      SkipStats::numericRangePointsArms,
      SkipStats::numericRangeComplementArms,
      SkipStats::numericRangeSparseVerifyArms,
    };
  };

  Run scaled = run(BooleanQuery::WINDOW_FILL_SPAN_SCALE);
  Run maxDocLike = run(N);
  EXPECT_EQ(expected, scaled.found);
  EXPECT_EQ(scaled.found, maxDocLike.found);
  EXPECT_GT(scaled.denseWindows, 0);
  EXPECT_EQ(0, scaled.pointsArms + scaled.complementArms);
  EXPECT_GT(scaled.sparseVerifyArms, 0);
  EXPECT_GT(maxDocLike.pointsArms + maxDocLike.complementArms, 0);
  EXPECT_EQ(0, maxDocLike.sparseVerifyArms);
}

TEST_F(SearchEngineTest,
       exhaustiveNumericUnionCountUsesGroupExposure) {
  constexpr std::string_view collection = "numeric_count_group_exposure";
  constexpr int32_t N = 2 * DocsEnumMeta::L1_DOCS + 257;
  constexpr int64_t hi = (int64_t) N * 4 / 5 - 1;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  SchemaBuilder schema;
  auto& range = schema.field("range_i");
  range.type = api::FieldDef::FieldClass::INT;
  range.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());

  std::vector<Doc> docs;
  docs.reserve(N);
  int64_t expected = 0;
  for (int32_t doc = 0; doc < N; doc++) {
    bool left = doc % 4 == 0;
    bool right = doc % 4 == 1;
    expected += (left || right) && doc <= hi;
    std::string body = left ? "union_a" : right ? "union_b" : "other";
    docs.push_back(flatdoc(
        "id", "numeric_group_" + std::to_string(doc),
        "body_w", body, "range_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Run {
    int64_t found;
    int64_t pointsArms;
    int64_t complementArms;
    int64_t sparseVerifyArms;
  };
  auto run = [&](bool disableShapes) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() = qb::boolean(
        topDocs.mr(), {},
        {qb::match(topDocs.mr(), "body_w", "union_a"),
         qb::match(topDocs.mr(), "body_w", "union_b")},
        {},
        {qb::range(
            topDocs.mr(), "range_i", qb::valI64(topDocs.mr(), 0),
            nullptr, qb::valI64(topDocs.mr(), hi), nullptr)},
        1);
    SkipStatsGuard stats;
    {
      NumericRangeShapeGuard shapeGuard(disableShapes);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::numericRangePointsArms,
      SkipStats::numericRangeComplementArms,
      SkipStats::numericRangeSparseVerifyArms,
    };
  };

  Run planned = run(false);
  Run shapesDisabled = run(true);
  EXPECT_EQ(expected, planned.found);
  EXPECT_EQ(planned.found, shapesDisabled.found);
  EXPECT_GT(planned.pointsArms + planned.complementArms, 0);
  EXPECT_EQ(0, planned.sparseVerifyArms);
  EXPECT_GT(shapesDisabled.pointsArms
                + shapesDisabled.complementArms, 0);
  EXPECT_EQ(0, shapesDisabled.sparseVerifyArms);
}

TEST_F(SearchEngineTest,
       exhaustiveNumericNegatedCountUsesPositiveExposure) {
  constexpr std::string_view collection = "numeric_count_negated_exposure";
  constexpr int32_t N = 2 * DocsEnumMeta::L1_DOCS + 257;
  constexpr int64_t hi = (int64_t) N * 4 / 5 - 1;
  CollectionHelper helper(collection);
  helper.getIndexWriter()->filterCache = std::make_shared<FilterCache>(
      FilterCacheConfig{.maxBytes = 0});
  SchemaBuilder schema;
  auto& range = schema.field("range_i");
  range.type = api::FieldDef::FieldClass::INT;
  range.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());

  std::vector<Doc> docs;
  docs.reserve(N);
  int64_t expected = 0;
  for (int32_t doc = 0; doc < N; doc++) {
    bool dense = (doc & 1) == 0;
    expected += dense && doc > hi;
    docs.push_back(flatdoc(
        "id", "numeric_negated_" + std::to_string(doc),
        "body_w", dense ? "dense" : "other", "range_i", doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Run {
    int64_t found;
    int64_t pointsArms;
    int64_t complementArms;
    int64_t sparseVerifyArms;
  };
  auto run = [&](bool disableShapes) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& topDocs = req->topDocs("q").getNumber().limit(0);
    topDocs.rawQuery() = qb::boolean(
        topDocs.mr(),
        {qb::match(topDocs.mr(), "body_w", "dense")}, {},
        {qb::range(
            topDocs.mr(), "range_i", qb::valI64(topDocs.mr(), 0),
            nullptr, qb::valI64(topDocs.mr(), hi), nullptr)});
    SkipStatsGuard stats;
    {
      NumericRangeShapeGuard shapeGuard(disableShapes);
      req->execute(false);
    }
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Run{
      req->getMatchCount("q"),
      SkipStats::numericRangePointsArms,
      SkipStats::numericRangeComplementArms,
      SkipStats::numericRangeSparseVerifyArms,
    };
  };

  Run planned = run(false);
  Run shapesDisabled = run(true);
  EXPECT_EQ(expected, planned.found);
  EXPECT_EQ(planned.found, shapesDisabled.found);
  EXPECT_GT(planned.pointsArms + planned.complementArms, 0);
  EXPECT_EQ(0, planned.sparseVerifyArms);
  EXPECT_GT(shapesDisabled.pointsArms
                + shapesDisabled.complementArms, 0);
  EXPECT_EQ(0, shapesDisabled.sparseVerifyArms);
}
