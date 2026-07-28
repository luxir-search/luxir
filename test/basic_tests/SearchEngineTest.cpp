
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
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

public:
  explicit TopKCountCompositionGuard(bool disabled)
    : saved(disableTopKCountComposition) {
    disableTopKCountComposition = disabled;
  }
  ~TopKCountCompositionGuard() {
    disableTopKCountComposition = saved;
  }
};

class SparseFilteredTopKRerouteGuard {
  bool savedDisabled;
  std::array<int32_t, 3> savedDensityInverse;

public:
  SparseFilteredTopKRerouteGuard(bool disabled, int32_t densityInverse)
    : savedDisabled(TopDocsReq::disableSparseFilteredTopKRerouteForTests),
      savedDensityInverse(
          TopDocsReq::sparseFilteredTopKDensityInverseForTests) {
    TopDocsReq::disableSparseFilteredTopKRerouteForTests = disabled;
    TopDocsReq::sparseFilteredTopKDensityInverseForTests.fill(
        densityInverse);
  }

  ~SparseFilteredTopKRerouteGuard() {
    TopDocsReq::disableSparseFilteredTopKRerouteForTests =
        savedDisabled;
    TopDocsReq::sparseFilteredTopKDensityInverseForTests =
        savedDensityInverse;
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

struct FilteredCountResult {
  int64_t count;
  int64_t denseWindows;
  int64_t disjGroupWindows;
  int64_t sparseFallbacks;
  int64_t bulkFillCalls;
  int64_t docsOnlyWordProbeAdvances;
  int64_t tfreqBlocksDecoded;
  int64_t filteredDisjBatchEngagements;
  int64_t filteredDisjBatchCountWindows;
  int64_t filteredDisjBatchScoreWindows;
  int64_t filteredConjBatchCountWindows;
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
    SkipStats::filteredDisjBatchCountWindows,
    SkipStats::filteredDisjBatchScoreWindows,
    SkipStats::filteredConjBatchCountWindows,
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
    SkipStats::filteredDisjBatchCountWindows,
    SkipStats::filteredDisjBatchScoreWindows,
    SkipStats::filteredConjBatchCountWindows,
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

  req->avg("root_avg", "foo_i");
  req->min("root_min", "foo_i");
  req->max("root_max", "foo_i");

  req->execute();

  ASSERT_EQ(1u, req->responses.size()) << req->toString();
  const auto& response = req->responses[0]->proto;
  ASSERT_FALSE(hasError(response)) << req->toString();
  ASSERT_TRUE(response.ops.contains("root_avg")) << req->toString();
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_avg")));
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_min")));
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_max")));

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(0, docs->found.value_or(0));
  ASSERT_TRUE(docs->ops.contains("nested_avg")) << req->toString();
  EXPECT_TRUE(std::isnan(std::get<double>(docs->ops.at("nested_avg")->kind)));
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

TEST_F(SearchEngineTest, singleTermLimitZeroFacetUsesBulkDomain) {
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
      kDocSetTermDenseThresholdInverse);
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
      kDocSetTermDenseThresholdInverse);
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

TEST_F(SearchEngineTest, sparseFilteredTopKRerouteIsConjunctionOnly) {
  constexpr std::string_view collection = "sparse_filtered_topk_reroute";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  constexpr int32_t nDocs = 1024;
  docs.reserve(nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string filter = (doc % 512) == 0 ? "sparse " : "";
    if ((doc & 1) == 0) filter += "dense";
    docs.push_back(flatdoc(
        "id", "reroute_" + std::to_string(doc),
        "body_w", "alpha beta quick fox",
        "filter_w", filter));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t reroutes;
    int64_t densityRejects;
    int64_t shapeRejects;
  };
  auto run = [&](FilteredCountShape shape, std::string_view filter,
                 bool exact, bool disabled) {
    SparseFilteredTopKRerouteGuard rerouteGuard(disabled, 64);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getScores().fields({"id"}).limit(10);
    if (exact) cur.getNumber();
    cur.rawQuery() = filteredCountBody(cur.mr(), shape);
    if (!filter.empty()) {
      cur.matchFilter("filter", "filter_w", filter);
    }
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      SkipStats::sparseFilteredTopKReroutes,
      SkipStats::sparseFilteredTopKDensityRejects,
      SkipStats::sparseFilteredTopKShapeRejects,
    };
  };

  for (FilteredCountShape shape :
       {FilteredCountShape::TERM, FilteredCountShape::INTERSECTION,
        FilteredCountShape::PHRASE}) {
    Result pruned = run(shape, "sparse", false, true);
    Result routed = run(shape, "sparse", false, false);
    EXPECT_EQ(pruned.ids, routed.ids);
    expectSameScoreMap(pruned.scores, routed.scores);
    EXPECT_EQ(0, pruned.reroutes);
    EXPECT_EQ(1, routed.reroutes);
  }

  Result dense = run(FilteredCountShape::TERM, "dense", false, false);
  EXPECT_EQ(0, dense.reroutes);
  EXPECT_EQ(1, dense.densityRejects);

  Result unionResult =
      run(FilteredCountShape::UNION, "sparse", false, false);
  EXPECT_EQ(0, unionResult.reroutes);
  EXPECT_EQ(1, unionResult.shapeRejects);

  Result exact = run(FilteredCountShape::TERM, "sparse", true, false);
  EXPECT_EQ(0, exact.reroutes);

  Result unfiltered = run(FilteredCountShape::TERM, "", false, false);
  EXPECT_EQ(0, unfiltered.reroutes);
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
    runFilteredCount(soluxNode->getSearchEngine(),
                     FilteredCountShape::UNION, "selected",
                     FilteredCountPath::FOLDED, collection);
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
    };
  };

  run(false);
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

