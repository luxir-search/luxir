#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
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

template <class Callback>
void withBKD(std::span<const Point> source, BKDWriter::Options options,
             Callback&& callback) {
  RAMDir dir;
  auto file = dir.createFile("bkd");
  OutputStream out(file.get());
  out.streamNumber = 0;
  out.writeBytes("pre");
  std::vector<Point> working(source.begin(), source.end());
  BKDWriter writer(out, options);
  BKDWriter::Data data = writer.write(working);
  out.close();
  dir.finishFile(*file);

  auto inputFile = dir.openFile("bkd");
  InputStream input = inputFile->getInputStream();
  BKDReader reader(input, data.pointsLoc.offset(), data.pointsMetaOff);
  reader.validate();
  callback(reader, data, input);
}

void expectRoundTrip(std::span<const Point> points,
                     uint16_t leafSize = BKDWriter::DEFAULT_MAX_POINTS_PER_LEAF) {
  withBKD(points, {.maxPointsPerLeaf = leafSize},
          [&](const BKDReader& reader, const BKDWriter::Data&,
              const InputStream&) {
    std::vector<std::tuple<int32_t, int32_t, int32_t>> expected;
    std::vector<std::tuple<int32_t, int32_t, int32_t>> actual;
    for (const Point& point : points) {
      expected.emplace_back(point.lat, point.lon, point.docid);
    }
    for (const BKDReader::Point& point : reader.readAll()) {
      actual.emplace_back(point.lat, point.lon, point.docid);
    }
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    EXPECT_EQ(expected, actual);
  });
}

std::vector<char> buildBytes(std::span<const Point> points) {
  std::vector<char> bytes;
  withBKD(points, {},
          [&](const BKDReader&, const BKDWriter::Data&,
              const InputStream& input) {
    bytes.assign(input.ptr(0), input.ptr(input.size()));
  });
  return bytes;
}

