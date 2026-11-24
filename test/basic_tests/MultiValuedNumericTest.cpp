// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <map>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "test/SchemaBuilder.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/value/ValueExprParser.h"
#include "luxir/search/FieldComparator.h"

using namespace luxir;
using namespace luxir::test;

// Numeric sets sort/deduplicate; arrays keep source order and repeats.
class MultiValuedNumericTest : public LuxirTest {
protected:
  void indexDocs(CollectionHelper& helper) {
    helper.index(flatdoc("id_s", "a", "n_is", vec_i(9, -4, 9, 0), "d_ds", vec(2.5, -7.0, -0.5)),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "b", "n_is", vec_i(8, 7), "d_ds", vec(1e300, -1e-300)),
                 UpdateMessage::COMMIT);
    helper.index(flatdoc("id_s", "c", "n_is", vec_i(20, 1), "d_ds", vec(3.0)),
                 UpdateMessage::COMMIT);
  }

  // Doc ids and each doc's emitted values, sorted by `sortExpr`.
  struct Rows {
    std::vector<std::string> ids;
    std::vector<std::vector<int64_t>> ints;
    std::vector<std::vector<double>> doubles;
  };

  Rows run(std::string_view sortExpr, qb::SortDir dir) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id_s", "n_is", "d_ds"});
    qb::sort(top, sortExpr, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto& docs = *req->docList("q");
    Rows rows;
    for (auto id : std::get<api::ColStr>(docs.columns.at("id_s").kind).v) {
      rows.ids.emplace_back(id);
    }
    for (const auto& arr : std::get<api::ArrArrInt>(docs.columns.at("n_is").kind).v) {
      rows.ints.emplace_back(arr.v.begin(), arr.v.end());
    }
    for (const auto& arr : std::get<api::ArrArrDouble>(docs.columns.at("d_ds").kind).v) {
      rows.doubles.emplace_back(arr.v.begin(), arr.v.end());
    }
    return rows;
  }

  void expectSortedAndOrdered(bool array = false) {
    auto rows = run("id_s", qb::ASC);
    ASSERT_EQ((std::vector<std::string>{"a", "b", "c"}), rows.ids);
    EXPECT_EQ(array ? vec_i(9, -4, 9, 0) : vec_i(-4, 0, 9), rows.ints[0]);
    EXPECT_EQ(array ? vec_i(8, 7) : vec_i(7, 8), rows.ints[1]);
    EXPECT_EQ(array ? vec_i(20, 1) : vec_i(1, 20), rows.ints[2]);
    EXPECT_EQ(array ? vec(2.5, -7.0, -0.5) : vec(-7.0, -0.5, 2.5), rows.doubles[0]);
    EXPECT_EQ(array ? vec(1e300, -1e-300) : vec(-1e-300, 1e300), rows.doubles[1]);

    // min ascending, max descending: ints a=-4/9, b=7/8, c=1/20.
    EXPECT_EQ((std::vector<std::string>{"a", "c", "b"}), run("n_is", qb::ASC).ids);
    EXPECT_EQ((std::vector<std::string>{"c", "a", "b"}), run("n_is", qb::DESC).ids);
    // doubles a=-7/2.5, b=-1e-300/1e300, c=3/3.
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), run("d_ds", qb::ASC).ids);
    EXPECT_EQ((std::vector<std::string>{"b", "c", "a"}), run("d_ds", qb::DESC).ids);
    EXPECT_EQ((std::vector<std::string>{"b", "c", "a"}), run("max(d_ds)", qb::DESC).ids);
    EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), run("min(d_ds)", qb::ASC).ids);
  }
};

TEST_F(MultiValuedNumericTest, valuesSortedPerDocAndSortReadsEdges) {
  CollectionHelper helper;
  indexDocs(helper);
  expectSortedAndOrdered();
}

TEST_F(MultiValuedNumericTest, mergePreservesPerDocOrder) {
  CollectionHelper helper;
  indexDocs(helper);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  ASSERT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  expectSortedAndOrdered();
}

