#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "solux/api/build.h"
#include "solux/index/PointsWriter.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/PointsReader.h"
#include "solux/schema/Schema.h"
#include "solux/store/Directory.h"
#include "solux/util/NumericUtils.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

namespace {

using Point = PointsReader::Point;

template <class T>
T load(const char* ptr) {
  T value;
  memcpy(&value, ptr, sizeof(value));
  return value;
}

template <class Callback>
void withPointsOptions(std::span<const Point> expected,
                       PointsWriter::Options options, Callback&& callback) {
  RAMDir dir;
  auto file = dir.createFile("points");
  OutputStream out(file.get());
  out.streamNumber = 0;
  out.writeBytes("pre");
  PointsWriter writer(out, options);
  for (const auto& point : expected) writer.addPoint(point.value, point.docid);
  auto data = writer.finish();
  out.close();
  dir.finishFile(*file);

  auto inputFile = dir.openFile("points");
  InputStream input = inputFile->getInputStream();
  PointsReader reader(input, data.pointsLoc.offset(), data.pointsMetaOff);
  reader.validate();
  EXPECT_EQ(expected.size(), reader.pointCount());
  EXPECT_EQ(std::vector<Point>(expected.begin(), expected.end()), reader.readAll());
  callback(reader, data, input);
}

template <class Callback>
void withPoints(std::span<const Point> expected, uint16_t maxPointsPerLeaf,
                Callback&& callback) {
  withPointsOptions(
      expected, {.maxPointsPerLeaf = maxPointsPerLeaf},
      std::forward<Callback>(callback));
}

void expectRoundTrip(std::span<const Point> expected, uint16_t maxPointsPerLeaf) {
  withPoints(expected, maxPointsPerLeaf, [](const PointsReader&, const PointsWriter::Data&,
                                            const InputStream&) {});
}

std::vector<int32_t> emitOrdinals(const PointsReader& reader,
                                  uint64_t begin, uint64_t end) {
  std::vector<int32_t> docs;
  std::vector<uint32_t> scratch(reader.maxPointsPerLeaf());
  reader.emitOrdinalRange(begin, end, scratch,
      [&docs](int32_t doc) { docs.push_back(doc); },
      [&docs](int32_t runBegin, int32_t runEnd) {
        for (int32_t doc = runBegin; doc < runEnd; doc++) docs.push_back(doc);
      },
      [&docs](int32_t wordIndex, uint64_t word) {
        while (word != 0) {
          int32_t bit = (int32_t)std::countr_zero(word);
          docs.push_back(wordIndex * 64 + bit);
          word &= word - 1;
        }
      });
  return docs;
}

std::vector<Point> pointsFromColumn(PostingsReader& postingsReader,
                                    const SegFieldInfo& fieldInfo) {
  IntColReader column(postingsReader, fieldInfo);
  IntColReader::Iterator docs(column);
  std::vector<Point> points;
  for (int32_t docid = docs.next(); docid != IntColReader::ENDDOC; docid = docs.next()) {
    if (!column.multiValued()) {
      points.push_back({docs.value(), docid});
      continue;
    }
    auto [start, end] = column.getStartEndValueRank(docs.rank());
    for (int64_t rank = start; rank < end; rank++) {
      points.push_back({docs.values().valueAt(rank), docid});
    }
  }
  std::sort(points.begin(), points.end(), [](const Point& lhs, const Point& rhs) {
    return lhs.value < rhs.value || (lhs.value == rhs.value && lhs.docid < rhs.docid);
  });
  return points;
}

SegFieldInfo readFieldInfo(MemPool& pool, PostingsReader& postingsReader,
                           std::string_view field) {
  FieldReader fields(pool, postingsReader);
  if (!fields.seek(field)) throw std::runtime_error("missing test field");
  SegFieldInfo info;
  fields.readFieldInfo(info);
  return info;
}

} // namespace

class PointsIndexTest : public SoluxTest {};

