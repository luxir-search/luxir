// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/SortField.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/random.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

using namespace luxir;
using namespace luxir::test;

namespace {

class StringSortModeGuard {
  StringSortMode saved;

public:
  explicit StringSortModeGuard(StringSortMode mode)
      : saved(SortField::setStringSortModeForTests(mode)) {}
  ~StringSortModeGuard() { SortField::setStringSortModeForTests(saved); }
};

struct StringSortResult {
  std::vector<std::string> ids;
  std::vector<float> scores;
  bool operator==(const StringSortResult&) const = default;
};

class FieldSortBulkGuard {
  bool saved;

public:
  explicit FieldSortBulkGuard(bool disabled)
      : saved(disableFieldSortBulk) {
    disableFieldSortBulk = disabled;
  }
  ~FieldSortBulkGuard() {
    disableFieldSortBulk = saved;
  }
};

class KeyGatherGuard {
  bool saved;

public:
  explicit KeyGatherGuard(bool disabled)
      : saved(FieldSortCollector::disableKeyGatherForTests) {
    FieldSortCollector::disableKeyGatherForTests = disabled;
  }
  ~KeyGatherGuard() {
    FieldSortCollector::disableKeyGatherForTests = saved;
  }
};

class SortPruningGuard {
  bool saved;

public:
  explicit SortPruningGuard(bool disabled)
      : saved(disableFieldSortPruning) {
    disableFieldSortPruning = disabled;
  }
  ~SortPruningGuard() {
    disableFieldSortPruning = saved;
  }
};

using SortSkipStatsGuard = SkipStatsScope;

class TopDocsFilterFoldGuard {
  bool saved;

public:
  explicit TopDocsFilterFoldGuard(bool disabled)
      : saved(disableTopDocsFilterFold) {
    disableTopDocsFilterFold = disabled;
  }
  ~TopDocsFilterFoldGuard() { disableTopDocsFilterFold = saved; }
};

class WholeMembershipPlanGuard {
  bool saved;

public:
  explicit WholeMembershipPlanGuard(bool disabled)
      : saved(QueryPrep::disableWholeMembershipPlanForTests) {
    QueryPrep::disableWholeMembershipPlanForTests = disabled;
  }
  ~WholeMembershipPlanGuard() {
    QueryPrep::disableWholeMembershipPlanForTests = saved;
  }
};

class BestFirstGuard {
  bool savedDisable;
  bool savedForce;

public:
  BestFirstGuard(bool disabled, bool force)
      : savedDisable(disableFieldSortBestFirst),
        savedForce(forceFieldSortBestFirst) {
    disableFieldSortBestFirst = disabled;
    forceFieldSortBestFirst = force;
  }
  ~BestFirstGuard() {
    disableFieldSortBestFirst = savedDisable;
    forceFieldSortBestFirst = savedForce;
  }
};

class WorkCapGuard {
  int64_t saved;

public:
  explicit WorkCapGuard(int64_t cap) : saved(forceFieldSortWorkCapForTests) {
    forceFieldSortWorkCapForTests = cap;
  }
  ~WorkCapGuard() { forceFieldSortWorkCapForTests = saved; }
};

class SeededGuard {
  bool savedDisable;
  bool savedForce;
  int32_t savedBudget;

public:
  SeededGuard(bool disabled, bool force, int32_t budgetPerMille = 0)
      : savedDisable(disableFieldSortSeeding),
        savedForce(forceFieldSortSeeding),
        savedBudget(fieldSortSeedBudgetPerMilleForTests) {
    disableFieldSortSeeding = disabled;
    forceFieldSortSeeding = force;
    fieldSortSeedBudgetPerMilleForTests = budgetPerMille;
  }
  ~SeededGuard() {
    disableFieldSortSeeding = savedDisable;
    forceFieldSortSeeding = savedForce;
    fieldSortSeedBudgetPerMilleForTests = savedBudget;
  }
};

struct FieldSortBulkResult {
  std::vector<std::string> ids;
  std::vector<int64_t> ints;
  std::vector<std::string> strings;
  int64_t hitCount = 0;
  int64_t denseMatchWindows = 0;
  int64_t sparseMatchWindows = 0;
  int64_t bulkFillCalls = 0;
};

} // namespace

class SortCollectorTest : public LuxirTest {
protected:
  struct WindowResult {
    std::vector<segdoc> docs;
    int64_t hitCount;
    bool operator==(const WindowResult&) const = default;
  };

  static std::vector<SortClause> columnPlan(const SortField& field) {
    return {SortClause(field)};
  }

  static std::vector<std::string> resultIds(const LocalReq& req) {
    const auto* docs = req.docList("q");
    if (docs == nullptr) return {};
    const auto* column = docs->columns.find("id_s");
    if (column == nullptr) return {};
    const auto& col = std::get<luxir::api::ColStr>(column->kind);
    std::vector<std::string> ids;
    for (auto id : col.v) ids.emplace_back(id);
    return ids;
  }

  void runStringCandidatePruning(int32_t nSegs);

  static WindowResult collectNumericWindows(
      IndexReader& reader, std::string_view field, int64_t topCount,
      SortField::SortOrder order, FieldComparator::MissingValue missing,
      bool disableGather, std::span<const int32_t> segmentOrder = {}) {
    IntFieldType fieldType(field);
    SortField sortField(field, fieldType, order, missing);
    FieldSortCollector collector(topCount, columnPlan(sortField));
    KeyGatherGuard guard(disableGather);

    auto collectSegment = [&](int32_t segment) {
      auto& leaf = reader.segments()[(size_t)segment];
      collector.setSegment(segment, &leaf.postingsReader());
      std::vector<int32_t> docs;
      for (int32_t doc = 0; doc < leaf.maxDoc(); doc++) {
        if (leaf.liveDocs() == nullptr || leaf.liveDocs()->bitset().get(doc)) {
          docs.push_back(doc);
        }
      }
      collector.collectWindow(segment, docs);
    };

    if (segmentOrder.empty()) {
      for (int32_t segment = 0;
           segment < (int32_t)reader.segments().size(); segment++) {
        collectSegment(segment);
      }
    } else {
      for (int32_t segment : segmentOrder) collectSegment(segment);
    }

    WindowResult result;
    result.hitCount = collector.totalHits();
    for (const auto& doc : collector.sort()) result.docs.push_back(doc.doc);
    return result;
  }
};

TEST_F(SortCollectorTest, unscoredDisjunctionBulkMatchesPull) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper;
  std::vector<Doc> segment;
  std::vector<std::string> deleted;
  for (int32_t i = 0; i < 384; i++) {
    std::string id = "d" + std::to_string(i);
    std::string body;
    if ((i & 1) == 0) body += "alpha ";
    if (i % 3 == 0) body += "beta ";
    if (i % 5 == 0) body += "gamma ";
    if (body.empty()) body = "other";
    int64_t sortValue = (i * 37) % 503;
    segment.push_back(flatdoc(
        "id", id, "id_s", id, "body_w", body,
        "sort_i", sortValue,
        "sort_s", "v" + std::to_string(1000 + sortValue)));
    if (i % 11 == 0) {
      deleted.push_back(id);
    }
    if (segment.size() == 192) {
      ASSERT_TRUE(helper.indexAll(segment, UpdateMessage::COMMIT).success);
      segment.clear();
    }
  }
  ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);

  enum Shape { DISJ, DISJ_CONJ, TERM, MAND_OPT };
  auto run = [&](bool disableBulk, bool disableGather, Shape shape,
                 std::string_view sortField, qb::SortDir direction,
                 int32_t limit) {
    FieldSortBulkGuard guard(disableBulk);
    KeyGatherGuard gatherGuard(disableGather);
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q").getNumber().limit(limit)
        .fields({"id_s", "sort_i", "sort_s"});
    auto& mr = cursor.mr();
    switch (shape) {
      case DISJ_CONJ: {
        auto betaGamma = qb::boolean(
            mr, {qb::match(mr, "body_w", "beta"),
                 qb::match(mr, "body_w", "gamma")});
        cursor.rawQuery() = qb::boolean(
            mr, {}, {qb::match(mr, "body_w", "alpha"), betaGamma});
        break;
      }
      case DISJ:
        cursor.rawQuery() = qb::boolean(
            mr, {}, {qb::match(mr, "body_w", "alpha"),
                     qb::match(mr, "body_w", "beta"),
                     qb::match(mr, "body_w", "gamma")});
        break;
      case TERM:
        cursor.rawQuery() = qb::match(mr, "body_w", "alpha");
        break;
      case MAND_OPT:
        // Unscored optional-drop reduces this to the bare alpha term, so the
        // windowed route exercises the term match windows through the
        // delegated supplier.
        cursor.rawQuery() = qb::boolean(
            mr, {qb::match(mr, "body_w", "alpha")},
            {qb::match(mr, "body_w", "beta")});
        break;
    }
    qb::sort(cursor, sortField, direction);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    FieldSortBulkResult result;
    const auto* docs = req->docList("q");
    EXPECT_NE(docs, nullptr);
    if (docs == nullptr) return result;
    result.hitCount = docs->found.value_or(0);
    const auto& ids =
        std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind).v;
    const auto& ints =
        std::get<luxir::api::ColInt>(docs->columns.at("sort_i").kind).v;
    const auto& strings =
        std::get<luxir::api::ColStr>(docs->columns.at("sort_s").kind).v;
    result.ids.assign(ids.begin(), ids.end());
    result.ints.assign(ints.begin(), ints.end());
    result.strings.reserve(strings.size());
    for (auto value : strings) result.strings.emplace_back(value);
    return result;
  };

  auto assertParity = [&](Shape shape, std::string_view sortField,
                          qb::SortDir direction, int32_t limit) {
    auto pull = run(true, true, shape, sortField, direction, limit);
    auto windowFallback =
        run(false, true, shape, sortField, direction, limit);
    auto gathered = run(false, false, shape, sortField, direction, limit);
    EXPECT_EQ(pull.ids, windowFallback.ids);
    EXPECT_EQ(pull.ints, windowFallback.ints);
    EXPECT_EQ(pull.strings, windowFallback.strings);
    EXPECT_EQ(pull.hitCount, windowFallback.hitCount);
    EXPECT_EQ(windowFallback.ids, gathered.ids);
    EXPECT_EQ(windowFallback.ints, gathered.ints);
    EXPECT_EQ(windowFallback.strings, gathered.strings);
    EXPECT_EQ(windowFallback.hitCount, gathered.hitCount);
  };

  assertParity(DISJ, "sort_i", qb::ASC, 9);
  assertParity(DISJ, "sort_i", qb::DESC, 1000);
  assertParity(DISJ, "sort_s", qb::ASC, 17);
  assertParity(DISJ_CONJ, "sort_s", qb::DESC, 1000);
  assertParity(TERM, "sort_i", qb::ASC, 9);
  assertParity(TERM, "sort_i", qb::DESC, 1000);
  assertParity(TERM, "sort_s", qb::ASC, 17);
  assertParity(MAND_OPT, "sort_i", qb::DESC, 9);
  assertParity(MAND_OPT, "sort_i", qb::ASC, 1000);
  assertParity(MAND_OPT, "sort_s", qb::DESC, 17);
}

TEST_F(SortCollectorTest, numericKeyGatherMatchesFallbackAcrossColumnShapes) {
  CollectionHelper helper;
  std::vector<std::string> deleted;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    for (int32_t doc = 0; doc < 96; doc++) {
      std::string id =
          "s" + std::to_string(segment) + "d" + std::to_string(doc);
      int64_t value = (doc * 37 + segment * 11) % 101;
      Doc input = flatdoc(
          "id", id, "id_s", id, "dense_i", value,
          "dense_is", vec_i(value + 200, -value));
      if (doc % 3 != 0) {
        input.push_back({"sparse_i", value - 50});
      }
      if (doc % 4 == 1 || doc % 4 == 2) {
        input.push_back({"sparse_is", vec_i(value + 300, value - 300)});
      }
      docs.push_back(std::move(input));
      if (doc == 7 || doc == 53) deleted.push_back(id);
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 2);
  int64_t hits = reader->liveDocs();
  for (std::string_view field :
       {"dense_i", "sparse_i", "dense_is", "sparse_is"}) {
    for (SortField::SortOrder order : {SortField::ASC, SortField::DESC}) {
      for (FieldComparator::MissingValue missing :
           {FieldComparator::MISSING_FIRST, FieldComparator::MISSING_LAST}) {
        for (int64_t topCount : {int64_t(7), hits + 5}) {
          auto fallback = collectNumericWindows(
              *reader, field, topCount, order, missing, true);
          auto gathered = collectNumericWindows(
              *reader, field, topCount, order, missing, false);
          EXPECT_EQ(gathered, fallback)
              << field << " order=" << order << " missing=" << missing
              << " topCount=" << topCount;
          EXPECT_EQ(gathered.hitCount, hits);
        }
      }
    }
  }
}

