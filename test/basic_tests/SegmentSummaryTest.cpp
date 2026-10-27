// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Segment summary sidecar: the file format and what its bounds prove, and the
// summaries flush and merge write for every field of a segment.

#include <algorithm>
#include <gtest/gtest.h>
#include "luxir/index/IndexWriter.h"
#include "luxir/index/SegmentSummary.h"
#include "luxir/index/SummaryCollector.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/SegFieldInfo.h"
#include "luxir/search/IndexReader.h"
#include "luxir/store/Directory.h"
#include "luxir/util/geo.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Presence = SegmentSummary::Presence;

std::string_view viewOf(::hpp_proto::bytes_view bytes) {
  return std::string_view((const char*)bytes.data(), bytes.size());
}

SegFieldInfo fieldInfo(FieldType::Type type, int32_t flags, int32_t docs, int64_t values = 0, int64_t nTerms = 0) {
  SegFieldInfo info{};
  info.type = type;
  info.flags = flags;
  info.docsWithField = docs;
  info.numValues = values;
  info.nTerms = nTerms;
  return info;
}

std::shared_ptr<const SegmentSummary> roundTrip(SegmentSummaryBuilder& builder) {
  RAMDir dir;
  auto name = SegmentSummary::overlayFileName(7);
  auto descriptor = builder.write(dir, name, 7, 100, true);
  EXPECT_EQ(name, descriptor.name);
  auto summary = SegmentSummary::open(dir, name);
  EXPECT_NE(nullptr, summary);
  EXPECT_EQ(descriptor.size, summary->bytes());
  EXPECT_EQ(7u, summary->segId());
  EXPECT_EQ(100, summary->maxDoc());
  EXPECT_TRUE(summary->complete());
  return summary;
}

class SegmentSummaryTest : public LuxirTest {
protected:
  std::unique_ptr<CollectionHelper> helper;

  void SetUp() override {
    LuxirTest::SetUp();
    helper = std::make_unique<CollectionHelper>("summary");
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(100);
  }
  void TearDown() override { helper.reset(); }

  std::shared_ptr<IndexReader> reader() { return helper->getIndexWriter()->snapshots.readers.getReader(); }

  // Three segments: prices 1..3 with tag a, 11..13 with tag b, 21..23 with tag c.
  void indexThreeSegments() {
    for (int segment = 0; segment < 3; segment++) {
      std::vector<Doc> batch;
      for (int i = 0; i < 3; i++) {
        int price = segment * 10 + 1 + i;
        batch.push_back(flatdoc("id", std::to_string(price), "price_i", (int64_t)price,
                                "tag_s", std::string(1, (char)('a' + segment)),
                                "body_t", "common word" + std::to_string(segment)));
      }
      ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT).success);
    }
  }
};

} // namespace