TEST_F(PointsIndexTest, leafBoundariesAndDirectory) {
  std::vector<Point> points = {
    {-5, 0}, {1, 1}, {7, 2}, {7, 3}, {7, 4}, {9, 8}, {12, 9}
  };
  withPoints(points, 3, [&](const PointsReader& reader, const PointsWriter::Data& data,
                            const InputStream& input) {
    ASSERT_EQ(3u, reader.leafCount());
    EXPECT_EQ(PointsWriter::FLAG_LEAF_MAX, reader.flags());
    EXPECT_EQ(3, reader.maxPointsPerLeaf());
    EXPECT_EQ(3, reader.leafInfo(0).count);
    EXPECT_EQ(3, reader.leafInfo(1).count);
    EXPECT_EQ(1, reader.leafInfo(2).count);
    EXPECT_EQ(-5, reader.leafMin(0));
    EXPECT_EQ(7, reader.leafMax(0));
    EXPECT_EQ(7, reader.leafMin(1));
    EXPECT_EQ(9, reader.leafMax(1));
    EXPECT_EQ(12, reader.leafMin(2));
    EXPECT_EQ(12, reader.leafMax(2));
    EXPECT_EQ((uint64_t)data.pointsMetaOff, reader.leafFP(reader.leafCount()));

    const char* leaf = input.ptr((int64_t)data.pointsLoc.offset());
    EXPECT_EQ(3, load<uint16_t>(leaf + 0));
    EXPECT_EQ(PointsWriter::DOC_CONTIG, (uint8_t)leaf[2]);
    EXPECT_EQ(0, (uint8_t)leaf[3]);
    EXPECT_EQ(reader.leafInfo(0).valueFormat, (uint8_t)leaf[4]);
    EXPECT_EQ(0u, load<uint32_t>(leaf + 8));
    EXPECT_EQ(0u, load<uint32_t>(leaf + 12));
    EXPECT_EQ(-5, load<int64_t>(leaf + 16));
    EXPECT_EQ(reader.leafInfo(0).valueGcd, load<uint64_t>(leaf + 24));

    const char* meta = input.ptr((int64_t)(data.pointsLoc.offset() + data.pointsMetaOff));
    EXPECT_EQ(PointsWriter::MAGIC, load<uint32_t>(meta + 0));
    EXPECT_EQ(PointsWriter::VERSION, load<uint16_t>(meta + 4));
    EXPECT_EQ(PointsWriter::FORMAT_KIND, (uint8_t)meta[6]);
    EXPECT_EQ(0x11, (uint8_t)meta[7]);
    EXPECT_EQ(PointsWriter::FLAG_LEAF_MAX, load<uint32_t>(meta + 8));
    EXPECT_EQ(3u, load<uint32_t>(meta + 12));
    EXPECT_EQ(3, load<uint16_t>(meta + 16));
    EXPECT_EQ(0x1111, load<uint16_t>(meta + 18));
    EXPECT_EQ(points.size(), load<uint64_t>(meta + 20));
    uint64_t headerEnd = data.pointsLoc.offset() + data.pointsMetaOff
                       + PointsWriter::FIXED_HEADER_SIZE;
    uint64_t directoryStart = (headerEnd + 7) & ~(uint64_t)7;
    EXPECT_EQ(0u, directoryStart % 8);
    for (uint64_t off = headerEnd; off < directoryStart; off++) {
      EXPECT_EQ(0x11, (uint8_t)*input.ptr((int64_t)off));
    }
  });

  std::vector<Point> singleLeaf = {{4, 7}, {4, 8}, {11, 10}};
  withPoints(singleLeaf, PointsWriter::DEFAULT_MAX_POINTS_PER_LEAF,
             [](const PointsReader& reader, const PointsWriter::Data&, const InputStream&) {
    EXPECT_EQ(1u, reader.leafCount());
  });
}