TEST_F(SortCollectorTest, keyGatherTieBreaksAcrossReversedSegments) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.index(
      flatdoc("id", "early", "key_i", 10), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(
      flatdoc("id", "late", "key_i", 10), UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(reader->segments().size(), 2);
  std::array<int32_t, 2> reversed = {1, 0};

  for (std::string_view field : {"key_i", "absent_i"}) {
    auto fallback = collectNumericWindows(
        *reader, field, 1, SortField::ASC, FieldComparator::MISSING_LAST,
        true, reversed);
    auto gathered = collectNumericWindows(
        *reader, field, 1, SortField::ASC, FieldComparator::MISSING_LAST,
        false, reversed);
    EXPECT_EQ(gathered, fallback);
    ASSERT_EQ(gathered.docs.size(), 1);
    EXPECT_EQ(gathered.docs[0], segdoc(0, 0));
    EXPECT_EQ(gathered.hitCount, 2);
  }
}

TEST_F(SortCollectorTest, keyGatherWarmupCrossesChunkPositions) {
  constexpr int32_t numDocs = 1030;
  const std::array<int32_t, 4> topCounts = {1, 512, 1024, 1025};
  CollectionHelper helper;
  std::vector<Doc> docs;
  docs.reserve(numDocs);
  for (int32_t doc = 0; doc < numDocs; doc++) {
    Doc input = flatdoc("id", "d" + std::to_string(doc));
    for (int32_t topCount : topCounts) {
      std::string field = "warm" + std::to_string(topCount) + "_i";
      int64_t value = doc == topCount ? -1 : 100000 + doc;
      input.push_back({std::move(field), value});
    }
    docs.push_back(std::move(input));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();

  for (int32_t topCount : topCounts) {
    std::string field = "warm" + std::to_string(topCount) + "_i";
    auto fallback = collectNumericWindows(
        *reader, field, topCount, SortField::ASC,
        FieldComparator::MISSING_LAST, true);
    auto gathered = collectNumericWindows(
        *reader, field, topCount, SortField::ASC,
        FieldComparator::MISSING_LAST, false);
    EXPECT_EQ(gathered, fallback) << "topCount=" << topCount;
    EXPECT_NE(std::find(
                  gathered.docs.begin(), gathered.docs.end(),
                  segdoc(0, topCount)),
              gathered.docs.end())
        << "the doc immediately after warmup was not admitted";
    EXPECT_EQ(gathered.hitCount, numDocs);
  }
}

TEST_F(SortCollectorTest, sparseLandingReusesPresentCandidate) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 8; doc++) {
    Doc input = flatdoc("id", "d" + std::to_string(doc));
    if (doc == 7) {
      input.push_back({"sparse_i", int64_t(30)});
      input.push_back({"sparse_is", vec_i(30, -100)});
    }
    docs.push_back(std::move(input));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
  std::array<int32_t, 2> candidates = {5, 7};

  auto run = [&](std::string_view field, bool disableGather) {
    IntFieldType fieldType(field);
    SortField sortField(
        field, fieldType, SortField::ASC, FieldComparator::MISSING_LAST);
    FieldSortCollector collector(2, columnPlan(sortField));
    KeyGatherGuard guard(disableGather);
    auto& segment = reader->segments()[0];
    collector.setSegment(0, &segment.postingsReader());
    collector.collectWindow(0, candidates);
    WindowResult result;
    result.hitCount = collector.totalHits();
    for (const auto& doc : collector.sort()) result.docs.push_back(doc.doc);
    return result;
  };

  for (std::string_view field : {"sparse_i", "sparse_is"}) {
    auto fallback = run(field, true);
    auto gathered = run(field, false);
    EXPECT_EQ(gathered, fallback);
    EXPECT_EQ(gathered.docs, (std::vector<segdoc>{segdoc(0, 7), segdoc(0, 5)}));
    EXPECT_EQ(gathered.hitCount, 2);
  }
}

TEST_F(SortCollectorTest, numericMissingPlacementIsDirectionIndependent) {
  CollectionHelper helper;
  std::vector<Doc> docs = {
      flatdoc("id", "d0"),
      flatdoc("id", "d1", "value_i", 10),
      flatdoc("id", "d2"),
      flatdoc("id", "d3", "value_i", 20),
  };
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();

  struct Case {
    SortField::SortOrder order;
    FieldComparator::MissingValue missing;
    std::vector<segdoc> expected;
  };
  std::vector<Case> cases = {
      {SortField::ASC, FieldComparator::MISSING_FIRST,
       {segdoc(0, 0), segdoc(0, 2), segdoc(0, 1), segdoc(0, 3)}},
      {SortField::DESC, FieldComparator::MISSING_FIRST,
       {segdoc(0, 0), segdoc(0, 2), segdoc(0, 3), segdoc(0, 1)}},
      {SortField::ASC, FieldComparator::MISSING_LAST,
       {segdoc(0, 1), segdoc(0, 3), segdoc(0, 0), segdoc(0, 2)}},
      {SortField::DESC, FieldComparator::MISSING_LAST,
       {segdoc(0, 3), segdoc(0, 1), segdoc(0, 0), segdoc(0, 2)}},
  };

  for (const auto& testCase : cases) {
    auto fallback = collectNumericWindows(
        *reader, "value_i", 4, testCase.order, testCase.missing, true);
    auto gathered = collectNumericWindows(
        *reader, "value_i", 4, testCase.order, testCase.missing, false);
    EXPECT_EQ(gathered, fallback);
    EXPECT_EQ(gathered.docs, testCase.expected);
  }
}

TEST_F(SortCollectorTest, collectWindowFallbackCountsEachHitOnce) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 9; doc++) {
    docs.push_back(flatdoc(
        "id", "d" + std::to_string(doc),
        "primary_i", doc % 3, "secondary_i", 9 - doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
  IntFieldType primaryType("primary_i");
  IntFieldType secondaryType("secondary_i");
  std::vector<SortClause> clauses = {
      SortClause(SortField("primary_i", primaryType, SortField::ASC)),
      SortClause(SortField("secondary_i", secondaryType, SortField::ASC)),
  };
  FieldSortCollector collector(4, clauses);
  auto& segment = reader->segments()[0];
  collector.setSegment(0, &segment.postingsReader());
  std::array<int32_t, 9> candidates = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  collector.collectWindow(0, candidates);

  EXPECT_EQ(collector.totalHits(), (int64_t)candidates.size());
  EXPECT_EQ(collector.size(), 4);
}

TEST_F(SortCollectorTest, unscoredConjunctionBulkMatchesPull) {
  WholeMembershipPlanGuard wholeGuard(true);
  constexpr int32_t segmentDocs = DocsEnumMeta::L1_DOCS + 257;
  CollectionHelper helper;
  std::vector<std::string> deleted;
  for (int32_t segment = 0; segment < 2; segment++) {
    std::vector<Doc> docs;
    docs.reserve(segmentDocs);
    for (int32_t local = 0; local < segmentDocs; local++) {
      int32_t doc = segment * segmentDocs + local;
      std::string id = "c" + std::to_string(doc);
      std::string body = "alpha beta gamma common quick fox ";
      body += (local & 1) == 0 ? "zero_a " : "zero_b ";
      if ((local % 5) != 0) body += "keep ";
      if ((local % 701) == 0) body += "rare2 ";
      if ((local % 733) == 0) body += "rare3 ";
      int64_t sortValue = (doc * 37) % 10007;
      docs.push_back(flatdoc(
          "id", id, "id_s", id, "body_w", body,
          "sort_i", sortValue,
          "sort_s", "s" + std::to_string(100000 + sortValue)));
      if ((local % 997) == 0) {
        deleted.push_back(id);
      }
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);

  enum class Shape {
    DENSE_TWO,
    DENSE_THREE,
    SPARSE_TWO,
    SPARSE_THREE,
    DISJ_GROUP,
    PHRASE,
    FILTERED,
    FILTER_ONLY,
    ZERO,
    TERM,
    MAND_OPT,  // unscored optional-drop reduces to the bare term
  };
  enum class MatchPath {
    DENSE,
    SPARSE,
    FILTER_ONLY,
    PULL,  // two-phase clauses keep the pull conjunction; no bulk windows
    TERM_FILL,  // TermBulkScorer window-bit fills
  };

  auto buildQuery = [](std::pmr::memory_resource& mr, Shape shape) {
    auto term = [&](std::string_view value) {
      return qb::match(mr, "body_w", value);
    };
    switch (shape) {
      case Shape::DENSE_TWO:
        return qb::boolean(mr, {term("alpha"), term("beta")});
      case Shape::DENSE_THREE:
        return qb::boolean(
            mr, {term("alpha"), term("beta"), term("gamma")});
      case Shape::SPARSE_TWO:
        return qb::boolean(mr, {term("rare2"), term("common")});
      case Shape::SPARSE_THREE:
        return qb::boolean(
            mr, {term("rare3"), term("alpha"), term("beta")});
      case Shape::DISJ_GROUP: {
        auto group = qb::boolean(
            mr, {}, {term("beta"), term("gamma")});
        return qb::boolean(mr, {term("alpha"), group});
      }
      case Shape::PHRASE:
        return qb::boolean(
            mr, {term("alpha"),
                 qb::phraseWords(mr, "body_w", {"quick", "fox"})});
      case Shape::FILTERED:
        return qb::boolean(
            mr, {term("alpha"), term("beta")}, {}, {}, {term("keep")});
      case Shape::FILTER_ONLY: {
        auto disjunction = qb::boolean(
            mr, {}, {term("rare2"), term("rare3")});
        return qb::boolean(
            mr, {}, {}, {}, {disjunction, term("common")});
      }
      case Shape::ZERO:
        return qb::boolean(mr, {term("zero_a"), term("zero_b")});
      case Shape::TERM:
        return term("alpha");
      case Shape::MAND_OPT:
        return qb::boolean(mr, {term("alpha")}, {term("beta")});
    }
    std::unreachable();
  };

  auto run = [&](bool disableBulk, Shape shape, std::string_view sortField,
                 qb::SortDir direction) {
    FieldSortBulkGuard bulkGuard(disableBulk);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q").getNumber().limit(97)
        .fields({"id_s"});
    cursor.rawQuery() = buildQuery(cursor.mr(), shape);
    qb::sort(cursor, sortField, direction);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    FieldSortBulkResult result;
    result.hitCount = req->getMatchCount("q");
    result.ids = resultIds(*req);
    result.denseMatchWindows = SkipStats::conjDenseMatchWindows;
    result.sparseMatchWindows = SkipStats::conjMatchFallbacks;
    result.bulkFillCalls = SkipStats::countBulkFillCalls;
    return result;
  };

  auto assertParity = [&](Shape shape, MatchPath expectedPath,
                          std::string_view sortField,
                          qb::SortDir direction) {
    auto pull = run(true, shape, sortField, direction);
    auto bulk = run(false, shape, sortField, direction);
    EXPECT_EQ(pull.ids, bulk.ids);
    EXPECT_EQ(pull.hitCount, bulk.hitCount);
    if (expectedPath == MatchPath::DENSE) {
      EXPECT_GT(bulk.denseMatchWindows, 0);
    } else if (expectedPath == MatchPath::SPARSE) {
      EXPECT_GT(bulk.sparseMatchWindows, 0);
    } else if (expectedPath == MatchPath::FILTER_ONLY
               || expectedPath == MatchPath::TERM_FILL) {
      EXPECT_GT(bulk.bulkFillCalls, 0);
    } else {
      EXPECT_EQ(bulk.denseMatchWindows, 0);
      EXPECT_EQ(bulk.sparseMatchWindows, 0);
    }
  };

  constexpr std::array<std::pair<std::string_view, qb::SortDir>, 4> sorts = {{
    {"sort_i", qb::ASC},
    {"sort_i", qb::DESC},
    {"sort_s", qb::ASC},
    {"sort_s", qb::DESC},
  }};
  constexpr std::array<std::pair<Shape, MatchPath>, 11> shapes = {{
    {Shape::DENSE_TWO, MatchPath::DENSE},
    {Shape::DENSE_THREE, MatchPath::DENSE},
    {Shape::SPARSE_TWO, MatchPath::SPARSE},
    {Shape::SPARSE_THREE, MatchPath::SPARSE},
    {Shape::DISJ_GROUP, MatchPath::DENSE},
    {Shape::PHRASE, MatchPath::PULL},
    {Shape::FILTERED, MatchPath::DENSE},
    {Shape::FILTER_ONLY, MatchPath::FILTER_ONLY},
    {Shape::ZERO, MatchPath::DENSE},
    {Shape::TERM, MatchPath::TERM_FILL},
    {Shape::MAND_OPT, MatchPath::TERM_FILL},
  }};
  if (effort == 1) {
    // Exercise every scorer shape with one sort, then cover the remaining sort
    // field/direction combinations with a representative dense conjunction.
    for (auto [shape, expectedPath] : shapes) {
      SCOPED_TRACE("shape=" + std::to_string((int32_t) shape));
      assertParity(shape, expectedPath, sorts[0].first, sorts[0].second);
    }
    for (size_t i = 1; i < sorts.size(); i++) {
      auto [field, direction] = sorts[i];
      SCOPED_TRACE("field=" + std::string(field)
                   + " direction=" + std::to_string((int32_t) direction));
      assertParity(Shape::DENSE_TWO, MatchPath::DENSE, field, direction);
    }
  } else {
    for (auto [shape, expectedPath] : shapes) {
      for (auto [field, direction] : sorts) {
        SCOPED_TRACE("shape=" + std::to_string((int32_t) shape)
                     + " field=" + std::string(field)
                     + " direction=" + std::to_string((int32_t) direction));
        assertParity(shape, expectedPath, field, direction);
      }
    }
  }
}

TEST_F(SortCollectorTest, testPQ) {
  // make sure segment takes priority over docid
  ASSERT_LT(segdoc(0,100), segdoc(1,0));

  class SortDoc {
  public:
    segdoc doc;
    int64_t sortValue;
  };

  // for an ascending compare we want the least competitive (highest sortValue) at the top of the heap
  // This is the normal case for a max-heap, so the sort order is just the natural order
  auto ascendingCompare = [](const SortDoc& a, const SortDoc& b) {
    if (a.sortValue != b.sortValue) {
      return a.sortValue < b.sortValue;
    }
    return a.doc < b.doc; // tie-breaker, low docid first
  };

  std::vector<SortDoc> sortDocs(3);
  DirectPQ<SortDoc, decltype(ascendingCompare)> pq(sortDocs);

  pq.insertWithOverflow({segdoc(0,1), 500});
  pq.insertWithOverflow({segdoc(0,2), 800});
  pq.insertWithOverflow({segdoc(0,3), 200});
  pq.insertWithOverflow({segdoc(0,4), 400});
  pq.insertWithOverflow({segdoc(0,5), 100});
  pq.insertWithOverflow({segdoc(0,6), 400});  // repeated value, tie-break with docid ascending
  pq.insertWithOverflow({segdoc(0,7), 700});

  // 100 200 400 500 700 800 - should have 100,200,400 in the heap with the least competitive at top()
  ASSERT_EQ(pq.top().sortValue, 400);
  ASSERT_EQ(pq.top().doc, segdoc(0,4));

  // now test that higher segment number loses
  pq.insertWithOverflow({segdoc(1,1), 400});  // same value as current top, but higher segment so should lose
  ASSERT_EQ(pq.top().sortValue, 400);
  ASSERT_EQ(pq.top().doc, segdoc(0,4));

  // now using the standard sort_heap with the comparator for an ascending sort should result in sorted order
  std::sort_heap(sortDocs.begin(), sortDocs.end(), ascendingCompare);
  ASSERT_EQ(sortDocs[0].sortValue, 100);
}

// Score ties must break by (seg, docid) ascending so the kept top-K is a deterministic
// total order - independent of collection order, merge order, and (eventually) slicing.
// This is the prerequisite that lets a sliced run be a valid oracle vs the unsliced run.
// Pre-fix (strict `>` admit + score-only heap) any permutation where the smallest-(seg,docid)
// tie member arrives after the heap fills produced a different / wrong top-K.
TEST_F(SortCollectorTest, scoreTieBreakDeterministic) {
  struct In { int32_t seg; int32_t doc; float score; };
  // Three docs share the boundary score 10; at k=2 the tie-break must keep the two with the
  // smallest (seg, docid): (0,2) then (0,5).  (1,0) loses (higher segment), 7 and 3 lose on score.
  std::vector<In> docs = {
    {0, 5, 10.0f},
    {0, 2, 10.0f},
    {1, 0, 10.0f},
    {1, 9, 7.0f},
    {0, 8, 3.0f},
  };
  std::vector<segdoc> expected = { segdoc(0, 2), segdoc(0, 5) };

  auto topKOf = [](TopDocsCollector& c) {
    auto out = c.sort();
    std::vector<segdoc> got;
    for (auto& sd : out) got.push_back(sd.doc);
    return got;
  };

  // Same multiset collected in several permutations - including ones where the tie-winner
  // (0,2) arrives after the heap is already full of other score-10 docs - must all match.
  std::vector<std::vector<int>> orders = {
    {0, 1, 2, 3, 4}, {4, 3, 2, 1, 0}, {2, 0, 1, 3, 4}, {0, 2, 1, 4, 3}, {3, 2, 0, 4, 1},
  };
  for (auto& order : orders) {
    TopDocsCollector c(2);
    for (int i : order) c.collect(docs[i].seg, docs[i].doc, docs[i].score);
    ASSERT_EQ(topKOf(c), expected) << "collection order dependence";
    ASSERT_EQ(c.totalHits(), (int64_t)docs.size());
  }

  // Merge must be order-independent too (parallel/sliced collection merges partials).
  auto runMerge = [&](std::vector<int> a, std::vector<int> b, bool reverse) {
    TopDocsCollector ca(2), cb(2);
    for (int i : a) ca.collect(docs[i].seg, docs[i].doc, docs[i].score);
    for (int i : b) cb.collect(docs[i].seg, docs[i].doc, docs[i].score);
    TopDocsCollector* lhs = reverse ? &cb : &ca;
    TopDocsCollector* rhs = reverse ? &ca : &cb;
    lhs->merge(*rhs);
    return std::make_pair(topKOf(*lhs), lhs->totalHits());
  };
  for (bool reverse : {false, true}) {
    auto [got1, hits1] = runMerge({0, 1}, {2, 3, 4}, reverse);
    ASSERT_EQ(got1, expected) << "merge order dependence";
    ASSERT_EQ(hits1, (int64_t)docs.size());
    auto [got2, hits2] = runMerge({2, 4}, {0, 1, 3}, reverse);
    ASSERT_EQ(got2, expected) << "merge order dependence";
    ASSERT_EQ(hits2, (int64_t)docs.size());
  }
}

TEST_F(SortCollectorTest, scoreThresholdIsExclusiveOnlyPastTieBreakFloor) {
  TopDocsCollector collector(2);
  EXPECT_EQ(std::numeric_limits<float>::lowest(),
            collector.minCompetitiveScoreForNextDoc(1, 0));

  collector.collect(1, 0, 5.0f);
  collector.collect(1, 2, 5.0f);
  EXPECT_EQ(std::nextafter(5.0f, std::numeric_limits<float>::infinity()),
            collector.minCompetitiveScoreForNextDoc(1, 3));
  EXPECT_EQ(5.0f, collector.minCompetitiveScoreForNextDoc(0, 0));

  collector.collect(0, 0, 5.0f);
  EXPECT_EQ(5.0f, collector.minCompetitiveScoreForNextDoc(0, 1));
  EXPECT_EQ(std::nextafter(5.0f, std::numeric_limits<float>::infinity()),
            collector.minCompetitiveScoreForNextDoc(1, 3));
}

// Tie-break edge cases: k==1 and an all-equal-score corpus (the flat-score path where
// every doc ties).  Both must keep the smallest (seg, docid) members deterministically
// regardless of collection or merge order.
TEST_F(SortCollectorTest, scoreTieBreakEdgeCases) {
  struct In { int32_t seg; int32_t doc; float score; };
  auto topKOf = [](TopDocsCollector& c) {
    auto out = c.sort();
    std::vector<segdoc> got;
    for (auto& sd : out) got.push_back(sd.doc);
    return got;
  };

  // k == 1: the single kept doc is the smallest (seg, docid) among the max-score docs.
  {
    std::vector<In> docs = { {1, 0, 10.0f}, {0, 7, 10.0f}, {0, 3, 10.0f}, {1, 5, 2.0f} };
    std::vector<segdoc> expected = { segdoc(0, 3) };
    std::vector<std::vector<int>> orders = { {0, 1, 2, 3}, {3, 2, 1, 0}, {1, 0, 3, 2} };
    for (auto& order : orders) {
      TopDocsCollector c(1);
      for (int i : order) c.collect(docs[i].seg, docs[i].doc, docs[i].score);
      ASSERT_EQ(topKOf(c), expected) << "k==1 tie-break order dependence";
    }
  }

  // All-equal-score corpus, k == 3: top-3 is the three smallest (seg, docid), and it must
  // be identical across collection permutations and a merge split.
  {
    std::vector<In> docs = {
      {0, 0, 5.0f}, {0, 4, 5.0f}, {0, 9, 5.0f}, {1, 1, 5.0f}, {1, 2, 5.0f}, {1, 8, 5.0f},
    };
    std::vector<segdoc> expected = { segdoc(0, 0), segdoc(0, 4), segdoc(0, 9) };
    std::vector<std::vector<int>> orders = { {0, 1, 2, 3, 4, 5}, {5, 4, 3, 2, 1, 0}, {3, 0, 5, 1, 4, 2} };
    for (auto& order : orders) {
      TopDocsCollector c(3);
      for (int i : order) c.collect(docs[i].seg, docs[i].doc, docs[i].score);
      ASSERT_EQ(topKOf(c), expected) << "flat-score order dependence";
      ASSERT_EQ(c.totalHits(), (int64_t)docs.size());
    }
    // merge split, both directions
    for (bool reverse : {false, true}) {
      TopDocsCollector ca(3), cb(3);
      for (int i : {3, 5, 1}) ca.collect(docs[i].seg, docs[i].doc, docs[i].score);
      for (int i : {2, 0, 4}) cb.collect(docs[i].seg, docs[i].doc, docs[i].score);
      TopDocsCollector* lhs = reverse ? &cb : &ca;
      lhs->merge(reverse ? ca : cb);
      ASSERT_EQ(topKOf(*lhs), expected) << "flat-score merge order dependence";
      ASSERT_EQ(lhs->totalHits(), (int64_t)docs.size());
    }
  }
}

// Test collecting in different segment orders with both merging and non-merging
// since this can happen with parallel searches.
TEST_F(SortCollectorTest, smallEdge) {
  // Hit edge cases by manually collecting and merging
  CollectionHelper helper;

  // Add documents with different prices
  helper.index(flatdoc("id_s", "doc1", "price_i", 50, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 25, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 50, "rating_i", 4), UpdateMessage::COMMIT);

  helper.index(flatdoc("id_s", "doc4", "price_i", 50, "rating_i", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "price_i", 25, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc6", "price_i", 50, "rating_i", 4), UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();

  // Create a mock IntFieldType for testing
  IntFieldType priceType("price_i");
  SortField sf("price_i", priceType, SortField::ASC);

  auto collect = [&](FieldSortCollector& collector, int32_t seg) {
    auto* postingsReader = &reader->segments()[seg].postingsReader();
    auto nDocs = postingsReader->maxDoc();
    collector.setSegment(seg, postingsReader);
    for (int32_t doc = 0; doc < nDocs; doc++) {
      collector.collect(seg, doc, 1.0f);
    }
  };

  // Test collecting segments in order
  {
    FieldSortCollector collector(2, columnPlan(sf));

    // collect 2nd segment first: should have [doc2,doc1]
    collect(collector, 0);
    ASSERT_EQ(collector.pq.top().doc, segdoc(0,0));  // least competitive is doc1

    // now collect 1st segment.  It should be [doc2, doc5] after
    collect(collector, 1);
    ASSERT_EQ(collector.pq.top().doc, segdoc(1,1));  // least competitive is doc5

    auto results = collector.sort();
    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results[0].doc, segdoc(0,1)); // doc2
    ASSERT_EQ(results[1].doc, segdoc(1,1)); // doc5
  }

  // Test collecting segments out of order
  {
    FieldSortCollector collector(2, columnPlan(sf));

    // collect 2nd segment first: should have [doc5, doc4]
    collect(collector, 1);
    ASSERT_EQ(collector.pq.top().doc, segdoc(1,0));  // least competitive is doc4

    // now collect 1st segment.  It should be [doc2, doc5] after
    collect(collector, 0);
    ASSERT_EQ(collector.pq.top().doc, segdoc(1,1));  // least competitive is doc5

    auto results = collector.sort();
    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results[0].doc, segdoc(0,1)); // doc2
    ASSERT_EQ(results[1].doc, segdoc(1,1)); // doc5
  }

  // Test merging collectors in different orders
  {
    FieldSortCollector collector0(2, columnPlan(sf));
    FieldSortCollector collector1(2, columnPlan(sf));

    collect(collector0, 0);
    collect(collector1, 1);

    collector0.merge(collector1);
    auto results = collector0.sort();
    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results[0].doc, segdoc(0,1)); // doc2
    ASSERT_EQ(results[1].doc, segdoc(1,1)); // doc5
  }

  // Test merging collectors in reverse order this time
  {
    FieldSortCollector collector0(2, columnPlan(sf));
    FieldSortCollector collector1(2, columnPlan(sf));

    collect(collector0, 0);
    collect(collector1, 1);

    collector1.merge(collector0);
    auto results = collector1.sort();
    ASSERT_EQ(results.size(), 2);
    ASSERT_EQ(results[0].doc, segdoc(0,1)); // doc2
    ASSERT_EQ(results[1].doc, segdoc(1,1)); // doc5
  }
}


// Test collecting in different segment orders with both merging and non-merging
// since this can happen with parallel searches.
TEST_F(SortCollectorTest, randomSmall) {
  // Hit edge cases by manually collecting and merging
  int32_t iterations = 100;
  CollectionHelper helper;

  // Create a mock IntFieldType for testing
  IntFieldType priceType("price_i");
  SortField sf("price_i", priceType, SortField::ASC);

  for (int iter=0; iter<iterations; iter++) {
    auto seed = rng();
    // LOG_ERROR("Iteration {} seed={}", iter, seed);
    Rng r(seed);
    helper.clear();
    std::vector<std::pair<segdoc, int64_t>> model;

    int s1Docs = r.rint(1,4);
    int s2Docs = r.rint(1,4);

    for (int d=0; d<s1Docs; d++) {
      int price = r.rint(10,13);
      model.emplace_back(segdoc(0,d), price);
      helper.index(flatdoc("id_s", "s1doc"+std::to_string(d), "price_i", price), d+1==s1Docs ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
    }

    for (int d=0; d<s2Docs; d++) {
      int price = r.rint(10,13);
      model.emplace_back(segdoc(1,d), price);
      helper.index(flatdoc("id_s", "s1doc"+std::to_string(d), "price_i", price), d+1==s2Docs ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
    }

    // sort the model by price asc, then segdoc asc
    std::sort(model.begin(), model.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) {
        return a.second < b.second;
      }
      return a.first < b.first;
    });

    auto reader = helper.getIndexWriter()->getIndexReader();

    auto collect = [&](FieldSortCollector& collector, int32_t seg) {
      auto* postingsReader = &reader->segments()[seg].postingsReader();
      auto nDocs = postingsReader->maxDoc();
      collector.setSegment(seg, postingsReader);
      for (int32_t doc = 0; doc < nDocs; doc++) {
        collector.collect(seg, doc, 1.0f);
      }
    };


    int topK = r.rint(1,(int)model.size() + 2);
    int expected = std::min(topK, int(model.size()));

    auto compare = [&](FieldSortCollector& collector, std::string_view testName) {
      auto results = collector.sort();
      ASSERT_EQ(results.size(), expected);
      for (int i=0; i<expected; i++) {
        if (results[i].doc != model[i].first) {
          // dump complete model
          for (auto j = 0u; j < model.size(); j++) {
            std::cout << "model[" << j << "] doc=[" << model[j].first.segment() << "," << model[j].first.docId() << "] price=" << model[j].second << std::endl;
          }
        }
        ASSERT_EQ(results[i].doc, model[i].first) << "TEST " << testName << " mismatch at index " << i << " in iteration " << i << " seed=" << seed
          << " topK=" << topK << " modelSize=" << model.size();
      }
    };

    // Test collecting segments in order
    {
      FieldSortCollector collector(topK, columnPlan(sf));

      // collect 2nd segment first: should have [doc5, doc4]
      collect(collector, 0);
      collect(collector, 1);
      compare(collector, "inOrder");
    }

    // Test collecting segments out of order
    {
      FieldSortCollector collector(topK, columnPlan(sf));

      // collect 2nd segment first: should have [doc5, doc4]
      collect(collector, 1);
      collect(collector, 0);
      compare(collector, "outOfOrder");
    }

    // Test merging collectors in different orders
    {
      FieldSortCollector collector0(topK, columnPlan(sf));
      FieldSortCollector collector1(topK, columnPlan(sf));
      collect(collector0, 0);
      collect(collector1, 1);
      collector0.merge(collector1);
      compare(collector0, "0merges1");
    }

    // Test merging collectors in reverse order this time
    {
      FieldSortCollector collector0(topK, columnPlan(sf));
      FieldSortCollector collector1(topK, columnPlan(sf));
      collect(collector0, 0);
      collect(collector1, 1);
      collector1.merge(collector0);
      compare(collector1, "1merges0");
    }
  }

}



// Slot storage grows on demand: a deep limit over few hits allocates for the
// hits, not the limit, and ranks identically to an exact-sized collector.
// Exercises growth past several doublings on all three slot-minting paths:
// collect() warmup, collectWindow's fill loop, and an underfull merge.
TEST_F(SortCollectorTest, DeepLimitGrowsSlotStorageOnDemand) {
  constexpr int32_t nDocs = 300;
  constexpr int64_t deepLimit = 1'000'000;
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t i = 0; i < nDocs; i++) {
    docs.push_back(flatdoc("id_s", "d" + std::to_string(i),
                           "sort_i", (int64_t)((i * 37) % 251)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->getIndexReader();

  // collectWindow path (direct slotKeys writes across growth).
  auto exact = collectNumericWindows(*reader, "sort_i", nDocs, SortField::ASC,
                                     FieldComparator::MISSING_LAST, false);
  auto deep = collectNumericWindows(*reader, "sort_i", deepLimit, SortField::ASC,
                                    FieldComparator::MISSING_LAST, false);
  EXPECT_EQ(exact, deep);

  IntFieldType fieldType("sort_i");
  SortField sortField("sort_i", fieldType, SortField::ASC);
  auto& leaf = reader->segments()[0];
  auto collect = [&](FieldSortCollector& collector, int32_t parity) {
    collector.setSegment(0, &leaf.postingsReader());
    for (int32_t doc = 0; doc < leaf.maxDoc(); doc++) {
      if (parity < 0 || doc % 2 == parity) collector.collect(0, doc, 1.0f);
    }
  };

  // collect() warmup path; capacity tracks the hits, not the limit.
  FieldSortCollector whole(deepLimit, columnPlan(sortField));
  collect(whole, -1);
  EXPECT_EQ(whole.size(), (uint64_t)nDocs);
  EXPECT_LE(whole.slotCapacity, (int64_t)nDocs * 2);

  // Underfull merge mints the merged-in slots.
  FieldSortCollector even(deepLimit, columnPlan(sortField));
  FieldSortCollector odd(deepLimit, columnPlan(sortField));
  collect(even, 0);
  collect(odd, 1);
  even.merge(odd);
  EXPECT_EQ(even.size(), (uint64_t)nDocs);

  auto wholeDocs = whole.sort();
  auto mergedDocs = even.sort();
  ASSERT_EQ(wholeDocs.size(), deep.docs.size());
  ASSERT_EQ(mergedDocs.size(), deep.docs.size());
  for (size_t i = 0; i < deep.docs.size(); i++) {
    EXPECT_EQ(wholeDocs[i].doc, deep.docs[i]);
    EXPECT_EQ(mergedDocs[i].doc, deep.docs[i]);
  }
}

TEST_F(SortCollectorTest, SortByStringField) {
  CollectionHelper helper;

  // Add documents with string values that will sort alphabetically
  helper.index(flatdoc("id_s", "doc1", "name_s", "charlie"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "name_s", "alice"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "name_s", "bob"), UpdateMessage::COMMIT);
  
  // Add more documents to create a second segment
  helper.index(flatdoc("id_s", "doc4", "name_s", "david"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "name_s", "alice"), UpdateMessage::COMMIT); // duplicate value
  
  // Create a search request that sorts by name ascending
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "name_s"});
  qb::sort(cur, "name_s", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_EQ(5, docs->found.value_or(0));

  // Verify sort order: alice (doc2), alice (doc5), bob, charlie, david
  auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
  auto& nameCol = std::get<luxir::api::ColStr>(docs->columns.at("name_s").kind);

  ASSERT_EQ(5, (int)idCol.v.size());
  ASSERT_EQ(5, (int)nameCol.v.size());

  // First two should be alice (ordered by docid as tiebreaker)
  ASSERT_EQ("alice", nameCol.v[0]);
  ASSERT_EQ("doc2", idCol.v[0]);

  ASSERT_EQ("alice", nameCol.v[1]);
  ASSERT_EQ("doc5", idCol.v[1]);

  ASSERT_EQ("bob", nameCol.v[2]);
  ASSERT_EQ("doc3", idCol.v[2]);

  ASSERT_EQ("charlie", nameCol.v[3]);
  ASSERT_EQ("doc1", idCol.v[3]);

  ASSERT_EQ("david", nameCol.v[4]);
  ASSERT_EQ("doc4", idCol.v[4]);

  // Test descending sort as well
  auto req3 = localReq(luxirNode->getSearchEngine());
  req3->collection("main");
  auto& cur3 = req3->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "name_s"});
  qb::sort(cur3, "name_s", qb::DESC);
  req3->execute(true);
  ASSERT_OK(req3);

  const auto* docs3 = req3->docList("q");
  ASSERT_EQ(5, docs3->found.value_or(0));

  // Verify descending sort order: david, charlie, bob, alice (doc2), alice (doc5)
  auto& idCol3 = std::get<luxir::api::ColStr>(docs3->columns.at("id_s").kind);
  auto& nameCol3 = std::get<luxir::api::ColStr>(docs3->columns.at("name_s").kind);

  ASSERT_EQ("david", nameCol3.v[0]);
  ASSERT_EQ("doc4", idCol3.v[0]);

  ASSERT_EQ("charlie", nameCol3.v[1]);
  ASSERT_EQ("doc1", idCol3.v[1]);

  ASSERT_EQ("bob", nameCol3.v[2]);
  ASSERT_EQ("doc3", idCol3.v[2]);

  // alice docs should be in docid order (reverse of ascending)
  ASSERT_EQ("alice", nameCol3.v[3]);
  ASSERT_EQ("alice", nameCol3.v[4]);
}

TEST_F(SortCollectorTest, SegmentOrdMatchesGlobalAcrossDictionaryShapes) {
  using Value = std::optional<std::string>;
  struct Corpus {
    std::string name;
    std::vector<std::vector<Value>> segments;
  };
  std::vector<Corpus> corpora = {
    {"identical", {{"", "a", "m", std::nullopt},
                    {"", "a", "m", std::nullopt}}},
    {"disjoint", {{"", "a", "b", std::nullopt},
                   {"x", "y", "z", std::nullopt}}},
    {"interleaved", {{"", "b", "d", std::nullopt},
                      {"a", "c", "e", std::nullopt}}},
    {"partial", {{"", "b", "d", "shared", std::nullopt},
                  {"a", "d", "shared", "z", std::nullopt}}},
  };

  CollectionHelper helper;
  auto run = [&](StringSortMode mode, qb::SortDir direction, int32_t limit) {
    StringSortModeGuard guard(mode);
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(limit).getScores().allQuery().fields({"id_s"});
    qb::sort(cur, "name_s", direction);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->docList("q");
    const auto& scoreCol =
        std::get<luxir::api::ColFloat>(docs->columns.at("_score_").kind).v;
    return StringSortResult{
        resultIds(*req), std::vector<float>(scoreCol.begin(), scoreCol.end())};
  };

  for (const auto& corpus : corpora) {
    helper.clear();
    int32_t docCount = 0;
    for (size_t segment = 0; segment < corpus.segments.size(); segment++) {
      const auto& values = corpus.segments[segment];
      for (size_t doc = 0; doc < values.size(); doc++) {
        std::string id = corpus.name + "_" + std::to_string(segment) + "_" +
                         std::to_string(doc);
        UpdateMessage::CommitType update = doc + 1 == values.size()
            ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
        if (values[doc].has_value()) {
          helper.index(flatdoc("id_s", id, "name_s", *values[doc]), update);
        } else {
          helper.index(flatdoc("id_s", id), update);
        }
        docCount++;
      }
    }

    for (qb::SortDir direction : {qb::ASC, qb::DESC}) {
      for (int32_t limit : {3, docCount, docCount + 3}) {
        StringSortResult expected = run(StringSortMode::GLOBAL, direction, limit);
        StringSortResult actual = run(StringSortMode::SEGMENT, direction, limit);
        EXPECT_EQ(expected, actual)
            << corpus.name << " direction=" << (direction == qb::ASC ? "asc" : "desc")
            << " limit=" << limit;
      }
    }
  }
}

TEST_F(SortCollectorTest, SegmentModeIsParseBoundAndDoesNotBuildOrdMap) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "name_s", "b"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "b", "name_s", "a"), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(2u, reader->segments().size());
  size_t cacheSize = reader->getOrdMapCacheSize();

  StrFieldType fieldType("name_s");
  StringSortModeGuard segmentGuard(StringSortMode::SEGMENT);
  SortField sortField("name_s", fieldType, SortField::ASC);
  {
    StringSortModeGuard globalGuard(StringSortMode::GLOBAL);
    auto comparator = sortField.createComparator(reader.get());
    EXPECT_NE(nullptr, dynamic_cast<SegmentOrdComparator*>(comparator.get()));
  }

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").limit(2).allQuery().fields({"id_s"});
  qb::sort(cur, "name_s", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<std::string>{"b", "a"}), resultIds(*req));
  EXPECT_EQ(cacheSize, reader->getOrdMapCacheSize());
}

