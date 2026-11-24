// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <map>

#include "luxir/reader/IntColReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/geo.h"
#include "test/CollectionHelper.h"
#include "test/FieldRecordTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {
namespace {

using namespace field_parts;

TEST(FieldStatsTest, dictionaryEndpointModes) {
  RAMDir directory;
  PostingsWriter writer(directory, 1, 1);
  using Lower = DictionaryBounds::Lower;
  using Upper = DictionaryBounds::Upper;
  MemPool pool;
  for (size_t len : {0, 31, 32}) {
    std::string single(len, 'z');
    EXPECT_EQ((DictionaryBounds{PackedTerm(pool, single), PackedTerm(pool, single), Lower::EXACT, Upper::INCLUSIVE}),
              writer.dictionaryBounds(single, single, FieldType::TEXT));
  }
  std::string first(33, 'a');
  auto prefix = std::string(31, 'z');
  std::string last = prefix + std::string(10, '\xff');
  auto next = std::string(30, 'z') + '{';
  auto rounded = writer.dictionaryBounds(first, last, FieldType::TEXT);
  auto lower = first.substr(0, 32);
  first.front() = 'b';
  last.front() = 'y';
  EXPECT_EQ((DictionaryBounds{PackedTerm(pool, lower), PackedTerm(pool, next), Lower::ROUNDED, Upper::EXCLUSIVE}),
            rounded);
  EXPECT_EQ((DictionaryBounds{PackedTerm(pool, "a"), PackedTerm(nullptr), Lower::EXACT, Upper::UNBOUNDED}),
            writer.dictionaryBounds("a", std::string(40, '\xff'), FieldType::TEXT));
}

template <class T>
AxisBounds<T> extrema(const std::vector<T>& values) {
  auto [lo, hi] = std::ranges::minmax_element(values);
  return {*lo, *hi};
}

Bounds scanColumn(PostingsReader& postings, const DecodedFieldRecord& info) {
  const auto* part = info.numberColumn();
  if (!part || info.numValues == 0) return {};
  IntColReader column(postings, info);
  IntColReader::BulkValues read(column);
  std::vector<int64_t> values;
  for (int64_t rank = 0; rank < column.numValues(); ++rank) values.push_back(read.valueAt(rank));
  if (info.type == FieldType::GEO_POINT) {
    std::vector<int32_t> lat, lon;
    for (int64_t value : values) {
      lat.push_back(geo::unpackLatitude(value));
      lon.push_back(geo::unpackLongitude(value));
    }
    return GeoBounds{extrema(lat), extrema(lon)};
  }
  if (info.type == FieldType::INTERVAL) {
    std::vector<int64_t> low, high;
    for (size_t i = 0; i < values.size(); i += 2) {
      low.push_back(values[i]);
      high.push_back(values[i + 1]);
    }
    return IntervalBounds{extrema(low), extrema(high)};
  }
  return extrema(values);
}

void checkSegment(SegmentCore& core, Directory& directory, std::span<const Doc> docs,
                  bool reportSize = false) {
  struct Expected {
    int32_t docsWithField = 0;
    int64_t numValues = 0;
  };
  std::map<std::string_view, Expected> expected;
  for (const auto& doc : docs) {
    for (const auto& field : doc) {
      auto& count = expected[field.name];
      count.docsWithField++;
      if (field.name == "posting" || field.name == "empty_w") continue;
      // A vector counts as one value. An interval contributes two endpoints.
      count.numValues += field.name == "vec_v" ? 1 : std::visit([](const auto& value) -> int64_t {
        if constexpr (requires { value.size(); } && !std::is_same_v<std::decay_t<decltype(value)>, std::string>) {
          return (int64_t)value.size();
        } else {
          return 1;
        }
      }, field.val);
    }
  }
  // The stored resource covers every doc, including those with no stored values.
  if (expected.contains("empty_w")) expected["_stored_"].docsWithField = (int32_t)docs.size();
  PostingsWriter boundsWriter(directory, core.id(), core.epoch());
  auto& postings = core.rootReader();
  auto files = segmentFileSizes(directory, core.id(), core.epoch());
  for (int32_t ord = 0; ord < postings.numFields(); ++ord) {
    const auto& info = *postings.fieldInfo(ord);
    auto name = std::string_view(info.name);
    SCOPED_TRACE(name);
    EncodedFieldRecord got(info, postings.maxDoc());
    got.check(postings.maxDoc(), files, reportSize);
    auto count = expected.find(name);
    ASSERT_NE(expected.end(), count);
    EXPECT_EQ(count->second.docsWithField, got.field.header.docsWithField);
    EXPECT_EQ(count->second.numValues, got.field.header.numValues);
    expected.erase(count);

    Bounds bounds = scanColumn(postings, info);
    if (info.flags & FieldType::INDEX_DOCS) {
      MemPool pool;
      TermsEnum terms(pool, postings, info);
      IndexedTotals totals{};
      std::string first, last;
      while (terms.nextTerm()) {
        last = std::string_view(terms.term());
        if (totals.numTerms++ == 0) first = last;
        totals.sumDocFreq += terms.docFreq();
        totals.sumTotalTermFreq += terms.totalTermFreq();
      }
      EXPECT_EQ(totals, got.field.header.indexedTotals);
      if (totals.numTerms && !info.numberColumn()) {
        bounds = boundsWriter.dictionaryBounds(first, last, info.type);
      }
    } else {
      EXPECT_EQ(IndexedTotals{}, got.field.header.indexedTotals);
    }
    EXPECT_EQ(bounds, recordBounds(info));
  }
  EXPECT_TRUE(expected.empty());
}

class FieldStatsOutputTest : public ::testing::TestWithParam<bool> {};

TEST_P(FieldStatsOutputTest, flushAndMergeMatchPhysicalValues) {
  CollectionHelper helper;
  SchemaBuilder schema;
  using Class = api::FieldDef::FieldClass;
  using Index = api::FieldDef::IndexMode;
  auto& where = schema.field("where");
  where.type = Class::GEO_POINT;
  where.multi = api::FieldDef::MultiMode::ARRAY;
  where.index = GetParam() ? Index::RANGE : Index::NONE;
  auto& interval = schema.field("interval");
  interval.type = Class::LONG_RANGE;
  interval.index = where.index;
  auto& match = schema.field("num_match");
  match.type = Class::INT;
  match.index = Index::MATCH;
  auto& posting = schema.field("posting");
  posting.type = Class::STRING;
  posting.column = false;
  posting.multi = api::FieldDef::MultiMode::ARRAY;
  for (auto [name, type] : {std::pair{"scalar_int", Class::INT}, {"scalar_float", Class::FLOAT},
                            {"scalar_double", Class::DOUBLE}, {"scalar_date", Class::DATE},
                            {"scalar_bool", Class::BOOLEAN}, {"scalar_decimal", Class::DECIMAL}}) {
    auto& scalar = schema.field(name);
    scalar.type = type;
    scalar.column = true;
    scalar.index = where.index;
    if (type == Class::DECIMAL) scalar.scale = 2;
    if (type == Class::DATE) scalar.unit = api::FieldDef::DateUnit::NS;
  }
  schema.field("address").type = Class::IP;
  schema.set(helper.collection());
  auto writer = helper.getIndexWriter();
  writer->setMergeFactor(100);

  double inf = std::numeric_limits<double>::infinity();
  double nan = std::numeric_limits<double>::quiet_NaN();
  std::string longId(300, 'x');
  std::string longTag(33, 'm');
  using Geos = std::vector<std::vector<double>>;
  std::array firstDocs{
      flatdoc("id", "gone", "nums_is", vec_i(INT64_MIN, INT64_MAX),
              "float_ds", std::vector<double>{-inf, nan}, "num_match", INT64_MIN,
              "tag_s", "a", "where", Geos{{0, -80}}),
      flatdoc("id", longId, "nums_is", vec_i(-100, -1),
              "float_ds", std::vector<double>{-0.0, 0.0}, "zero_ds", std::vector<double>{-0.0, 0.0},
              "num_match", 50, "tag_s", longTag, "where", Geos{{179.5, 10}, {178.0, 12}},
              "posting", vecs("one", "two"), "empty_s", "", "empty_w", "", "raw_sc", "raw",
              "vec_v", std::vector<float>{1, 0}, "address", "2001:db8::1",
              "scalar_int", INT64_MIN, "scalar_float", -0.0f, "scalar_double", inf,
              "scalar_date", "2024-03-02T00:00:00Z", "scalar_bool", true, "scalar_decimal", "1.25"),
      flatdoc("id", "wide", "interval", vec_i(-50, 100)),
  };
  std::array secondDocs{
      flatdoc("id", "keep", "nums_is", vec_i(50, 75), "float_ds", std::vector<double>{inf},
              "num_match", 75, "tag_s", "z", "where", Geos{{-179.5, 20}}),
      flatdoc("id", "narrow", "interval", vec_i(10, 30)),
  };
  auto commit = [&](std::span<const Doc> docs) {
    CollectionHelper::UpdateBuilder update;
    for (const auto& doc : docs) {
      if (const auto* interval = find(doc, "interval")) {
        const auto& endpoints = std::get<std::vector<int64_t>>(*interval);
        const auto& id = std::get<std::string>(*find(doc, "id"));
        auto json = std::format(R"({{"id":"{}","interval":{{"gte":{},"lte":{}}}}})", id, endpoints[0], endpoints[1]);
        update.addRaw([&](api::Map& map, std::pmr::memory_resource& mr) {
          ASSERT_TRUE(api::read_json(map, json, mr));
        });
      } else {
        update.add(doc);
      }
    }
    update.commit();
    ASSERT_TRUE(helper.submit(update).success);
  };
  commit(firstDocs);
  commit(secondDocs);
  const std::array<std::span<const Doc>, 2> input{firstDocs, secondDocs};

  auto before = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, before->segments().size());
  for (auto& segment : before->segments()) {
    SegmentCore core(writer->dir, segment.segmentCore().id(), segment.segmentCore().epoch());
    checkSegment(core, writer->dir, input[segment.ord], segment.ord == 0);
  }
  const auto& firstCore = before->segments()[0].segmentCore();
  SegmentCore firstReopened(writer->dir, firstCore.id(), firstCore.epoch());
  EXPECT_EQ(Bounds(ScalarBounds{INT64_MIN, INT64_MAX}), recordBounds(*firstReopened.rootReader().fieldInfo("nums_is")));
  EXPECT_EQ(Bounds(ScalarBounds{doubleToSortableInt64(-inf), doubleToSortableInt64(nan)}),
            recordBounds(*firstReopened.rootReader().fieldInfo("float_ds")));
  EXPECT_EQ(Bounds(ScalarBounds{doubleToSortableInt64(-0.0), doubleToSortableInt64(0.0)}),
            recordBounds(*firstReopened.rootReader().fieldInfo("zero_ds")));
  EXPECT_EQ(2, firstReopened.rootReader().fieldInfo("where")->docsWithField);
  const auto* empty = firstReopened.rootReader().fieldInfo("empty_s")->dictionaryBounds();
  ASSERT_TRUE(empty);
  EXPECT_EQ(0u, empty->lower.size());
  EXPECT_EQ(0u, empty->upper.size());
  EXPECT_EQ(DictionaryBounds::Lower::EXACT, empty->lowerMode);
  EXPECT_EQ(DictionaryBounds::Upper::INCLUSIVE, empty->upperMode);
  for (auto [name, totals] : {std::pair{"empty_w", IndexedTotals{}}, {"posting", IndexedTotals{2, 2, 2}}}) {
    const auto& info = *firstReopened.rootReader().fieldInfo(name);
    EXPECT_EQ(1, info.docsWithField);
    EXPECT_EQ(0, info.numValues);
    EXPECT_EQ(totals, info.indexedTotals());
  }