TEST_F(SearchEngineTest, exactCountTopKComposesCountAndPrunedRanking) {
  constexpr std::string_view collection = "exact_count_topk_composition";
  constexpr int32_t nDocs = DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    if ((doc & 1) == 0) body += "alpha ";
    if ((doc % 3) != 0) body += "beta ";
    body += "filler";
    docs.push_back(flatdoc(
        "id", "compose_" + std::to_string(doc),
        "body_w", body,
        "filter_w", (doc % 100) == 0 ? "selected" : "other"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  struct Result {
    std::vector<std::string> ids;
    std::map<std::string, float> scores;
    int64_t count;
    int64_t compositions;
  };
  auto run = [&](bool disabled) {
    TopKCountCompositionGuard compositionGuard(disabled);
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").getNumber().withStats()
        .fields({"id"}).limit(100);
    cur.rawQuery() = qb::boolean(cur.mr(), {},
        {qb::match(cur.mr(), "body_w", "alpha"),
         qb::match(cur.mr(), "body_w", "beta")});
    cur.matchFilter("filter", "filter_w", "selected");
    SkipStatsGuard statsGuard;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{
      resultIds(*req, "q"),
      resultScoreMap(*req, "q"),
      req->getMatchCount("q"),
      SkipStats::exactCountTopKCompositions,
    };
  };

  run(false);
  run(false);
  Result composed = run(false);
  Result exhaustive = run(true);
  EXPECT_EQ(exhaustive.count, composed.count);
  EXPECT_EQ(exhaustive.ids, composed.ids);
  EXPECT_EQ(exhaustive.scores, composed.scores);
  EXPECT_GT(composed.compositions, 0);
  EXPECT_EQ(0, exhaustive.compositions);
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
      .getNumber().fields({"id"}).limit(-1).matchFilter("keep", "keep_s", "yes");
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
  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("../bad");
  req->topDocs("q").allQuery();
  ExpectLog quiet("Search request failed:");
  req->execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_NE(req->errorMsg().find("single path component"), std::string::npos) << req->errorMsg();
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