TEST_F(SortCollectorTest, SegmentOrdBottomAnchors) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "exact", "name_s", "b"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "between", "name_s", "c"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "before", "name_s", "0"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "past", "name_s", "z"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "empty", "name_s", ""), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "missing"), UpdateMessage::COMMIT);

  helper.index(flatdoc("id_s", "a", "name_s", "a"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "name_s", "b"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "name_s", "d"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "missing2"), UpdateMessage::COMMIT);

  helper.index(flatdoc("id_s", "empty2", "name_s", ""), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "q", "name_s", "q"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "missing3"), UpdateMessage::COMMIT);

  helper.index(flatdoc("id_s", "absent"), UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();
  auto compare = [&](int32_t pivotDoc, int32_t targetSegment, int32_t candidateDoc,
                     FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST,
                     bool reversed = false) {
    SegmentOrdComparator comparator("name_s", reversed, missing);
    comparator.growSlots(1);
    comparator.setSegment(0, &reader->segments()[0].postingsReader());
    comparator.copy(0, segdoc(0, pivotDoc));
    comparator.setSegment(targetSegment,
                          &reader->segments()[targetSegment].postingsReader());
    comparator.setBottom(0);
    return comparator.compareBottom(0, segdoc(0, pivotDoc),
                                    segdoc(targetSegment, candidateDoc));
  };

  EXPECT_GT(compare(0, 1, 0), 0);  // exact promotion: b > a
  EXPECT_EQ(compare(0, 1, 1), 0);  // exact promotion: b == b
  EXPECT_LT(compare(0, 1, 2), 0);  // exact promotion: b < d
  EXPECT_GT(compare(1, 1, 1), 0);  // absent pivot c > lower bound b
  EXPECT_LT(compare(1, 1, 2), 0);  // absent pivot c < ceiling d
  EXPECT_LT(compare(2, 1, 0), 0);  // pivot before first term
  EXPECT_GT(compare(3, 1, 2), 0);  // pivot past last term
  EXPECT_LT(compare(1, 1, 3), 0);  // missing candidate sorts last
  EXPECT_GT(compare(5, 1, 0), 0);  // missing bottom sorts last
  EXPECT_EQ(compare(5, 1, 3), 0);
  EXPECT_GT(compare(1, 1, 3, FieldComparator::MISSING_FIRST), 0);
  EXPECT_LT(compare(5, 1, 0, FieldComparator::MISSING_FIRST), 0);
  EXPECT_LT(compare(4, 2, 2), 0);  // empty term is not missing
  EXPECT_GT(compare(5, 2, 0), 0);
  EXPECT_LT(compare(1, 3, 0), 0);  // absent field: every candidate is missing
  EXPECT_LT(compare(0, 1, 0, FieldComparator::MISSING_LAST, true), 0);
}

