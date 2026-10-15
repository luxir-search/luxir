// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/ops/TopDocsReq.h"
#include "luxir/search/SortField.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/random.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <tuple>
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

class LeafDescentGuard {
  std::optional<bool> saved;

public:
  explicit LeafDescentGuard(std::optional<bool> descend)
      : saved(FieldSortCollector::forceLeafDescentForTests) {
    FieldSortCollector::forceLeafDescentForTests = descend;
  }
  ~LeafDescentGuard() {
    FieldSortCollector::forceLeafDescentForTests = saved;
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

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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

  // Field-sort planning reads the same leaf metadata from each segment's
  // PostingsReader, once: the dense single-valued column offers it in every
  // segment, the sparse and multi-valued ones in none.
  ASSERT_EQ(2u, reader->segments().size());
  for (std::string_view field :
       {"dense_i", "sparse_i", "dense_is", "sparse_is"}) {
    SimpleNumericFieldComparator comparator(std::string(field), true,
                                            FieldComparator::MISSING_LAST);
    for (auto& segment : reader->segments()) {
      comparator.setSegment(segment.ord, &segment.postingsReader());
      auto keys = comparator.keyBatch()->segmentKeys(std::nullopt);
      const auto* meta = segment.postingsReader().numericLeafMeta(field);
      EXPECT_EQ(meta, segment.postingsReader().numericLeafMeta(field));
      EXPECT_EQ(field == "dense_i", meta != nullptr) << field;
      ASSERT_EQ(keys.has_value(), meta != nullptr) << field;
      if (meta == nullptr) continue;
      FieldComparator::SegmentKeys cached(*meta, true, std::nullopt);
      ASSERT_EQ(keys->leafCount(), cached.leafCount());
      for (int64_t rank = 0; rank < cached.leafCount(); rank++) {
        EXPECT_EQ(keys->boundOrderLeaf(rank).leaf,
                  cached.boundOrderLeaf(rank).leaf);
        EXPECT_EQ(keys->boundOrderLeaf(rank).bound,
                  cached.boundOrderLeaf(rank).bound);
      }
    }
  }
}

// A delete-only or an overlay-only commit opens a new snapshot core under
// the same core generation. The leaf metadata a field sort plans from points
// into the mapped column bytes, so it lives on the segment's PostingsReader,
// which the new core shares: a multi-segment field sort after the old reader
// is released must read only mappings a live holder keeps. A file-backed
// directory unmaps with the last holder; RAMDir buffers would hide a stale
// view.
TEST_F(SortCollectorTest, fieldSortLeafMetaLivesWithItsSegment) {
  bool savedOverlays = TestOverlayAuxReader::enabledForTests;
  TestOverlayAuxReader::enabledForTests = true;
  auto path = std::filesystem::temp_directory_path() / "luxir-fieldsort-reopen";
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] {
    TestOverlayAuxReader::enabledForTests = savedOverlays;
    std::filesystem::remove_all(path);
  });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  LuxirNode node(config);
  CollectionHelper helper(node, "reopen");
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  int32_t docId = 0;
  for (int32_t docs : {3 * 512 + 17, 2 * 512, 700}) {
    std::vector<Doc> batch;
    for (int32_t i = 0; i < docs; i++, docId++) {
      batch.push_back(flatdoc("id", std::to_string(docId), "id_s",
                              std::to_string(docId), "time_i",
                              (int64_t)docId));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }
  auto newest = [&] {
    auto req = localReq(node.getSearchEngine());
    req->collection("reopen");
    auto& cur = req->topDocs("q").limit(5).fields({"id_s"});
    cur.allQuery();
    qb::sort(cur, "time_i", qb::DESC);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  };
  std::vector<std::string> expected;
  for (int32_t doc = docId - 1; doc >= docId - 5; doc--) {
    expected.push_back(std::to_string(doc));
  }
  auto reopen = [&](auto&& commit) {
    std::weak_ptr<IndexReader> old = writer->snapshots.readers.getReader();
    ASSERT_EQ(3u, old.lock()->segments().size());
    uint64_t coreGen = old.lock()->coreGen();
    EXPECT_EQ(expected, newest());  // reads each segment's leaf metadata
    const auto* meta =
        old.lock()->segments()[0].postingsReader().numericLeafMeta("time_i");
    ASSERT_NE(nullptr, meta);
    commit();
    auto current = writer->snapshots.readers.getReader();
    EXPECT_EQ(coreGen, current->coreGen());
    EXPECT_TRUE(old.expired());
    EXPECT_EQ(meta,
              current->segments()[0].postingsReader().numericLeafMeta("time_i"));
    current.reset();
    EXPECT_EQ(expected, newest());
  };
  reopen([&] {
    ASSERT_TRUE(helper.deleteById("1", UpdateMessage::COMMIT).success);
  });
  reopen([&] {
    helper.commit({std::string(TestOverlayAuxReader::NAME)});
    ASSERT_FALSE(readDurableIndexInfo(writer->dir)->segments[0]
                     .overlays.empty());
  });
}

