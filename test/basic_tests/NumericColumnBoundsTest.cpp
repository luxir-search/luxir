#include <gtest/gtest.h>

#include <limits>
#include <optional>

#include "luxir/index/IntColWriter.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

std::optional<IntColReader::EncodedBounds> readBounds(
    IndexReader::Segment& segment, std::string_view field) {
  MemPool pool;
  FieldReader fields(segment.postingsReader());
  if (!fields.seek(field)) return std::nullopt;
  SegFieldInfo info;
  fields.readFieldInfo(info);
  IntColReader column(segment.postingsReader(), info);
  return column.encodedBounds();
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

  auto inputFile = directory.openFile("numeric");
  InputStream input(inputFile->getInputStream());
  IntColReader reader(input, data.columnLoc, data.columnMetaOff, data.numValues);
  auto bounds = reader.encodedBounds();
  EXPECT_FALSE(bounds.hasValues);
}

TEST_F(NumericColumnBoundsTest, typedEndpointsDecodeFromTrailer) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "lo", "wide_i", std::numeric_limits<int64_t>::min(),
                       "when_dt", int64_t{1000}, "price_f", -9.5f,
                       "weight_d", -1.25), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "hi", "wide_i", std::numeric_limits<int64_t>::max(),
                       "when_dt", int64_t{9000}, "price_f", 3.75f,
                       "weight_d", 8.5), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1, reader->segments().size());
  auto& segment = reader->segments()[0];

  auto integers = readBounds(segment, "wide_i");
  ASSERT_TRUE(integers && integers->hasValues);
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), integers->min);
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), integers->max);

  auto dates = readBounds(segment, "when_dt");
  ASSERT_TRUE(dates && dates->hasValues);
  EXPECT_EQ(1000, dates->min);
  EXPECT_EQ(9000, dates->max);

  auto floats = readBounds(segment, "price_f");
  ASSERT_TRUE(floats && floats->hasValues);
  EXPECT_FLOAT_EQ(-9.5f, sortableInt32ToFloat((int32_t)floats->min));
  EXPECT_FLOAT_EQ(3.75f, sortableInt32ToFloat((int32_t)floats->max));

  auto doubles = readBounds(segment, "weight_d");
  ASSERT_TRUE(doubles && doubles->hasValues);
  EXPECT_DOUBLE_EQ(-1.25, sortableInt64ToDouble(doubles->min));
  EXPECT_DOUBLE_EQ(8.5, sortableInt64ToDouble(doubles->max));
}

TEST_F(NumericColumnBoundsTest, missingOnlySegmentBindsAsMissing) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "with", "price_i", 4), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "without"), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(2, reader->segments().size());
  EXPECT_FALSE(readBounds(reader->segments()[1], "price_i").has_value());

  google::protobuf::Arena* arena = createArena();
  ValueExprOptions options{helper.collection().getSchema().get(), {}};
  ValueProgram* program = ValueExprParser(options, *arena).parse("price_i");
  MemPool pool;
  auto bound = program->bind(pool, reader->segments()[1]);
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

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1, reader->segments().size());
  auto bounds = readBounds(reader->segments()[0], "price_i");
  ASSERT_TRUE(bounds && bounds->hasValues);
  EXPECT_EQ(7, bounds->min);
  EXPECT_EQ(7, bounds->max);
}
