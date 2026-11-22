// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <limits>
#include <optional>

#include "luxir/index/IntColWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

std::optional<field_parts::ScalarBounds> readBounds(
    IndexReader::Segment& segment, std::string_view field) {
  const auto* info = segment.postingsReader().fieldInfo(field);
  const auto* bounds = info ? info->scalarBounds() : nullptr;
  if (!bounds) return std::nullopt;
  return *bounds;
}

} // namespace

class NumericColumnBoundsTest : public LuxirTest {};

TEST_F(NumericColumnBoundsTest, emptyWriterHasNoBounds) {
  RAMDir directory;
  auto file = directory.createFile("numeric");
  OutputStream output(file.get());
  IntColWriter writer(output);
  auto data = writer.finish();
  output.close();
  directory.finishFile(*file);

  EXPECT_TRUE(data.bounds.empty());
  EXPECT_EQ(0, data.numValues);
  EXPECT_EQ(0u, data.indexBytes);
  EXPECT_EQ(0, file->size());
}

TEST_F(NumericColumnBoundsTest, typedEndpointsDecodeFromRecord) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "lo", "wide_i", std::numeric_limits<int64_t>::min(),
                       "when_dt", int64_t{1000}, "price_f", -9.5f,
                       "weight_d", -1.25), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "hi", "wide_i", std::numeric_limits<int64_t>::max(),
                       "when_dt", int64_t{9000}, "price_f", 3.75f,
                       "weight_d", 8.5), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1, reader->segments().size());
  auto& segment = reader->segments()[0];

  auto integers = readBounds(segment, "wide_i");
  ASSERT_TRUE(integers);
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), integers->min);
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), integers->max);

  auto dates = readBounds(segment, "when_dt");
  ASSERT_TRUE(dates);
  EXPECT_EQ(1000, dates->min);
  EXPECT_EQ(9000, dates->max);

  auto floats = readBounds(segment, "price_f");
  ASSERT_TRUE(floats);
  EXPECT_FLOAT_EQ(-9.5f, sortableInt32ToFloat((int32_t)floats->min));
  EXPECT_FLOAT_EQ(3.75f, sortableInt32ToFloat((int32_t)floats->max));

  auto doubles = readBounds(segment, "weight_d");
  ASSERT_TRUE(doubles);
  EXPECT_DOUBLE_EQ(-1.25, sortableInt64ToDouble(doubles->min));
  EXPECT_DOUBLE_EQ(8.5, sortableInt64ToDouble(doubles->max));
}

TEST_F(NumericColumnBoundsTest, missingOnlySegmentBindsAsMissing) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "with", "price_i", 4), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "without"), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(2, reader->segments().size());
  EXPECT_FALSE(readBounds(reader->segments()[1], "price_i").has_value());

  google::protobuf::Arena* arena = createArena();
  ValueExprOptions options{helper.collection().getSchema().get(), {}};
  ValueProgram* program = ValueExprParser(options, *arena).parse("price_i");
  MemPool pool;
  auto bound = program->bind(pool, reader->segments()[1], program->resolveFields(pool, *reader));
  EXPECT_TRUE(bound->bounds(program->rootNode).alwaysMissing);
  EXPECT_FALSE(bound->evalPoint(0, 0.0f).valid);
  releaseArena(arena);
}

TEST_F(NumericColumnBoundsTest, mergeRecomputesDeletedExtrema) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "min", "price_i", -100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "keep", "price_i", 7), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "max", "price_i", 200), UpdateMessage::COMMIT);
  std::array<std::string, 2> deleted{"min", "max"};
  ASSERT_TRUE(helper.deleteByIds(deleted, UpdateMessage::COMMIT).success);
  helper.getIndexWriter()->mergeSegments();

  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1, reader->segments().size());
  auto bounds = readBounds(reader->segments()[0], "price_i");
  ASSERT_TRUE(bounds);
  EXPECT_EQ(7, bounds->min);
  EXPECT_EQ(7, bounds->max);
}