TEST_F(SortCollectorTest, keyGatherTieBreaksAcrossReversedSegments) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.index(
      flatdoc("id", "early", "key_i", 10), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(
      flatdoc("id", "late", "key_i", 10), UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();

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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();

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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();

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

    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();

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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();

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
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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
      int64_t exactDomainRuns = 0;
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
    result.blocksSkipped = SkipStats::fieldSortBlocksSkipped
        + SkipStats::fieldSortLeavesSkipped;
    result.bulkCollections = SkipStats::fieldSortBulkCollections;
    result.exactDomainRuns = SkipStats::fieldSortBestFirstActivations
        + SkipStats::fieldSortDocOrderSweeps + SkipStats::fieldSortDomainScans;
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

  // The walk's leaf descent is priced (descendsToLeaves); both arms must
  // match exhaustive collection whichever the pricing picks here.
  for (std::optional<bool> descend :
       {std::optional<bool>{}, std::optional<bool>{false},
        std::optional<bool>{true}}) {
    LeafDescentGuard descentGuard(descend);
    for (bool matchAll : {true, false}) {
      for (const Sorts& sorts :
           {randAsc, randDesc, tiesAsc, tiesThenRand, monoAsc, revDesc}) {
        for (int32_t limit : {1, 9, 987, 4500}) {
          for (bool forcePull : {false, true}) {
            auto exhaustive =
                run(true, forcePull, matchAll, sorts, limit, false);
            auto pruned = run(false, forcePull, matchAll, sorts, limit, false);
            EXPECT_EQ(exhaustive.ids, pruned.ids)
                << "matchAll=" << matchAll << " limit=" << limit
                << " forcePull=" << forcePull
                << " sort=" << sorts.clauses[0].first
                << " descend=" << (descend ? (int)*descend : -1);
          }
        }
      }
    }
  }

  // Shallow sole-clause sorts must actually skip blocks or leaves. The
  // random column cannot skip strictly at this corpus size (the
  // k-th-smallest bottom sits above a 4096-value block's expected min until
  // tens of thousands of docs have been seen), so the deterministic
  // assertions use doc-order-monotonic columns in each direction plus the
  // segdoc-guarded equality rule on the tie column.
  EXPECT_GT(run(false, true, true, monoAsc, 9, false).blocksSkipped, 0);
  EXPECT_GT(run(false, true, true, revDesc, 9, false).blocksSkipped, 0);
  EXPECT_GT(run(false, false, false, monoAsc, 9, false).blocksSkipped, 0);
  // Match-all is a materialized domain (every doc, or the live docs), so
  // the exact-domain driver serves it with its zone skips; a term query
  // rides the bulk scorer's match windows, and forcing pull keeps the
  // scorer loop.
  {
    auto matchAll = run(false, false, true, monoAsc, 9, false);
    EXPECT_GT(matchAll.exactDomainRuns, 0);
    EXPECT_EQ(matchAll.bulkCollections, 0);
    EXPECT_GT(matchAll.blocksSkipped, 0);
    auto termBulk = run(false, false, false, monoAsc, 9, false);
    EXPECT_GT(termBulk.bulkCollections, 0);
    EXPECT_EQ(termBulk.exactDomainRuns, 0);
    EXPECT_EQ(run(false, true, false, monoAsc, 9, false).bulkCollections, 0);
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

// The bound-ordered drivers walk KeyBatch::boundOrderLeaf as the leaf visit
// order. In both directions it must be a permutation of the leaves, carry
// each leaf's leafBestKey as its bound, and ascend by (bound, leaf). Keys
// rise by block with coarse noise, so neighboring leaves tie on min and max.
TEST_F(SortCollectorTest, boundOrderLeavesAscendByBoundThenLeaf) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 3 * 4096 + 300; doc++) {
    int64_t key = (int64_t)(((uint32_t)doc * 2654435761u) % 1000) / 100
        + doc / 2048;
    docs.push_back(flatdoc("id", std::to_string(doc), "key_i", key));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  for (bool reversed : {false, true}) {
    for (auto& segment : reader->segments()) {
      SimpleNumericFieldComparator comparator(
          "key_i", reversed, FieldComparator::MISSING_LAST);
      comparator.setSegment(segment.ord, &segment.postingsReader());
      FieldComparator::KeyBatch* batch = comparator.keyBatch();
      ASSERT_NE(0, batch->leafBlockSize());
      int64_t leaves = batch->leafBlockCount();
      std::vector<bool> seen((size_t)leaves);
      std::pair<int64_t, int64_t> previous{
          std::numeric_limits<int64_t>::min(), -1};
      for (int64_t rank = 0; rank < leaves; rank++) {
        auto [leaf, bound] = batch->boundOrderLeaf(rank);
        ASSERT_TRUE(leaf >= 0 && leaf < leaves);
        EXPECT_FALSE(seen[(size_t)leaf]) << "leaf=" << leaf;
        seen[(size_t)leaf] = true;
        EXPECT_EQ(batch->leafBestKey(leaf), bound) << "leaf=" << leaf;
        std::pair<int64_t, int64_t> current{bound, leaf};
        EXPECT_LT(previous, current) << "reversed=" << reversed
                                     << " rank=" << rank;
        previous = current;
      }
    }
  }
}

// SegmentKeys::keyFloor maps a value envelope to the best-side edge in the
// comparator's key space (lo ascending, ~hi descending, FLOAT as sortable
// bits), every doc inside the envelope gathers a key at or past it, and a
// multi-valued column offers no keys. The collector's leavesBeforeKeyFloor
// counts exactly the leaves whose bound lies before that floor, and a
// segment with every leaf clipped is one competitive range.
TEST_F(SortCollectorTest, keyFloorClipsValueEnvelopes) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 3 * 4096 + 300; doc++) {
    int64_t key = (int64_t)(((uint32_t)doc * 2654435761u) % 100000) - 50000;
    docs.push_back(flatdoc("id", std::to_string(doc), "key_i", key,
                           "p_f", (double)key / 8.0, "mono_i", (int64_t)doc,
                           "m_is", vec_i(key, key + 1)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments()[0];

  auto floorOf = [&](std::string_view field, bool reversed,
                     Query::ValueEnvelope values) {
    SimpleNumericFieldComparator comparator(
        std::string(field), reversed, FieldComparator::MISSING_LAST);
    comparator.setSegment(segment.ord, &segment.postingsReader());
    auto keys = comparator.keyBatch()->segmentKeys(values);
    return keys.has_value() ? std::optional<int64_t>(keys->keyFloor())
                            : std::nullopt;
  };
  EXPECT_EQ(std::optional<int64_t>(-100), floorOf("key_i", false, {-100, 700}));
  EXPECT_EQ(std::optional<int64_t>(~(int64_t)700),
            floorOf("key_i", true, {-100, 700}));
  Query::ValueEnvelope prices{(int64_t)floatToSortableInt32(-12.5f),
                              (int64_t)floatToSortableInt32(87.5f)};
  EXPECT_EQ(std::optional<int64_t>(prices.lo), floorOf("p_f", false, prices));
  EXPECT_EQ(std::optional<int64_t>(~prices.hi), floorOf("p_f", true, prices));
  EXPECT_FALSE(floorOf("m_is", false, {-100, 700}).has_value());
  EXPECT_FALSE(floorOf("absent_i", false, {-100, 700}).has_value());

  // Uniform keys clip every leaf; doc-ordered keys clip only the leaves
  // past the envelope's best edge (docs 0-4999 ascending, 9001+ descending).
  struct Case {
    std::string_view field;
    Query::ValueEnvelope values;
  };
  for (Case c : {Case{"key_i", {-20000, 9000}}, Case{"p_f", prices},
                 Case{"mono_i", {5000, 9000}}}) {
    for (bool reversed : {false, true}) {
      std::string_view field = c.field;
      Query::ValueEnvelope values = c.values;
      bool isFloat = field == "p_f";
      FloatFieldType floatType(field);
      IntFieldType intType(field);
      SortField sortField(field,
                          isFloat ? (FieldType&)floatType : (FieldType&)intType,
                          reversed ? SortField::DESC : SortField::ASC);
      FieldSortCollector collector(10, columnPlan(sortField));
      collector.primaryValues = values;
      collector.setSegment(segment.ord, &segment.postingsReader());
      auto* batch = collector.clauses[0].comparator->keyBatch();
      ASSERT_NE(0, batch->leafBlockSize());
      int64_t floor = collector.primaryKeys->keyFloor();
      int64_t clipped = 0;
      for (int64_t leaf = 0; leaf < batch->leafBlockCount(); leaf++) {
        clipped += batch->leafBestKey(leaf) < floor;
      }
      EXPECT_EQ(clipped, collector.leavesBeforeKeyFloor)
          << field << " reversed=" << reversed;
      int64_t leaves = batch->leafBlockCount();
      int64_t expected = field != "mono_i" ? leaves
          : reversed ? leaves - 9001 / 512 : (5000 + 511) / 512;
      EXPECT_EQ(expected, clipped) << field << " reversed=" << reversed;
      // A fully clipped segment is one competitive range; otherwise the
      // empty heap warms up block by block.
      EXPECT_EQ(clipped == leaves ? PostingsReader::END : 4096,
                collector.nextCompetitiveRange(segment.ord, 0).end)
          << field << " reversed=" << reversed;
      // Every in-envelope doc's key is at or past the floor.
      std::vector<int32_t> all(segment.maxDoc());
      std::iota(all.begin(), all.end(), 0);
      std::vector<int64_t> keys(all.size());
      batch->gatherKeys(all, keys);
      SimpleNumericFieldComparator raw(std::string(field), false,
                                       FieldComparator::MISSING_LAST);
      raw.setSegment(segment.ord, &segment.postingsReader());
      std::vector<int64_t> stored(all.size());
      raw.keyBatch()->gatherKeys(all, stored);
      for (size_t doc = 0; doc < all.size(); doc++) {
        if (stored[doc] < values.lo || stored[doc] > values.hi) continue;
        ASSERT_GE(keys[doc], floor) << field << " doc=" << doc;
      }
    }
  }
  // Without an envelope nothing is clipped.
  IntFieldType intType("key_i");
  SortField sortField("key_i", intType, SortField::DESC);
  FieldSortCollector collector(10, columnPlan(sortField));
  collector.setSegment(segment.ord, &segment.postingsReader());
  EXPECT_EQ(0, collector.leavesBeforeKeyFloor);
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
  // The driver off (the doc-order ladder), on by cost, or forced into
  // bound order.
  enum class Route { LADDER, COST, FORCED };
  using enum Route;
  auto run = [&](Route route, bool disablePruning, bool foldFilters,
                 const Sorts& sorts, int32_t limit, bool exactCount,
                 std::string_view field = "body_w",
                 std::string_view value = "alpha") {
    BestFirstGuard bfGuard(route == LADDER, route == FORCED);
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
      int64_t walks = 0;
    } result;
    result.ids = resultIds(*req);
    const auto* docs = req->docList("q");
    if (docs != nullptr && docs->found) result.found = *docs->found;
    result.activations = SkipStats::fieldSortBestFirstActivations;
    result.walks = SkipStats::fieldSortDocOrderSweeps
        + SkipStats::fieldSortDomainScans;
    return result;
  };

  Sorts randAsc{{{"rand_i", qb::ASC}}};
  Sorts randDesc{{{"rand_i", qb::DESC}}};
  Sorts tiesAsc{{{"ties_i", qb::ASC}}};
  Sorts tiesThenRand{{{"ties_i", qb::ASC}, {"rand_i", qb::ASC}}};
  Sorts monoAsc{{{"mono_i", qb::ASC}}};

  // Two sightings admit and materialize the filter; the route serves hits.
  run(LADDER, true, true, randAsc, 9, false);
  run(LADDER, true, true, randAsc, 9, false);

  for (const Sorts& sorts :
       {randAsc, randDesc, tiesAsc, tiesThenRand, monoAsc}) {
    for (int32_t limit : {1, 9, 987, 4500}) {
      auto exhaustive = run(LADDER, true, true, sorts, limit, false);
      for (Route route : {LADDER, COST, FORCED}) {
        EXPECT_EQ(exhaustive.ids,
                  run(route, false, true, sorts, limit, false).ids)
            << "limit=" << limit << " sort=" << sorts.clauses[0].first
            << " route=" << (int)route;
      }
      // Pure match-all: no filter at all, the empty-mask domain form.
      auto allExhaustive = run(LADDER, true, true, sorts, limit, false, "");
      for (Route route : {COST, FORCED}) {
        EXPECT_EQ(allExhaustive.ids,
                  run(route, false, true, sorts, limit, false, "").ids)
            << "match-all limit=" << limit
            << " sort=" << sorts.clauses[0].first << " route=" << (int)route;
      }
    }
  }

  // A tiny forced work cap crosses to the forward-sweep fallback on every
  // shape; parity must hold and the fallback counter must fire.
  {
    WorkCapGuard capGuard(2);
    SortSkipStatsGuard statsGuard;
    for (const Sorts& sorts : {randAsc, randDesc, tiesAsc, monoAsc}) {
      for (int32_t limit : {9, 987}) {
        auto exhaustive = run(LADDER, true, true, sorts, limit, false);
        auto capped = run(FORCED, false, true, sorts, limit, false);
        EXPECT_EQ(exhaustive.ids, capped.ids)
            << "capped limit=" << limit << " sort=" << sorts.clauses[0].first;
        auto allExhaustive = run(LADDER, true, true, sorts, limit, false, "");
        auto allCapped = run(FORCED, false, true, sorts, limit, false, "");
        EXPECT_EQ(allExhaustive.ids, allCapped.ids)
            << "capped match-all limit=" << limit
            << " sort=" << sorts.clauses[0].first;
      }
    }
    EXPECT_GT(SkipStats::fieldSortBestFirstFallbacks, 0);
  }

  // Route engagement and gates, proven by the activation counter.
  EXPECT_GT(run(FORCED, false, true, randAsc, 9, false).activations, 0);
  // Pure match-all with no deletes rides the empty-mask domain form.
  EXPECT_GT(run(FORCED, false, true, randAsc, 9, false, "").activations, 0);
  // Unforced, the costs serve this domain without bound order: the
  // predicted floor saturates the first segment's ten leaves, so the walk
  // takes it. The second segment's keys are missing on some docs, so its
  // column offers no masked leaf gather and the ladder serves it.
  {
    auto cost = run(COST, false, true, randAsc, 9, false);
    EXPECT_EQ(0, cost.activations);
    EXPECT_EQ(1, cost.walks);
  }
  // Secondary clause: no sole column, no masked plan.
  EXPECT_EQ(run(FORCED, false, true, tiesThenRand, 9, false).activations, 0);
  // Exact count keeps every pruning route off and stays exact.
  {
    auto exact = run(FORCED, false, true, randAsc, 9, true);
    auto exactExh = run(LADDER, true, true, randAsc, 9, true);
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
    run(FORCED, false, true, randAsc, 9, false, field, value);
    run(FORCED, false, true, randAsc, 9, false, field, value);
    for (int32_t limit : {1, 9, 987}) {
      auto arrBestFirst =
          run(FORCED, false, true, randAsc, limit, false, field, value);
      auto arrExhaustive =
          run(LADDER, true, true, randAsc, limit, false, field, value);
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
  auto deletedExhaustive = run(LADDER, true, true, randAsc, 987, false);
  auto deletedFolded = run(FORCED, false, true, randAsc, 987, false);
  EXPECT_EQ(deletedFolded.activations, 0);
  EXPECT_EQ(deletedExhaustive.ids, deletedFolded.ids);
  auto deletedUnfolded = run(FORCED, false, false, randAsc, 987, false);
  EXPECT_GT(deletedUnfolded.activations, 0);
  EXPECT_EQ(deletedExhaustive.ids, deletedUnfolded.ids);
  // Pure match-all with deletes: liveDocs becomes the domain bitset per the
  // root domain contract, so the route still activates and stays correct.
  auto deletedAllExhaustive = run(LADDER, true, true, randAsc, 987, false, "");
  auto deletedAllBestFirst = run(FORCED, false, true, randAsc, 987, false, "");
  EXPECT_GT(deletedAllBestFirst.activations, 0);
  EXPECT_EQ(deletedAllExhaustive.ids, deletedAllBestFirst.ids);
  // Array domain with deletes (doc 1000 is an arr_s member): the folded raw
  // borrow declines, the unfolded effective set stays an array and correct.
  auto deletedArrExhaustive =
      run(LADDER, true, true, randAsc, 987, false, "arr_s", "y");
  auto deletedArrUnfolded =
      run(FORCED, false, false, randAsc, 987, false, "arr_s", "y");
  EXPECT_GT(deletedArrUnfolded.activations, 0);
  EXPECT_EQ(deletedArrExhaustive.ids, deletedArrUnfolded.ids);
}

// The exact-domain economics on the 5M benchgame geometry (9829 leaves):
// the plan over a whole segment, bound order's reading at a checkpoint, and
// the one re-decision rule over the rest.
TEST_F(SortCollectorTest, exactDomainSortCostsDecide) {
  using Costs = ExactDomainSortCosts;
  using Phase = ExactDomainSortPhase;
  constexpr int64_t kLeaves = 9829;
  auto planWithin = [&](int64_t card, int64_t floor, int64_t admitted,
                        bool unskippable) {
    return Costs::plan({card, kLeaves, floor, admitted, 0, kLeaves, 0},
                       unskippable);
  };
  auto plan = [&](int64_t card, int64_t floor) {
    return planWithin(card, floor, kLeaves, false);
  };
  // Every leaf before the key floor: nothing can be skipped, so scan.
  EXPECT_EQ(Phase::SCAN, planWithin(2000000, 1, kLeaves, true));
  // Saturated: a sparse domain is scanned, its few docs per leaf not worth
  // classifying.
  EXPECT_EQ(Phase::SCAN, plan(20000, kLeaves));
  // A small floor on a dense domain goes to bound order; on a domain too
  // sparse for a visit to repay itself, to the scan.
  EXPECT_EQ(Phase::BOUND_ORDER, plan(1000000, 50));
  EXPECT_EQ(Phase::SCAN, plan(5000, 2000));
  // A bottom admitting few leaves bounds bound order's visits whatever the
  // prior; one admitting none leaves it nothing to visit.
  EXPECT_EQ(Phase::BOUND_ORDER, planWithin(100000, kLeaves, 30, false));
  EXPECT_EQ(Phase::BOUND_ORDER, planWithin(20000, kLeaves, 0, false));

  // Bound order's reading: among the empty leaves that lead its order, or
  // with the heap still filling, no proof can be expected, and the prior
  // stands; past them a full heap without one leaves only the admitted
  // leaves to bound it; proofs, read twice over, project
  // (walked - empty)*k/(2p) past the empty leaves.
  auto read = [&](int64_t prior, int64_t walked, int64_t proofs,
                  int64_t admitted, int64_t empty, bool full) {
    return Costs::readBoundOrder(prior, walked, proofs, 10, admitted, empty,
                                 full);
  };
  EXPECT_EQ(400, read(400, 100, 0, 5000, 300, true));
  EXPECT_EQ(400, read(400, 160, 0, 5000, 0, false));
  EXPECT_EQ(5000, read(400, 160, 0, 5000, 0, true));
  EXPECT_EQ(125, read(400, 100, 4, 5000, 0, true));
  EXPECT_EQ(425, read(400, 400, 4, 5000, 300, true));
  EXPECT_EQ(900, read(400, 400, 1, 900, 0, true));
  EXPECT_EQ(16, Costs::firstCheckpoint(2, 10, 0));
  EXPECT_EQ(2014, Costs::firstCheckpoint(2517, 10, 0));
  EXPECT_EQ(8847, Costs::firstCheckpoint(8847, 10, 8843));

  // The re-decision: the incumbent keeps the rest unless the cheapest phase
  // costs less - by kProgressMargin for the walk. A dense domain bound order
  // walks without proofs prices every admitted leaf and hands the rest to
  // the walk.
  Costs::Rest dense{2000000, kLeaves - 600, kLeaves, kLeaves, 0,
                    kLeaves - 600, 0};
  auto denseCosts = Costs::costs(dense);
  EXPECT_NE(Phase::BOUND_ORDER, denseCosts.cheapest());
  EXPECT_NE(Phase::BOUND_ORDER,
            denseCosts.redecide(Phase::BOUND_ORDER, 1));
  // A walk whose bottom admits few leaves hands the rest to bound order.
  Costs::Rest tight{500000, 6000, 20, 20, 0, 6000, 0};
  EXPECT_EQ(Phase::BOUND_ORDER,
            Costs::costs(tight).redecide(Phase::SCAN, Costs::kProgressMargin));
  EXPECT_EQ(Phase::BOUND_ORDER,
            Costs::costs(tight).redecide(Phase::SWEEP,
                                         Costs::kProgressMargin));
  // A loose bottom keeps the walk.
  Costs::Rest loose{500000, 6000, 6000, 6000, 0, 6000, 0};
  EXPECT_EQ(Phase::SCAN,
            Costs::costs(loose).redecide(Phase::SCAN, Costs::kProgressMargin));

  // The oldest tenth of a time-ordered segment sorted newest first: bound
  // order's floor covers the 8843 leaves past the range, all empty, and the
  // four holding its newest docs, while the walk would gather the whole
  // range (its lead) before meeting them - bound order. The same domain met
  // at its start (no lead) still walks nothing but its first leaves.
  Costs::Rest window{418682, kLeaves, 8847, kLeaves, 8843, 986, 982};
  auto windowCosts = Costs::costs(window);
  EXPECT_EQ(Phase::BOUND_ORDER, windowCosts.cheapest());
  EXPECT_GT(windowCosts.sweep, 10 * windowCosts.boundOrder);
  window.lead = 0;
  EXPECT_LT(Costs::costs(window).sweep, windowCosts.sweep / 10);

  // A small domain sorted newest first at k = 1000 on a time-ordered
  // segment: a walk admits every doc of its lead, each a sift through the
  // heap, so bound order wins over 6000 of the 9829 leaves; on uniform keys
  // the walk admits a few thousand and is the cheaper.
  Costs::Rest late{14000, kLeaves, 6000, kLeaves, 0, kLeaves, 9765, 1000};
  EXPECT_EQ(Phase::BOUND_ORDER, Costs::costs(late).cheapest());
  late.lead = 0;
  EXPECT_NE(Phase::BOUND_ORDER, Costs::costs(late).cheapest());

  // The doc-order walk's leaf descent (walkDescends) on a 10M-doc segment
  // (19532 leaves) under uniform keys at k = 10: a 0.1% domain's bottom
  // admits 40% of the leaves with half a domain doc in each, so the 2.3k
  // docs the skipped leaves hold cannot pay for classifying every leaf; at
  // 1% it admits 5% and skips 80k docs. The boundary falls near 1.4 docs a
  // leaf: 0.24% stays off, 0.3% descends.
  constexpr int64_t kGridLeaves = 19532;
  auto descends = [&](int64_t card, int64_t floor) {
    return Costs::walkDescends({card, kGridLeaves, floor, kGridLeaves, 0,
                                kGridLeaves, 0, 10});
  };
  EXPECT_FALSE(descends(10000, 7829));
  EXPECT_TRUE(descends(100000, 975));
  EXPECT_FALSE(descends(24000, 3752));
  EXPECT_TRUE(descends(30000, 3065));
  // Best keys last in doc order (a lead over every inside leaf) leave the
  // walk nothing to skip, however dense the domain.
  EXPECT_FALSE(Costs::walkDescends({100000, kGridLeaves, 975, kGridLeaves, 0,
                                    kGridLeaves, kGridLeaves, 10}));
}

// The walk prices leaf descent on the docs it expects to collect
// (TopDocsReq::walkMembershipWork), not on what its source costs. One
// 256-leaf segment under uniform keys at k = 1: a 10% term alone admits few
// leaves and holds 51 docs in each, so the walk descends; ANDed with an
// independent 2.5% term its lead costs the same, but it collects 0.25% - 1.3
// docs a leaf over the 80% of leaves the bottom admits - and does not.
TEST_F(SortCollectorTest, walkDescentPricesTheCollectedDomain) {
  WholeMembershipPlanGuard wholeGuard(true);
  SeededGuard seededGuard(true, false);
  CollectionHelper helper("walk_descent");
  constexpr int32_t kDocs = 256 * 512;
  for (int32_t docId = 0; docId < kDocs; docId++) {
    SplitMix64 r((uint64_t)docId);
    helper.index(flatdoc("id", std::to_string(docId),
                         "id_s", std::to_string(docId),
                         "rnd_i", r.rint(1 << 30),
                         "lead_s", r.rint(10) == 0 ? "y" : "n",
                         "rare_s", r.rint(40) == 0 ? "y" : "n"),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();

  auto leavesSkipped = [&](bool conjunction) {
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("walk_descent");
    auto& cur = req->topDocs("q").limit(1).fields({"id_s"});
    auto& mr = cur.mr();
    cur.rawQuery() = conjunction
        ? qb::boolean(mr, {qb::match(mr, "lead_s", "y"),
                           qb::match(mr, "rare_s", "y")})
        : qb::match(mr, "lead_s", "y");
    qb::sort(cur, "rnd_i", qb::ASC);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return SkipStats::fieldSortLeavesSkipped;
  };
  EXPECT_GT(leavesSkipped(false), 0);
  EXPECT_EQ(0, leavesSkipped(true));
}

// Every exact-domain phase over every representation matches unpruned
// collection of the same docs. One segment of 24 full leaves and a 300-doc
// last leaf (four coarse blocks, the last partial); one key column ties
// heavily, the other ascends with the doc. The domains walk the forward
// cursor through runs of empty leaves, docs on both sides of leaf and
// coarse-block boundaries, the last partial leaf alone, a lone last doc, and
// every doc (also as the empty every-doc bitset). Each phase runs pure; bound
// order also runs capped at two gathers, so the walk resumes past its
// visited leaves; and each runs with its checkpoints on a prior far from the
// truth (a one-leaf floor under a walk, every leaf under bound order), so
// re-decisions hand the rest between bound order and the walk's levels.
TEST_F(SortCollectorTest, exactDomainPhasesMatchExhaustive) {
  CollectionHelper helper;
  constexpr int32_t kDocs = 24 * 512 + 300;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < kDocs; doc++) {
    int64_t key = (int64_t)(((uint32_t)doc * 2654435761u) % 1000);
    docs.push_back(flatdoc("id", std::to_string(doc), "key_i", key,
                           "mono_i", (int64_t)doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  ASSERT_EQ(kDocs, segment.maxDoc());

  auto leafDocs = [&](std::initializer_list<int32_t> leaves) {
    std::vector<int32_t> out;
    for (int32_t leaf : leaves) {
      for (int32_t doc = leaf * 512; doc < std::min(leaf * 512 + 512, kDocs);
           doc++) {
        out.push_back(doc);
      }
    }
    return out;
  };
  std::vector<int32_t> every(kDocs);
  std::iota(every.begin(), every.end(), 0);
  std::vector<int32_t> sparse;
  for (int32_t doc = 5; doc < kDocs; doc += 37) sparse.push_back(doc);
  std::vector<std::vector<int32_t>> domains = {
      {0, 1, 511, 512, 1023, 1024, 4095, 4096, 4097, 8191, 8192, 12287,
       12288, kDocs - 1},
      leafDocs({24}),
      leafDocs({0, 7, 8, 23}),
      {kDocs - 1},
      sparse,
      every,
  };

  // rep: 0 array, 1 bitset, 2 the empty every-doc bitset.
  using Phase = ExactDomainSortPhase;
  auto collect = [&](const SortField& sortField,
                     const std::vector<int32_t>& domain, int rep,
                     int64_t topCount, std::optional<Phase> phase,
                     int64_t cap, bool checkProgress, int64_t floor) {
    MemPool pool;
    FieldSortCollector collector(topCount, columnPlan(sortField));
    collector.setSegment(0, &segment.postingsReader(), &pool,
                         (int64_t)domain.size());
    if (!phase.has_value()) {
      collector.collectWindow(0, domain);
    } else {
      ExactDomainSortRoute route;
      route.card = (int64_t)domain.size();
      route.floor = floor;
      route.phase = *phase;
      route.checkProgress = checkProgress;
      route.gatherCapForTests = cap;
      std::vector<uint64_t> words((size_t)(kDocs + 63) / 64);
      for (int32_t doc : domain) words[(size_t)doc >> 6] |= 1ULL << (doc & 63);
      if (rep == 0) {
        collectTopKArrayDomain(0, domain, collector, pool, route);
      } else {
        collectTopKBitSetDomain(
            0, rep == 1 ? std::span<const uint64_t>(words)
                        : std::span<const uint64_t>(),
            collector, pool, route);
      }
    }
    std::vector<segdoc> docs;
    for (const auto& doc : collector.sort()) docs.push_back(doc.doc);
    return docs;
  };
  for (std::string_view field : {"key_i", "mono_i"}) {
    IntFieldType fieldType(field);
    for (auto order : {SortField::ASC, SortField::DESC}) {
      SortField sortField(field, fieldType, order,
                          FieldComparator::MISSING_LAST);
      for (const auto& domain : domains) {
        for (int64_t topCount : {int64_t(1), int64_t(7), int64_t(100)}) {
          auto expected = collect(sortField, domain, 0, topCount,
                                  std::nullopt, 0, false, 0);
          for (int rep = 0; rep < 3; rep++) {
            if (rep == 2 && domain.size() != every.size()) continue;
            // {phase, cap, checkpoints, prior floor}
            for (auto [phase, cap, check, floor] :
                 {std::tuple{Phase::BOUND_ORDER, int64_t(0), false, int64_t(1)},
                  std::tuple{Phase::BOUND_ORDER, int64_t(2), false, int64_t(1)},
                  std::tuple{Phase::SCAN, int64_t(0), false, int64_t(1)},
                  std::tuple{Phase::SWEEP, int64_t(0), false, int64_t(1)},
                  std::tuple{Phase::BOUND_ORDER, int64_t(0), true, int64_t(25)},
                  std::tuple{Phase::SCAN, int64_t(0), true, int64_t(1)},
                  std::tuple{Phase::SWEEP, int64_t(0), true, int64_t(1)}}) {
              EXPECT_EQ(expected, collect(sortField, domain, rep, topCount,
                                          phase, cap, check, floor))
                  << field << " order=" << order << " card=" << domain.size()
                  << " k=" << topCount << " phase=" << (int)phase
                  << " cap=" << cap << " check=" << check << " rep=" << rep;
            }
          }
        }
      }
    }
  }

  // leavesWithinBottom: every leaf until the heap fills, then exactly the
  // leaves bounded at or before the bottom's key.
  IntFieldType monoType("mono_i");
  for (auto order : {SortField::ASC, SortField::DESC}) {
    SortField sortField("mono_i", monoType, order,
                        FieldComparator::MISSING_LAST);
    MemPool pool;
    FieldSortCollector collector(7, columnPlan(sortField));
    collector.setSegment(0, &segment.postingsReader(), &pool, kDocs);
    auto* batch = collector.maskedKeyBlockPlan().batch;
    ASSERT_NE(nullptr, batch);
    collector.collectWindow(0, std::span(sparse).first(3));
    EXPECT_EQ(batch->leafBlockCount(), collector.leavesWithinBottom());
    collector.collectWindow(0, std::span(sparse).subspan(3));
    std::vector<int64_t> keys;
    for (int32_t doc : sparse) {
      keys.push_back(order == SortField::DESC ? ~(int64_t)doc : doc);
    }
    std::sort(keys.begin(), keys.end());
    int64_t within = 0;
    for (int64_t leaf = 0; leaf < batch->leafBlockCount(); leaf++) {
      within += batch->leafBestKey(leaf) <= keys[6];
    }
    EXPECT_GT(within, 0);
    EXPECT_LT(within, batch->leafBlockCount());
    EXPECT_EQ(within, collector.leavesWithinBottom()) << order;
  }
}

// The exact-domain driver picks its phases by cost (ExactDomainSortCosts).
// One 256-leaf segment, k = 1: a small ARRAY filter (4 docs per leaf) at half
// the leaves' floor is scanned, its few docs per leaf not worth a visit or a
// classification; a half-corpus BITSET filter proves in bound order; a
// filter of only the lowest keys, sorted descending, makes every leaf bound
// beat every member, so bound order starts on the uniform-key floor, makes
// no proof by its first checkpoint, and hands the rest to the scan. At
// k = 300 both filters saturate the floor and are scanned. A filter of only
// the lowest keys, sorted ascending, beats the prior, which plans a scan:
// the scan's first checkpoint hands the rest to bound order, which proves.
// Keys in doc order (ascending) are tight zones the value sketch reads, so
// the predicted bottom plans bound order, which proves after the leaves
// holding the first matches. Every route matches exhaustive collection, the
// ladder, and forced bound order.
TEST_F(SortCollectorTest, bestFirstRouteFollowsCostModel) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper("best_first_cost");
  constexpr int32_t kDocs = 256 * 512;
  constexpr int64_t kLowKeys = 0x7fffffff / 4;
  for (int32_t docId = 0; docId < kDocs; docId++) {
    int64_t key = (int64_t)((uint32_t)docId * 2654435761u) & 0x7fffffff;
    std::string body = docId % 2 == 0 ? "large" : "odd";
    if (docId % 127 == 0) body += " small";
    if (key < kLowKeys) body += " low";
    if (key < kLowKeys && docId % 16 == 0) body += " lowsparse";
    if (key < kLowKeys / 16 && docId % 2 == 0) body += " favored";
    helper.index(flatdoc("id", std::to_string(docId),
                         "id_s", std::to_string(docId), "body_w", body,
                         "key_i", key, "mono_i", (int64_t)docId),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();

  struct Result {
    std::vector<std::string> ids;
    int64_t sweeps = 0;
    int64_t boundOrder = 0;
    int64_t proofs = 0;
    int64_t handoffs = 0;
    int64_t scans = 0;
    int64_t walkHandoffs = 0;
    int64_t sweptLeaves = 0;
    int64_t gathered = 0;
    int64_t ladder = 0;
  };
  auto run = [&](std::string_view value, qb::SortDir dir, int32_t limit,
                 bool disableBestFirst = false, bool forceBestFirst = false,
                 bool disablePruning = false,
                 std::string_view field = "key_i") {
    BestFirstGuard bfGuard(disableBestFirst, forceBestFirst);
    SortPruningGuard pruningGuard(disablePruning);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("best_first_cost");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    cur.allQuery().matchFilter("body_w", value);
    qb::sort(cur, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{resultIds(*req), SkipStats::fieldSortDocOrderSweeps,
                  SkipStats::fieldSortBestFirstActivations,
                  SkipStats::fieldSortBestFirstTerminations,
                  SkipStats::fieldSortBestFirstFallbacks,
                  SkipStats::fieldSortDomainScans,
                  SkipStats::fieldSortWalkHandOffs,
                  SkipStats::fieldSortSweepLeaves,
                  SkipStats::fieldSortDocsGathered,
                  SkipStats::fieldSortBulkCollections};
  };
  for (std::string_view value :
       {"small", "large", "low", "lowsparse", "favored"}) {
    run(value, qb::DESC, 1);  // two sightings admit the filter
    run(value, qb::DESC, 1);
  }

  auto small = run("small", qb::DESC, 1);
  EXPECT_EQ(1, small.scans);
  EXPECT_EQ(0, small.boundOrder + small.sweeps + small.walkHandoffs);
  auto large = run("large", qb::DESC, 1);
  EXPECT_EQ(0, large.sweeps + large.scans);
  EXPECT_EQ(1, large.boundOrder);
  EXPECT_EQ(1, large.proofs);
  EXPECT_EQ(0, large.handoffs);
  auto low = run("low", qb::DESC, 1);
  EXPECT_EQ(1, low.boundOrder);
  EXPECT_EQ(0, low.proofs);
  EXPECT_EQ(1, low.handoffs);
  EXPECT_EQ(1, low.scans);
  // Ascending, the same filter holds the best keys and bound order proves.
  auto lowAsc = run("low", qb::ASC, 1);
  EXPECT_EQ(1, lowAsc.proofs);
  EXPECT_EQ(0, lowAsc.handoffs);
  {
    WorkCapGuard capGuard(2);
    auto lowSparse = run("lowsparse", qb::DESC, 1);
    EXPECT_EQ(1, lowSparse.boundOrder);
    EXPECT_EQ(1, lowSparse.handoffs);
    EXPECT_EQ(1, lowSparse.scans);
    EXPECT_EQ(0, lowSparse.sweptLeaves);
  }
  for (std::string_view value : {"small", "large"}) {
    auto saturated = run(value, qb::DESC, 300);
    EXPECT_EQ(0, saturated.ladder) << value;
    EXPECT_EQ(0, saturated.boundOrder) << value;
    EXPECT_EQ(1, saturated.scans) << value;
  }
  // The lowest keys only, ascending: the prior plans a scan, whose first
  // checkpoint finds a bottom admitting few leaves and hands the rest to
  // bound order, which proves at once.
  auto favored = run("favored", qb::ASC, 1);
  EXPECT_EQ(1, favored.scans);
  EXPECT_EQ(1, favored.walkHandoffs);
  EXPECT_EQ(1, favored.boundOrder);
  EXPECT_EQ(1, favored.proofs);
  EXPECT_LE(favored.gathered, 512);
  auto docOrdered = run("small", qb::ASC, 10, false, false, false, "mono_i");
  EXPECT_EQ(0, docOrdered.scans + docOrdered.handoffs);
  EXPECT_EQ(1, docOrdered.boundOrder);
  EXPECT_EQ(1, docOrdered.proofs);
  EXPECT_LT(docOrdered.gathered, 1032 * 3 / 4);

  for (std::string_view value :
       {"small", "large", "low", "lowsparse", "favored"}) {
    for (std::string_view field : {"key_i", "mono_i"}) {
      for (qb::SortDir dir : {qb::DESC, qb::ASC}) {
        for (int32_t limit : {1, 10, 300}) {
          auto exhaustive =
              run(value, dir, limit, false, false, true, field);
          EXPECT_EQ(exhaustive.ids,
                    run(value, dir, limit, false, false, false, field).ids)
              << value << " " << field << " limit=" << limit;
          EXPECT_EQ(exhaustive.ids,
                    run(value, dir, limit, true, false, false, field).ids)
              << value << " " << field << " limit=" << limit;
          EXPECT_EQ(exhaustive.ids,
                    run(value, dir, limit, false, true, false, field).ids)
              << value << " " << field << " limit=" << limit;
        }
      }
    }
  }
}

// The unforced cost route over three segments with deletes in each: 128, 64
// (small) and 96 leaves, the last leaf partial. Match-all serves each
// segment's liveDocs domain, an unfolded filter its live effective set. Keys:
// uniform; doc order across segments (time_i, both directions); three values
// tying across segments; and perm_i, which in the first segment gives each
// leaf one disjoint key range, the first 48 leaves mid-ranked, and in later
// segments only keys worse than all of those. Every route matches
// exhaustive collection, and the counters show what single-segment uniform
// data cannot. The predicted bottom (FieldSortCollector::predictBottom)
// reads each segment's key distribution, so every correlated shape below is
// planned as bound order from its first leaf, and segments are visited best
// first (TopDocsReq::fieldSortVisitOrder), so the first one visited fills
// the heap and each later one settles at its first bound:
// - a 2.5% ARRAY domain sorted by perm_i gathers one leaf, in the first
//   segment;
// - time_i ascending over a sparse domain (oldest first) gathers two leaves
//   of the oldest segment, descending over a dense one (newest first) one
//   leaf of the newest, and so does descending at k = 1 over every51;
// - a range on time_i clips half of the last segment's leaves, which bound
//   order walks before it proves;
// - ties across segments: the prior counts only the tied leaves the heap
//   still needs (the tie rule skips the rest in segdoc order), so bound
//   order gathers one leaf of the first segment and every other one is
//   skipped.
TEST_F(SortCollectorTest, exactDomainCostRouteAcrossSegments) {
  WholeMembershipPlanGuard wholeGuard(true);
  TopDocsFilterFoldGuard foldGuard(true);
  CollectionHelper helper("cost_route_segments");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr std::array<int32_t, 3> kSegDocs{128 * 512, 64 * 512,
                                            96 * 512 - 200};
  int32_t docId = 0;
  std::vector<std::string> deletes;
  for (int32_t seg = 0; seg < 3; seg++) {
    for (int32_t i = 0; i < kSegDocs[(size_t)seg]; i++, docId++) {
      int64_t leaf = i / 512;
      int64_t permLeaf = leaf < 48 ? 27 + leaf : leaf < 75 ? leaf - 48 : leaf;
      std::string body = docId % 2 == 0 ? "half" : "odd";
      if (docId % 40 == 0) body += " every40";
      if (docId % 51 == 0) body += " every51";
      if (docId % 101 == 0) body += " sparse";
      helper.index(
          flatdoc("id", std::to_string(docId), "id_s", std::to_string(docId),
                  "body_w", body,
                  "rnd_i",
                  (int64_t)((uint32_t)docId * 2654435761u) & 0x7fffffff,
                  "time_i", (int64_t)docId, "tie_i", (int64_t)(docId % 3),
                  "perm_i",
                  seg == 0 ? permLeaf * 512 + i % 512
                           : (int64_t)(1 << 20) + docId),
          UpdateMessage::NO_COMMIT);
      if (docId % 1009 == 7) deletes.push_back(std::to_string(docId));
    }
    helper.commit();
  }
  ASSERT_TRUE(helper.deleteByIds(deletes, UpdateMessage::COMMIT).success);
  ASSERT_EQ(3u, helper.getIndexWriter()
                    ->snapshots.readers.getReader()
                    ->segments()
                    .size());

  struct Result {
    std::vector<std::string> ids;
    int64_t boundOrder = 0;
    int64_t proofs = 0;
    int64_t fallbacks = 0;
    int64_t scans = 0;
    int64_t walkHandOffs = 0;
    int64_t sweptLeaves = 0;
    int64_t boundOrderLeaves = 0;
    int64_t leavesSkipped = 0;
  };
  using Range = std::optional<std::pair<int64_t, int64_t>>;
  // filter: a body_w term, or empty for none; range: on time_i.
  auto run = [&](std::string_view field, qb::SortDir dir, int32_t limit,
                 std::string_view filter, Range range = {},
                 bool exhaustive = false) {
    SortPruningGuard pruningGuard(exhaustive);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("cost_route_segments");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    cur.allQuery();
    if (!filter.empty()) cur.matchFilter("body_w", filter);
    if (range.has_value()) {
      cur.filter(qb::range(cur.mr(), "time_i",
                           qb::valI64(cur.mr(), range->first), nullptr,
                           qb::valI64(cur.mr(), range->second), nullptr));
    }
    qb::sort(cur, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{resultIds(*req),
                  SkipStats::fieldSortBestFirstActivations,
                  SkipStats::fieldSortBestFirstTerminations,
                  SkipStats::fieldSortBestFirstFallbacks,
                  SkipStats::fieldSortDomainScans,
                  SkipStats::fieldSortWalkHandOffs,
                  SkipStats::fieldSortSweepLeaves,
                  SkipStats::fieldSortBestFirstLeaves,
                  SkipStats::fieldSortLeavesSkipped};
  };
  // Half the last segment's leaves lie above the range's top, half of the
  // first segment's below its bottom.
  constexpr int64_t kSplit = 128 * 512 + 64 * 512 + 48 * 512;
  const std::array<Range, 2> ranges{Range{{0, kSplit - 1}},
                                    Range{{kSplit, 1 << 30}}};
  const std::array<std::string_view, 5> filters{"", "half", "every40",
                                                "every51", "sparse"};
  for (std::string_view filter : filters) {  // two sightings cache it
    run("rnd_i", qb::ASC, 10, filter);
    run("rnd_i", qb::ASC, 10, filter);
  }
  for (const Range& range : ranges) {
    run("rnd_i", qb::ASC, 10, "", range);
    run("rnd_i", qb::ASC, 10, "", range);
  }

  for (std::string_view field : {"rnd_i", "time_i", "tie_i", "perm_i"}) {
    for (qb::SortDir dir : {qb::ASC, qb::DESC}) {
      for (int32_t limit : {1, 10, 100, 1000}) {
        for (std::string_view filter : filters) {
          EXPECT_EQ(run(field, dir, limit, filter, {}, true).ids,
                    run(field, dir, limit, filter).ids)
              << field << " " << (int)dir << " limit=" << limit << " "
              << filter;
        }
        if (field != "time_i" && field != "rnd_i") continue;
        for (const Range& range : ranges) {
          EXPECT_EQ(run(field, dir, limit, "", range, true).ids,
                    run(field, dir, limit, "", range).ids)
              << field << " " << (int)dir << " limit=" << limit
              << " range " << range->first;
        }
      }
    }
  }

  for (auto [field, dir, limit, filter, leaves] :
       {std::tuple{"perm_i", qb::ASC, 10, "every40", 1},
        std::tuple{"time_i", qb::ASC, 10, "sparse", 2},
        std::tuple{"time_i", qb::DESC, 10, "half", 1},
        std::tuple{"time_i", qb::DESC, 1, "every51", 1}}) {
    auto result = run(field, dir, limit, filter);
    EXPECT_EQ(3, result.boundOrder) << field << " " << filter;
    EXPECT_EQ(3, result.proofs) << field << " " << filter;
    EXPECT_EQ(leaves, result.boundOrderLeaves) << field << " " << filter;
    EXPECT_EQ(0, result.fallbacks + result.scans
                     + result.sweptLeaves)
        << field << " " << filter;
  }

  auto clipped = run("time_i", qb::DESC, 10, "", ranges[0]);
  EXPECT_EQ(3, clipped.boundOrder);
  EXPECT_EQ(3, clipped.proofs);
  EXPECT_EQ(0, clipped.fallbacks);
  EXPECT_GE(clipped.boundOrderLeaves, 48);

  auto ties = run("tie_i", qb::ASC, 10, "");
  EXPECT_EQ(1, ties.boundOrderLeaves);
  EXPECT_EQ(0, ties.sweptLeaves + ties.scans);
}

// A range on the sort field itself puts every domain key at or past the
// range's best-side edge, so leaves bounded before it can never be proven
// out (k = 1, one 256-leaf segment, values over [0, 2^20)). Uniform keys
// under a DESC sort over the lower half clip every leaf, so nothing can be
// skipped: the exact-domain driver scans a BITSET domain and an ARRAY
// domain (a 1.5% range) whole. Doc-ordered keys over the same range clip
// only the upper half's leaves: bound order still visits them first, and
// its first progress checkpoint waits past them, so it proves instead of
// handing off. ASC over the same range clips nothing. The
// clip holds for the points-indexed and the column-only field, for a folded
// filter (the cached range set) and an unfolded one (a collector filter
// under match-all), and for a query's resident whole membership; every
// route matches exhaustive collection.
TEST_F(SortCollectorTest, rangeOnSortFieldClipsBoundOrder) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper("range_clip");
  SchemaBuilder schema;
  for (std::string_view name : {"rnd_pt", "mono_pt"}) {
    auto& field = schema.field(name);
    field.type = luxir::api::FieldDef::FieldClass::INT;
    field.index = luxir::api::FieldDef::IndexMode::RANGE;
  }
  schema.set(helper.collection());
  constexpr int32_t kDocs = 256 * 512;
  constexpr int64_t kHalf = 1 << 19;
  for (int32_t docId = 0; docId < kDocs; docId++) {
    int64_t rnd = (int64_t)(((uint32_t)docId * 2654435761u) >> 12);
    int64_t mono = (int64_t)docId * 8;
    helper.index(flatdoc("id", std::to_string(docId),
                         "id_s", std::to_string(docId),
                         "body_w", docId % 2 == 0 ? "even" : "odd",
                         "rnd_i", rnd, "rnd_pt", rnd,
                         "mono_i", mono, "mono_pt", mono),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();

  struct Result {
    std::vector<std::string> ids;
    int64_t sweeps = 0;
    int64_t boundOrder = 0;
    int64_t proofs = 0;
    int64_t handoffs = 0;
    int64_t scans = 0;
  };
  enum class Domain { FOLDED, UNFOLDED, WHOLE };
  auto run = [&](std::string_view field, qb::SortDir dir, int64_t hi,
                 int32_t limit, Domain domain = Domain::FOLDED,
                 bool disablePruning = false) {
    SortPruningGuard pruningGuard(disablePruning);
    TopDocsFilterFoldGuard foldGuard(domain == Domain::UNFOLDED);
    WholeMembershipPlanGuard whole(domain != Domain::WHOLE);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("range_clip");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    if (domain == Domain::WHOLE) {
      cur.matchQuery("body_w", "even");
    } else {
      cur.allQuery();
    }
    cur.filter(qb::range(cur.mr(), field, qb::valI64(cur.mr(), 0), nullptr,
                         qb::valI64(cur.mr(), hi - 1), nullptr));
    qb::sort(cur, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{resultIds(*req), SkipStats::fieldSortDocOrderSweeps,
                  SkipStats::fieldSortBestFirstActivations,
                  SkipStats::fieldSortBestFirstTerminations,
                  SkipStats::fieldSortBestFirstFallbacks,
                  SkipStats::fieldSortDomainScans};
  };
  constexpr int64_t kSparse = kHalf / 32;  // a 1.5% range: an ARRAY domain
  for (std::string_view field : {"rnd_i", "rnd_pt", "mono_i", "mono_pt"}) {
    for (int64_t hi : {kHalf, kSparse}) {
      run(field, qb::DESC, hi, 1);  // two sightings admit the filter
      run(field, qb::DESC, hi, 1);
    }
  }

  for (std::string_view field : {"rnd_i", "rnd_pt"}) {
    for (int64_t hi : {kHalf, kSparse}) {
      for (Domain domain : {Domain::FOLDED, Domain::UNFOLDED}) {
        auto clipped = run(field, qb::DESC, hi, 1, domain);
        EXPECT_EQ(1, clipped.scans) << field << " hi=" << hi;
        EXPECT_EQ(0, clipped.sweeps + clipped.boundOrder)
            << field << " hi=" << hi;
      }
    }
    auto ascending = run(field, qb::ASC, kHalf, 1);
    EXPECT_EQ(1, ascending.boundOrder) << field;
    EXPECT_EQ(1, ascending.proofs) << field;
  }
  for (std::string_view field : {"mono_i", "mono_pt"}) {
    auto correlated = run(field, qb::DESC, kHalf, 1);
    EXPECT_EQ(0, correlated.sweeps) << field;
    EXPECT_EQ(1, correlated.boundOrder) << field;
    EXPECT_EQ(1, correlated.proofs) << field;
    EXPECT_EQ(0, correlated.handoffs) << field;
  }
  // A fully clipped domain cannot bound the driver's visits, so the field
  // sort builds its membership only when forced to.
  {
    BestFirstGuard force(false, true);
    run("rnd_i", qb::DESC, kHalf, 1, Domain::WHOLE);  // two sightings
    run("rnd_i", qb::DESC, kHalf, 1, Domain::WHOLE);
  }
  auto resident = run("rnd_i", qb::DESC, kHalf, 1, Domain::WHOLE);
  EXPECT_EQ(1, resident.scans);
  EXPECT_EQ(0, resident.sweeps + resident.boundOrder);

  for (std::string_view field : {"rnd_i", "rnd_pt", "mono_i", "mono_pt"}) {
    for (int64_t hi : {kHalf, kSparse}) {
      for (qb::SortDir dir : {qb::DESC, qb::ASC}) {
        for (Domain domain : {Domain::FOLDED, Domain::UNFOLDED, Domain::WHOLE}) {
          for (int32_t limit : {1, 10, 300}) {
            auto exhaustive = run(field, dir, hi, limit, domain, true);
            EXPECT_EQ(exhaustive.ids, run(field, dir, hi, limit, domain).ids)
                << field << " hi=" << hi << " limit=" << limit
                << " domain=" << (int)domain;
          }
        }
      }
    }
  }
}

// SegmentKeys reads a segment's sort keys off the persisted value sketch:
// in both directions, with and without a value envelope, keyAt(f) is the key
// of the in-envelope value ranked f of the way from the best and
// fractionBefore inverts it, exactly in the sketch's exact tails and within
// a few percent between them; keyFloor is the envelope's best edge; the
// galloped leavesBoundedBefore counts the leaves a linear scan does; and
// docOrderTrend tells keys rising with doc order (near 1 sorted descending,
// -1 ascending) from keys in random order (near 0).
TEST_F(SortCollectorTest, segmentKeysEstimateKeysWithinEnvelopes) {
  CollectionHelper helper;
  constexpr int32_t kDocs = 96 * 512 + 100;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < kDocs; doc++) {
    docs.push_back(flatdoc(
        "id", std::to_string(doc),
        "key_i", (int64_t)(((uint32_t)doc * 2654435761u) % 1000003),
        "mono_i", (int64_t)doc * 3));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto& segment = reader->segments()[0];
  using Envelope = std::optional<Query::ValueEnvelope>;
  for (std::string_view field : {"key_i", "mono_i"}) {
    SimpleNumericFieldComparator raw(std::string(field), false,
                                     FieldComparator::MISSING_LAST);
    raw.setSegment(segment.ord, &segment.postingsReader());
    std::vector<int32_t> all(kDocs);
    std::iota(all.begin(), all.end(), 0);
    std::vector<int64_t> values(kDocs);
    raw.keyBatch()->gatherKeys(all, values);
    for (bool reversed : {false, true}) {
      for (Envelope envelope :
           {Envelope{}, Envelope{{40000, 250000}}, Envelope{{-5, 9}}}) {
        SCOPED_TRACE(std::string(field) + (reversed ? " desc" : " asc")
                     + (envelope ? " env " + std::to_string(envelope->lo)
                                 : " all"));
        SimpleNumericFieldComparator comparator(
            std::string(field), reversed, FieldComparator::MISSING_LAST);
        comparator.setSegment(segment.ord, &segment.postingsReader());
        auto keys = comparator.keyBatch()->segmentKeys(envelope);
        ASSERT_TRUE(keys.has_value());
        if (field == "mono_i") {
          EXPECT_GT(reversed ? keys->docOrderTrend() : -keys->docOrderTrend(),
                    0.9);
        } else {
          EXPECT_LT(std::abs(keys->docOrderTrend()), 0.5);
        }
        std::vector<int64_t> oracle;  // in-envelope keys, best first
        for (int64_t v : values) {
          if (envelope && (v < envelope->lo || v > envelope->hi)) continue;
          oracle.push_back(reversed ? ~v : v);
        }
        std::sort(oracle.begin(), oracle.end());
        double n = (double)oracle.size();
        EXPECT_NEAR(n, keys->values(), std::max(1.0, n / 25));
        int64_t lo = envelope ? envelope->lo
                              : std::numeric_limits<int64_t>::min();
        int64_t hi = envelope ? envelope->hi
                              : std::numeric_limits<int64_t>::max();
        EXPECT_EQ(reversed ? ~hi : lo, keys->keyFloor());
        if (oracle.empty()) continue;
        auto rankOf = [&](int64_t key) {
          return (double)(std::lower_bound(oracle.begin(), oracle.end(), key)
                          - oracle.begin());
        };
        // An envelope edge inside the column brings the sketch's spacing
        // there into every in-envelope rank.
        std::vector<int64_t> sortedValues = values;
        std::sort(sortedValues.begin(), sortedValues.end());
        auto spacingAt = [&](int64_t v) {
          double rank = (double)(std::lower_bound(sortedValues.begin(),
                                                  sortedValues.end(), v)
                                 - sortedValues.begin());
          double fromEnd = std::min(rank, (double)kDocs - rank);
          return fromEnd < NumColumnFormat::SKETCH_TAIL
              ? std::max(1.0, fromEnd / 32) : (double)kDocs / 25;
        };
        double edges = envelope
            ? spacingAt(envelope->lo) + spacingAt(envelope->hi) : 0;
        for (double f : {0.0, 0.0002, 0.001, 0.01, 0.1, 0.5, 0.9, 1.0}) {
          double rank = std::max(0.0, f * n - 1);
          double fromEnd = std::min(rank, n - rank);
          double tolerance = edges
              + (fromEnd < NumColumnFormat::SKETCH_TAIL
                     ? std::max(2.0, fromEnd / 16) : n / 20);
          int64_t key = keys->keyAt(f);
          EXPECT_NEAR(rank, rankOf(key), tolerance) << "f=" << f;
          int64_t truth = oracle[(size_t)rank];
          EXPECT_NEAR(rank / n, keys->fractionBefore(truth),
                      tolerance / n + 1e-9)
              << "f=" << f;
        }
        for (int64_t key : {oracle.front(), oracle[oracle.size() / 3],
                            oracle.back(), oracle.back() + 1}) {
          int64_t linear = 0;
          for (int64_t leaf = 0; leaf < keys->leafCount(); leaf++) {
            linear += keys->leafBestKey(leaf) < key;
          }
          EXPECT_EQ(linear, keys->leavesBoundedBefore(key)) << key;
        }
      }
    }
  }
}

// The predicted bottom (FieldSortCollector::predictBottom) as a heap moves
// across segments: from an empty heap, a segment's own k-th best; a full
// heap merged with a segment drawn alike moves to the k-th best of both,
// with one wholly better (newer keys, descending) to that segment's own
// k-th, and with one wholly worse not at all; a domain that cannot fill
// the heap predicts none. Two equal uniform segments and a time-ordered
// pair, one leaf each side of the boundary.
TEST_F(SortCollectorTest, predictedBottomFollowsHeapAcrossSegments) {
  CollectionHelper helper;
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr int32_t kSegDocs = 64 * 512;
  int32_t docId = 0;
  for (int32_t seg = 0; seg < 2; seg++) {
    std::vector<Doc> docs;
    for (int32_t i = 0; i < kSegDocs; i++, docId++) {
      docs.push_back(flatdoc(
          "id", std::to_string(docId),
          "key_i", (int64_t)(((uint32_t)docId * 2654435761u) % 1000003),
          "mono_i", (int64_t)docId));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(2u, reader->segments().size());
  auto keysOf = [&](std::string_view field, bool reversed, size_t seg) {
    SimpleNumericFieldComparator comparator(std::string(field), reversed,
                                            FieldComparator::MISSING_LAST);
    auto& segment = reader->segments()[seg];
    comparator.setSegment(segment.ord, &segment.postingsReader());
    return *comparator.keyBatch()->segmentKeys(std::nullopt);
  };
  constexpr int64_t k = 100;
  constexpr int64_t card = kSegDocs / 2;
  auto predict = [&](const FieldComparator::SegmentKeys& keys, int64_t heap,
                     int64_t bottom) {
    return FieldSortCollector::predictBottom(keys, card, k, heap, bottom);
  };

  // Uniform, alike: k / card, then k / (2 * card).
  auto a = keysOf("key_i", false, 0);
  auto b = keysOf("key_i", false, 1);
  auto first = predict(a, 0, 0);
  ASSERT_TRUE(first.has_value());
  EXPECT_NEAR((double)k / card, a.fractionBefore(*first), 0.2 * k / card);
  auto both = predict(b, k, *first);
  ASSERT_TRUE(both.has_value());
  EXPECT_LT(*both, *first);
  EXPECT_NEAR((double)k / (2 * card), b.fractionBefore(*both),
              0.2 * k / card);
  // A domain smaller than the heap's room fills nothing.
  EXPECT_FALSE(FieldSortCollector::predictBottom(a, k - 10, k, 9, 0));

  // Time-ordered, newest first: segment 1 is wholly newer.
  auto older = keysOf("mono_i", true, 0);
  auto newer = keysOf("mono_i", true, 1);
  auto olderBottom = predict(older, 0, 0);
  ASSERT_TRUE(olderBottom.has_value());
  auto newerAfter = predict(newer, k, *olderBottom);
  auto newerAlone = predict(newer, 0, 0);
  ASSERT_TRUE(newerAfter.has_value() && newerAlone.has_value());
  EXPECT_NEAR((double)k / card, newer.fractionBefore(*newerAfter),
              0.05 * k / card);
  EXPECT_EQ(1, newer.leavesBoundedAtOrBefore(*newerAlone));
  // Visited the other way, the older segment leaves the bottom alone.
  EXPECT_EQ(*newerAlone, predict(older, k, *newerAlone));
  EXPECT_EQ(0, older.leavesBoundedAtOrBefore(*newerAlone));
}

// Segments of 64, 8 and 1 leaves and two of 64 and 8 docs, oldest and
// largest first (the shape a merge policy leaves): every segment saturates
// its own visit floor at k = 100 over a half-density domain, but the
// index-wide plan (TopDocsReq::fieldSortVisitsBounded) visits segments
// newest first for a descending time-ordered key, and past the small ones
// the heap's bottom bounds the rest: the planned floors cover a few leaves,
// so the router builds the whole membership and later requests serve every
// segment from it. A uniform key at the same depth stays saturated
// index-wide and builds nothing; at k = 10 it builds. The visit order puts
// the newest segment first for that sort and the oldest first ascending.
// Every request matches exhaustive collection.
TEST_F(SortCollectorTest, indexWidePlanBuildsAcrossSmallSegments) {
  WholeMembershipPlanGuard wholeGuard(false);
  CollectionHelper helper("index_wide_plan");
  helper.getIndexWriter()->snapshots.readers.filterCache =
      std::make_shared<FilterCache>(
          FilterCacheConfig{.minSegmentDocs = 0, .admissionThreshold = 1});
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr std::array<int32_t, 5> kSegDocs{64 * 512, 8 * 512, 512, 64, 8};
  int32_t docId = 0;
  for (int32_t docs : kSegDocs) {
    std::vector<Doc> batch;
    for (int32_t i = 0; i < docs; i++, docId++) {
      batch.push_back(flatdoc(
          "id", std::to_string(docId), "id_s", std::to_string(docId),
          "body_w", docId % 4 == 0 ? "half quarter"
                    : docId % 2 == 0 ? "half" : "odd",
          "time_i", (int64_t)docId,
          "rnd_i", (int64_t)(((uint32_t)docId * 2654435761u) % 1000003)));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(kSegDocs.size(), reader->segments().size());

  // The planner's view, from the per-segment estimates the router uses.
  auto plan = [&](std::string_view field, qb::SortDir dir, int64_t k) {
    IntFieldType type(field);
    SortField sortField(field, type,
                        dir == qb::DESC ? SortField::DESC : SortField::ASC);
    SortPlan sortPlan;
    sortPlan.useFieldSort = true;
    sortPlan.clauses = columnPlan(sortField);
    std::vector<int64_t> cards;
    for (int32_t docs : kSegDocs) cards.push_back(docs / 2);
    auto keys = TopDocsReq::fieldSortSegmentKeys(*reader, sortPlan, {});
    auto order = TopDocsReq::fieldSortVisitOrder(keys, k, std::numeric_limits<int64_t>::max());
    bool bounded = TopDocsReq::fieldSortVisitsBounded(
        *reader, sortPlan, {}, cards, k);
    return std::pair(order, bounded);
  };
  auto [newestFirst, timeBounded] = plan("time_i", qb::DESC, 100);
  EXPECT_EQ((std::vector<int32_t>{4, 3, 2, 1, 0}), newestFirst);
  EXPECT_TRUE(timeBounded);
  EXPECT_EQ((std::vector<int32_t>{0, 1, 2, 3, 4}),
            plan("time_i", qb::ASC, 100).first);
  EXPECT_FALSE(plan("rnd_i", qb::DESC, 100).second);
  EXPECT_TRUE(plan("rnd_i", qb::DESC, 10).second);

  struct Result {
    std::vector<std::string> ids;
    int64_t builds = 0;
    int64_t hits = 0;
    int64_t bypasses = 0;
  };
  auto run = [&](std::string_view field, qb::SortDir dir, int32_t limit,
                 bool exhaustive = false) {
    SortPruningGuard pruningGuard(exhaustive);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("index_wide_plan");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    // A union (half or quarter, which is half), a shape whose membership
    // the field sort itself may build.
    cur.rawQuery() = qb::boolean(
        cur.mr(), {}, {qb::match(cur.mr(), "body_w", "half"),
                       qb::match(cur.mr(), "body_w", "quarter")});
    qb::sort(cur, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{resultIds(*req), SkipStats::wholeFieldSortBuilds,
                  SkipStats::wholeFieldSortHits,
                  SkipStats::wholeFieldSortRoutingBypasses};
  };
  // The membership is the query's, whatever the sort: a uniform sort that
  // cannot bound its visits builds nothing, a time-ordered one builds it,
  // and then any sort is served from it.
  auto uniform = run("rnd_i", qb::DESC, 100);
  EXPECT_EQ(0, uniform.builds);
  EXPECT_EQ((int64_t)kSegDocs.size(), uniform.bypasses);
  auto built = run("time_i", qb::DESC, 100);
  EXPECT_EQ((int64_t)kSegDocs.size(), built.builds);
  auto served = run("time_i", qb::DESC, 100);
  EXPECT_EQ((int64_t)kSegDocs.size(), served.hits);
  EXPECT_EQ(run("time_i", qb::DESC, 100, true).ids, served.ids);
  EXPECT_EQ((int64_t)kSegDocs.size(), run("rnd_i", qb::DESC, 100).hits);
  for (std::string_view field : {"time_i", "rnd_i"}) {
    for (qb::SortDir dir : {qb::DESC, qb::ASC}) {
      for (int32_t limit : {1, 10, 100, 3000}) {
        EXPECT_EQ(run(field, dir, limit, true).ids,
                  run(field, dir, limit).ids)
            << field << " " << (int)dir << " limit=" << limit;
      }
    }
  }
}

// A segment without keys - here the sort field is absent from it - holds
// docs without a value, which MISSING_FIRST sorts before every key: the
// visit order takes it first. Under MISSING_LAST, or with an envelope (no
// missing value satisfies one), it goes last.
TEST_F(SortCollectorTest, visitOrderPlacesSegmentsWithoutKeysAtMissingEdge) {
  CollectionHelper helper("missing_edge");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  int32_t docId = 0;
  for (bool keyed : {true, false, true}) {
    std::vector<Doc> batch;
    for (int32_t i = 0; i < 2 * 512; i++, docId++) {
      Doc doc = flatdoc("id", std::to_string(docId));
      if (keyed) doc.push_back({"time_i", (int64_t)docId});
      batch.push_back(std::move(doc));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(3u, reader->segments().size());
  auto order = [&](FieldComparator::MissingValue missing,
                   std::optional<Query::ValueEnvelope> envelope) {
    IntFieldType type("time_i");
    SortField sortField("time_i", type, SortField::DESC, missing);
    SortPlan sortPlan;
    sortPlan.useFieldSort = true;
    sortPlan.clauses = columnPlan(sortField);
    auto keys = TopDocsReq::fieldSortSegmentKeys(*reader, sortPlan, envelope);
    return TopDocsReq::fieldSortVisitOrder(
        keys, 10, TopDocsReq::fieldSortMissingKey(sortPlan, envelope));
  };
  using Order = std::vector<int32_t>;
  EXPECT_EQ((Order{1, 2, 0}), order(FieldComparator::MISSING_FIRST, {}));
  EXPECT_EQ((Order{2, 0, 1}), order(FieldComparator::MISSING_LAST, {}));
  EXPECT_EQ((Order{2, 0, 1}),
            order(FieldComparator::MISSING_FIRST,
                  Query::ValueEnvelope{0, 1 << 20}));
}

// Sequential field sorts visit segments in fieldSortVisitOrder; the kept
// top K must not depend on it. Five segments in time order with keys that
// tie across segments (time_i / 4096 and three-valued tie_i), deletes in
// each, and match-all (liveDocs domains), a dense filter and a sparse one:
// every sort, direction and depth matches exhaustive (unordered) collection
// and a parallel request, and an exact count stays exact. Newest first on
// match-all, the newest segment fills the heap and every older one settles
// at its first bound: only the newest segment's leaves are gathered.
TEST_F(SortCollectorTest, segmentVisitOrderKeepsTopKExact) {
  WholeMembershipPlanGuard wholeGuard(true);
  TopDocsFilterFoldGuard foldGuard(true);
  CollectionHelper helper("visit_order");
  helper.getIndexWriter()->mergePolicy->setMergeFactor(10);
  constexpr std::array<int32_t, 5> kSegDocs{16 * 512, 16 * 512 - 77, 4 * 512,
                                            512 + 40, 300};
  int32_t docId = 0;
  std::vector<std::string> deletes;
  for (int32_t docs : kSegDocs) {
    std::vector<Doc> batch;
    for (int32_t i = 0; i < docs; i++, docId++) {
      std::string body = docId % 2 == 0 ? "half" : "odd";
      if (docId % 97 == 0) body += " sparse";
      batch.push_back(flatdoc(
          "id", std::to_string(docId), "id_s", std::to_string(docId),
          "body_w", body, "time_i", (int64_t)docId,
          "coarse_i", (int64_t)(docId / 4096), "tie_i", (int64_t)(docId % 3),
          "rnd_i", (int64_t)(((uint32_t)docId * 2654435761u) % 1000003)));
      if (docId % 811 == 5) deletes.push_back(std::to_string(docId));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteByIds(deletes, UpdateMessage::COMMIT).success);
  ASSERT_EQ(kSegDocs.size(), helper.getIndexWriter()
                                 ->snapshots.readers.getReader()
                                 ->segments()
                                 .size());

  struct Result {
    std::vector<std::string> ids;
    int64_t found = -1;
    int64_t gathered = 0;
  };
  auto run = [&](std::string_view field, qb::SortDir dir, int32_t limit,
                 std::string_view filter, bool exhaustive = false,
                 bool parallel = false, bool exactCount = false) {
    SortPruningGuard pruningGuard(exhaustive);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("visit_order");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    cur.allQuery();
    if (!filter.empty()) cur.matchFilter("body_w", filter);
    if (exactCount) cur.getNumber();
    qb::sort(cur, field, dir);
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    Result result{resultIds(*req), -1, SkipStats::fieldSortDocsGathered};
    const auto* list = req->docList("q");
    if (list != nullptr && list->found) result.found = *list->found;
    return result;
  };
  for (std::string_view filter : {"half", "sparse"}) {  // admit the filters
    run("time_i", qb::DESC, 10, filter);
    run("time_i", qb::DESC, 10, filter);
  }
  for (std::string_view field : {"time_i", "coarse_i", "tie_i", "rnd_i"}) {
    for (qb::SortDir dir : {qb::DESC, qb::ASC}) {
      for (int32_t limit : {1, 7, 100, 3000}) {
        for (std::string_view filter : {"", "half", "sparse"}) {
          auto exhaustive = run(field, dir, limit, filter, true);
          EXPECT_EQ(exhaustive.ids, run(field, dir, limit, filter).ids)
              << field << " " << (int)dir << " limit=" << limit << " "
              << filter;
          EXPECT_EQ(exhaustive.ids,
                    run(field, dir, limit, filter, false, true).ids)
              << "parallel " << field << " " << (int)dir
              << " limit=" << limit << " " << filter;
          auto counted = run(field, dir, limit, filter, false, false, true);
          EXPECT_EQ(exhaustive.ids, counted.ids);
          EXPECT_EQ(run(field, dir, limit, filter, true, false, true).found,
                    counted.found)
              << field << " limit=" << limit << " " << filter;
        }
      }
    }
  }
  EXPECT_LE(run("time_i", qb::DESC, 10, "").gathered, 512);
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
      if (docId % 7 == 0) body += " excluded";
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
  enum class Main { TERM, BOOLEAN, UNION, PHRASE_UNION };
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
    } else if (main == Main::UNION) {
      cur.rawQuery() = qb::boolean(
          cur.mr(), {}, {qb::match(cur.mr(), "body_w", value),
                         qb::match(cur.mr(), "body_w", "head")});
    } else if (main == Main::PHRASE_UNION) {
      cur.rawQuery() = qb::boolean(
          cur.mr(), {}, {qb::match(cur.mr(), "body_w", value),
                         qb::phraseWords(cur.mr(), "body_w", {"alpha", "alpha"})});
    } else {
      cur.rawQuery() = qb::boolean(
          cur.mr(), {qb::match(cur.mr(), "body_w", value)}, {},
          {qb::match(cur.mr(), "body_w", "excluded")});
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
  // Supported compound products reuse the same graph for seeds and sweep.
  for (Main main : {Main::BOOLEAN, Main::UNION}) {
    for (const Sorts& sorts : {randAsc, randDesc, tiesAsc, monoAsc}) {
      for (int32_t limit : {9, 987, 4500}) {
        auto seeded = run(true, false, sorts, limit, false, "alpha", 500, main);
        auto exhaustive = run(false, true, sorts, limit, false, "alpha", 0, main);
        EXPECT_GT(seeded.activations, 0);
        EXPECT_EQ(exhaustive.ids, seeded.ids);
      }
    }
  }
  // An unsupported executing child keeps the ordinary forward-only route.
  {
    auto mixed = run(true, false, randAsc, 9, false, "alpha", 0, Main::PHRASE_UNION);
    EXPECT_EQ(0, mixed.activations);
    EXPECT_EQ(run(false, true, randAsc, 9, false, "alpha", 0, Main::PHRASE_UNION).ids,
              mixed.ids);
  }
  // Exact count keeps every pruning route off and stays exact.
  for (Main main : {Main::TERM, Main::UNION}) {
    auto exact = run(true, false, randAsc, 9, true, "alpha", 0, main);
    auto exactExh = run(false, true, randAsc, 9, true, "alpha", 0, main);
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
  for (Main main : {Main::TERM, Main::UNION}) {
    for (const Sorts& sorts : {randAsc, tiesAsc}) {
      auto exhaustive = run(false, true, sorts, 987, false, "alpha", 0, main);
      auto seeded = run(true, false, sorts, 987, false, "alpha", 0, main);
      EXPECT_GT(seeded.activations, 0);
      EXPECT_EQ(exhaustive.ids, seeded.ids)
          << "deletes sort=" << sorts.clauses[0].first;
    }
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

  auto run = [&](bool seeded, bool disablePruning,
                 int32_t budgetPerMille = 0) {
    SeededGuard seededGuard(!seeded, seeded, budgetPerMille);
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
  // The predicted floor plans a single seed leaf; twenty times that
  // schedule reaches the abort.
  auto seeded = run(true, false, 20000);
  EXPECT_GT(seeded.activations, 0);
  EXPECT_GT(seeded.fillAborts, 0);
  EXPECT_EQ(exhaustive.ids, seeded.ids);
}

// A term confined to the older half falsifies the newest-first yield prior.
// A late head must still abandon empty rounds within the common budget and
// let the sweep finish. Oldest-first over the newer half does the same.
// Both terms and unions must retain exhaustive ranked-ID parity.
TEST_F(SortCollectorTest, seededLateHeadObeysAbortBudget) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper("seeded_rounds");
  constexpr int32_t kDocs = 256 * 512;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < kDocs; doc++) {
    docs.push_back(flatdoc("id", std::to_string(doc), "id_s",
                           std::to_string(doc), "body_w",
                           doc < kDocs / 2
                               ? (doc % 2 == 0 ? "early first" : "early second")
                               : "late",
                           "mono_i", (int64_t)doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  struct Result {
    std::vector<std::string> ids;
    int64_t rounds = 0;
    int64_t aborts = 0;
    int64_t gathered = 0;
  };
  auto run = [&](std::string_view term, qb::SortDir dir, int32_t limit,
                 bool disablePruning, bool unionQuery = false) {
    SortPruningGuard pruningGuard(disablePruning);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("seeded_rounds");
    auto& cur = req->topDocs("q").limit(limit).fields({"id_s"});
    if (unionQuery) {
      cur.rawQuery() = qb::boolean(
          cur.mr(), {}, {qb::match(cur.mr(), "body_w", "first"),
                         qb::match(cur.mr(), "body_w", "second")});
    } else {
      // Optionals retain the query-driven route while preserving term matches.
      cur.rawQuery() = qb::boolean(
          cur.mr(), {qb::match(cur.mr(), "body_w", term)},
          {qb::match(cur.mr(), "body_w", "early"),
           qb::match(cur.mr(), "body_w", "late")});
    }
    qb::sort(cur, "mono_i", dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return Result{resultIds(*req), SkipStats::fieldSortSeedRounds,
                  SkipStats::fieldSortSeedFillAborts,
                  SkipStats::fieldSortDocsGathered};
  };
  for (int32_t limit : {1, 10, 100}) {
    auto newest = run("early", qb::DESC, limit, false);
    EXPECT_EQ(run("early", qb::DESC, limit, true).ids, newest.ids);
    // The newer half is empty. Late-head benefit cannot excuse repeated
    // production against a falsified yield prior; the sweep completes it.
    EXPECT_LE(newest.rounds, 1) << limit;
    EXPECT_GT(newest.aborts, 0) << limit;
    auto unionNewest = run("early", qb::DESC, limit, false, true);
    EXPECT_EQ(run("early", qb::DESC, limit, true, true).ids, unionNewest.ids);
    EXPECT_LE(unionNewest.rounds, 1);
    EXPECT_GT(unionNewest.aborts, 0);
    auto oldest = run("late", qb::ASC, limit, false);
    EXPECT_EQ(run("late", qb::ASC, limit, true).ids, oldest.ids);
    EXPECT_EQ(0, oldest.rounds) << limit;
  }
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
    // Every segment: a later one starts with the bottom the earlier ones
    // set, so it can activate at its first docid.
    EXPECT_EQ(nSegs, run(false, true, true, randAsc, 9).activations);
    EXPECT_EQ(nSegs, run(false, true, true, randDesc, 9).activations);
    // So must the windowed (term query) route.
    EXPECT_GT(run(false, false, false, randAsc, 9).activations, 0);
    EXPECT_GT(run(false, false, false, randDesc, 9).activations, 0);
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

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
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

// The candidate gate reads each docFreq at most once per segment, however
// many probes consult it. The first domain docs carry the ten best values in
// either direction, so the bottom settles at once and every later probe sees
// the same nine-term interval; each of those terms is heavy outside the 10%
// domain, so the union never pays. Re-reading the interval per probe would
// read at least one docFreq on every probe the capture charge admits.
TEST_F(SortCollectorTest, stringCandidateGateReadsEachDocFreqOnce) {
  WholeMembershipPlanGuard wholeGuard(true);
  CollectionHelper helper;
  constexpr int32_t kDocs = 65536;
  constexpr int32_t kDomainEvery = 10;
  SplitMix64 rng(42);
  std::vector<Doc> docs;
  docs.reserve(kDocs);
  for (int32_t docId = 0; docId < kDocs; docId++) {
    int32_t rank = docId / kDomainEvery;
    bool inDomain = docId % kDomainEvery == 0;
    std::string value;
    if (inDomain && rank < 10) {
      value = "a" + std::to_string(rank);
    } else if (inDomain && rank < 20) {
      value = "z" + std::to_string(rank - 10);
    } else if (!inDomain && rng.rint(2) == 0) {
      value = (rng.rint(2) == 0 ? "a" : "z") + std::to_string(rng.rint(10));
    } else {
      value = std::format("m{:05}", rng.rint(100000));
    }
    std::string id = std::to_string(docId);
    docs.push_back(flatdoc("id_s", id, "body_w", inDomain ? "alpha" : "other",
                           "rand_s", value));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT, false, 1).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());

  struct Result {
    std::vector<std::string> ids;
    int64_t activations = 0;
    int64_t probes = 0;
    int64_t docFreqs = 0;
  };
  auto run = [&](bool disablePruning, bool forcePull, qb::SortDir dir) {
    SortPruningGuard pruningGuard(disablePruning);
    FieldSortBulkGuard bulkGuard(forcePull);
    SortSkipStatsGuard statsGuard;
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").limit(10).fields({"id_s"});
    cur.rawQuery() = qb::match(cur.mr(), "body_w", "alpha");
    qb::sort(cur, "rand_s", dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    Result result;
    result.ids = resultIds(*req);
    result.activations = SkipStats::fieldSortCandidateActivations;
    result.probes = SkipStats::fieldSortCandidateProbes;
    result.docFreqs = SkipStats::fieldSortCandidateDocFreqs;
    return result;
  };

  for (qb::SortDir dir : {qb::ASC, qb::DESC}) {
    for (bool forcePull : {false, true}) {
      auto exhaustive = run(true, forcePull, dir);
      auto pruned = run(false, forcePull, dir);
      std::string context = "dir=" + std::to_string((int)dir)
          + " forcePull=" + std::to_string(forcePull);
      EXPECT_EQ(exhaustive.ids, pruned.ids) << context;
      ASSERT_EQ(10u, pruned.ids.size()) << context;
      EXPECT_EQ(0, pruned.activations) << context;
      // A probe every 4096 docids past the heap fill, most of them within
      // the capture charge's limit.
      EXPECT_GE(pruned.probes, 12) << context;
      EXPECT_LE(pruned.docFreqs, 9) << context;
    }
  }
}

// The candidate gate's budget waits while capture still shrinks with the
// source docs seen, counts earlier segments' docs as seen, refuses where too
// few docs are left, and stays in range for any source cost.
TEST_F(SortCollectorTest, stringCandidateBudget) {
  constexpr int32_t M = 1 << 20;
  constexpr int64_t src = M / 10;
  auto budget = &FieldSortCollector::candidateBudget;
  // A wide interval at the first probe waits; the same interval pays later.
  EXPECT_EQ(-1, budget(src, 0, M, 4096, 500));
  EXPECT_GT(budget(src, 0, M, M / 4, 500), 0);
  // A later segment can take over at docid 0; a first one cannot.
  EXPECT_EQ(-1, budget(src, 0, M, 0, 50));
  EXPECT_GT(budget(src, 10 * src, M, 0, 50), 0);
  // Near the end the docs left bound it, however many were seen.
  EXPECT_EQ(-1, budget(src, 100 * src, M, M - 4096, 50));
  EXPECT_LE(budget(src, 100 * src, M, M / 2, 1), src);
  EXPECT_LT(budget(src, 100 * src, M, M / 2, 1000),
            budget(src, 100 * src, M, M / 2, 1));
  // A disjunction's cost can exceed the segment's docs.
  constexpr int32_t big = std::numeric_limits<int32_t>::max();
  int64_t huge = int64_t{1} << 40;
  int64_t wide = budget(huge, huge, big, big / 2, 1);
  EXPECT_GT(wide, 0);
  EXPECT_LE(wide, (int64_t)big);
  EXPECT_EQ(-1, budget(huge, 0, big, big, 1));
  EXPECT_EQ(-1, budget(-1, 0, M, M / 2, 1));
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
      auto reader = helper.getIndexWriter()->snapshots.readers.getReader();

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