TEST(SegmentSummary, numericBoundsAreInclusiveToTheInt64Extremes) {
  SegmentSummaryBuilder builder;
  FieldSummaryCollector price;
  price.addInt64(-5);
  price.addInt64(40);
  builder.add("price_i", fieldInfo(FieldType::INT, FieldType::COLUMN_STORED, 2, 2), price);
  FieldSummaryCollector extremes;
  extremes.addNumericBounds(INT64_MIN, INT64_MAX);
  builder.add("wide_is", fieldInfo(FieldType::INT, FieldType::COLUMN_STORED | FieldType::MULTI_VALUED, 1, 2),
              extremes);
  builder.add("gone_i", fieldInfo(FieldType::INT, FieldType::COLUMN_STORED, 0), FieldSummaryCollector{});
  auto summary = roundTrip(builder);

  EXPECT_EQ(nullptr, summary->field("missing_i"));
  const auto* record = summary->field("price_i");
  ASSERT_NE(nullptr, record);
  EXPECT_EQ((uint32_t)FieldType::INT, record->type);
  EXPECT_EQ((uint32_t)FieldType::COLUMN_STORED, record->flags);
  EXPECT_EQ(SegmentSummary::HAS_NUM_VALUES | SegmentSummary::HAS_NUMERIC, record->summary_flags);
  EXPECT_EQ(-5, record->num_min);
  EXPECT_EQ(40, record->num_max);
  EXPECT_EQ(Presence::MAYBE, SegmentSummary::mayIntersectNumeric(*record, 40, 50));
  EXPECT_EQ(Presence::MAYBE, SegmentSummary::mayIntersectNumeric(*record, -5, -5));
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*record, 41, INT64_MAX));
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*record, INT64_MIN, -6));
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*record, 10, 5));

  const auto* wide = summary->field("wide_is");
  ASSERT_NE(nullptr, wide);
  EXPECT_TRUE(wide->summary_flags & SegmentSummary::MULTI_VALUED);
  EXPECT_EQ(INT64_MIN, wide->num_min);
  EXPECT_EQ(INT64_MAX, wide->num_max);
  EXPECT_EQ(Presence::MAYBE, SegmentSummary::mayIntersectNumeric(*wide, INT64_MAX, INT64_MAX));
  EXPECT_EQ(Presence::MAYBE, SegmentSummary::mayIntersectNumeric(*wide, INT64_MIN, INT64_MIN));

  // A record with nothing written proves the field empty.
  const auto* gone = summary->field("gone_i");
  ASSERT_NE(nullptr, gone);
  EXPECT_TRUE(gone->summary_flags & SegmentSummary::EMPTY);
  EXPECT_FALSE(gone->summary_flags & SegmentSummary::HAS_NUMERIC);
  EXPECT_EQ(Presence::NO, SegmentSummary::mayIntersectNumeric(*gone, INT64_MIN, INT64_MAX));
}

TEST(SegmentSummary, termBoundsAndTruncation) {
  SegmentSummaryBuilder builder;
  FieldSummaryCollector tag;
  tag.setTermBounds("alpha", "gamma", 3);
  builder.add("tag_s", fieldInfo(FieldType::STRING, FieldType::INDEX_DOCS, 3, 0, 3), tag);
  // Bounds past 32 bytes: the lower one is cut, the upper one rounded up.
  std::string first(40, 'a');
  std::string last = std::string(31, 'z') + std::string(10, '\xff');
  FieldSummaryCollector cut;
  cut.setTermBounds(first, last, 2);
  builder.add("cut_s", fieldInfo(FieldType::STRING, FieldType::INDEX_DOCS, 2, 0, 2), cut);
  // An all-0xFF prefix has no successor: the upper bound is unbounded.
  FieldSummaryCollector open;
  open.setTermBounds("a", std::string(40, '\xff'), 2);
  builder.add("open_s", fieldInfo(FieldType::STRING, FieldType::INDEX_DOCS, 2, 0, 2), open);
  auto summary = roundTrip(builder);

  const auto* record = summary->field("tag_s");
  ASSERT_NE(nullptr, record);
  EXPECT_EQ(3u, record->n_terms);
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*record, "alpha"));
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*record, "beta"));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTerm(*record, "aaa"));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTerm(*record, "gamma2"));
  EXPECT_FALSE(SegmentSummary::boundsExcludePrefix(*record, "ga"));
  EXPECT_TRUE(SegmentSummary::boundsExcludePrefix(*record, "zz"));
  EXPECT_TRUE(SegmentSummary::boundsExcludePrefix(*record, "al\x01"));
  EXPECT_FALSE(SegmentSummary::boundsExcludeTermRange(*record, std::string_view("b"), true, std::string_view("c"), true));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTermRange(*record, std::string_view("gamma"), false, std::nullopt, true));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTermRange(*record, std::nullopt, true, std::string_view("alpha"), false));

  const auto* truncated = summary->field("cut_s");
  ASSERT_NE(nullptr, truncated);
  EXPECT_TRUE(truncated->summary_flags & SegmentSummary::TERM_BOUNDS_TRUNCATED);
  EXPECT_EQ(32u, truncated->term_min.size());
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*truncated, first));
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*truncated, last));
  // Truncated bounds only exclude what lies outside the truncated range.
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*truncated, std::string(39, 'a')));
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*truncated, std::string(33, 'z')));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTerm(*truncated, std::string(31, 'a')));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTerm(*truncated, "{"));

  const auto* unbounded = summary->field("open_s");
  ASSERT_NE(nullptr, unbounded);
  EXPECT_TRUE(unbounded->summary_flags & SegmentSummary::TERM_MAX_UNBOUNDED);
  EXPECT_FALSE(SegmentSummary::boundsExcludeTerm(*unbounded, std::string(50, '\xff')));
  EXPECT_TRUE(SegmentSummary::boundsExcludeTerm(*unbounded, "0"));
  EXPECT_EQ(std::nullopt, SegmentSummary::successor("\xff\xff"));
  EXPECT_EQ(std::string("b"), *SegmentSummary::successor("a\xff"));
}