TEST_F(SortCollectorTest, SegmentOrdCollectorReuseAcrossAdversarialSegments) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "s0b", "name_s", "b"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "s0c", "name_s", "c"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "s0z", "name_s", "z"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "s1m", "name_s", "m"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "s1n", "name_s", "n"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "missing"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "s3a", "name_s", "a"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "s3d", "name_s", "d"), UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();
  StrFieldType fieldType("name_s");
  SortField field("name_s", fieldType, SortField::ASC,
                  FieldComparator::MISSING_LAST, StringSortMode::SEGMENT);
  FieldSortCollector collector(4, columnPlan(field), reader.get());
  auto collectSegment = [&](int32_t segment) {
    auto& postings = reader->segments()[segment].postingsReader();
    collector.setSegment(segment, &postings);
    for (int32_t doc = 0; doc < postings.maxDoc(); doc++) {
      collector.collect(segment, doc, 1.0f);
    }
  };
  for (int32_t segment : {0, 2, 1, 3}) collectSegment(segment);

  auto results = collector.sort();
  std::vector<segdoc> actual;
  for (const auto& result : results) actual.push_back(result.doc);
  EXPECT_EQ((std::vector<segdoc>{{3, 0}, {0, 0}, {0, 1}, {3, 1}}), actual);
}

TEST_F(SortCollectorTest, SegmentOrdPairwiseMergeOwnsCopiedTerms) {
  CollectionHelper helper;
  std::vector<std::vector<std::optional<std::string>>> values = {
      {{"b"}, {"f"}, std::nullopt},
      {{"a"}, {"e"}, {"i"}},
      {{"c"}, {"g"}, std::nullopt},
      {{"d"}, {"h"}, {"i"}},
  };
  std::vector<std::pair<segdoc, std::optional<std::string>>> model;
  for (size_t segment = 0; segment < values.size(); segment++) {
    for (size_t doc = 0; doc < values[segment].size(); doc++) {
      std::string id = "s" + std::to_string(segment) + "d" + std::to_string(doc);
      UpdateMessage::CommitType update = doc + 1 == values[segment].size()
          ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
      if (values[segment][doc].has_value()) {
        helper.index(flatdoc("id_s", id, "name_s", *values[segment][doc]), update);
      } else {
        helper.index(flatdoc("id_s", id), update);
      }
      model.emplace_back(segdoc((int32_t)segment, (int32_t)doc), values[segment][doc]);
    }
  }
  std::sort(model.begin(), model.end(), [](const auto& a, const auto& b) {
    if (!a.second.has_value() || !b.second.has_value()) {
      if (a.second.has_value() != b.second.has_value()) return a.second.has_value();
    } else if (*a.second != *b.second) {
      return *a.second < *b.second;
    }
    return a.first < b.first;
  });

  auto reader = helper.getIndexWriter()->getIndexReader();
  StrFieldType fieldType("name_s");
  SortField field("name_s", fieldType, SortField::ASC,
                  FieldComparator::MISSING_LAST, StringSortMode::SEGMENT);
  auto run = [&](bool reverse) {
    auto makeCollector = [&]() {
      return std::make_unique<FieldSortCollector>(8, columnPlan(field), reader.get());
    };
    auto left = makeCollector();
    auto right = makeCollector();
    auto collect = [&](FieldSortCollector& collector, int32_t segment) {
      auto& postings = reader->segments()[segment].postingsReader();
      collector.setSegment(segment, &postings);
      for (int32_t doc = 0; doc < postings.maxDoc(); doc++) {
        collector.collect(segment, doc, 1.0f);
      }
    };
    collect(*left, 0);
    collect(*left, 2);
    collect(*right, 1);
    collect(*right, 3);
    auto& destination = reverse ? right : left;
    auto& source = reverse ? left : right;
    destination->merge(*source);
    source.reset();
    auto sorted = destination->sort();
    std::vector<segdoc> actual;
    for (const auto& result : sorted) actual.push_back(result.doc);
    return actual;
  };

  std::vector<segdoc> expected;
  for (size_t i = 0; i < 8; i++) expected.push_back(model[i].first);
  EXPECT_EQ(expected, run(false));
  EXPECT_EQ(expected, run(true));
}

TEST_F(SortCollectorTest, SegmentOrdMultiValuedSelectsMinAscAndMaxDesc) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "d1", "tags_ss", vecs("m", "z")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d2", "tags_ss", vecs("a", "y")),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "d3", "tags_ss", vecs("b", "c")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d4"), UpdateMessage::COMMIT);

  auto run = [&](qb::SortDir direction) {
    StringSortModeGuard guard(StringSortMode::SEGMENT);
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(10).allQuery().fields({"id_s"});
    qb::sort(cur, "tags_ss", direction);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  };

  EXPECT_EQ((std::vector<std::string>{"d2", "d3", "d1", "d4"}), run(qb::ASC));
  EXPECT_EQ((std::vector<std::string>{"d1", "d2", "d3", "d4"}), run(qb::DESC));
}

// A single segment routes to GlobalOrdComparator in both modes; it must apply
// the same min-asc/max-desc multi-valued selection as SegmentOrdComparator.
TEST_F(SortCollectorTest, SingleSegmentMultiValuedStringSort) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "d1", "tags_ss", vecs("m", "z")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d2", "tags_ss", vecs("a", "y")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d3", "tags_ss", vecs("b", "c")),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d4"), UpdateMessage::COMMIT);

  for (StringSortMode mode : {StringSortMode::SEGMENT, StringSortMode::GLOBAL}) {
    StringSortModeGuard guard(mode);
    auto run = [&](qb::SortDir direction) {
      auto req = localReq(luxirNode->getSearchEngine());
      req->collection("main");
      auto& cur = req->topDocs("q").limit(10).allQuery().fields({"id_s"});
      qb::sort(cur, "tags_ss", direction);
      req->execute(true);
      EXPECT_TRUE(req->ok()) << req->errorMsg();
      return resultIds(*req);
    };
    EXPECT_EQ((std::vector<std::string>{"d2", "d3", "d1", "d4"}), run(qb::ASC));
    EXPECT_EQ((std::vector<std::string>{"d1", "d2", "d3", "d4"}), run(qb::DESC));
  }
}

TEST_F(SortCollectorTest, SortByPriceAscending) {
  CollectionHelper helper;

  // Add documents with different prices - also add zero-padded string prices
  helper.index(flatdoc("id_s", "doc1", "price_i", 100, "price_s", "00100", "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50, "price_s", "00050", "rating_i", 4), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150, "price_s", "00150", "rating_i", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc4", "price_i", 75, "price_s", "00075", "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "price_i", 100, "price_s", "00100", "rating_i", 4), UpdateMessage::COMMIT);

  // Create a search request that sorts by price ascending
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "price_i"});
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_EQ(5, docs->found.value_or(0));

  // Check if we have columns
  ASSERT_GT((int)docs->columns.size(), 0) << "No columns returned";

  // Check if values were actually loaded
  auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
  auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);

  ASSERT_GT((int)idCol.v.size(), 0) << "No id values loaded";
  ASSERT_GT((int)priceCol.v.size(), 0) << "No price values loaded";

  // Verify sort order by price: doc2(50), doc4(75), doc1(100), doc5(100), doc3(150)
  ASSERT_EQ("doc2", idCol.v[0]);
  ASSERT_EQ(50, priceCol.v[0]);

  ASSERT_EQ("doc4", idCol.v[1]);
  ASSERT_EQ(75, priceCol.v[1]);

  // doc1 and doc5 have same price, so they should be ordered by docid
  ASSERT_EQ(100, priceCol.v[2]);
  ASSERT_EQ(100, priceCol.v[3]);

  ASSERT_EQ("doc3", idCol.v[4]);
  ASSERT_EQ(150, priceCol.v[4]);

  // Now test string sorting with the same data - should give identical results
  auto req2 = localReq(luxirNode->getSearchEngine());
  req2->collection("main");
  auto& cur2 = req2->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "price_s"});
  qb::sort(cur2, "price_s", qb::ASC);
  req2->execute(true);
  ASSERT_OK(req2);

  const auto* docs2 = req2->docList("q");
  ASSERT_EQ(5, docs2->found.value_or(0));

  // Verify same sort order as integer sort
  auto& idCol2 = std::get<luxir::api::ColStr>(docs2->columns.at("id_s").kind);
  auto& priceStrCol = std::get<luxir::api::ColStr>(docs2->columns.at("price_s").kind);

  ASSERT_EQ("doc2", idCol2.v[0]);
  ASSERT_EQ("00050", priceStrCol.v[0]);

  ASSERT_EQ("doc4", idCol2.v[1]);
  ASSERT_EQ("00075", priceStrCol.v[1]);

  // doc1 and doc5 have same price string, ordered by docid
  ASSERT_EQ("00100", priceStrCol.v[2]);
  ASSERT_EQ("00100", priceStrCol.v[3]);

  ASSERT_EQ("doc3", idCol2.v[4]);
  ASSERT_EQ("00150", priceStrCol.v[4]);
}

TEST_F(SortCollectorTest, SortByMultipleFields) {
  CollectionHelper helper;

  // Add documents with different ratings and prices
  helper.index(flatdoc("id_s", "doc1", "price_i", 100, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50, "rating_i", 4), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150, "rating_i", 3), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc4", "price_i", 75, "rating_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "price_i", 100, "rating_i", 4), UpdateMessage::COMMIT);

  // Create a search request that sorts by rating desc, then price asc
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").getNumber().limit(10).allQuery()
      .fields({"id_s", "price_i", "rating_i"});

  // Sort by rating descending, then price ascending
  qb::sort(cur, "rating_i", qb::DESC);
  qb::sort(cur, "price_i", qb::ASC);

  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_EQ(5, docs->found.value_or(0));

  // Expected order:
  // rating 5: doc4(75), doc1(100)
  // rating 4: doc2(50), doc5(100)
  // rating 3: doc3(150)

  auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
  auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);
  auto& ratingCol = std::get<luxir::api::ColInt>(docs->columns.at("rating_i").kind);
  std::vector<std::string_view> expectedIds = {"doc4", "doc1", "doc2", "doc5", "doc3"};
  std::vector<int64_t> expectedPrices = {75, 100, 50, 100, 150};
  std::vector<int64_t> expectedRatings = {5, 5, 4, 4, 3};
  ASSERT_EQ(expectedIds, std::vector<std::string_view>(idCol.v.begin(), idCol.v.end()));
  ASSERT_EQ(expectedPrices, std::vector<int64_t>(priceCol.v.begin(), priceCol.v.end()));
  ASSERT_EQ(expectedRatings, std::vector<int64_t>(ratingCol.v.begin(), ratingCol.v.end()));
}

TEST_F(SortCollectorTest, MixedColumnSortsSingleAndMultiSegment) {
  CollectionHelper helper;

  auto indexDocs = [&](bool multiSegment) {
    helper.clear();
    auto mode = [&](bool last) {
      return (multiSegment || last) ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
    };
    helper.index(flatdoc("id_s", "doc1", "price_i", 100, "rating_i", 5, "cat_s", "b"), mode(false));
    helper.index(flatdoc("id_s", "doc2", "price_i", 50, "rating_i", 4, "cat_s", "a"), mode(false));
    helper.index(flatdoc("id_s", "doc3", "price_i", 150, "rating_i", 3, "cat_s", "a"), mode(false));
    helper.index(flatdoc("id_s", "doc4", "price_i", 75, "rating_i", 5, "cat_s", "c"), mode(false));
    helper.index(flatdoc("id_s", "doc5", "price_i", 100, "rating_i", 4, "cat_s", "b"), mode(true));
  };
  auto run = [&](std::string_view secondary) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(10).allQuery().fields({"id_s"});
    qb::sort(cur, "rating_i", qb::DESC);
    qb::sort(cur, secondary, qb::ASC);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  };

  for (bool multiSegment : {false, true}) {
    indexDocs(multiSegment);
    EXPECT_EQ((std::vector<std::string>{"doc4", "doc1", "doc2", "doc5", "doc3"}),
              run("price_i"));
    EXPECT_EQ((std::vector<std::string>{"doc1", "doc4", "doc2", "doc5", "doc3"}),
              run("cat_s"));
  }
}

TEST_F(SortCollectorTest, ScoreAndDocClausesInEveryPosition) {
  CollectionHelper helper;
  struct ModelDoc {
    std::string id;
    int64_t number;
    std::string text;
    float score;
    segdoc doc;
  };
  std::vector<ModelDoc> model = {
    {"a", 0, "b", 2.0f, {0, 0}},
    {"b", 0, "a", 1.0f, {0, 1}},
    {"c", 1, "a", 3.0f, {0, 2}},
    {"d", 0, "a", 3.0f, {1, 0}},
    {"e", 1, "b", 1.0f, {1, 1}},
    {"f", 1, "a", 2.0f, {1, 2}},
  };
  for (size_t i = 0; i < model.size(); i++) {
    const auto& doc = model[i];
    std::string scoreTag = doc.score == 1.0f ? "one" : doc.score == 2.0f ? "two" : "three";
    helper.index(flatdoc("id_s", doc.id, "number_i", doc.number,
                         "text_s", doc.text, "score_s", scoreTag),
                 i == 2 || i + 1 == model.size()
                   ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  using Spec = std::pair<std::string, qb::SortDir>;
  std::vector<std::vector<Spec>> cases = {
    {{"_score_", qb::DESC}, {"number_i", qb::ASC}, {"text_s", qb::ASC}},
    {{"_score_", qb::ASC}, {"text_s", qb::DESC}, {"number_i", qb::DESC}},
    {{"number_i", qb::ASC}, {"_score_", qb::DESC}, {"text_s", qb::ASC}},
    {{"text_s", qb::DESC}, {"_score_", qb::ASC}, {"number_i", qb::DESC}},
    {{"number_i", qb::ASC}, {"text_s", qb::ASC}, {"_score_", qb::DESC}},
    {{"text_s", qb::DESC}, {"number_i", qb::DESC}, {"_score_", qb::ASC}},
    {{"_docid_", qb::ASC}, {"number_i", qb::DESC}, {"text_s", qb::DESC}},
    {{"_docid_", qb::DESC}, {"text_s", qb::ASC}, {"number_i", qb::ASC}},
    {{"number_i", qb::ASC}, {"_docid_", qb::DESC}, {"text_s", qb::ASC}},
    {{"text_s", qb::DESC}, {"_docid_", qb::ASC}, {"number_i", qb::DESC}},
    {{"number_i", qb::ASC}, {"text_s", qb::ASC}, {"_docid_", qb::DESC}},
    {{"text_s", qb::DESC}, {"number_i", qb::DESC}, {"_docid_", qb::ASC}},
  };

  auto compare = [](const ModelDoc& a, const ModelDoc& b, const std::vector<Spec>& specs) {
    for (const auto& [field, direction] : specs) {
      int cmp = 0;
      if (field == "_score_") {
        cmp = (a.score > b.score) - (a.score < b.score);
      } else if (field == "_docid_") {
        cmp = (a.doc > b.doc) - (a.doc < b.doc);
      } else if (field == "number_i") {
        cmp = (a.number > b.number) - (a.number < b.number);
      } else {
        cmp = (a.text > b.text) - (a.text < b.text);
      }
      if (direction == qb::DESC) cmp = -cmp;
      if (cmp != 0) return cmp < 0;
    }
    return a.doc < b.doc;
  };

  for (const auto& specs : cases) {
    auto expected = model;
    std::sort(expected.begin(), expected.end(),
              [&](const auto& a, const auto& b) { return compare(a, b, specs); });

    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(10).fields({"id_s"});
    cur.rawQuery() = qb::boolean(cur.mr(), {}, {
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "one"), 1.0f),
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "two"), 2.0f),
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "three"), 3.0f),
    });
    for (const auto& [field, direction] : specs) qb::sort(cur, field, direction);
    req->execute(true);
    ASSERT_OK(req);

    std::vector<std::string> expectedIds;
    for (const auto& doc : expected) expectedIds.push_back(doc.id);
    EXPECT_EQ(expectedIds, resultIds(*req)) << "primary=" << specs[0].first;
  }
}