TEST_F(MultiValuedNumericTest, layoutFollowsValuesThroughMerges) {
  using Multi = api::FieldDef::MultiMode;
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->setMergeFactor(100);
  SchemaBuilder schema;
  auto& singleton = schema.field("n_is");
  singleton.type = api::FieldDef::FieldClass::INT;
  singleton.index = api::FieldDef::IndexMode::RANGE;
  singleton.multi = Multi::SET;
  auto& mixed = schema.field("mixed");
  mixed.type = api::FieldDef::FieldClass::INT;
  mixed.index = api::FieldDef::IndexMode::RANGE;
  mixed.multi = Multi::SINGLE;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({
    flatdoc("id", "a", "n_is", vec_i(4), "mixed", 4),
    flatdoc("id", "dead-single", "n_is", vec_i(70), "mixed", 70)
  }, UpdateMessage::COMMIT).success);

  ASSERT_TRUE(helper.index(flatdoc("id", "b", "n_is", vec_i(8, 8), "mixed", 8),
                           UpdateMessage::COMMIT).success);

  auto checkLayout = [](IndexReader::Segment& seg, std::string_view field,
                        int32_t docs, int64_t values, bool endRanks, Multi mode, bool dense) {
    auto* info = seg.postingsReader().fieldInfo(field);
    ASSERT_NE(nullptr, info);
    EXPECT_EQ(docs, info->docsWithField);
    EXPECT_EQ(values, info->numValues);
    EXPECT_EQ(endRanks, info->endValueRanks().has_value());
    EXPECT_EQ(mode != Multi::SINGLE, (info->flags & FieldType::MULTI_VALUED) != 0);
    EXPECT_EQ(mode == Multi::ARRAY, (info->flags & FieldType::MULTI_ARRAY) != 0);
    IntColReader column(seg.postingsReader(), *info);
    EXPECT_EQ(dense, column.docsReader().allRows());
    EXPECT_EQ(endRanks, column.leafMeta().valueSketch().empty());
  };
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, reader->segments().size());
  for (auto& seg : reader->segments()) {
    checkLayout(seg, "n_is", seg.maxDoc(), seg.maxDoc(), false, Multi::SET, true);
    checkLayout(seg, "mixed", seg.maxDoc(), seg.maxDoc(), false, Multi::SINGLE, true);
  }

  writer->mergeSegments();
  reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  checkLayout(reader->segments()[0], "mixed", 3, 3, false, Multi::SINGLE, true);
  checkLayout(reader->segments()[0], "n_is", 3, 3, false, Multi::SET, true);

  mixed.multi = Multi::ARRAY;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({
    flatdoc("id", "dead-multi", "n_is", vec_i(99), "mixed", vec_i(99, 100)),
    flatdoc("id", "c", "n_is", vec_i(10), "mixed", vec_i(14, 1, 14)),
    flatdoc("id", "e")
  }, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "d"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("dead-single", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("dead-multi", UpdateMessage::COMMIT).success);
  reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(3u, reader->segments().size());
  checkLayout(reader->segments()[1], "mixed", 2, 5, true, Multi::ARRAY, false);
  EXPECT_EQ(nullptr, reader->segments()[2].postingsReader().fieldInfo("mixed"));

  auto checkReads = [&] {
    auto req = localReq(helper.getSearchEngine());
    for (auto direction : {qb::ASC, qb::DESC}) {
      auto& top = req->topDocs(direction == qb::ASC ? "asc" : "desc")
          .allQuery().fields({"id", "mixed"}).limit(10);
      qb::sort(top, "mixed", direction);
      qb::sort(top, "id", qb::ASC);
    }
    for (std::string_view name : {"hits", "count"}) {
      auto& top = req->topDocs(name).fields({"id"}).countLimit().limit(name == "hits" ? 10 : 0);
      top.rawQuery() = qb::range(req->mr, "mixed", qb::valI64(req->mr, 4), nullptr,
                                qb::valI64(req->mr, 8), nullptr);
      if (name == "hits") qb::sort(top, "id", qb::ASC);
    }
    auto& facet = req->facet("values", "mixed").limit(-1);
    std::get<api::FieldFacet>(facet.rawOp().kind).missing = true;
    req->execute();
    ASSERT_OK(req);
    for (std::string_view name : {"asc", "desc", "hits"}) {
      auto ids = std::get<api::ColStr>(req->docList(name)->columns.at("id").kind).v;
      using Ids = std::vector<std::string_view>;
      Ids expected = name == "hits" ? Ids{"a", "b"}
          : name == "asc" ? Ids{"c", "a", "b", "d", "e"} : Ids{"c", "b", "a", "d", "e"};
      EXPECT_EQ(expected, Ids(ids.begin(), ids.end())) << name;
    }
    auto arrays = std::get<api::ArrArrInt>(req->docList("asc")->columns.at("mixed").kind).v;
    ASSERT_EQ(5u, arrays.size());
    EXPECT_TRUE(std::ranges::equal(arrays[0].v, vec_i(14, 1, 14)));
    EXPECT_TRUE(std::ranges::equal(arrays[1].v, vec_i(4)));
    EXPECT_TRUE(std::ranges::equal(arrays[2].v, vec_i(8)));
    EXPECT_EQ(2, req->getMatchCount("hits"));
    EXPECT_EQ(2, req->getMatchCount("count"));
    const auto* result = req->responses[0]->proto.ops.at("values")->facetResult();
    ASSERT_NE(nullptr, result);
    auto keys = std::get<api::ColInt>(result->bucket_ids->kind).v;
    std::map<int64_t, int64_t> counts;
    for (size_t i = 0; i < keys.size(); i++) counts.emplace(keys[i], result->counts[i]);
    EXPECT_EQ((std::map<int64_t, int64_t>{{1, 1}, {4, 1}, {8, 1}, {14, 1}}), counts);
    EXPECT_EQ(2, result->missing.value_or(0));
  };
  checkReads();
  writer->mergeSegments();
  reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  checkLayout(reader->segments()[0], "mixed", 3, 5, true, Multi::ARRAY, false);
  checkLayout(reader->segments()[0], "n_is", 3, 3, false, Multi::SET, false);
  checkReads();
}