TEST_F(PointsIndexTest, ordinalOfFirstAtLeast) {
  std::vector<Point> points = {
    {-5000, 0}, {-1000, 1}, {0, 2}, {0, 3}, {1000, 4}, {3000, 5}, {7000, 6}
  };
  withPoints(points, 3, [](const PointsReader& reader,
                            const PointsWriter::Data&, const InputStream&) {
    std::vector<uint32_t> residuals(reader.maxPointsPerLeaf());
    std::vector<int64_t> raw(reader.maxPointsPerLeaf());
    auto ordinal = [&](int64_t value) {
      return reader.ordinalOfFirstAtLeast(value, residuals, raw);
    };
    EXPECT_EQ(0u, ordinal(std::numeric_limits<int64_t>::min()));
    EXPECT_EQ(0u, ordinal(-5000));
    EXPECT_EQ(1u, ordinal(-4999));
    EXPECT_EQ(2u, ordinal(-999));
    EXPECT_EQ(2u, ordinal(0));
    EXPECT_EQ(4u, ordinal(1));
    EXPECT_EQ(6u, ordinal(7000));
    EXPECT_EQ(reader.pointCount(), ordinal(7001));
    EXPECT_EQ(reader.pointCount(), ordinal(std::numeric_limits<int64_t>::max()));
  });
}

TEST_F(PointsIndexTest, valueEncodingExtremes) {
  const int64_t i64min = std::numeric_limits<int64_t>::min();
  const int64_t i64max = std::numeric_limits<int64_t>::max();

  std::vector<Point> gcd63 = {{i64min, 0}, {0, 1}};
  withPoints(gcd63, 2, [](const PointsReader& reader, const PointsWriter::Data&,
                          const InputStream&) {
    EXPECT_EQ((uint64_t)1 << 63, reader.leafInfo(0).valueGcd);
    EXPECT_EQ(1, reader.leafInfo(0).valueFormat);
  });

  std::vector<Point> gcdMax = {{i64min, 0}, {i64max, 1}};
  withPoints(gcdMax, 2, [](const PointsReader& reader, const PointsWriter::Data&,
                           const InputStream&) {
    EXPECT_EQ(std::numeric_limits<uint64_t>::max(), reader.leafInfo(0).valueGcd);
    EXPECT_EQ(1, reader.leafInfo(0).valueFormat);
  });

  std::vector<Point> raw = {{i64min, 4}, {i64min + 1, 1}, {i64max, 2}};
  withPoints(raw, 3, [](const PointsReader& reader, const PointsWriter::Data&,
                        const InputStream&) {
    EXPECT_GT(reader.leafInfo(0).valueFormat, 32);
  });

  std::vector<int64_t> rawDoubleBits = {i64min, i64min + 1, i64max - 1, i64max};
  std::vector<Point> sortablePatterns;
  for (size_t i = 0; i < rawDoubleBits.size(); i++) {
    sortablePatterns.push_back({sortableDoubleBits(rawDoubleBits[i]), (int32_t)i});
  }
  std::sort(sortablePatterns.begin(), sortablePatterns.end(), [](const Point& lhs, const Point& rhs) {
    return lhs.value < rhs.value || (lhs.value == rhs.value && lhs.docid < rhs.docid);
  });
  expectRoundTrip(sortablePatterns, 3);
}