TEST_F(SortCollectorTest, SecondaryClauseControlsHeapEviction) {
  CollectionHelper helper;
  for (int i = 0; i < 100; i++) {
    helper.index(flatdoc("id_s", "d" + std::to_string(i),
                         "tier_i", i % 2, "order_i", 1000 - i),
                 i == 99 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").limit(5).allQuery().fields({"id_s"});
  qb::sort(cur, "tier_i", qb::ASC);
  qb::sort(cur, "order_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<std::string>{"d98", "d96", "d94", "d92", "d90"}),
            resultIds(*req));
}

TEST_F(SortCollectorTest, EqualScoreCandidateCanDisplaceHeapBottom) {
  CollectionHelper helper;
  for (int i = 0; i < 20; i++) {
    helper.index(flatdoc("id_s", "d" + std::to_string(i), "rank_i", 20 - i),
                 i == 19 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").limit(3).allQuery().fields({"id_s"});
  qb::sort(cur, "_score_", qb::DESC);
  qb::sort(cur, "rank_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<std::string>{"d19", "d18", "d17"}), resultIds(*req));
}

TEST_F(SortCollectorTest, CanonicalScoreSortsMatchDefault) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "score_s", "low"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "score_s", "high"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "score_s", "high"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "score_s", "low"), UpdateMessage::COMMIT);

  struct Result {
    std::vector<std::string> ids;
    std::vector<float> scores;
  };
  auto run = [&](int mode) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(4).getScores().fields({"id_s"});
    cur.rawQuery() = qb::boolean(cur.mr(), {}, {
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "low"), 1.0f),
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "high"), 2.0f),
    });
    if (mode == 1) qb::sort(cur, "_score_", qb::DESC);
    if (mode == 2) {
      qb::sort(cur, "_score_", qb::DESC);
      qb::sort(cur, "_docid_", qb::ASC);
    }
    if (mode == 3) qb::sort(cur, "_score_");
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->docList("q");
    const auto& scores = std::get<luxir::api::ColFloat>(docs->columns.at("_score_").kind).v;
    return Result{resultIds(*req), std::vector<float>(scores.begin(), scores.end())};
  };

  Result baseline = run(0);
  for (int mode : {1, 2, 3}) {
    Result actual = run(mode);
    EXPECT_EQ(baseline.ids, actual.ids);
    EXPECT_EQ(baseline.scores, actual.scores);
  }
}

TEST_F(SortCollectorTest, DocSortAcrossSegmentsAndMissingSecondary) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "tier_i", 0, "secondary_i", 2), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "tier_i", 0), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "tier_i", 0, "secondary_i", 1), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "d", "tier_i", 0), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "e", "tier_i", 0, "secondary_i", 3), UpdateMessage::COMMIT);

  auto run = [&](std::vector<std::pair<std::string_view, qb::SortDir>> specs) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(10).allQuery().fields({"id_s"});
    for (auto [field, direction] : specs) qb::sort(cur, field, direction);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  };

  EXPECT_EQ((std::vector<std::string>{"a", "b", "c", "d", "e"}),
            run({{"_docid_", qb::ASC}}));
  EXPECT_EQ((std::vector<std::string>{"e", "d", "c", "b", "a"}),
            run({{"_docid_", qb::DESC}}));
  EXPECT_EQ((std::vector<std::string>{"c", "a", "e", "b", "d"}),
            run({{"tier_i", qb::ASC}, {"secondary_i", qb::ASC}}));
}

TEST_F(SortCollectorTest, FieldSortReturnsScoresAndColumnDefaultsAscending) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "rank_i", 0, "score_s", "one"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "rank_i", 0, "score_s", "three"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "rank_i", 1, "score_s", "two"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "rank_i", 1, "score_s", "three"), UpdateMessage::COMMIT);

  auto makeQuery = [](OpCursor& cur) {
    cur.rawQuery() = qb::boolean(cur.mr(), {}, {
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "one"), 1.0f),
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "two"), 2.0f),
      qb::constantScore(cur.mr(), qb::match(cur.mr(), "score_s", "three"), 3.0f),
    });
  };
  auto runColumn = [&](qb::SortDir direction, bool omitted) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(10).fields({"id_s"});
    makeQuery(cur);
    if (omitted) qb::sort(cur, "rank_i");
    else qb::sort(cur, "rank_i", direction);
    req->execute(true);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  };
  EXPECT_EQ(runColumn(qb::ASC, false), runColumn(qb::ASC, true));

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").limit(10).getScores().fields({"id_s"});
  makeQuery(cur);
  qb::sort(cur, "rank_i", qb::ASC);
  qb::sort(cur, "_score_", qb::DESC);
  req->execute(true);
  ASSERT_OK(req);
  EXPECT_EQ((std::vector<std::string>{"b", "a", "d", "c"}), resultIds(*req));
  const auto* docs = req->docList("q");
  const auto& scores = std::get<luxir::api::ColFloat>(docs->columns.at("_score_").kind).v;
  EXPECT_EQ((std::vector<float>{3.0f, 1.0f, 3.0f, 2.0f}),
            std::vector<float>(scores.begin(), scores.end()));
}

TEST_F(SortCollectorTest, SortByPriceDescending) {
  CollectionHelper helper;

  // Add documents
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150), UpdateMessage::COMMIT);

  // Create a search request that sorts by price descending
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").limit(10).allQuery().fields({"id_s", "price_i"});
  qb::sort(cur, "price_i", qb::DESC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
  auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);

  // Verify sort order by price descending: doc3(150), doc1(100), doc2(50)
  ASSERT_EQ("doc3", idCol.v[0]);
  ASSERT_EQ(150, priceCol.v[0]);

  ASSERT_EQ("doc1", idCol.v[1]);
  ASSERT_EQ(100, priceCol.v[1]);

  ASSERT_EQ("doc2", idCol.v[2]);
  ASSERT_EQ(50, priceCol.v[2]);
}

TEST_F(SortCollectorTest, SortWithBatchedResponses) {
  CollectionHelper helper;

  // Add 10 documents with different prices
  for (int i = 1; i <= 10; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i * 10),
      i == 10 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  // Create a search request with small batch size to trigger multiple responses
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").limit(10)
      .batchSize(3)  // Small batch size to get multiple responses
      .getNumber().allQuery().fields({"id_s", "price_i"});
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  // Should have multiple responses due to batch size
  ASSERT_GT(req->responses.size(), 1) << "Expected multiple batched responses";

  // Verify we got all documents across all responses
  int totalDocs = 0;
  std::vector<int> allPrices;

  for (size_t i = 0; i < req->responses.size(); i++) {
    auto& response = req->responses[i]->proto;
    const auto* docs = response.ops.at("q")->docList();

    // Check offset is correct for each batch
    EXPECT_EQ(docs->offset, totalDocs) << "Incorrect offset for batch " << i;

    // All but last response should have more flag
    if (i < req->responses.size() - 1) {
      EXPECT_TRUE(response.more) << "Expected more flag on response " << i;
      EXPECT_TRUE(docs->more) << "Expected more flag on docs " << i;
    }
    else {
      EXPECT_FALSE(response.more) << "Unexpected more flag on last response";
      EXPECT_FALSE(docs->more) << "Unexpected more flag on last docs";
    }

    // Collect all prices to verify complete sort order
    auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);
    for (int j = 0; j < (int)priceCol.v.size(); j++) {
      allPrices.push_back(priceCol.v[j]);
    }

    totalDocs += (int)priceCol.v.size();
  }

  // Verify we got all 10 documents
  ASSERT_EQ(totalDocs, 10);
  ASSERT_EQ(req->responses.back()->proto.ops.at("q")->docList()->found.value_or(0), 10);

  // Verify complete sort order: 10, 20, 30, ..., 100
  ASSERT_EQ(allPrices.size(), 10);
  for (int i = 0; i < 10; i++) {
    EXPECT_EQ(allPrices[i], (i + 1) * 10) << "Incorrect price at position " << i;
  }
}

TEST_F(SortCollectorTest, SortWithMissingValues) {
  CollectionHelper helper;
  
  // Add documents with some missing the sort field
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2"), UpdateMessage::NO_COMMIT);  // Missing price
  helper.index(flatdoc("id_s", "doc3", "price_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc4"), UpdateMessage::NO_COMMIT);  // Missing price
  helper.index(flatdoc("id_s", "doc5", "price_i", 75), UpdateMessage::COMMIT);
  
  // Create a search request that sorts by price ascending
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  // Sort by price ascending (missing values should be last by default)
  auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "price_i"});
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");
  ASSERT_EQ(5, docs->found.value_or(0));

  // Check order: documents with values first (50, 75, 100), then missing values
  auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
  auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);

  ASSERT_EQ(5, (int)idCol.v.size());
  ASSERT_EQ(5, (int)priceCol.v.size());

  // Documents with values come first in ascending order
  ASSERT_EQ("doc3", idCol.v[0]);
  ASSERT_EQ(50, priceCol.v[0]);

  ASSERT_EQ("doc5", idCol.v[1]);
  ASSERT_EQ(75, priceCol.v[1]);

  ASSERT_EQ("doc1", idCol.v[2]);
  ASSERT_EQ(100, priceCol.v[2]);

  // Documents with missing values come last.  Their slots hold the column's
  // batch-chosen filler (0 here, since no real price is 0).
  ASSERT_EQ(0, priceCol.missing_val);
  ASSERT_EQ(priceCol.missing_val, priceCol.v[3]);
  ASSERT_EQ(priceCol.missing_val, priceCol.v[4]);
}

TEST_F(SortCollectorTest, EmptyResults) {
  CollectionHelper helper;
  
  // Add documents but search for non-existent field value
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50), UpdateMessage::COMMIT);
  
  // Create a search request that matches no documents
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  // Search for a field value that doesn't exist
  auto& cur = req->topDocs("q").getNumber().limit(10).matchQuery("id_s", "nonexistent");
  // Sort by price
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");

  // Should have 0 matches but still have a valid response
  ASSERT_EQ(0, docs->found.value_or(0));
  ASSERT_EQ(0, (int)docs->columns.size()) << "Should have no columns for empty results";
}

TEST_F(SortCollectorTest, SingleDocument) {
  CollectionHelper helper;
  
  // Add only one document
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::COMMIT);
  
  // Create a search request
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "price_i"});
  // Sort by price
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");

  ASSERT_EQ(1, docs->found.value_or(0));
  ASSERT_EQ("doc1", std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind).v[0]);
  ASSERT_EQ(100, std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind).v[0]);
}

TEST_F(SortCollectorTest, ResultsExceedingTopCount) {
  CollectionHelper helper;
  
  // Add 20 documents
  for (int i = 1; i <= 20; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i * 10),
      i == 20 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Create a search request with limit of 5
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").getNumber().limit(5).allQuery().fields({"id_s", "price_i"});  // Only get top 5
  // Sort by price ascending
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");

  // Should report total of 20 matches but only return 5
  ASSERT_EQ(20, docs->found.value_or(0));

  auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);
  ASSERT_EQ(5, (int)priceCol.v.size()) << "Should only return top 5 documents";

  // Verify we got the 5 lowest prices: 10, 20, 30, 40, 50
  for (int i = 0; i < 5; i++) {
    EXPECT_EQ((i + 1) * 10, priceCol.v[i]) << "Wrong price at position " << i;
  }
}

TEST_F(SortCollectorTest, LimitOne) {
  CollectionHelper helper;
  
  // Add multiple documents
  helper.index(flatdoc("id_s", "doc1", "price_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "price_i", 50), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "price_i", 150), UpdateMessage::COMMIT);
  
  // Create a search request with limit=1
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").getNumber().limit(1).allQuery().fields({"id_s", "price_i"});  // Only get the top 1
  // Sort by price ascending
  qb::sort(cur, "price_i", qb::ASC);
  req->execute(true);
  ASSERT_OK(req);

  const auto* docs = req->docList("q");

  // Should report 3 matches but only return 1
  ASSERT_EQ(3, docs->found.value_or(0));

  auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
  auto& priceCol = std::get<luxir::api::ColInt>(docs->columns.at("price_i").kind);

  ASSERT_EQ(1, (int)idCol.v.size());
  ASSERT_EQ(1, (int)priceCol.v.size());

  // Should get the document with lowest price
  ASSERT_EQ("doc2", idCol.v[0]);
  ASSERT_EQ(50, priceCol.v[0]);
}


TEST_F(SortCollectorTest, DeterministicParallelSort) {
  CollectionHelper helper;
  
  // Create multiple segments to trigger parallel execution
  // First segment
  for (int i = 1; i <= 100; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i % 10),
      i == 100 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Second segment
  for (int i = 101; i <= 200; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i % 10),
      i == 200 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Third segment
  for (int i = 201; i <= 300; i++) {
    helper.index(flatdoc("id_s", "doc" + std::to_string(i), "price_i", i % 10),
      i == 300 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }
  
  // Run query multiple times to verify deterministic results
  std::vector<int64_t> fingerprints;
  
  for (int run = 0; run < 2; run++) {  // Just 2 runs for debugging
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").getNumber().limit(50).allQuery().fields({"id_s", "price_i"});
    // Sort by price ascending
    qb::sort(cur, "price_i", qb::ASC);
    // Run in parallel mode
    req->execute(false);
    ASSERT_OK(req);

    const auto* docs = req->docList("q");
    ASSERT_EQ(300, docs->found.value_or(0));

    // Calculate fingerprint of results
    int64_t fp = docs->found.value_or(0);
    const auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);

    for (int i = 0; i < (int)idCol.v.size(); i++) {
      int64_t id = 0;
      std::from_chars(idCol.v[i].data() + 3, idCol.v[i].data() + idCol.v[i].size(), id);
      fp = fp * 31 + id;
    }

    fingerprints.push_back(fp);
  }
  
  // Verify all runs produced the same fingerprint
  for (size_t i = 1; i < fingerprints.size(); i++) {
    ASSERT_EQ(fingerprints[0], fingerprints[i]) 
      << "Run " << i << " produced different results (fingerprint mismatch)";
  }
}

TEST_F(SortCollectorTest, SortByNonIndexedStringColumn) {
  CollectionHelper helper;
  
  // Add documents with both indexed string fields (_s) and non-indexed string columns (_sc)
  helper.index(flatdoc("id_s", "doc1", "name_s", "charlie", "description_sc", "third person"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc2", "name_s", "alice", "description_sc", "first person"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc3", "name_s", "bob", "description_sc", "second person"), UpdateMessage::COMMIT);
  
  // Add more documents to create a second segment
  helper.index(flatdoc("id_s", "doc4", "name_s", "david", "description_sc", "fourth person"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "doc5", "name_s", "alice", "description_sc", "first duplicate"), UpdateMessage::COMMIT);
  
  // Test sorting by indexed string field (name_s)
  {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    // Sort by indexed string field
    auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "name_s"});
    qb::sort(cur, "name_s", qb::ASC);
    req->execute(true);
    ASSERT_OK(req);

    const auto* docs = req->docList("q");
    ASSERT_EQ(5, docs->found.value_or(0));

    auto& nameCol = std::get<luxir::api::ColStr>(docs->columns.at("name_s").kind);

    // Verify sort order: alice, alice, bob, charlie, david
    ASSERT_EQ("alice", nameCol.v[0]);
    ASSERT_EQ("alice", nameCol.v[1]);
    ASSERT_EQ("bob", nameCol.v[2]);
    ASSERT_EQ("charlie", nameCol.v[3]);
    ASSERT_EQ("david", nameCol.v[4]);
  }
  
  // Test sorting by non-indexed string column (description_sc)
  {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    // Sort by non-indexed string column
    auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "description_sc"});
    qb::sort(cur, "description_sc", qb::ASC);
    req->execute(true);
    ASSERT_OK(req);

    const auto* docs = req->docList("q");
    ASSERT_EQ(5, docs->found.value_or(0));

    auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
    auto& descCol = std::get<luxir::api::ColStr>(docs->columns.at("description_sc").kind);

    // Verify sort order by description: "first duplicate", "first person", "fourth person", "second person", "third person"
    ASSERT_EQ("first duplicate", descCol.v[0]);
    ASSERT_EQ("doc5", idCol.v[0]);

    ASSERT_EQ("first person", descCol.v[1]);
    ASSERT_EQ("doc2", idCol.v[1]);

    ASSERT_EQ("fourth person", descCol.v[2]);
    ASSERT_EQ("doc4", idCol.v[2]);

    ASSERT_EQ("second person", descCol.v[3]);
    ASSERT_EQ("doc3", idCol.v[3]);

    ASSERT_EQ("third person", descCol.v[4]);
    ASSERT_EQ("doc1", idCol.v[4]);
  }
  
  // Test descending sort on non-indexed string column
  {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    // Sort by non-indexed string column descending
    auto& cur = req->topDocs("q").getNumber().limit(10).allQuery().fields({"id_s", "description_sc"});
    qb::sort(cur, "description_sc", qb::DESC);
    req->execute(true);
    ASSERT_OK(req);

    const auto* docs = req->docList("q");
    ASSERT_EQ(5, docs->found.value_or(0));

    auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
    auto& descCol = std::get<luxir::api::ColStr>(docs->columns.at("description_sc").kind);

    // Verify descending sort order
    ASSERT_EQ("third person", descCol.v[0]);
    ASSERT_EQ("doc1", idCol.v[0]);

    ASSERT_EQ("second person", descCol.v[1]);
    ASSERT_EQ("doc3", idCol.v[1]);

    ASSERT_EQ("fourth person", descCol.v[2]);
    ASSERT_EQ("doc4", idCol.v[2]);

    ASSERT_EQ("first person", descCol.v[3]);
    ASSERT_EQ("doc2", idCol.v[3]);

    ASSERT_EQ("first duplicate", descCol.v[4]);
    ASSERT_EQ("doc5", idCol.v[4]);
  }
}

