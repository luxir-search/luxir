// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/reader/BKDReader.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

class RangeFieldParityTest : public LuxirTest {
  using Type = api::FieldDef::FieldClass;
  struct Window { int64_t lo, hi; };
  struct Interval { Window window; bool present, live = true; };
  CollectionHelper helper;

  static Window domain(Type type) {
    // Sortable double infinities; long/date open ends use the int64 extremes.
    return type == Type::DOUBLE_RANGE ? Window{-0x7ff0000000000001LL, 0x7ff0000000000000LL}
                                     : Window{INT64_MIN, INT64_MAX};
  }

  static api::Val literal(Type type, int64_t encoded) {
    api::Val value;
    if (type == Type::DOUBLE_RANGE) {
      // Invert the sortable encoding directly, including the two signed zeros.
      uint64_t bits = (uint64_t)encoded ^ (encoded < 0 ? 0x7fffffffffffffffULL : 0);
      value.kind = std::bit_cast<double>(bits);
    } else value.kind = encoded;
    return value;
  }

  static Window generate(Type type, std::mt19937_64& random, int pattern) {
    Window limits = domain(type);
    auto sample = [&]() -> int64_t {
      if (type == Type::DATE_RANGE) return (int64_t)(random() % 600000000000001ULL) - 300000000000000LL;
      uint64_t span = (uint64_t)limits.hi - (uint64_t)limits.lo;
      return span == UINT64_MAX ? std::bit_cast<int64_t>(random())
          : (int64_t)((uint64_t)limits.lo + random() % (span + 1));
    };
    int64_t a = sample(), b = sample();
    switch (pattern % 12) {
      case 0: return {a, a};
      case 1: {
        uint64_t delta = std::min((uint64_t)limits.hi - (uint64_t)a, random() % 32);
        return {a, (int64_t)((uint64_t)a + delta)};
      }
      case 3: return {limits.lo, b};
      case 4: return {a, limits.hi};
      case 5: return limits;
      case 6: case 7: case 8: case 9: {
        int64_t base = type == Type::DOUBLE_RANGE ? 0x3ff0000000000000LL
            : type == Type::DATE_RANGE ? 1704067200000LL : 0;
        a = base + (int64_t)(random() % 1024);
        b = pattern % 2 ? a : base + (int64_t)(random() % 1024);
        break;
      }
      case 10: return {type == Type::DOUBLE_RANGE ? -1 : 0, 0};
      default: break;
    }
    return {std::min(a, b), std::max(a, b)};
  }

  static void fillRange(api::Map& map, Type type, Window window, std::pmr::memory_resource& mr) {
    Window limits = domain(type);
    if (window.lo != limits.lo) *api::build::mapSlot<api::Val>(map.fields, 2, "gte", mr) = literal(type, window.lo);
    if (window.hi != limits.hi) *api::build::mapSlot<api::Val>(map.fields, 2, "lte", mr) = literal(type, window.hi);
  }

  static bool matches(Window stored, Window query, api::RangeQuery::Relation relation) {
    switch (relation) {
      case api::RangeQuery::Relation::INTERSECTS: return stored.lo <= query.hi && stored.hi >= query.lo;
      case api::RangeQuery::Relation::CONTAINS: return stored.lo <= query.lo && stored.hi >= query.hi;
      case api::RangeQuery::Relation::WITHIN: return stored.lo >= query.lo && stored.hi <= query.hi;
    }
    std::unreachable();
  }

  void check(Type type, const std::vector<Interval>& intervals, int visible, const std::vector<Window>& queries) {
    for (const auto& query : queries) for (auto relation : {api::RangeQuery::Relation::INTERSECTS,
        api::RangeQuery::Relation::CONTAINS, api::RangeQuery::Relation::WITHIN}) {
      SCOPED_TRACE(fmt::format("{} {} {}", (int)relation, query.lo, query.hi));
      std::vector<int64_t> expected;
      for (int i = 0; i < visible; i++) {
        const auto& stored = intervals[i];
        if (stored.present && stored.live && matches(stored.window, query, relation)) expected.push_back(i);
      }
      for (auto field : {"bkd", "col"}) {
        SCOPED_TRACE(field);
        auto req = localReq(helper.getSearchEngine());
        auto& cursor = req->collection("main").topDocs("q").fields({"ordinal_i"}).limit(-1);
        auto& range = cursor.rawQuery().kind.emplace<api::RangeQuery>();
        range.field = field;
        range.relation = relation;
        if (query.lo != domain(type).lo) {
          auto* value = api::build::allocMessage<api::Val>(req->mr);
          *value = literal(type, query.lo);
          range.gte = value;
        }
        if (query.hi != domain(type).hi) {
          auto* value = api::build::allocMessage<api::Val>(req->mr);
          *value = literal(type, query.hi);
          range.lte = value;
        }
        req->execute();
        ASSERT_TRUE(req->ok()) << req->errorMsg();
        std::vector<int64_t> actual;
        for (auto* response : req->responses) {
          ASSERT_FALSE(response->proto.error);
          const auto* result = response->proto.ops.find("q");
          if (!result) continue;
          const auto* list = (**result).docList();
          if (!list || list->row_count == 0) continue;
          auto values = std::get<api::ColInt>(list->columns.at("ordinal_i").kind).v;
          actual.insert(actual.end(), values.begin(), values.end());
        }
        std::sort(actual.begin(), actual.end());
        ASSERT_EQ(expected, actual) << "visible=" << visible << " expected hits=" << expected.size()
                                    << " actual hits=" << actual.size();
      }
    }
  }