TEST_F(PointsIndexTest, docidCodecSelection) {
  std::vector<Point> points = {
    {1000, 3}, {2000, 4}, {3000, 5},
    {4000, 3}, {5000, 3}, {6000, 5},
    {7000, 5}, {8000, 9}, {9000, 17},
    {10'000, 0}, {11'000, std::numeric_limits<int32_t>::max()}
  };
  withPoints(points, 3, [](const PointsReader& reader, const PointsWriter::Data&,
                           const InputStream&) {
    EXPECT_EQ(PointsWriter::DOC_CONTIG, reader.leafInfo(0).docCodec);
    EXPECT_EQ(0, reader.leafInfo(0).docBytes);
    EXPECT_EQ(PointsWriter::DOC_FOR, reader.leafInfo(1).docCodec);
    EXPECT_EQ(2, reader.leafInfo(1).docBits);
    EXPECT_EQ(PointsWriter::DOC_BITSET, reader.leafInfo(2).docCodec);
    EXPECT_EQ(0, reader.leafInfo(2).docBits);
    EXPECT_EQ(8, reader.leafInfo(2).docBytes);
    EXPECT_EQ(PointsWriter::DOC_FOR, reader.leafInfo(3).docCodec);
    EXPECT_EQ(31, reader.leafInfo(3).docBits);
  });

  std::vector<Point> sameDocDuplicates = {{9, 17}, {9, 17}};
  withPoints(sameDocDuplicates, 2, [](const PointsReader& reader, const PointsWriter::Data&,
                                      const InputStream&) {
    EXPECT_EQ(PointsWriter::DOC_FOR, reader.leafInfo(0).docCodec);
    EXPECT_EQ(0, reader.leafInfo(0).docBits);
    EXPECT_EQ(0, reader.leafInfo(0).docBytes);
  });
}

TEST_F(PointsIndexTest, bitsetOrdinalWindowsAndOptions) {
  std::vector<Point> points;
  for (int32_t doc = 5; doc <= 150; doc++) {
    if (doc % 3 != 0 || doc == 63 || doc == 64
        || doc == 127 || doc == 128) {
      points.push_back({(int64_t)points.size() * 1000, doc});
    }
  }

  withPointsOptions(points, {.maxPointsPerLeaf = 64},
      [&](const PointsReader& reader, const PointsWriter::Data&,
          const InputStream&) {
    ASSERT_EQ(2u, reader.leafCount());
    EXPECT_EQ(PointsWriter::DOC_BITSET, reader.leafInfo(0).docCodec);
    EXPECT_EQ(PointsWriter::DOC_BITSET, reader.leafInfo(1).docCodec);
    EXPECT_GT(reader.leafInfo(0).valueGcd, 1);
    EXPECT_EQ(5u, reader.leafInfo(0).docBase);

    auto expectRange = [&](uint64_t begin, uint64_t end) {
      std::vector<int32_t> expected;
      for (uint64_t i = begin; i < end; i++) {
        expected.push_back(points[(size_t)i].docid);
      }
      EXPECT_EQ(expected, emitOrdinals(reader, begin, end));
    };
    expectRange(0, points.size());
    expectRange(7, points.size() - 7);
    expectRange(60, 68);

    auto ordinal = [&](int32_t doc) {
      return (uint64_t)(std::find_if(points.begin(), points.end(),
          [doc](const Point& point) { return point.docid == doc; })
          - points.begin());
    };
    expectRange(ordinal(64), ordinal(127));
    expectRange(ordinal(63), ordinal(128) + 1);
  });

  withPointsOptions(points,
      {.maxPointsPerLeaf = 128, .valueGcd = false},
      [](const PointsReader& reader, const PointsWriter::Data&,
         const InputStream&) {
    EXPECT_EQ(1u, reader.leafInfo(0).valueGcd);
    EXPECT_EQ(PointsWriter::DOC_BITSET, reader.leafInfo(0).docCodec);
  });
  withPointsOptions(points,
      {.maxPointsPerLeaf = 128, .bitsetDocids = false},
      [](const PointsReader& reader, const PointsWriter::Data&,
         const InputStream&) {
    EXPECT_GT(reader.leafInfo(0).valueGcd, 1);
    EXPECT_EQ(PointsWriter::DOC_FOR, reader.leafInfo(0).docCodec);
  });
}

TEST_F(PointsIndexTest, emptyInputRejectedWithoutWriting) {
  RAMDir dir;
  auto file = dir.createFile("points");
  OutputStream out(file.get());
  out.streamNumber = 0;
  out.writeBytes("prefix");
  size_t before = out.size();
  PointsWriter writer(out);
  EXPECT_THROW(writer.finish(), std::invalid_argument);
  EXPECT_EQ(before, out.size());
  out.close();
  dir.finishFile(*file);
}