// Zone-based competitive pruning must return exactly what exhaustive
// collection returns. The corpus spans multiple 4096-value zone blocks per
// segment, includes deletes, ties, and a segment with missing values, and is
// swept over match-all, term, exact-phrase, and sloppy-phrase shapes, both
// drivers, both directions, and limits around the block size. Phrase runs
// additionally cover null/live-doc, dense BITSET, and sparse ARRAY domains.
TEST_F(SortCollectorTest, numericBlockPruningMatchesExhaustive) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr int32_t kDocsPerSeg = 5000;
  std::vector<std::string> deleted;
  int32_t docId = 0;
  for (int32_t seg = 0; seg < 2; seg++) {
    for (int32_t i = 0; i < kDocsPerSeg; i++, docId++) {
      std::string id = std::to_string(docId);
      int64_t rand = (int64_t)((uint32_t)docId * 2654435761u) & 0x7fffffff;
      std::string body;
      switch (docId & 3) {
        case 0: body = "alpha quick fox"; break;
        case 1: body = "other quick brown fox"; break;
        case 2: body = "alpha fox quick"; break;
        default: body = "other quick slow red fox"; break;
      }
      Doc doc;
      if (seg == 1 && i % 13 == 0) {
        doc = flatdoc("id", id, "id_s", id, "body_w", body,
                      "ties_i", (int64_t)(docId % 7),
                      "mono_i", (int64_t)docId,
                      "rev_i", (int64_t)(20000 - docId),
                      "group_s", "g" + std::to_string(docId % 5));
      } else {
        doc = flatdoc("id", id, "id_s", id, "body_w", body,
                      "rand_i", rand,
                      "ties_i", (int64_t)(docId % 7),
                      "mono_i", (int64_t)docId,
                      "rev_i", (int64_t)(20000 - docId),
                      "group_s", "g" + std::to_string(docId % 5));
      }
      if ((docId & 1) == 0) doc.push_back(NameVal{"dense_s", "y"});
      if (docId % 100 == 0) doc.push_back(NameVal{"sparse_s", "y"});
      helper.index(doc, UpdateMessage::NO_COMMIT);
      // Segment 0 retains a null live-doc domain; segment 1 exercises deletes.
      if (seg == 1 && docId % 97 == 0) deleted.push_back(id);
    }
    helper.commit();
  }
  ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);

  enum class PhraseShape { NONE, EXACT, SLOPPY };
  enum class FilterDomain { NONE, BITSET, ARRAY };
  struct Sorts {
    std::vector<std::pair<std::string_view, qb::SortDir>> clauses;
  };
  auto run = [&](bool disablePruning, bool forcePull, bool matchAll,
                 const Sorts& sorts, int32_t limit, bool exactCount,
                 PhraseShape phraseShape = PhraseShape::NONE,
                 FilterDomain filterDomain = FilterDomain::NONE,
                 bool domainBuilder = false, bool forcePrepare = false) {
    SortPruningGuard pruningGuard(disablePruning);
    FieldSortBulkGuard bulkGuard(forcePull);
    TopDocsFilterFoldGuard foldGuard(true);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->testForcePrepare = forcePrepare;
    req->collection("main");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    if (phraseShape != PhraseShape::NONE) {
      auto phrase = qb::phraseWords(cur.mr(), "body_w", {"quick", "fox"});
      if (phraseShape == PhraseShape::SLOPPY) {
        std::get<api::PhraseQuery>(phrase.kind).slop = 1;
      }
      cur.rawQuery() = std::move(phrase);
    } else if (matchAll) {
      cur.allQuery();
    } else {
      cur.rawQuery() = qb::match(cur.mr(), "body_w", "alpha");
    }
    if (filterDomain == FilterDomain::BITSET) {
      cur.matchFilter("dense_s", "y");
    } else if (filterDomain == FilterDomain::ARRAY) {
      cur.matchFilter("sparse_s", "y");
    }
    if (exactCount) cur.getNumber();
    if (domainBuilder) cur.facet("groups", "group_s").limit(-1);
    for (const auto& [field, dir] : sorts.clauses) qb::sort(cur, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    struct Result {
      std::vector<std::string> ids;
      int64_t found = 0;
      int64_t blocksSkipped = 0;
      int64_t bulkCollections = 0;
      int64_t externalActivations = 0;
      int64_t externalCandidates = 0;
      int64_t externalDomainRejects = 0;
      int64_t externalVerifications = 0;
      int64_t externalBoundRejects = 0;
      int64_t phraseVerifies = 0;
    } result;
    result.ids = resultIds(*req);
    const auto* docs = req->docList("q");
    if (docs != nullptr && docs->found) result.found = *docs->found;
    result.blocksSkipped = SkipStats::fieldSortBlocksSkipped;
    result.bulkCollections = SkipStats::fieldSortBulkCollections;
    result.externalActivations =
        SkipStats::fieldSortExternalApproxActivations;
    result.externalCandidates = SkipStats::fieldSortExternalApproxCandidates;
    result.externalDomainRejects =
        SkipStats::fieldSortExternalApproxDomainRejects;
    result.externalVerifications =
        SkipStats::fieldSortExternalApproxVerifications;
    result.externalBoundRejects =
        SkipStats::fieldSortExternalApproxBoundRejects;
    result.phraseVerifies = SkipStats::phraseVerifies;
    return result;
  };

  Sorts randAsc{{{"rand_i", qb::ASC}}};
  Sorts randDesc{{{"rand_i", qb::DESC}}};
  Sorts tiesAsc{{{"ties_i", qb::ASC}}};
  Sorts tiesThenRand{{{"ties_i", qb::ASC}, {"rand_i", qb::ASC}}};
  Sorts monoAsc{{{"mono_i", qb::ASC}}};
  Sorts revDesc{{{"rev_i", qb::DESC}}};

  for (bool matchAll : {true, false}) {
    for (const Sorts& sorts :
         {randAsc, randDesc, tiesAsc, tiesThenRand, monoAsc, revDesc}) {
      for (int32_t limit : {1, 9, 987, 4500}) {
        for (bool forcePull : {false, true}) {
          auto exhaustive = run(true, forcePull, matchAll, sorts, limit, false);
          auto pruned = run(false, forcePull, matchAll, sorts, limit, false);
          EXPECT_EQ(exhaustive.ids, pruned.ids)
              << "matchAll=" << matchAll << " limit=" << limit
              << " forcePull=" << forcePull
              << " sort=" << sorts.clauses[0].first;
        }
      }
    }
  }

  // Shallow sole-clause sorts must actually skip blocks. The random column
  // cannot skip strictly at this corpus size (the k-th-smallest bottom sits
  // above a 4096-value block's expected min until tens of thousands of docs
  // have been seen), so the deterministic assertions use doc-order-monotonic
  // columns in each direction plus the segdoc-guarded equality rule on the
  // tie column.
  EXPECT_GT(run(false, true, true, monoAsc, 9, false).blocksSkipped, 0);
  EXPECT_GT(run(false, true, true, revDesc, 9, false).blocksSkipped, 0);
  EXPECT_GT(run(false, false, false, monoAsc, 9, false).blocksSkipped, 0);
  // Match-all rides the null-source bulk scorer's match windows (with zone
  // skips intact); forcing pull keeps the scorer loop.
  {
    auto matchAllBulk = run(false, false, true, monoAsc, 9, false);
    EXPECT_GT(matchAllBulk.bulkCollections, 0);
    EXPECT_GT(matchAllBulk.blocksSkipped, 0);
    EXPECT_EQ(run(false, true, true, monoAsc, 9, false).bulkCollections, 0);
  }
  EXPECT_GT(run(false, true, true, tiesAsc, 5, false).blocksSkipped, 0);
  // A secondary clause forbids equality skipping; an all-ties primary then
  // proves nothing, so no blocks may be skipped.
  EXPECT_EQ(run(false, true, true, tiesThenRand, 5, false).blocksSkipped, 0);

  // An exact hit count disables pruning and stays exact.
  auto exactExhaustive = run(true, true, true, randAsc, 9, true);
  auto exactPruned = run(false, true, true, randAsc, 9, true);
  EXPECT_EQ(exactExhaustive.ids, exactPruned.ids);
  EXPECT_EQ(exactExhaustive.found, exactPruned.found);
  EXPECT_EQ(exactPruned.blocksSkipped, 0);

  // External two-phase collection must preserve the exhaustive result for
  // exact and sloppy phrases across all numeric bound/tie shapes and outer
  // domain representations. Dense and sparse term filters materialize above
  // and below DocSetBuilder's BITSET promotion threshold respectively.
  for (PhraseShape phraseShape : {PhraseShape::EXACT, PhraseShape::SLOPPY}) {
    for (FilterDomain filterDomain :
         {FilterDomain::NONE, FilterDomain::BITSET, FilterDomain::ARRAY}) {
      for (const Sorts& sorts :
           {randAsc, randDesc, tiesAsc, tiesThenRand, monoAsc, revDesc}) {
        auto exhaustive = run(true, true, false, sorts, 9, false,
                              phraseShape, filterDomain);
        auto pruned = run(false, true, false, sorts, 9, false,
                          phraseShape, filterDomain);
        EXPECT_EQ(exhaustive.ids, pruned.ids)
            << "phrase=" << (int32_t)phraseShape
            << " domain=" << (int32_t)filterDomain
            << " sort=" << sorts.clauses[0].first;
        EXPECT_EQ(0, exhaustive.externalActivations);
        EXPECT_GT(pruned.externalActivations, 0);
      }
    }
  }

  auto exactProof = run(false, true, false, randAsc, 9, false,
                        PhraseShape::EXACT);
  EXPECT_GT(exactProof.externalActivations, 0);
  EXPECT_GT(exactProof.externalCandidates, exactProof.externalVerifications);
  EXPECT_GT(exactProof.externalBoundRejects, 0);
  EXPECT_EQ(exactProof.externalVerifications, exactProof.phraseVerifies);

  auto sloppyProof = run(false, true, false, randDesc, 9, false,
                         PhraseShape::SLOPPY);
  EXPECT_GT(sloppyProof.externalActivations, 0);
  EXPECT_GT(sloppyProof.externalBoundRejects, 0);
  EXPECT_EQ(sloppyProof.externalVerifications, sloppyProof.phraseVerifies);

  auto bitDomain = run(false, true, false, randAsc, 9, false,
                       PhraseShape::EXACT, FilterDomain::BITSET);
  auto arrayDomain = run(false, true, false, randAsc, 9, false,
                         PhraseShape::EXACT, FilterDomain::ARRAY);
  EXPECT_GT(bitDomain.externalDomainRejects, 0);
  EXPECT_GT(arrayDomain.externalDomainRejects, 0);

  // Exact counts and produced sub-op domains require exhaustive membership and
  // therefore cannot select the external approximation arm.
  auto phraseCount = run(false, true, false, randAsc, 9, true,
                         PhraseShape::EXACT);
  auto phraseDomain = run(false, true, false, randAsc, 9, false,
                          PhraseShape::EXACT, FilterDomain::NONE, true);
  EXPECT_EQ(0, phraseCount.externalActivations);
  EXPECT_EQ(0, phraseDomain.externalActivations);
  EXPECT_GT(phraseCount.phraseVerifies, 0);
  EXPECT_GT(phraseDomain.phraseVerifies, 0);

  // Whole-index preparation changes the segment source, not the scorer's
  // external two-phase capability.
  auto prepared = run(false, true, false, randAsc, 9, false,
                      PhraseShape::EXACT, FilterDomain::NONE, false, true);
  EXPECT_GT(prepared.externalActivations, 0);
  EXPECT_GT(prepared.externalBoundRejects, 0);

}