TEST_F(MultiValuedNumericTest, arraysPreserveSourceAndSelectExtremaThroughMerge) {
  CollectionHelper helper;
  SchemaBuilder b;
  auto& ints = b.templ("_is");
  ints.type = api::FieldDef::FieldClass::INT;
  ints.multi = api::FieldDef::MultiMode::ARRAY;
  auto& doubles = b.templ("_ds");
  doubles.type = api::FieldDef::FieldClass::DOUBLE;
  doubles.multi = api::FieldDef::MultiMode::ARRAY;
  b.set(helper.collection());
  indexDocs(helper);
  expectSortedAndOrdered(true);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  expectSortedAndOrdered(true);
}

TEST_F(MultiValuedNumericTest, materializationScratchIsAccountedAndReused) {
  for (bool typedInts : {false, true}) {
    CollectionHelper helper;
    helper.clear();
    auto iw = helper.getIndexWriter();
    auto& inverter = iw->obtainInverter();
    auto& field = inverter.getIndexHandler(typedInts ? "v_is" : "v_ds");
    size_t before = inverter.extraRamBytes;
    std::vector<double> doubles(100000, 1.5);
    std::vector<int64_t> ints(100000, 1);
    api::Val input;
    input.kind = api::ArrDouble{.v = doubles};
    auto index = [&] {
      if (typedInts) field.index(inverter, std::span<const int64_t>(ints));
      else field.index(inverter, input);
    };
    inverter.startDoc();
    index();
    size_t retained = inverter.extraRamBytes;
    EXPECT_GE(retained - before, ints.size() * sizeof(int64_t));
    inverter.finishDoc();
    inverter.startDoc();
    index();
    EXPECT_EQ(retained, inverter.extraRamBytes);
    inverter.finishDoc();
    iw->releaseInverter(inverter, true);
    helper.commit();
  }
}