TEST(SegmentSummary, geoBoxAndWrappingQueries) {
  SegmentSummaryBuilder builder;
  FieldSummaryCollector where;
  where.addGeoPacked(geo::encodePoint(10.0, 170.0));
  where.addGeoPacked(geo::encodePoint(20.0, 175.0));
  builder.add("where_p", fieldInfo(FieldType::GEO_POINT, FieldType::COLUMN_STORED, 2, 2), where);
  auto summary = roundTrip(builder);
  const auto* record = summary->field("where_p");
  ASSERT_TRUE(record->summary_flags & SegmentSummary::HAS_GEO_BOX);
  auto box = [&](double minLat, double maxLat, double minLon, double maxLon) {
    return SegmentSummary::mayIntersectGeo(*record, geo::encodeLatitude(minLat), geo::encodeLatitude(maxLat),
                                           geo::encodeLongitude(minLon), geo::encodeLongitude(maxLon));
  };
  EXPECT_EQ(Presence::MAYBE, box(15.0, 25.0, 160.0, 172.0));
  EXPECT_EQ(Presence::NO, box(30.0, 40.0, 160.0, 180.0));
  EXPECT_EQ(Presence::NO, box(10.0, 20.0, -10.0, 10.0));
  // A query box wrapping the antimeridian covers [165, 180] and [-180, -170].
  EXPECT_EQ(Presence::MAYBE, box(0.0, 30.0, 165.0, -170.0));
  EXPECT_EQ(Presence::NO, box(0.0, 30.0, 178.0, -170.0));
}

TEST(SegmentSummary, malformedImagesAreRejected) {
  RAMDir dir;
  EXPECT_EQ(nullptr, SegmentSummary::open(dir, "absent"));
  SegmentSummaryBuilder builder;
  builder.add("price_i", fieldInfo(FieldType::INT, FieldType::COLUMN_STORED, 1, 1), FieldSummaryCollector{});
  auto image = builder.encode(3, 1, true);
  EXPECT_NO_THROW(SegmentSummary::fromBytes(image));
  EXPECT_THROW(SegmentSummary::fromBytes(std::span(image).first(8)), std::runtime_error);
  auto corrupt = image;
  corrupt[0] = std::byte{'X'};
  EXPECT_THROW(SegmentSummary::fromBytes(corrupt), std::runtime_error);
  auto future = image;
  future[SegmentSummary::MAGIC.size()] = std::byte{2};
  EXPECT_THROW(SegmentSummary::fromBytes(future), std::runtime_error);
}