  std::vector<std::string> removed{"gone", "wide"};
  ASSERT_TRUE(helper.deleteByIds(removed, UpdateMessage::COMMIT).success);
  auto deleted = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, deleted->segments().size());
  for (auto& segment : deleted->segments()) {
    SegmentCore core(writer->dir, segment.segmentCore().id(), segment.segmentCore().epoch());
    checkSegment(core, writer->dir, input[segment.ord]);
  }
  writer->mergeSegments();
  auto merged = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, merged->segments().size());
  auto& segment = merged->segments()[0];
  SegmentCore reopened(writer->dir, segment.segmentCore().id(), segment.segmentCore().epoch());
  std::vector<Doc> survivors;
  for (auto docs : input) {
    for (const auto& doc : docs) {
      const auto& id = std::get<std::string>(*find(doc, "id"));
      if (std::ranges::find(removed, id) == removed.end()) survivors.push_back(doc);
    }
  }
  checkSegment(reopened, writer->dir, survivors);
  EXPECT_EQ(Bounds(ScalarBounds{-100, 75}), recordBounds(*reopened.rootReader().fieldInfo("nums_is")));
  EXPECT_EQ(Bounds(ScalarBounds{doubleToSortableInt64(-0.0), doubleToSortableInt64(inf)}),
            recordBounds(*reopened.rootReader().fieldInfo("float_ds")));
  EXPECT_EQ(Bounds(GeoBounds{{geo::encodeLatitude(10), geo::encodeLatitude(20)},
                             {geo::encodeLongitude(-179.5), geo::encodeLongitude(179.5)}}),
            recordBounds(*reopened.rootReader().fieldInfo("where")));
  EXPECT_EQ(Bounds(IntervalBounds{{10, 10}, {30, 30}}), recordBounds(*reopened.rootReader().fieldInfo("interval")));
  EXPECT_EQ(GetParam(), segment.postingsReader().fieldInfo("where")->points() != nullptr);
  EXPECT_EQ(GetParam(), segment.postingsReader().fieldInfo("interval")->points() != nullptr);
  auto idRange = writer->segInfos.begin()->second->idRange.load();
  ASSERT_TRUE(idRange);
  const auto* bounds = reopened.rootReader().fieldInfo("id")->dictionaryBounds();
  ASSERT_TRUE(bounds);
  EXPECT_EQ(DictionaryBounds::Upper::INCLUSIVE, bounds->upperMode);
  EXPECT_EQ(PackedTerm::MAX_LEN, bounds->upper.size());
  EXPECT_EQ(bounds->lower, idRange->min);
  EXPECT_EQ(bounds->upper, idRange->max);
  MemPool pool;
  TermsEnum ids(pool, segment.postingsReader(), *segment.postingsReader().fieldInfo("id"));
  EXPECT_EQ(ids.numBlocks(), writer->segInfos.begin()->second->idDictionaryBlocks.load());
}

INSTANTIATE_TEST_SUITE_P(Points, FieldStatsOutputTest, ::testing::Bool());

} // namespace
} // namespace luxir::test