TEST_F(MultiValuedNumericTest, modeChangesUseSegmentModeBeforeAndAfterMerge) {
  CollectionHelper helper;
  SchemaBuilder b;
  auto& field = b.field("n_is");
  field.type = api::FieldDef::FieldClass::INT;
  field.multi = api::FieldDef::MultiMode::ARRAY;
  b.set(helper.collection());
  helper.index(flatdoc("id_s", "a", "n_is", vec_i(9, -4, 9, 0), "d_ds", vec(2.5)), UpdateMessage::COMMIT);
  field.multi = api::FieldDef::MultiMode::SET;
  b.set(helper.collection());
  helper.index(flatdoc("id_s", "b", "n_is", vec_i(8, 7), "d_ds", vec(1.0)), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "n_is", vec_i(20, 1), "d_ds", vec(3.0)), UpdateMessage::COMMIT);
  for (int pass = 0; pass < 2; pass++) {
    auto rows = run("id_s", qb::ASC);
    ASSERT_EQ(3u, rows.ints.size());
    EXPECT_EQ(vec_i(9, -4, 9, 0), rows.ints[0]);
    EXPECT_EQ(vec_i(7, 8), rows.ints[1]);
    EXPECT_EQ(vec_i(1, 20), rows.ints[2]);
    EXPECT_EQ((std::vector<std::string>{"a", "c", "b"}), run("n_is", qb::ASC).ids);
    EXPECT_EQ((std::vector<std::string>{"c", "a", "b"}), run("n_is", qb::DESC).ids);
    EXPECT_EQ(run("n_is", qb::ASC).ids, run("min(n_is)", qb::ASC).ids);
    EXPECT_EQ(run("n_is", qb::DESC).ids, run("max(n_is)", qb::DESC).ids);
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(pass == 0 ? 3u : 1u, reader->segments().size());
    for (size_t i = 0; i < reader->segments().size(); i++) {
      auto& segment = reader->segments()[i];
      const auto& columnInfo = *reader->field("n_is").at(segment);
      IntColReader column(segment.postingsReader(), columnInfo);
      EXPECT_EQ(i != 0, column.multiSet()); // A mixed merge conservatively keeps ARRAY.
    }
    if (pass == 0) {
      CollectionHelper::UpdateBuilder merge;
      merge.commit(true, 1);
      ASSERT_TRUE(helper.submit(merge).success);
    }
  }
}

TEST_F(MultiValuedNumericTest, comparatorPointAndBatchReadDenseAndSparseModes) {
  for (bool array : {false, true}) for (bool dense : {false, true}) {
    CollectionHelper helper;
    helper.clear();
    SchemaBuilder b;
    auto& field = b.field("n_is");
    field.type = api::FieldDef::FieldClass::INT;
    field.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
    b.set(helper.collection());
    helper.indexAll({flatdoc("n_is", vec_i(9, -4, 9)), flatdoc("n_is", vec_i(8, 7)),
                     flatdoc("n_is", dense ? vec_i(20, -10) : std::vector<int64_t>{}),
                     flatdoc("n_is", 5)}, UpdateMessage::COMMIT);
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    for (bool descending : {false, true}) {
      SimpleNumericFieldComparator comparator(descending, FieldComparator::MISSING_LAST, reader->field("n_is"));
      comparator.setSegment(reader->segments()[0]);
      std::array<int32_t, 4> docs{0, 1, 2, 3};
      std::array<int64_t, 4> keys;
      comparator.gatherKeys(docs, keys);
      auto expected = descending ? vec_i(9, 8, 20, 5) : vec_i(-4, 7, -10, 5);
      for (size_t i = 0; i < expected.size(); i++) {
        int64_t key = i == 2 && !dense ? INT64_MAX : descending ? ~expected[i] : expected[i];
        EXPECT_EQ(key, keys[i]);
        EXPECT_EQ(key, comparator.getDocValue((int32_t)i));
      }
    }
  }
}

