// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/task_arena.h>

#include "luxir/index/BKDWriter.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/store/Directory.h"
#include "luxir/util/geo.h"
#include "luxir/util/random.h"
#include "test/LuxirTest.h"

using namespace luxir;

namespace {

using Point = BKDWriter::Point;

template <class Traits = BKDInt32Traits, class Callback>
void withBKD(std::span<const typename BasicBKDWriter<Traits>::Point> source,
             typename BasicBKDWriter<Traits>::Options options,
             Callback&& callback) {
  using Writer = BasicBKDWriter<Traits>;
  using Reader = BasicBKDReader<Traits>;
  using Point = typename Writer::Point;
  RAMDir dir;
  auto file = dir.createFile("bkd");
  OutputStream out(file.get());
  out.writeBytes("pre");
  std::vector<Point> working(source.begin(), source.end());
  Writer writer(out, options);
  auto data = writer.write(working);
  out.close();
  dir.finishFile(*file);

  auto inputFile = dir.openFile("bkd");
  auto hold = inputFile->acquire({0, inputFile->size()});
  InputStream input = hold.stream();
  Reader reader(input, data.pointsStart, data.pointsMetaOff, data.directoryBytes);
  reader.validate();
  callback(reader, data, input);
}

template <class Traits = BKDInt32Traits>
void expectRoundTrip(std::span<const typename BasicBKDWriter<Traits>::Point> points,
                     uint16_t leafSize = BasicBKDWriter<Traits>::DEFAULT_MAX_POINTS_PER_LEAF) {
  using Reader = BasicBKDReader<Traits>;
  using Writer = BasicBKDWriter<Traits>;
  using Scalar = typename Traits::Scalar;
  withBKD<Traits>(points, {.maxPointsPerLeaf = leafSize},
          [&](const Reader& reader, const typename Writer::Data&,
              const InputStream&) {
    std::vector<std::tuple<Scalar, Scalar, int32_t>> expected;
    std::vector<std::tuple<Scalar, Scalar, int32_t>> actual;
    for (const auto& point : points) {
      expected.emplace_back(point.dim0, point.dim1, point.docid);
    }
    for (const auto& point : reader.readAll()) {
      actual.emplace_back(point.dim0, point.dim1, point.docid);
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(expected, actual);
  });
}

template <class Traits = BKDInt32Traits>
std::vector<char> buildBytes(std::span<const typename BasicBKDWriter<Traits>::Point> points) {
  std::vector<char> bytes;
  withBKD<Traits>(points, {},
          [&](const auto&, const auto&, const InputStream& input) {
    bytes.assign(input.ptr(0), input.ptr(input.size()));
  });
  return bytes;
}

template <class Traits = BKDInt32Traits>
std::vector<int32_t> intersectDocs(const BasicBKDReader<Traits>& reader,
                                   auto&& relation) {
  std::vector<int32_t> docs;
  std::vector<uint32_t> docScratch(reader.maxPointsPerLeaf());
  std::vector<typename Traits::Unsigned> latScratch(reader.maxPointsPerLeaf());
  std::vector<typename Traits::Unsigned> lonScratch(reader.maxPointsPerLeaf());
  typename BasicBKDReader<Traits>::Scratch scratch{docScratch, latScratch, lonScratch};
  reader.intersect(relation, scratch,
      [&](int32_t doc) { docs.push_back(doc); },
      [&](int32_t begin, int32_t end) {
        for (int32_t doc = begin; doc < end; doc++) docs.push_back(doc);
      },
      [&](int32_t wordIndex, uint64_t word) {
        while (word != 0) {
          docs.push_back(wordIndex * 64 + (int32_t)std::countr_zero(word));
          word &= word - 1;
        }
      });
  std::sort(docs.begin(), docs.end());
  return docs;
}

struct CountingBox {
  BKDBoxRelation box;
  const std::set<std::array<int32_t, 4>>* tightBounds = nullptr;
  int32_t outside = 0;
  int32_t inside = 0;
  int32_t crosses = 0;
  int32_t tightOutside = 0;
  int32_t tightInside = 0;
  int32_t tightCrosses = 0;

  BKDRelation compare(int32_t minLat, int32_t maxLat,
                      int32_t minLon, int32_t maxLon) {
    BKDRelation result = box.compare(minLat, maxLat, minLon, maxLon);
    if (result == BKDRelation::OUTSIDE) outside++;
    else if (result == BKDRelation::INSIDE) inside++;
    else crosses++;
    if (tightBounds != nullptr
        && tightBounds->contains({minLat, maxLat, minLon, maxLon})) {
      if (result == BKDRelation::OUTSIDE) tightOutside++;
      else if (result == BKDRelation::INSIDE) tightInside++;
      else tightCrosses++;
    }
    return result;
  }

  bool matches(int32_t lat, int32_t lon) const {
    return box.matches(lat, lon);
  }
};

template <class Scalar>
struct PointBox {
  Scalar min0, max0, min1, max1;
  BKDRelation compare(Scalar lo0, Scalar hi0, Scalar lo1, Scalar hi1) const {
    if (hi0 < min0 || max0 < lo0 || hi1 < min1 || max1 < lo1) return BKDRelation::OUTSIDE;
    return min0 <= lo0 && hi0 <= max0 && min1 <= lo1 && hi1 <= max1
        ? BKDRelation::INSIDE : BKDRelation::CROSSES;
  }
  bool matches(Scalar v0, Scalar v1) const {
    return min0 <= v0 && v0 <= max0 && min1 <= v1 && v1 <= max1;
  }
};

uint32_t referenceLeftLeaves(uint32_t leaves) {
  uint32_t floorPower = std::bit_floor(leaves);
  return floorPower / 2
       + std::min(leaves - floorPower, floorPower / 2);
}

void referenceShape(uint32_t node, uint32_t leaves,
                    std::set<uint32_t>& inner, uint32_t& leafCount) {
  if (leaves == 1) {
    leafCount++;
    return;
  }
  inner.insert(node);
  uint32_t left = referenceLeftLeaves(leaves);
  referenceShape(node * 2, left, inner, leafCount);
  referenceShape(node * 2 + 1, leaves - left, inner, leafCount);
}

} // namespace

class BKDIndexTest : public LuxirTest {};

template <class Traits>
class BKDIndexTraitsTest : public LuxirTest {};
using BKDTypes = ::testing::Types<BKDInt32Traits, BKDInt64Traits>;
TYPED_TEST_SUITE(BKDIndexTraitsTest, BKDTypes);

TYPED_TEST(BKDIndexTraitsTest, roundTripDistributionsExtremesAndLeafBoundaries) {
  using Writer = BasicBKDWriter<TypeParam>;
  using Point = typename Writer::Point;
  using Scalar = typename TypeParam::Scalar;
  SplitMix64 rng(0x42c7e35a);
  std::vector<Point> random;
  for (int32_t doc = 0; doc < 1200; doc++) {
    random.push_back({(Scalar)rng(), (Scalar)rng(), doc});
  }
  expectRoundTrip<TypeParam>(random);

  std::vector<Point> clustered;
  for (int32_t doc = 0; doc < 1100; doc++) {
    int32_t cluster = doc % 4;
    clustered.push_back({cluster * 1000 + (int32_t)(rng() % 9),
                         cluster * -2000 + (int32_t)(rng() % 7), doc});
  }
  expectRoundTrip<TypeParam>(clustered);

  std::vector<Point> identical(1100, {17, -29, 0});
  for (int32_t doc = 0; doc < (int32_t)identical.size(); doc++) {
    identical[(size_t)doc].docid = doc;
  }
  expectRoundTrip<TypeParam>(identical);

  std::vector<Point> latitudeBand;
  for (int32_t doc = 0; doc < 1030; doc++) {
    latitudeBand.push_back({123, (Scalar)rng(), doc});
  }
  expectRoundTrip<TypeParam>(latitudeBand);

  const std::vector<Point> extremes = {
      {std::numeric_limits<Scalar>::min(), std::numeric_limits<Scalar>::min(), 0}, {std::numeric_limits<Scalar>::min(), std::numeric_limits<Scalar>::max(), 1},
      {std::numeric_limits<Scalar>::max(), std::numeric_limits<Scalar>::min(), 2}, {std::numeric_limits<Scalar>::max(), std::numeric_limits<Scalar>::max(), 3},
      {0, 0, 4}};
  expectRoundTrip<TypeParam>(extremes);

  std::vector<Point> singleLeaf(37);
  for (int32_t i = 0; i < (int32_t)singleLeaf.size(); i++) {
    singleLeaf[(size_t)i] = {i * 7, -i * 11, i};
  }
  expectRoundTrip<TypeParam>(singleLeaf);

  std::vector<Point> exactFull(1024);
  for (int32_t i = 0; i < (int32_t)exactFull.size(); i++) {
    exactFull[(size_t)i] = {i, -i, i};
  }
  expectRoundTrip<TypeParam>(exactFull);

  std::vector<Point> partialLast(1025);
  for (int32_t i = 0; i < (int32_t)partialLast.size(); i++) {
    partialLast[(size_t)i] = {i - 700, i * 3, i};
  }
  expectRoundTrip<TypeParam>(partialLast);

  std::vector<Point> duplicateDocs;
  for (int32_t i = 0; i < 100; i++) {
    duplicateDocs.push_back({i * 100, -i * 50, i % 9});
  }
  expectRoundTrip<TypeParam>(duplicateDocs, 8);
}

TYPED_TEST(BKDIndexTraitsTest, parallelBuildIsByteIdentical) {
  using Writer = BasicBKDWriter<TypeParam>;
  using Point = typename Writer::Point;
  using Scalar = typename TypeParam::Scalar;
  constexpr int32_t pointCount = 1 << 18;
  constexpr int32_t sectionSize = pointCount / 3;
  SplitMix64 rng(0xb7a23f91);
  std::vector<Point> points;
  points.reserve(pointCount);
  for (int32_t i = 0; i < pointCount; i++) {
    if (i < sectionSize) {
      points.push_back({(Scalar)rng(), (Scalar)rng(), i});
    } else if (i < sectionSize * 2) {
      int32_t duplicate = (i - sectionSize) / 2;
      points.push_back({(duplicate % 32) * 11, (duplicate % 17) * -19,
                        sectionSize + duplicate});
    } else {
      int32_t cluster = i % 6;
      points.push_back({cluster * 100000 + (int32_t)(rng() % 65),
                        cluster * -200000 + (int32_t)(rng() % 49), i});
    }
  }

  // A single-worker environment would compare two serial builds and prove
  // nothing; skip loudly rather than pass vacuously.
  if (oneapi::tbb::this_task_arena::max_concurrency() < 2) {
    GTEST_SKIP() << "needs >= 2 TBB workers to exercise the parallel build";
  }

  std::vector<char> serial;
  {
    oneapi::tbb::global_control control(
        oneapi::tbb::global_control::max_allowed_parallelism, 1);
    serial = buildBytes<TypeParam>(points);
  }
  for (int32_t threads : {2, 0}) {  // 0 = the default arena
    for (int32_t rep = 0; rep < 3; rep++) {
      std::vector<char> parallel;
      if (threads == 0) {
        parallel = buildBytes<TypeParam>(points);
      } else {
        oneapi::tbb::task_arena arena(threads);
        arena.execute([&] { parallel = buildBytes<TypeParam>(points); });
      }
      ASSERT_TRUE(serial == parallel)
          << "threads=" << threads << " rep=" << rep;
    }
  }
}

TYPED_TEST(BKDIndexTraitsTest, derivedTreeShapeAndEverythingTraversal) {
  using Writer = BasicBKDWriter<TypeParam>;
  using Point = typename Writer::Point;
  using Scalar = typename TypeParam::Scalar;
  for (uint32_t leafCount : {1U, 2U, 3U, 5U, 8U, 13U, 1000U}) {
    SCOPED_TRACE(leafCount);
    std::vector<Point> points(leafCount);
    for (uint32_t i = 0; i < leafCount; i++) {
      points[i] = {(int32_t)(i * 31), (int32_t)(i * 17), (int32_t)i};
    }
    withBKD<TypeParam>(points, {.maxPointsPerLeaf = 1},
            [&](const auto& reader, const auto&,
                const InputStream&) {
      std::set<uint32_t> expectedInner;
      uint32_t expectedLeaves = 0;
      referenceShape(1, leafCount, expectedInner, expectedLeaves);
      EXPECT_EQ(leafCount, expectedLeaves);
      EXPECT_EQ(leafCount - 1, expectedInner.size());
      for (uint32_t node = 1; node < leafCount; node++) {
        EXPECT_TRUE(expectedInner.contains(node));
        EXPECT_NO_THROW(reader.innerNode(node));
      }
      PointBox<Scalar> everything(std::numeric_limits<Scalar>::min(), std::numeric_limits<Scalar>::max(),
                                std::numeric_limits<Scalar>::min(), std::numeric_limits<Scalar>::max());
      std::vector<int32_t> docs = intersectDocs<TypeParam>(reader, everything);
      ASSERT_EQ(leafCount, docs.size());
      for (uint32_t i = 0; i < leafCount; i++) EXPECT_EQ((int32_t)i, docs[i]);
    });
  }
}

TYPED_TEST(BKDIndexTraitsTest, docidCodecSelection) {
  using Writer = BasicBKDWriter<TypeParam>;
  using Point = typename Writer::Point;
  std::vector<Point> contiguous;
  for (int32_t doc = 20; doc < 52; doc++) contiguous.push_back({doc, 0, doc});
  withBKD<TypeParam>(contiguous, {.maxPointsPerLeaf = 64},
          [](const auto& reader, const auto&,
             const InputStream&) {
    EXPECT_EQ(Writer::DOC_CONTIG, reader.leafInfo(0).docCodec);
  });

  std::vector<Point> sparse;
  for (int32_t i = 0; i < 32; i++) {
    sparse.push_back({i, i, i * 1000000});
  }
  withBKD<TypeParam>(sparse, {.maxPointsPerLeaf = 64},
          [](const auto& reader, const auto&,
             const InputStream&) {
    EXPECT_EQ(Writer::DOC_FOR, reader.leafInfo(0).docCodec);
  });

  std::vector<Point> denseBitset;
  for (int32_t i = 0; i < 64; i++) {
    denseBitset.push_back({i, -i, 1000 + i * 2});
  }
  withBKD<TypeParam>(denseBitset, {.maxPointsPerLeaf = 128},
          [](const auto& reader, const auto&,
             const InputStream&) {
    EXPECT_EQ(Writer::DOC_BITSET, reader.leafInfo(0).docCodec);
  });
}

TEST_F(BKDIndexTest, randomizedBoxOracleWrappedBoundsAndCount) {
  SplitMix64 rng(0xa21b9746);
  std::vector<Point> points;
  for (int32_t doc = 0; doc < 400; doc++) {
    int32_t lat = geo::encodeLatitude(-80.0 + (double)(rng() % 160000) / 1000.0);
    int32_t lon = geo::encodeLongitude(-180.0 + (double)(rng() % 360000) / 1000.0);
    points.push_back({lat, lon, doc});
    if (doc % 37 == 0) {
      points.push_back({lat / 2, geo::encodeLongitude(179.5), doc});
    }
  }

  withBKD(points, {.maxPointsPerLeaf = 16},
          [&](const BKDReader& reader, const BKDWriter::Data&,
              const InputStream&) {
    std::vector<BKDBoxRelation> boxes;
    boxes.emplace_back(INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX);
    boxes.emplace_back(geo::encodeLatitude(-10), geo::encodeLatitude(10),
                       geo::encodeLongitude(170), geo::encodeLongitude(-170));
    boxes.emplace_back(points[5].dim0, points[5].dim0,
                       points[5].dim1, points[5].dim1);
    boxes.emplace_back(geo::encodeLatitude(89), geo::encodeLatitude(89),
                       geo::encodeLongitude(0), geo::encodeLongitude(0));
    boxes.emplace_back(geo::encodeLatitude(-90), geo::encodeLatitude(90),
                       geo::encodeLongitude(179), geo::encodeLongitude(-179));
    for (int32_t i = 0; i < 100; i++) {
      int32_t latA = (int32_t)rng();
      int32_t latB = (int32_t)rng();
      int32_t lonA = (int32_t)rng();
      int32_t lonB = (int32_t)rng();
      if (latB < latA) std::swap(latA, latB);
      if ((i & 1) == 0 && lonA < lonB) std::swap(lonA, lonB);
      boxes.emplace_back(latA, latB, lonA, lonB);
    }

    int32_t outside = 0;
    int32_t inside = 0;
    int32_t crosses = 0;
    int32_t tightOutside = 0;
    int32_t tightInside = 0;
    int32_t tightCrosses = 0;
    std::set<std::array<int32_t, 4>> tightBounds;
    for (uint32_t leaf = 0; leaf < reader.leafCount(); leaf++) {
      tightBounds.insert({reader.leafMin0(leaf), reader.leafMax0(leaf),
                          reader.leafMin1(leaf), reader.leafMax1(leaf)});
    }
    for (const BKDBoxRelation& box : boxes) {
      std::vector<int32_t> expected;
      for (const Point& point : points) {
        if (box.matches(point.dim0, point.dim1)) expected.push_back(point.docid);
      }
      std::sort(expected.begin(), expected.end());

      CountingBox counted{box, &tightBounds};
      EXPECT_EQ(expected, intersectDocs(reader, counted));
      std::vector<uint32_t> latScratch(reader.maxPointsPerLeaf());
      std::vector<uint32_t> lonScratch(reader.maxPointsPerLeaf());
      BKDReader::Scratch scratch{{}, latScratch, lonScratch};
      EXPECT_EQ(expected.size(), reader.countIntersect(box, scratch).exactCount);
      outside += counted.outside;
      inside += counted.inside;
      crosses += counted.crosses;
      tightOutside += counted.tightOutside;
      tightInside += counted.tightInside;
      tightCrosses += counted.tightCrosses;
    }
    EXPECT_GT(outside, 0);
    EXPECT_GT(inside, 0);
    EXPECT_GT(crosses, 0);
    EXPECT_GT(tightOutside, 0);
    EXPECT_GT(tightInside, 0);
    EXPECT_GT(tightCrosses, 0);
  });
}

TEST_F(BKDIndexTest, insideSubtreeCountsFromFullLeaves) {
  SplitMix64 rng(0x5d1f02b3);
  // One leaf, an exact multiple of the leaf size, and a short final leaf
  // whose documents repeat (counts are points, not documents).
  for (int32_t size : {5, 128, 117}) {
    std::vector<Point> points;
    for (int32_t i = 0; i < size; i++) {
      points.push_back(
          {geo::encodeLatitude(-60.0 + (double)(rng() % 120000) / 1000.0),
           geo::encodeLongitude(-170.0 + (double)(rng() % 340000) / 1000.0),
           size == 117 ? i / 2 : i});
    }
    withBKD(points, {.maxPointsPerLeaf = 16},
            [&](const BKDReader& reader, const BKDWriter::Data&,
                const InputStream&) {
      for (uint32_t first = 0; first < reader.leafCount(); first++) {
        uint64_t sum = 0;
        for (uint32_t leaves = 1; first + leaves <= reader.leafCount();
             leaves++) {
          sum += reader.leafInfo(first + leaves - 1).count;
          EXPECT_EQ(sum, reader.leafRangePoints(first, leaves));
        }
      }

      std::vector<uint32_t> latScratch(reader.maxPointsPerLeaf());
      std::vector<uint32_t> lonScratch(reader.maxPointsPerLeaf());
      BKDReader::Scratch scratch{{}, latScratch, lonScratch};
      auto check = [&](auto&& relation) {
        uint64_t expected = 0;
        for (const Point& point : points) {
          expected += relation.matches(point.dim0, point.dim1);
        }
        EXPECT_EQ(expected, reader.countIntersect(relation, scratch).exactCount);
        auto estimate = reader.estimateIntersect(relation);
        EXPECT_LE(estimate.upperBound - estimate.crossingPoints, expected);
        EXPECT_GE(estimate.upperBound, expected);
        return estimate;
      };

      auto all = check(BKDBoxRelation(INT32_MIN, INT32_MAX, INT32_MIN, INT32_MAX));
      EXPECT_EQ((uint64_t)size, all.estimatedCount);
      EXPECT_EQ((uint64_t)size, all.upperBound);
      EXPECT_EQ(0U, all.crossingPoints);
      for (int32_t i = 0; i + 1 < size; i += 3) {
        const Point& a = points[i];
        const Point& b = points[size - 1 - i];
        check(BKDBoxRelation(std::min(a.dim0, b.dim0), std::max(a.dim0, b.dim0),
                             std::min(a.dim1, b.dim1), std::max(a.dim1, b.dim1)));
        double latA = geo::decodeLatitude(a.dim0);
        double lonA = geo::decodeLongitude(a.dim1);
        double radius = geo::haversinMeters(latA, lonA,
            geo::decodeLatitude(b.dim0), geo::decodeLongitude(b.dim1));
        check(BKDDistanceRelation(latA, lonA, radius));
        check(BKDDistanceRelation(latA, lonA, 4000000.0 + i * 100000.0));
      }
    });
  }
}

// Reopen a copy so corruption checks cannot change the writer's original file.
template <class Traits, class Data, class Callback>
void withBKDBytes(const std::vector<char>& bytes, const Data& data, Callback&& callback) {
  RAMDir dir;
  auto file = dir.createFile("copy");
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size());
  out.close();
  dir.finishFile(*file);
  auto inputFile = dir.openFile("copy");
  auto hold = inputFile->acquire({0, inputFile->size()});
  auto input = hold.stream();
  BasicBKDReader<Traits> reader(input, data.pointsStart, data.pointsMetaOff, data.directoryBytes);
  callback(reader);
}