TEST_F(PointsIndexTest, deepValidationIsExplicit) {
  std::vector<Point> expected = {{3, 0}, {7, 2}};
  withPoints(expected, 2, [](const PointsReader&, const PointsWriter::Data& data,
                             const InputStream& input) {
    const char* start = input.ptr(0);
    std::vector<char> corrupt(start, start + input.size());
    corrupt[data.pointsLoc.offset() + 5] = 0;
    InputStream corruptInput(corrupt.data(), corrupt.data() + corrupt.size());

    PointsReader reader(corruptInput, data.pointsLoc.offset(), data.pointsMetaOff);
    EXPECT_THROW(reader.validate(), std::runtime_error);
  });
}

// Large equal-value runs at a scale where integer_sort actually radix
// partitions (small inputs fall back to comparison sort, which hides tie
// bugs): a naive single-pass spreadsort with a tie-refining comparator
// leaves equal-value docids scrambled once radix bits are exhausted.
TEST_F(PointsIndexTest, sortPointsKeepsDocidOrderInLongTieRuns) {
  constexpr size_t N = 200'000;
  struct TestPoint {
    int64_t value;
    int32_t docid;
  };
  std::vector<TestPoint> points(N);
  uint64_t state = 0x9e3779b97f4a7c15ULL;
  for (size_t i = 0; i < N; i++) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    points[i] = {(int64_t)(state % 16) * 1'000'000'007LL, (int32_t)i};
  }
  auto reference = points;
  std::sort(reference.begin(), reference.end(),
            [](const TestPoint& a, const TestPoint& b) {
    return a.value < b.value || (a.value == b.value && a.docid < b.docid);
  });
  sortPointsByValueDocid(std::span<TestPoint>(points));
  for (size_t i = 0; i < N; i++) {
    ASSERT_EQ(points[i].value, reference[i].value) << i;
    ASSERT_EQ(points[i].docid, reference[i].docid) << i;
  }
}

TEST_F(PointsIndexTest, flushMatchesSingleAndMultiValuedColumns) {
  CollectionHelper helper;
  helper.clear();

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  api::FieldDef* fields = api::build::allocArray(def.fields, 2, arena);
  fields[0].name = "range_single";
  fields[0].field_class = api::FieldDef::FieldClass::INT;
  fields[0].index = api::FieldDef::IndexMode::RANGE;
  fields[1].name = "range_multi";
  fields[1].field_class = api::FieldDef::FieldClass::INT;
  fields[1].index = api::FieldDef::IndexMode::RANGE;
  fields[1].multi_valued = true;
  auto schema = Schema::fromProto(def, helper.collection().getSchema().get());
  helper.collection().setSchema(schema);

  std::vector<Doc> docs = {
    flatdoc("id_s", "a", "range_single", 30, "range_multi", vec_i(7, 7, 2)),
    flatdoc("id_s", "b"),
    flatdoc("id_s", "c", "range_single", -5, "range_multi", vec_i(9)),
    flatdoc("id_s", "d", "range_single", 30),
    flatdoc("id_s", "e", "range_multi", vec_i(-1, 2))
  };
  IndexResult result = helper.indexAll(docs, UpdateMessage::COMMIT);
  ASSERT_TRUE(result.success) << result.error_message;

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& postingsReader = reader->segments()[0].postingsReader();
  MemPool pool;
  for (std::string_view field : {"range_single", "range_multi"}) {
    SegFieldInfo info = readFieldInfo(pool, postingsReader, field);
    ASSERT_NE(0, info.pointsMetaOff);
    PointsReader points(postingsReader, info);
    points.validate();
    EXPECT_EQ(pointsFromColumn(postingsReader, info), points.readAll()) << field;
  }

  helper.clear();
}