TEST_F(SegmentSummaryTest, everyFieldOfEverySegmentHasARecord) {
  indexThreeSegments();
  auto r = reader();
  ASSERT_EQ(3u, r->segments().size());
  for (const auto& segment : r->segments()) {
    const SegmentSummary* summary = segment.summary();
    ASSERT_NE(nullptr, summary) << segment.segInfo.seg_id;
    EXPECT_TRUE(summary->complete());
    EXPECT_EQ(segment.segInfo.seg_id, summary->segId());
    EXPECT_EQ(3, summary->maxDoc());
    auto& postings = segment.postingsReader();
    EXPECT_EQ((size_t)postings.numFields(), summary->fields().size());
    for (int32_t ordinal = 0; ordinal < postings.numFields(); ordinal++) {
      const auto& info = *postings.fieldInfo(ordinal);
      const auto* record = summary->field((std::string_view)info.fieldname);
      ASSERT_NE(nullptr, record) << (std::string_view)info.fieldname;
      EXPECT_EQ((uint32_t)info.type, record->type);
      EXPECT_EQ((uint32_t)info.flags, record->flags);
      EXPECT_EQ(info.typeFlags, record->type_flags);
      EXPECT_EQ((uint64_t)info.docsWithField, record->docs_with_field);
      EXPECT_EQ((uint64_t)info.nTerms, record->n_terms);
      EXPECT_EQ((uint64_t)info.sumDocFreq, record->sum_doc_freq);
      EXPECT_EQ((uint64_t)info.sumTotalTermFreq, record->sum_total_term_freq);
    }
    // Stored-only resources count as fields of the segment too.
    const auto* stored = summary->field(Postings::STORED_DEFAULT_RESOURCE);
    ASSERT_NE(nullptr, stored);
    EXPECT_TRUE(stored->flags & FieldType::STORED);
    EXPECT_FALSE(stored->summary_flags & SegmentSummary::EMPTY);

    const auto* price = summary->field("price_i");
    ASSERT_NE(nullptr, price);
    EXPECT_EQ(3u, price->docs_with_field);
    EXPECT_EQ(3u, price->num_values);
    EXPECT_EQ(2, price->num_max - price->num_min);
    const auto* tag = summary->field("tag_s");
    ASSERT_NE(nullptr, tag);
    EXPECT_TRUE(tag->summary_flags & SegmentSummary::HAS_TERM_BOUNDS);
    EXPECT_EQ(viewOf(tag->term_min), viewOf(tag->term_max));
    const auto* body = summary->field("body_t");
    ASSERT_NE(nullptr, body);
    EXPECT_EQ(2u, body->n_terms);
    EXPECT_EQ(6u, body->sum_total_term_freq);
    EXPECT_EQ(nullptr, summary->field("never_i"));
  }
}

TEST_F(SegmentSummaryTest, multiValuedBoundsCoverEveryValue) {
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "neg", "nums_is", vec_i(-100, -1)),
                                flatdoc("id", "high", "nums_is", vec_i(50, INT64_MAX)),
                                flatdoc("id", "min", "nums_is", vec_i(INT64_MIN)),
                                flatdoc("id", "none")},
                               UpdateMessage::COMMIT).success);
  auto r = reader();
  ASSERT_EQ(1u, r->segments().size());
  const auto* nums = r->segments()[0].summary()->field("nums_is");
  ASSERT_NE(nullptr, nums);
  EXPECT_TRUE(nums->summary_flags & SegmentSummary::MULTI_VALUED);
  EXPECT_TRUE(nums->summary_flags & SegmentSummary::HAS_NUMERIC);
  EXPECT_EQ(3u, nums->docs_with_field);
  EXPECT_EQ(5u, nums->num_values);
  EXPECT_EQ(INT64_MIN, nums->num_min);
  EXPECT_EQ(INT64_MAX, nums->num_max);
}

TEST_F(SegmentSummaryTest, nanosecondDatesKeepTheirTypeFlags) {
  SchemaBuilder schema;
  auto& stamp = schema.field("stamp");
  stamp.type = api::FieldDef::FieldClass::DATE;
  stamp.unit = api::FieldDef::DateUnit::NS;
  schema.set(helper->collection());
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "early", "stamp", (int64_t)1000),
                                flatdoc("id", "late", "stamp", (int64_t)5'000'000'000)},
                               UpdateMessage::COMMIT).success);
  auto r = reader();
  ASSERT_EQ(1u, r->segments().size());
  const auto& segment = r->segments()[0];
  const auto* record = segment.summary()->field("stamp");
  ASSERT_NE(nullptr, record);
  EXPECT_EQ((uint32_t)FieldType::DATE, record->type);
  EXPECT_NE(0u, record->type_flags);
  EXPECT_EQ(segment.postingsReader().fieldInfo("stamp")->typeFlags, record->type_flags);
  // Bounds are in the column's own unit.
  EXPECT_EQ(1000, record->num_min);
  EXPECT_EQ(5'000'000'000, record->num_max);
}