// The external two-phase field-sort driver (approximation conjunction plus
// the live-bottom competitive gate ahead of positional verification) must
// return exactly what exhaustive collection returns. Exact and sloppy
// phrases, both directions, a secondary sort clause (which forbids equality
// skipping), missing values, deletes, folded and materialized filters, and
// limits from 1 to most-of-corpus. The parity matrix runs with whole
// membership disabled so the pull arm itself executes; a separate enabled
// section pins the resident ladder taking over on repeats - including the
// fallback where built membership becomes the execution source while the
// main weight is retained.
TEST_F(SortCollectorTest, phraseFieldSortPruningMatchesExhaustive) {
  CollectionHelper helper("phrase_field_sort");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr int32_t kDocsPerSeg = 5000;
  std::vector<std::string> deleted;
  int32_t docId = 0;
  for (int32_t seg = 0; seg < 2; seg++) {
    for (int32_t i = 0; i < kDocsPerSeg; i++, docId++) {
      std::string id = std::to_string(docId);
      int64_t rand = (int64_t)((uint32_t)docId * 2654435761u) & 0x7fffffff;
      // ~1/3 exact "red fox", ~1/3 slop-1 only, ~1/3 non-match at slop <= 1.
      std::string body = (docId % 3) == 0 ? "one red fox two"
          : (docId % 3) == 1              ? "one red pad fox two"
                                          : "one fox red two";
      std::string par = (docId & 1) == 0 ? "even" : "odd";
      if (seg == 1 && i % 13 == 0) {
        helper.index(flatdoc("id", id, "id_s", id, "body_w", body,
                             "par_w", par, "ties_i", (int64_t)(docId % 7)),
                     UpdateMessage::NO_COMMIT);
      } else {
        helper.index(flatdoc("id", id, "id_s", id, "body_w", body,
                             "par_w", par, "rand_i", rand,
                             "ties_i", (int64_t)(docId % 7)),
                     UpdateMessage::NO_COMMIT);
      }
      if (docId % 97 == 0) deleted.push_back(id);
    }
    helper.commit();
  }
  ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);

  struct Sorts {
    std::vector<std::pair<std::string_view, qb::SortDir>> clauses;
  };
  struct Result {
    std::vector<std::string> ids;
    int64_t found = 0;
    int64_t activations = 0;
    int64_t candidates = 0;
    int64_t domainRejects = 0;
    int64_t boundRejects = 0;
    int64_t verifications = 0;
    int64_t wholeHits = 0;
    int64_t wholeBuilds = 0;
  };
  // filterMode: 0 = none, 1 = folded into the query, 2 = materialized
  // effective filter (the arm's domain-intersection shape).
  auto run = [&](bool disablePruning, int32_t slop, const Sorts& sorts,
                 int32_t limit, bool exactCount, int32_t filterMode) {
    SortPruningGuard pruningGuard(disablePruning);
    TopDocsFilterFoldGuard foldGuard(filterMode == 2);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("phrase_field_sort");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    auto& phrase = cur.rawQuery().kind.emplace<api::PhraseQuery>();
    phrase.field = "body_w";
    phrase.text = "red fox";
    phrase.slop = slop;
    if (filterMode != 0) {
      cur.filter(qb::match(cur.mr(), "par_w", "even"));
    }
    if (exactCount) cur.getNumber();
    for (const auto& [field, dir] : sorts.clauses) qb::sort(cur, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    Result result;
    result.ids = resultIds(*req);
    const auto* docs = req->docList("q");
    if (docs != nullptr && docs->found) result.found = *docs->found;
    result.activations = SkipStats::fieldSortExternalApproxActivations;
    result.candidates = SkipStats::fieldSortExternalApproxCandidates;
    result.domainRejects = SkipStats::fieldSortExternalApproxDomainRejects;
    result.boundRejects = SkipStats::fieldSortExternalApproxBoundRejects;
    result.verifications = SkipStats::fieldSortExternalApproxVerifications;
    result.wholeHits = SkipStats::wholeFieldSortHits;
    result.wholeBuilds = SkipStats::wholeFieldSortBuilds;
    return result;
  };

  Sorts randAsc{{{"rand_i", qb::ASC}}};
  Sorts randDesc{{{"rand_i", qb::DESC}}};
  Sorts tiesThenRand{{{"ties_i", qb::ASC}, {"rand_i", qb::ASC}}};
  Sorts idAsc{{{"id_s", qb::ASC}}};

  {
    WholeMembershipPlanGuard wholeGuard(true);
    for (int32_t slop : {0, 1}) {
      for (const Sorts& sorts : {randAsc, randDesc, tiesThenRand, idAsc}) {
        for (int32_t limit : {1, 9, 987}) {
          for (int32_t filterMode : {0, 2}) {
            auto exhaustive = run(true, slop, sorts, limit, false, filterMode);
            auto pruned = run(false, slop, sorts, limit, false, filterMode);
            EXPECT_EQ(exhaustive.ids, pruned.ids)
                << "slop=" << slop << " limit=" << limit
                << " filterMode=" << filterMode
                << " sort=" << sorts.clauses[0].first;
          }
        }
      }
    }

    // Proof signature: the arm activates, the live-bottom gate rejects
    // candidates before verification, and verification stays below the
    // candidate stream. An exhaustive run never activates it.
    auto pruned = run(false, 0, randAsc, 9, false, 0);
    EXPECT_GT(pruned.activations, 0);
    EXPECT_GT(pruned.boundRejects, 0);
    EXPECT_LT(pruned.verifications, pruned.candidates);
    EXPECT_EQ(run(true, 0, randAsc, 9, false, 0).activations, 0);

    // The materialized filter rejects candidates ahead of verification.
    EXPECT_GT(run(false, 0, randAsc, 9, false, 2).domainRejects, 0);

    // An exact hit count disables the arm and stays exact.
    auto exactExhaustivePhrase = run(true, 0, randAsc, 9, true, 0);
    auto exactPrunedPhrase = run(false, 0, randAsc, 9, true, 0);
    EXPECT_EQ(exactExhaustivePhrase.ids, exactPrunedPhrase.ids);
    EXPECT_EQ(exactExhaustivePhrase.found, exactPrunedPhrase.found);
    EXPECT_EQ(exactPrunedPhrase.activations, 0);

    // Sloppy widens the match set: the slop-1 spelling must change found.
    EXPECT_GT(run(true, 1, randAsc, 9, true, 0).found,
              run(true, 0, randAsc, 9, true, 0).found);
  }

  // Whole membership enabled: repeats climb the bypass/build/hit ladder and
  // the resident value takes over as the execution source (the main weight
  // stays constructed - the fallback must still drop to the built DocSet).
  // Results must stay identical to the membership-off exhaustive answer on
  // every rung, for both the numeric sort and the string sort (whose
  // best-first plan is unavailable, forcing the DocSetSupplier fallback).
  {
    std::vector<std::string> expectedRand;
    std::vector<std::string> expectedId;
    {
      WholeMembershipPlanGuard wholeGuard(true);
      expectedRand = run(true, 0, randAsc, 9, false, 0).ids;
      expectedId = run(true, 0, idAsc, 9, false, 0).ids;
    }
    WholeMembershipPlanGuard wholeGuard(false);
    int64_t ladder = 0;
    for (int32_t rep = 0; rep < 4; rep++) {
      auto viaRand = run(false, 0, randAsc, 9, false, 0);
      auto viaId = run(false, 0, idAsc, 9, false, 0);
      EXPECT_EQ(expectedRand, viaRand.ids) << "rep=" << rep;
      EXPECT_EQ(expectedId, viaId.ids) << "rep=" << rep;
      ladder += viaRand.wholeHits + viaRand.wholeBuilds
          + viaId.wholeHits + viaId.wholeBuilds;
    }
    EXPECT_GT(ladder, 0);
  }
}

// The best-first exact-domain driver must return exactly what the doc-order
// windowed path and exhaustive collection return. Its bitset domain arrives
// two ways: the folded filter-only Boolean's cached set (delete-free
// segments only - a raw cache borrow must be live-exact), or an explicit
// effective filter with folding disabled (which handles deletes). The corpus
// spans multiple key blocks, a missing-value segment (sparse column declines
// the masked plan, so segments mix arms), ties, and both directions; the
// small block count means the expected-floor gate always declines, so the
// force override drives the route and the gate itself is asserted once.
TEST_F(SortCollectorTest, bestFirstFieldSortMatchesExhaustive) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper("best_first_sort");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr int32_t kDocsPerSeg = 5000;
  int32_t docId = 0;
  for (int32_t seg = 0; seg < 2; seg++) {
    for (int32_t i = 0; i < kDocsPerSeg; i++, docId++) {
      std::string id = std::to_string(docId);
      int64_t rand = (int64_t)((uint32_t)docId * 2654435761u) & 0x7fffffff;
      std::string body = (docId & 1) == 0 ? "alpha" : "other";
      Doc doc;
      if (seg == 1 && i % 13 == 0) {
        doc = flatdoc("id", id, "id_s", id, "body_w", body,
                      "ties_i", (int64_t)(docId % 7),
                      "mono_i", (int64_t)docId);
      } else {
        doc = flatdoc("id", id, "id_s", id, "body_w", body,
                      "rand_i", rand,
                      "ties_i", (int64_t)(docId % 7),
                      "mono_i", (int64_t)docId);
      }
      // ~2% of docs: materializes under the bitset promotion threshold, so
      // this filter is the array-domain route's shape.
      if (docId % 50 == 0) {
        doc.push_back(NameVal{"arr_s", "y"});
      }
      helper.index(doc, UpdateMessage::NO_COMMIT);
    }
    helper.commit();
  }

  struct Sorts {
    std::vector<std::pair<std::string_view, qb::SortDir>> clauses;
  };
  auto run = [&](bool bestFirst, bool disablePruning, bool foldFilters,
                 const Sorts& sorts, int32_t limit, bool exactCount,
                 std::string_view field = "body_w",
                 std::string_view value = "alpha") {
    BestFirstGuard bfGuard(!bestFirst, bestFirst);
    SortPruningGuard pruningGuard(disablePruning);
    TopDocsFilterFoldGuard foldGuard(!foldFilters);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("best_first_sort");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    cur.allQuery();
    if (!field.empty()) cur.matchFilter(field, value);
    if (exactCount) cur.getNumber();
    for (const auto& [f, dir] : sorts.clauses) qb::sort(cur, f, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    struct Result {
      std::vector<std::string> ids;
      int64_t found = 0;
      int64_t activations = 0;
    } result;
    result.ids = resultIds(*req);
    const auto* docs = req->docList("q");
    if (docs != nullptr && docs->found) result.found = *docs->found;
    result.activations = SkipStats::fieldSortBestFirstActivations;
    return result;
  };

  Sorts randAsc{{{"rand_i", qb::ASC}}};
  Sorts randDesc{{{"rand_i", qb::DESC}}};
  Sorts tiesAsc{{{"ties_i", qb::ASC}}};
  Sorts tiesThenRand{{{"ties_i", qb::ASC}, {"rand_i", qb::ASC}}};
  Sorts monoAsc{{{"mono_i", qb::ASC}}};

  // Two sightings admit and materialize the filter; the route serves hits.
  run(false, true, true, randAsc, 9, false);
  run(false, true, true, randAsc, 9, false);

  for (const Sorts& sorts :
       {randAsc, randDesc, tiesAsc, tiesThenRand, monoAsc}) {
    for (int32_t limit : {1, 9, 987, 4500}) {
      auto exhaustive = run(false, true, true, sorts, limit, false);
      auto docOrder = run(false, false, true, sorts, limit, false);
      auto bestFirst = run(true, false, true, sorts, limit, false);
      EXPECT_EQ(exhaustive.ids, docOrder.ids)
          << "limit=" << limit << " sort=" << sorts.clauses[0].first;
      EXPECT_EQ(exhaustive.ids, bestFirst.ids)
          << "limit=" << limit << " sort=" << sorts.clauses[0].first;
      // Pure match-all: no filter at all, the empty-mask domain form.
      auto allExhaustive = run(false, true, true, sorts, limit, false, "");
      auto allBestFirst = run(true, false, true, sorts, limit, false, "");
      EXPECT_EQ(allExhaustive.ids, allBestFirst.ids)
          << "match-all limit=" << limit
          << " sort=" << sorts.clauses[0].first;
    }
  }

  // A tiny forced work cap crosses to the forward-sweep fallback on every
  // shape; parity must hold and the fallback counter must fire.
  {
    WorkCapGuard capGuard(2);
    SortSkipStatsGuard statsGuard;
    for (const Sorts& sorts : {randAsc, randDesc, tiesAsc, monoAsc}) {
      for (int32_t limit : {9, 987}) {
        auto exhaustive = run(false, true, true, sorts, limit, false);
        auto capped = run(true, false, true, sorts, limit, false);
        EXPECT_EQ(exhaustive.ids, capped.ids)
            << "capped limit=" << limit << " sort=" << sorts.clauses[0].first;
        auto allExhaustive = run(false, true, true, sorts, limit, false, "");
        auto allCapped = run(true, false, true, sorts, limit, false, "");
        EXPECT_EQ(allExhaustive.ids, allCapped.ids)
            << "capped match-all limit=" << limit
            << " sort=" << sorts.clauses[0].first;
      }
    }
    EXPECT_GT(SkipStats::fieldSortBestFirstFallbacks, 0);
  }

  // Route engagement and gates, proven by the activation counter.
  EXPECT_GT(run(true, false, true, randAsc, 9, false).activations, 0);
  // Pure match-all with no deletes rides the empty-mask domain form.
  EXPECT_GT(run(true, false, true, randAsc, 9, false, "").activations, 0);
  // Without the force override the expected floor saturates this corpus's
  // two key blocks per segment, so the gate declines.
  EXPECT_EQ(run(false, false, true, randAsc, 9, false).activations, 0);
  // Secondary clause: no sole column, no masked plan.
  EXPECT_EQ(run(true, false, true, tiesThenRand, 9, false).activations, 0);
  // Exact count keeps every pruning route off and stays exact.
  {
    auto exact = run(true, false, true, randAsc, 9, true);
    auto exactExh = run(false, true, true, randAsc, 9, true);
    EXPECT_EQ(exact.activations, 0);
    EXPECT_EQ(exact.ids, exactExh.ids);
    EXPECT_EQ(exact.found, exactExh.found);
  }
  // Array DocSet domains (below the bitset promotion threshold) ride the
  // gallop-and-gather entry: a ~2% filter and the degenerate single-doc
  // filter both activate (force bypasses the floor gate) and match
  // exhaustive collection.
  for (auto [field, value] : {std::pair<std::string_view, std::string_view>
                                  {"arr_s", "y"}, {"id_s", "42"}}) {
    run(true, false, true, randAsc, 9, false, field, value);
    run(true, false, true, randAsc, 9, false, field, value);
    for (int32_t limit : {1, 9, 987}) {
      auto arrBestFirst =
          run(true, false, true, randAsc, limit, false, field, value);
      auto arrExhaustive =
          run(false, true, true, randAsc, limit, false, field, value);
      EXPECT_GT(arrBestFirst.activations, 0)
          << field << " limit=" << limit;
      EXPECT_EQ(arrExhaustive.ids, arrBestFirst.ids)
          << field << " limit=" << limit;
      EXPECT_EQ(arrExhaustive.found, arrBestFirst.found)
          << field << " limit=" << limit;
    }
  }

  // Deletes: the folded route's raw cache borrow must decline (live-exact
  // only), while the unfolded effective-filter route folds liveness and
  // stays usable - and both remain correct.
  ASSERT_TRUE(helper.deleteByIds({"42", "1000", "7003"},
                                 UpdateMessage::COMMIT).success);
  auto deletedExhaustive = run(false, true, true, randAsc, 987, false);
  auto deletedFolded = run(true, false, true, randAsc, 987, false);
  EXPECT_EQ(deletedFolded.activations, 0);
  EXPECT_EQ(deletedExhaustive.ids, deletedFolded.ids);
  auto deletedUnfolded = run(true, false, false, randAsc, 987, false);
  EXPECT_GT(deletedUnfolded.activations, 0);
  EXPECT_EQ(deletedExhaustive.ids, deletedUnfolded.ids);
  // Pure match-all with deletes: liveDocs becomes the domain bitset per the
  // root domain contract, so the route still activates and stays correct.
  auto deletedAllExhaustive = run(false, true, true, randAsc, 987, false, "");
  auto deletedAllBestFirst = run(true, false, true, randAsc, 987, false, "");
  EXPECT_GT(deletedAllBestFirst.activations, 0);
  EXPECT_EQ(deletedAllExhaustive.ids, deletedAllBestFirst.ids);
  // Array domain with deletes (doc 1000 is an arr_s member): the folded raw
  // borrow declines, the unfolded effective set stays an array and correct.
  auto deletedArrExhaustive =
      run(false, true, true, randAsc, 987, false, "arr_s", "y");
  auto deletedArrUnfolded =
      run(true, false, false, randAsc, 987, false, "arr_s", "y");
  EXPECT_GT(deletedArrUnfolded.activations, 0);
  EXPECT_EQ(deletedArrExhaustive.ids, deletedArrUnfolded.ids);
}