TEST_F(MultiValuedNumericTest, floatingAndDateModesThroughMerge) {
  for (bool array : {false, true}) {
    SCOPED_TRACE(array);
    CollectionHelper helper;
    helper.clear();
    SchemaBuilder b;
    for (auto [name, type] : {std::pair{"d", api::FieldDef::FieldClass::DOUBLE},
                             {"f", api::FieldDef::FieldClass::FLOAT},
                             {"date", api::FieldDef::FieldClass::DATE}}) {
      auto& field = b.field(name);
      field.type = type;
      field.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
    }
    b.set(helper.collection());
    double inf = std::numeric_limits<double>::infinity();
    auto dv = vec(0.0, std::numeric_limits<double>::denorm_min(), -0.0,
                  -std::numeric_limits<double>::denorm_min(), inf, -inf, inf);
    auto fv = vec(0.0, (double)std::numeric_limits<float>::denorm_min(), -0.0,
                  -(double)std::numeric_limits<float>::denorm_min(), inf, -inf, inf);
    helper.index(flatdoc("d", dv, "f", fv, "date", vec_i(1000, -1000, 0, 1000)), UpdateMessage::COMMIT);
    helper.index(flatdoc("d", 7.0, "f", 7.0, "date", 7), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("d", std::vector<double>{}, "f", std::vector<double>{},
                         "date", std::vector<int64_t>{}), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id_s", "missing"), UpdateMessage::COMMIT);
    for (int pass = 0; pass < 2; pass++) {
      auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
      for (auto name : {"d", "f", "date"}) {
        auto& segment = reader->segments()[0];
        const auto& columnInfo = *reader->field(name).at(segment);
        IntColReader column(segment.postingsReader(), columnInfo);
        EXPECT_EQ(!array, column.multiSet());
        IntColReader::Iterator iter(column);
        ASSERT_EQ(0, iter.advance(0));
        std::vector<int64_t> expected;
        if (name == std::string_view("date")) expected = vec_i(1000, -1000, 0, 1000);
        else for (double value : name == std::string_view("d") ? dv : fv) {
          expected.push_back(name == std::string_view("d") ? doubleToSortableInt64(value)
              : (int64_t)floatToSortableInt32((float)value));
        }
        if (!array) {
          std::ranges::sort(expected);
          expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
        }
        auto [start, end] = column.getStartEndValueRank(iter.rank());
        ASSERT_EQ((int64_t)expected.size(), end - start);
        for (size_t i = 0; i < expected.size(); i++) EXPECT_EQ(expected[i], iter.values().valueAt(start + i));
        auto* arena = createArena();
        {
          MemPool pool;
          for (bool maximum : {false, true}) {
            std::string expr = std::string(maximum ? "max(" : "min(") + name + ")";
            auto* program = ValueExprParser({helper.collection().getSchema().get(), {}}, *arena).parse(expr);
            for (auto& seg : reader->segments()) {
              auto* bound = program->bind(pool, seg, program->resolveFields(pool, *reader));
              for (int32_t doc = 0; doc < seg.maxDoc(); doc++) {
                auto result = bound->evalPoint(doc, 0.0f);
                int32_t global = seg.base + doc;
                EXPECT_EQ(global < 2, result.valid);
                if (!result.valid) continue;
                if (result.type == ValueType::DOUBLE) {
                  EXPECT_EQ(global == 1 ? 7.0 : maximum ? inf : -inf, result.doubleValue);
                } else { EXPECT_EQ(global == 1 ? 7 : maximum ? 1000 : -1000, result.intValue); }
              }
            }
          }
        }
        releaseArena(arena);
      }
      if (pass == 0) {
        CollectionHelper::UpdateBuilder merge;
        merge.commit(true, 1);
        ASSERT_TRUE(helper.submit(merge).success);
      }
    }
  }
}