TYPED_TEST(BKDIndexTraitsTest, coordinateWidths) {
  using Writer = BasicBKDWriter<TypeParam>;
  using Scalar = typename TypeParam::Scalar;
  using Unsigned = typename TypeParam::Unsigned;
  for (uint8_t bits : {0, 31, 32, 33, 57, 58, 63, 64}) {
    if (bits > std::numeric_limits<Unsigned>::digits) continue;
    SCOPED_TRACE((int)bits);
    Unsigned span = bits == std::numeric_limits<Unsigned>::digits
        ? std::numeric_limits<Unsigned>::max() : ((Unsigned)1 << bits) - 1;
    Scalar lo = std::numeric_limits<Scalar>::min();
    Scalar hi = (Scalar)((Unsigned)lo + span);
    for (uint16_t count : {1, 127, 128, 129, 513}) {
      std::vector<typename Writer::Point> points;
      for (int32_t i = 0; i < count; i++) points.push_back({i % 2 ? hi : lo, lo, i});
      expectRoundTrip<TypeParam>(points, 1024);
      withBKD<TypeParam>(points, {.maxPointsPerLeaf = 1024}, [&](const auto& reader, const auto&, const auto&) {
        EXPECT_EQ(count == 1 ? 0 : bits, reader.leafInfo(0).bits0);
        EXPECT_EQ(0, reader.leafInfo(0).bits1);
      });
    }
  }
}