// Seeded two-pass query-driven collection must match doc-order pruning and
// exhaustive collection. The corpus gives every query shape the driver must
// survive: "alpha" is a mid-density term uncorrelated with the sort keys,
// "head" lives only inside segment 0's first key block (the term exhausts in
// an early seed and later seed windows must come back empty), and "tail"
// lives only in the last docs (anti-correlated with mono_i asc: the
// best-bounded seeds hold no matches, exercising the underfilled-heap
// path). A 500-per-mille seed budget forces undershoot so pass 2 has real
// work; ties exercise the segdoc guard when the pass-2 cursor runs behind
// pass-1 admissions.
TEST_F(SortCollectorTest, seededFieldSortMatchesExhaustive) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper("seeded_sort");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr int32_t kDocsPerSeg = 5000;
  int32_t docId = 0;
  for (int32_t seg = 0; seg < 2; seg++) {
    for (int32_t i = 0; i < kDocsPerSeg; i++, docId++) {
      std::string id = std::to_string(docId);
      int64_t rand = (int64_t)((uint32_t)docId * 2654435761u) & 0x7fffffff;
      std::string body = (docId & 1) == 0 ? "alpha" : "other";
      if (docId < 500) body = "head";
      if (docId >= 9000) body = "tail";
      if (seg == 1 && i % 13 == 0) {
        helper.index(flatdoc("id", id, "id_s", id, "body_w", body,
                             "ties_i", (int64_t)(docId % 7),
                             "mono_i", (int64_t)docId),
                     UpdateMessage::NO_COMMIT);
      } else {
        helper.index(flatdoc("id", id, "id_s", id, "body_w", body,
                             "rand_i", rand,
                             "ties_i", (int64_t)(docId % 7),
                             "mono_i", (int64_t)docId),
                     UpdateMessage::NO_COMMIT);
      }
    }
    helper.commit();
  }

  struct Sorts {
    std::vector<std::pair<std::string_view, qb::SortDir>> clauses;
  };
  enum class Main { TERM, BOOLEAN };
  auto run = [&](bool seeded, bool disablePruning, const Sorts& sorts,
                 int32_t limit, bool exactCount,
                 std::string_view value = "alpha",
                 int32_t budgetPerMille = 0, Main main = Main::TERM) {
    SeededGuard seededGuard(!seeded, seeded, budgetPerMille);
    SortPruningGuard pruningGuard(disablePruning);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("seeded_sort");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    if (main == Main::TERM) {
      cur.matchQuery("body_w", value);
    } else {
      cur.rawQuery() = qb::boolean(
          cur.mr(), {qb::match(cur.mr(), "body_w", value)}, {},
          {qb::match(cur.mr(), "body_w", "tail")});
    }
    if (exactCount) cur.getNumber();
    for (const auto& [f, dir] : sorts.clauses) qb::sort(cur, f, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    struct Result {
      std::vector<std::string> ids;
      int64_t found = 0;
      int64_t activations = 0;
      int64_t seedBlocks = 0;
      int64_t fillAborts = 0;
      int64_t pass2Skips = 0;
    } result;
    result.ids = resultIds(*req);
    const auto* docs = req->docList("q");
    if (docs != nullptr && docs->found) result.found = *docs->found;
    result.activations = SkipStats::fieldSortSeededActivations;
    result.seedBlocks = SkipStats::fieldSortSeedLeaves;
    result.fillAborts = SkipStats::fieldSortSeedFillAborts;
    result.pass2Skips = SkipStats::fieldSortSeedPass2Skips;
    return result;
  };

  Sorts randAsc{{{"rand_i", qb::ASC}}};
  Sorts randDesc{{{"rand_i", qb::DESC}}};
  Sorts tiesAsc{{{"ties_i", qb::ASC}}};
  Sorts tiesThenRand{{{"ties_i", qb::ASC}, {"rand_i", qb::ASC}}};
  Sorts monoAsc{{{"mono_i", qb::ASC}}};

  for (const Sorts& sorts :
       {randAsc, randDesc, tiesAsc, tiesThenRand, monoAsc}) {
    for (int32_t limit : {1, 9, 987, 4500}) {
      for (std::string_view term : {"alpha", "head", "tail"}) {
        auto exhaustive = run(false, true, sorts, limit, false, term);
        auto docOrder = run(false, false, sorts, limit, false, term);
        auto seeded = run(true, false, sorts, limit, false, term);
        // Undershot seed budget: pass 2 must complete the floor.
        auto underseeded = run(true, false, sorts, limit, false, term, 500);
        EXPECT_EQ(exhaustive.ids, docOrder.ids)
            << "limit=" << limit << " term=" << term
            << " sort=" << sorts.clauses[0].first;
        EXPECT_EQ(exhaustive.ids, seeded.ids)
            << "limit=" << limit << " term=" << term
            << " sort=" << sorts.clauses[0].first;
        EXPECT_EQ(exhaustive.ids, underseeded.ids)
            << "underseeded limit=" << limit << " term=" << term
            << " sort=" << sorts.clauses[0].first;
      }
    }
  }

  // Route engagement, proven by the activation counter; multi-clause sorts
  // have no sole column and must decline.
  EXPECT_GT(run(true, false, randAsc, 9, false).activations, 0);
  EXPECT_EQ(run(true, false, tiesThenRand, 9, false).activations, 0);
  // Without the force override the two-block corpus can never clear the
  // materiality gate.
  EXPECT_EQ(run(false, false, randAsc, 9, false).activations, 0);
  // A compound main query offers no independentReplan capability.
  EXPECT_EQ(run(true, false, randAsc, 9, false, "alpha", 0,
                Main::BOOLEAN).activations, 0);
  // Exact count keeps every pruning route off and stays exact.
  {
    auto exact = run(true, false, randAsc, 9, true);
    auto exactExh = run(false, true, randAsc, 9, true);
    EXPECT_EQ(exact.activations, 0);
    EXPECT_EQ(exact.ids, exactExh.ids);
    EXPECT_EQ(exact.found, exactExh.found);
  }
  // "head" matches exactly segment 0's first key block and limit 987 keeps
  // the heap underfull: every match must be collected exactly once, so any
  // double-collect across the passes inflates found.
  {
    auto seededHead = run(true, false, randAsc, 987, false, "head");
    auto exhHead = run(false, true, randAsc, 987, false, "head");
    EXPECT_GT(seededHead.activations, 0);
    EXPECT_EQ(exhHead.found, seededHead.found);
    EXPECT_EQ(exhHead.ids, seededHead.ids);
  }
  // Anti-correlated "tail" under mono asc with an undershot budget: the one
  // seed block holds no matches, the heap stays underfull, and the sweep
  // completes correctness.
  {
    auto seededTail = run(true, false, monoAsc, 987, false, "tail", 500);
    auto exhTail = run(false, true, monoAsc, 987, false, "tail");
    EXPECT_GT(seededTail.activations, 0);
    EXPECT_EQ(exhTail.ids, seededTail.ids);
    EXPECT_EQ(exhTail.found, seededTail.found);
  }

  // Deletes arrive as the liveDocs domain bitset and intersect both passes.
  ASSERT_TRUE(helper.deleteByIds({"600", "1042", "7003"},
                                 UpdateMessage::COMMIT).success);
  for (const Sorts& sorts : {randAsc, tiesAsc}) {
    auto exhaustive = run(false, true, sorts, 987, false);
    auto seeded = run(true, false, sorts, 987, false);
    EXPECT_GT(seeded.activations, 0);
    EXPECT_EQ(exhaustive.ids, seeded.ids)
        << "deletes sort=" << sorts.clauses[0].first;
  }
}

// The seed-schedule abort must fire against an anti-correlated query on a
// segment with more key blocks than the minimum abort budget (8): the term
// lives only in the last of 13 blocks while the sort prefers the first
// blocks, so seed probes come back empty. Depending on task scheduling the
// segment collects cold (underfilled-heap abort) or against a heap warmed
// by the small all-matching segment whose keys are all worse (consecutive
// matchless-seed abort); both must abandon the schedule and let the sweep
// complete correctness.
TEST_F(SortCollectorTest, seededFieldSortAntiCorrelationAborts) {
  WholeMembershipPlanGuard wholeGuard(true);
  constexpr int32_t kBigSegDocs = 13 * 4096;
  constexpr int32_t kProbeStart = 12 * 4096;
  CollectionHelper helper("seeded_abort");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  for (int32_t i = 0; i < 5000; i++) {
    helper.index(flatdoc("id", std::to_string(i), "id_s", std::to_string(i),
                         "body_w", "probe", "mono_i", (int64_t)(100000 + i)),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();
  for (int32_t i = 0; i < kBigSegDocs; i++) {
    int32_t docId = 5000 + i;
    helper.index(flatdoc("id", std::to_string(docId),
                         "id_s", std::to_string(docId),
                         "body_w", i >= kProbeStart ? "probe" : "filler",
                         "mono_i", (int64_t)i),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();

  auto run = [&](bool seeded, bool disablePruning) {
    SeededGuard seededGuard(!seeded, seeded);
    SortPruningGuard pruningGuard(disablePruning);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("seeded_abort");
    auto& cur = req->topDocs("q").limit(9).fields({"id_s"})
        .matchQuery("body_w", "probe");
    qb::sort(cur, "mono_i", qb::ASC);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    struct Result {
      std::vector<std::string> ids;
      int64_t activations = 0;
      int64_t fillAborts = 0;
    } result;
    result.ids = resultIds(*req);
    result.activations = SkipStats::fieldSortSeededActivations;
    result.fillAborts = SkipStats::fieldSortSeedFillAborts;
    return result;
  };

  auto exhaustive = run(false, true);
  auto seeded = run(true, false);
  EXPECT_GT(seeded.activations, 0);
  EXPECT_GT(seeded.fillAborts, 0);
  EXPECT_EQ(exhaustive.ids, seeded.ids);
}

// String candidate pruning (postings-union over the competitive ord interval)
// must match exhaustive collection. One segment exercises the identity
// GlobalOrdComparator path, two segments the SegmentOrdComparator path with
// collector reuse; the corpus carries a high-cardinality column (activation),
// a 7-value tie column (empty-interval termination), missing values, and
// deletes.
void SortCollectorTest::runStringCandidatePruning(int32_t nSegs) {
  WholeMembershipPlanGuard wholeGuard(true);
  {
    CollectionHelper helper;
    helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
    constexpr int32_t kDocsPerSeg = 12000;
    std::vector<std::string> deleted;
    int32_t docId = 0;
    for (int32_t seg = 0; seg < nSegs; seg++) {
      for (int32_t i = 0; i < kDocsPerSeg; i++, docId++) {
        std::string id = std::to_string(docId);
        uint32_t rand = (uint32_t)((uint32_t)docId * 2654435761u) % 200000;
        std::string randValue = std::format("t{:06}", rand);
        std::string body = (docId & 1) == 0 ? "alpha" : "other";
        std::string tieValue = "v" + std::to_string(docId % 7);
        if (docId % 13 == 0) {
          helper.index(flatdoc("id", id, "id_s", id, "body_w", body,
                               "ties_s", tieValue),
                       UpdateMessage::NO_COMMIT);
        } else {
          helper.index(flatdoc("id", id, "id_s", id, "body_w", body,
                               "rand_s", randValue, "ties_s", tieValue),
                       UpdateMessage::NO_COMMIT);
        }
        if (docId % 97 == 0) deleted.push_back(id);
      }
      helper.commit();
    }
    ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);

    struct Sorts {
      std::vector<std::pair<std::string_view, qb::SortDir>> clauses;
    };
    struct Result {
      std::vector<std::string> ids;
      int64_t activations = 0;
      int64_t terminations = 0;
    };
    auto run = [&](bool disablePruning, bool forcePull, bool matchAll,
                   const Sorts& sorts, int32_t limit) {
      SortPruningGuard pruningGuard(disablePruning);
      FieldSortBulkGuard bulkGuard(forcePull);
      SortSkipStatsGuard statsGuard;
      auto req = localReq(luxirNode->getSearchEngine());
      req->collection("main");
      auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
      if (matchAll) {
        cur.allQuery();
      } else {
        cur.rawQuery() = qb::match(cur.mr(), "body_w", "alpha");
      }
      for (const auto& [field, dir] : sorts.clauses) qb::sort(cur, field, dir);
      req->execute(false);
      EXPECT_TRUE(req->ok()) << req->errorMsg();
      Result result;
      result.ids = resultIds(*req);
      result.activations = SkipStats::fieldSortCandidateActivations;
      result.terminations = SkipStats::fieldSortCandidateTerminations;
      return result;
    };

    Sorts randAsc{{{"rand_s", qb::ASC}}};
    Sorts randDesc{{{"rand_s", qb::DESC}}};
    Sorts tiesAsc{{{"ties_s", qb::ASC}}};
    Sorts tiesThenRand{{{"ties_s", qb::ASC}, {"rand_s", qb::ASC}}};

    for (bool matchAll : {true, false}) {
      for (const Sorts& sorts : {randAsc, randDesc, tiesAsc, tiesThenRand}) {
        for (int32_t limit : {1, 9, 987}) {
          for (bool forcePull : {false, true}) {
            auto exhaustive = run(true, forcePull, matchAll, sorts, limit);
            auto pruned = run(false, forcePull, matchAll, sorts, limit);
            EXPECT_EQ(exhaustive.ids, pruned.ids)
                << "nSegs=" << nSegs << " matchAll=" << matchAll
                << " limit=" << limit << " forcePull=" << forcePull
                << " sort=" << sorts.clauses[0].first;
          }
        }
      }
    }

    // Shallow high-cardinality sorts must activate the candidate union in
    // both directions and drivers; the sole-clause tie column empties its
    // interval and terminates instead of activating.
    EXPECT_GT(run(false, true, true, randAsc, 9).activations, 0);
    EXPECT_GT(run(false, true, true, randDesc, 9).activations, 0);
    // The windowed (term query) route only clears the cost gate once the
    // interval is very tight relative to the filtered remainder: k=1.
    EXPECT_GT(run(false, false, false, randAsc, 1).activations, 0);
    auto ties = run(false, true, true, tiesAsc, 5);
    EXPECT_EQ(ties.activations, 0);
    EXPECT_GT(ties.terminations, 0);
  }
}

// Missing-first docs beat any real-valued bottom but have no postings, so a
// term interval can never represent them: both ord comparators must withhold
// the interval (PENDING) whenever missing sorts first.
TEST_F(SortCollectorTest, missingFirstBlocksCandidateIntervals) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "d0", "s_s", "b"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d1"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d2", "s_s", "a"), UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();
  auto& leaf = reader->segments()[0];
  using Status = FieldComparator::OrdIntervalStatus;
  for (bool missingFirst : {false, true}) {
    auto missing = missingFirst ? FieldComparator::MISSING_FIRST
                                : FieldComparator::MISSING_LAST;
    Status expected = missingFirst ? Status::PENDING : Status::READY;

    SegmentOrdComparator seg("s_s", false, missing);
    seg.growSlots(1);
    seg.setSegment(0, &leaf.postingsReader());
    seg.copy(0, segdoc(0, 0));
    seg.setBottom(0);
    FieldComparator::CompetitiveOrdState state;
    EXPECT_EQ(seg.competitiveOrdInterval(state, 0, segdoc(0, 0), 0, 1, true),
              expected);

    GlobalOrdComparator glob("s_s", nullptr, false, missing);
    glob.growSlots(1);
    glob.setSegment(0, &leaf.postingsReader());
    glob.copy(0, segdoc(0, 0));
    EXPECT_EQ(glob.competitiveOrdInterval(state, 0, segdoc(0, 0), 0, 1, true),
              expected);
  }
}

TEST_F(SortCollectorTest, stringCandidatePruningSingleSegment) {
  runStringCandidatePruning(1);
}

TEST_F(SortCollectorTest, stringCandidatePruningTwoSegments) {
  runStringCandidatePruning(2);
}

TEST_F(SortCollectorTest, RandomValuesWithTieBreaking) {
  CollectionHelper helper;

  int nSegs = 9;
  int docsPerSeg = 10;
  int maxVal = 5;
  int totalDocs = nSegs * docsPerSeg;

  // set the mergeFactor very high to avoid merges during indexing
  // we want many segments to try and get
  helper.getIndexWriter()->mergePolicy->setMergeFactor(nSegs+1);
  
  // Track expected results: tuples of (value, docid, segment)
  struct DocInfo {
    int32_t value;
    int32_t docId;
    int32_t segment;
    int32_t docInSegment;
  };
  std::vector<DocInfo> expectedOrder;
  
  // Create multiple segments with random values
  int docId = 0;

  for (int seg = 0; seg < nSegs; seg++) {
    SplitMix64 rng(seg); // Predictable random numbers per segment
    
    // Index documents for this segment
    for (int i = 0; i < docsPerSeg; i++) {
      int32_t value = rng.rint(maxVal);
      // make a string value that sorts the same as the int value
      std::string svalue = std::format("{:05}", value);
      
      helper.index(flatdoc("id_s", std::to_string(docId), "value_i", value, "value_s", svalue),
                   UpdateMessage::NO_COMMIT);
      
      expectedOrder.push_back({value, docId, seg, i});
      docId++;
    }
    
    // Commit to create a segment
    helper.commit();
  }

  for (std::string_view sortField : {"value_i", "value_s"}) {
    for (int direction = 0; direction < 2; direction++) {
      // TODO: test missing values as well.

      // Sort expected order: by value descending, then by segment/doc ascending for ties
      std::sort(expectedOrder.begin(), expectedOrder.end(),
        [&](const auto& a, const auto& b) {
          // ASC
          if (direction == 0) {
            if (a.value != b.value) {
              return a.value < b.value; // Higher values first (DESC)
            }
          }
          else {
            //DESC
            if (a.value != b.value) {
              return a.value > b.value; // Higher values first (DESC)
            }
          }
          // For ties, sort by segment first, then by doc within segment
          if (a.segment != b.segment) {
            return a.segment < b.segment;
          }
          return a.docInSegment < b.docInSegment;
        });

      // Search with sorting
      auto req = localReq(luxirNode->getSearchEngine());
      req->collection("main");

      int limit = rng.rint(1, docsPerSeg*3/2);  // Get all results

      // Sort by value (direction varies per iteration)
      auto& cur = req->topDocs("q").getNumber().limit(limit).allQuery().fields({"id_s", "value_i"});
      qb::sort(cur, sortField, direction ? qb::DESC : qb::ASC);

      req->execute(true); // Run multi-threaded
      ASSERT_OK(req);

      const auto* docs = req->docList("q");

      ASSERT_EQ(docs->found.value_or(0), docId) << "Should match all documents";

      // Debug: print how many segments we have
      auto reader = helper.getIndexWriter()->getIndexReader();

      // Verify results are in expected order
      const auto& idCol = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
      const auto& valueCol = std::get<luxir::api::ColInt>(docs->columns.at("value_i").kind);

      // verify that the number of results match either the limit or the number of docs indexed (whichever is smaller)
      ASSERT_EQ(idCol.v.size(), std::min(limit, totalDocs));

      for (int i = 0; i < std::min((int)idCol.v.size(), (int)expectedOrder.size()); i++) {
        int64_t actualId = 0;
        std::from_chars(idCol.v[i].data(), idCol.v[i].data() + idCol.v[i].size(), actualId);
        int64_t actualValue = valueCol.v[i];

        if (actualId != expectedOrder[i].docId) {
          // Debug: print nearby entries
          std::cout << "Mismatch at position " << i << ":\n";
          for (int j = std::max(0, i-2); j < std::min(i+3, (int)expectedOrder.size()); j++) {
            std::cout << "  [" << j << "] expected: id=" << expectedOrder[j].docId
                      << " value=" << expectedOrder[j].value
                      << " seg=" << expectedOrder[j].segment
                      << " docInSeg=" << expectedOrder[j].docInSegment << "\n";
          }
          std::cout << "  Actual at [" << i << "]: id=" << actualId
                    << " value=" << actualValue << "\n";
        }
        ASSERT_EQ(actualId, expectedOrder[i].docId)
          << "Position " << i << ": Expected id=" << expectedOrder[i].docId
          << " but got id=" << actualId << " (value=" << actualValue << ")";
        ASSERT_EQ(actualValue, expectedOrder[i].value)
          << "Position " << i << ": Expected value=" << expectedOrder[i].value
          << " but got value=" << actualValue;
      }
    }
  }
}