std::vector<int32_t> intersectDocs(const BKDReader& reader,
                                   auto&& relation) {
  std::vector<int32_t> docs;
  std::vector<uint32_t> docScratch(reader.maxPointsPerLeaf());
  std::vector<uint32_t> latScratch(reader.maxPointsPerLeaf());
  std::vector<uint32_t> lonScratch(reader.maxPointsPerLeaf());
  BKDReader::Scratch scratch{docScratch, latScratch, lonScratch};
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

TEST_F(BKDIndexTest, roundTripDistributionsExtremesAndLeafBoundaries) {
  SplitMix64 rng(0x42c7e35a);
  std::vector<Point> random;
  for (int32_t doc = 0; doc < 1200; doc++) {
    random.push_back({(int32_t)rng(), (int32_t)rng(), doc});
  }
  expectRoundTrip(random);

  std::vector<Point> clustered;
  for (int32_t doc = 0; doc < 1100; doc++) {
    int32_t cluster = doc % 4;
    clustered.push_back({cluster * 1000 + (int32_t)(rng() % 9),
                         cluster * -2000 + (int32_t)(rng() % 7), doc});
  }
  expectRoundTrip(clustered);

  std::vector<Point> identical(1100, {17, -29, 0});
  for (int32_t doc = 0; doc < (int32_t)identical.size(); doc++) {
    identical[(size_t)doc].docid = doc;
  }
  expectRoundTrip(identical);

  std::vector<Point> latitudeBand;
  for (int32_t doc = 0; doc < 1030; doc++) {
    latitudeBand.push_back({123, (int32_t)rng(), doc});
  }
  expectRoundTrip(latitudeBand);

  const std::vector<Point> extremes = {
      {INT32_MIN, INT32_MIN, 0}, {INT32_MIN, INT32_MAX, 1},
      {INT32_MAX, INT32_MIN, 2}, {INT32_MAX, INT32_MAX, 3},
      {0, 0, 4}};
  expectRoundTrip(extremes);

  std::vector<Point> singleLeaf(37);
  for (int32_t i = 0; i < (int32_t)singleLeaf.size(); i++) {
    singleLeaf[(size_t)i] = {i * 7, -i * 11, i};
  }
  expectRoundTrip(singleLeaf);

  std::vector<Point> exactFull(1024);
  for (int32_t i = 0; i < (int32_t)exactFull.size(); i++) {
    exactFull[(size_t)i] = {i, -i, i};
  }
  expectRoundTrip(exactFull);

  std::vector<Point> partialLast(1025);
  for (int32_t i = 0; i < (int32_t)partialLast.size(); i++) {
    partialLast[(size_t)i] = {i - 700, i * 3, i};
  }
  expectRoundTrip(partialLast);

  std::vector<Point> duplicateDocs;
  for (int32_t i = 0; i < 100; i++) {
    duplicateDocs.push_back({i * 100, -i * 50, i % 9});
  }
  expectRoundTrip(duplicateDocs, 8);
}

TEST_F(BKDIndexTest, parallelBuildIsByteIdentical) {
  constexpr int32_t pointCount = 1 << 18;
  constexpr int32_t sectionSize = pointCount / 3;
  SplitMix64 rng(0xb7a23f91);
  std::vector<Point> points;
  points.reserve(pointCount);
  for (int32_t i = 0; i < pointCount; i++) {
    if (i < sectionSize) {
      points.push_back({(int32_t)rng(), (int32_t)rng(), i});
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
    serial = buildBytes(points);
  }
  for (int32_t threads : {2, 0}) {  // 0 = the default arena
    for (int32_t rep = 0; rep < 3; rep++) {
      std::vector<char> parallel;
      if (threads == 0) {
        parallel = buildBytes(points);
      } else {
        oneapi::tbb::task_arena arena(threads);
        arena.execute([&] { parallel = buildBytes(points); });
      }
      ASSERT_TRUE(serial == parallel)
          << "threads=" << threads << " rep=" << rep;
    }
  }
}

TEST_F(BKDIndexTest, derivedTreeShapeAndEverythingTraversal) {
  for (uint32_t leafCount : {1U, 2U, 3U, 5U, 8U, 13U, 1000U}) {
    SCOPED_TRACE(leafCount);
    std::vector<Point> points(leafCount);
    for (uint32_t i = 0; i < leafCount; i++) {
      points[i] = {(int32_t)(i * 31), (int32_t)(i * 17), (int32_t)i};
    }
    withBKD(points, {.maxPointsPerLeaf = 1},
            [&](const BKDReader& reader, const BKDWriter::Data&,
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
      BKDBoxRelation everything(INT32_MIN, INT32_MAX,
                                INT32_MIN, INT32_MAX);
      std::vector<int32_t> docs = intersectDocs(reader, everything);
      ASSERT_EQ(leafCount, docs.size());
      for (uint32_t i = 0; i < leafCount; i++) EXPECT_EQ((int32_t)i, docs[i]);
    });
  }
}

TEST_F(BKDIndexTest, docidCodecSelection) {
  std::vector<Point> contiguous;
  for (int32_t doc = 20; doc < 52; doc++) contiguous.push_back({doc, 0, doc});
  withBKD(contiguous, {.maxPointsPerLeaf = 64},
          [](const BKDReader& reader, const BKDWriter::Data&,
             const InputStream&) {
    EXPECT_EQ(BKDWriter::DOC_CONTIG, reader.leafInfo(0).docCodec);
  });

  std::vector<Point> sparse;
  for (int32_t i = 0; i < 32; i++) {
    sparse.push_back({i, i, i * 1000000});
  }
  withBKD(sparse, {.maxPointsPerLeaf = 64},
          [](const BKDReader& reader, const BKDWriter::Data&,
             const InputStream&) {
    EXPECT_EQ(BKDWriter::DOC_FOR, reader.leafInfo(0).docCodec);
  });

  std::vector<Point> denseBitset;
  for (int32_t i = 0; i < 64; i++) {
    denseBitset.push_back({i, -i, 1000 + i * 2});
  }
  withBKD(denseBitset, {.maxPointsPerLeaf = 128},
          [](const BKDReader& reader, const BKDWriter::Data&,
             const InputStream&) {
    EXPECT_EQ(BKDWriter::DOC_BITSET, reader.leafInfo(0).docCodec);
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
    boxes.emplace_back(points[5].lat, points[5].lat,
                       points[5].lon, points[5].lon);
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
      tightBounds.insert({reader.leafMinLat(leaf), reader.leafMaxLat(leaf),
                          reader.leafMinLon(leaf), reader.leafMaxLon(leaf)});
    }
    for (const BKDBoxRelation& box : boxes) {
      std::vector<int32_t> expected;
      for (const Point& point : points) {
        if (box.matches(point.lat, point.lon)) expected.push_back(point.docid);
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