TYPED_TEST(BKDIndexTraitsTest, coveredSubtreeCountsAndCorruption) {
  using Writer = BasicBKDWriter<TypeParam>;
  using Reader = BasicBKDReader<TypeParam>;
  using Scalar = typename TypeParam::Scalar;
  using Header = typename Writer::LeafHeader;
  std::vector<typename Writer::Point> points;
  for (int32_t i = 0; i < 39; i++) points.push_back({i, -i, i});
  withBKD<TypeParam>(points, {.maxPointsPerLeaf = 8}, [&](const auto& reader, const auto& data, const InputStream& input) {
    PointBox<Scalar> all{std::numeric_limits<Scalar>::min(), std::numeric_limits<Scalar>::max(),
                         std::numeric_limits<Scalar>::min(), std::numeric_limits<Scalar>::max()};
    EXPECT_EQ(5u, reader.leafCount());
    EXPECT_EQ(7u, reader.leafInfo(4).count);
    std::vector<typename TypeParam::Unsigned> dim0(8), dim1(8);
    typename Reader::Scratch scratch{{}, dim0, dim1};
    uint64_t count = reader.leafRangePoints(0, reader.leafCount());
    EXPECT_EQ(count, reader.countIntersect(all, scratch).exactCount);
    auto estimate = reader.estimateIntersect(all);
    EXPECT_EQ(count, estimate.estimatedCount);
    EXPECT_EQ(count, estimate.upperBound);
    EXPECT_EQ(0u, estimate.crossingPoints);

    auto corrupt = [&](size_t offset, const auto& value, bool covered = false) {
      std::vector<char> bytes(input.ptr(0), input.ptr(input.size()));
      memcpy(bytes.data() + offset, &value, sizeof(value));
      withBKDBytes<TypeParam>(bytes, data, [&](const auto& bad) {
        EXPECT_THROW(bad.validate(), std::runtime_error);
        if (covered) {
          EXPECT_EQ(count, bad.countIntersect(all, scratch).exactCount);
          EXPECT_EQ(count, bad.estimateIntersect(all).estimatedCount);
        }
      });
    };
    size_t leaf = data.pointsStart;
    corrupt(leaf + offsetof(Header, count), (uint16_t)0, true);
    corrupt(leaf + offsetof(Header, reserved), (uint16_t)0);
    corrupt(leaf + offsetof(Header, docCodec), (uint8_t)255);
    corrupt(leaf + offsetof(Header, bits0), (uint8_t)255);
    corrupt(leaf + offsetof(Header, bytes0), (uint32_t)0xffffffff);
    size_t directory = (leaf + data.pointsMetaOff + Writer::FIXED_HEADER_SIZE + 7) & ~(size_t)7;
    corrupt(directory + offsetof(typename Writer::InnerNode, reserved8), (uint8_t)0);
  });
}

TEST_F(BKDIndexTest, int64DimensionLengthsMustMatchBitWidths) {
  using Writer = BasicBKDWriter<BKDInt64Traits>;
  std::vector<Writer::Point> points{{INT64_MIN, INT64_MIN, 0}, {INT64_MAX, INT64_MAX, 1}};
  withBKD<BKDInt64Traits>(points, {}, [&](const auto&, const auto& data, const InputStream& input) {
    auto reject = [&](size_t offset, auto value) {
      std::vector<char> bytes(input.ptr(0), input.ptr(input.size()));
      memcpy(bytes.data() + data.pointsStart + offset, &value, sizeof(value));
      withBKDBytes<BKDInt64Traits>(bytes, data, [](const auto& reader) {
        EXPECT_THROW(reader.validate(), std::runtime_error);
      });
    };
    reject(offsetof(Writer::LeafHeader, bytes0), (uint32_t)16);
    reject(offsetof(Writer::LeafHeader, bits1), (uint8_t)32);
  });
}