  void inspectTree(bool merged) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    uint32_t leaves = 0, depth = 0, wide = 0, compact = 0;
    bool partial = false;
    for (auto& segment : reader->segments()) {
      auto& postings = segment.postingsReader();
      BasicBKDReader<BKDInt64Traits> bkd(postings, *postings.fieldInfo("bkd"));
      bkd.validate();
      leaves += bkd.leafCount();
      depth = std::max(depth, (uint32_t)std::bit_width(bkd.leafCount() - 1));
      partial |= bkd.pointCount() % bkd.maxPointsPerLeaf() != 0;
      for (uint32_t leaf = 0; leaf < bkd.leafCount(); leaf++) {
        auto info = bkd.leafInfo(leaf);
        wide += info.bits0 > 32 || info.bits1 > 32;
        compact += info.bits0 <= 32 && info.bits1 <= 32;
      }
    }
    EXPECT_TRUE(partial);
    EXPECT_GT(wide, 0u);
    if (merged) {
      ASSERT_EQ(1u, reader->segments().size());
      EXPECT_GE(depth, 3u);
      EXPECT_GT(compact, 0u);
      RecordProperty("leaves", leaves);
      RecordProperty("depth_edges", depth);
      RecordProperty("wide_leaves", wide);
      RecordProperty("compact_leaves", compact);
    }
  }

protected:
  void run(Type type) {
    SchemaBuilder schema;
    auto& indexed = schema.field("bkd");
    indexed.type = type;
    indexed.index = api::FieldDef::IndexMode::RANGE;
    schema.field("col").type = type;
    schema.set(helper.collection());
    // Corpus and query count are independent dimensions of the work budget.
    int count = (int)scaleTestDimension(4099, 2);
    // Keep the first flush and the merged tree's final leaves partial at any effort.
    auto partialLeaf = [&](int end, bool deleted) {
      int populated = 0;
      for (int i = 0; i < end; i++) populated += i % 29 != 0 && (!deleted || i % 7 != 0);
      return populated % BasicBKDWriter<BKDInt64Traits>::DEFAULT_MAX_POINTS_PER_LEAF != 0;
    };
    while (!partialLeaf(count / 3, false) || !partialLeaf(count, true)) count++;
    int queryCount = (int)scaleTestDimension(16, 2);
    std::mt19937_64 random(0x72616e676573ULL);
    std::vector<Interval> intervals;
    for (int i = 0; i < count; i++) intervals.push_back({generate(type, random, i), i % 29 != 0});
    std::vector<Window> queries;
    for (int i = 0; i < queryCount; i++) queries.push_back(generate(type, random, i));
    for (int flush = 0; flush < 3; flush++) {
      SCOPED_TRACE(fmt::format("flush {}", flush));
      CollectionHelper::UpdateBuilder batch;
      int begin = count * flush / 3, end = count * (flush + 1) / 3;
      for (int i = begin; i < end; i++) batch.addRaw([&](api::Map& doc, auto& mr) {
        api::build::mapSlot<api::Val>(doc.fields, 4, "id", mr)->kind = api::build::arenaStr(mr, std::to_string(i));
        api::build::mapSlot<api::Val>(doc.fields, 4, "ordinal_i", mr)->kind = (int64_t)i;
        if (intervals[i].present) for (auto field : {"bkd", "col"}) {
          auto& map = api::build::mapSlot<api::Val>(doc.fields, 4, field, mr)->kind.template emplace<api::Map>();
          fillRange(map, type, intervals[i].window, mr);
        }
      });
      batch.commit();
      ASSERT_TRUE(helper.submit(batch).success);
      inspectTree(false);
      check(type, intervals, end, queries);
    }
    CollectionHelper::UpdateBuilder deletes;
    for (int i = 0; i < count; i += 7) {
      intervals[i].live = false;
      deletes.remove(std::to_string(i));
    }
    deletes.commit();
    ASSERT_TRUE(helper.submit(deletes).success);
    {
      SCOPED_TRACE("after deletes");
      check(type, intervals, count, queries);
    }
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(helper.submit(merge).success);
    inspectTree(true);
    SCOPED_TRACE("after merge");
    check(type, intervals, count, queries);
    RecordProperty("corpus", count);
    RecordProperty("query_windows", queryCount);
  }
};

TEST_F(RangeFieldParityTest, longIntervals) { run(api::FieldDef::FieldClass::LONG_RANGE); }
TEST_F(RangeFieldParityTest, doubleIntervals) { run(api::FieldDef::FieldClass::DOUBLE_RANGE); }
TEST_F(RangeFieldParityTest, dateIntervals) { run(api::FieldDef::FieldClass::DATE_RANGE); }