TEST_F(SegmentSummaryTest, mergeRecomputesSummaries) {
  indexThreeSegments();
  ASSERT_TRUE(helper->deleteByIds({"1", "23"}, UpdateMessage::COMMIT).success);
  {
    // Delete-only commits leave the summaries alone: bounds stay conservative.
    auto r = reader();
    ASSERT_EQ(3u, r->segments().size());
    const auto* first = r->segments()[0].summary()->field("price_i");
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(1, first->num_min);
    EXPECT_EQ(3u, first->docs_with_field);
  }
  helper->getIndexWriter()->mergeSegments();
  auto r = reader();
  ASSERT_EQ(1u, r->segments().size());
  const SegmentSummary* summary = r->segments()[0].summary();
  ASSERT_NE(nullptr, summary);
  EXPECT_EQ(7, summary->maxDoc());
  const auto* price = summary->field("price_i");
  ASSERT_NE(nullptr, price);
  EXPECT_EQ(2, price->num_min);
  EXPECT_EQ(22, price->num_max);
  EXPECT_EQ(7u, price->docs_with_field);
  const auto* tag = summary->field("tag_s");
  ASSERT_NE(nullptr, tag);
  EXPECT_EQ("a", viewOf(tag->term_min));
  EXPECT_EQ("c", viewOf(tag->term_max));
  EXPECT_EQ(3u, tag->n_terms);
  ASSERT_NE(nullptr, summary->field(Postings::STORED_DEFAULT_RESOURCE));
}

TEST_F(SegmentSummaryTest, sidecarIsAGenerationZeroOverlayListedInStats) {
  ASSERT_TRUE(helper->index(flatdoc("id", "1", "price_i", (int64_t)5), UpdateMessage::COMMIT).success);
  auto stats = helper->getIndexWriter()->stats(true);
  ASSERT_EQ(1u, stats.segmentStats.size());
  const auto& segment = stats.segmentStats[0];
  ASSERT_EQ(1u, segment.overlays.size());
  const auto& overlay = segment.overlays[0];
  EXPECT_EQ(SegmentSummary::OVERLAY_KIND, overlay.kind);
  EXPECT_EQ(SegmentSummary::OVERLAY_NAME, overlay.name);
  EXPECT_EQ(0u, overlay.gen);
  ASSERT_EQ(1u, overlay.files.size());
  EXPECT_EQ(SegmentSummary::overlayFileName(segment.segId), overlay.files[0]);
  EXPECT_EQ(Postings::getIndexFileNamePrefix(segment.segId) + "__summary_00_00", overlay.files[0]);
  EXPECT_EQ(reader()->segments()[0].summary()->bytes(), overlay.bytes);
}

TEST_F(SegmentSummaryTest, segmentWithoutSummaryHasNone) {
  indexThreeSegments();
  auto& writer = *helper->getIndexWriter();
  ASSERT_TRUE(writer.testDropSegmentOverlay(SegmentSummary::OVERLAY_NAME, 0));
  helper->commit();
  auto r = reader();
  ASSERT_EQ(3u, r->segments().size());
  size_t without = 0;
  for (const auto& segment : r->segments()) without += segment.summary() == nullptr;
  EXPECT_EQ(1u, without);
}

TEST_F(SegmentSummaryTest, deleteOnlyFlushWritesNoSummary) {
  ASSERT_TRUE(helper->indexAll({flatdoc("id", "1", "price_i", (int64_t)5), flatdoc("id", "2", "price_i", (int64_t)6)},
                               UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper->deleteByIds({"1"}, UpdateMessage::COMMIT).success);
  auto& writer = *helper->getIndexWriter();
  std::vector<Directory::FileInfo> files;
  writer.dir.listFiles(files);
  size_t summaries = 0;
  for (const auto& file : files) summaries += file.name.find("__summary_") != std::string::npos;
  EXPECT_EQ(writer.stats(true).segmentStats.size(), summaries);
}
