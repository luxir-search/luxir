#include "luxir/index/Inverter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include <vector>

#include "luxir/index/IntColWriter.h"

using namespace luxir;
using namespace luxir::test;

class IntColTest : public LuxirTest {
protected:



  void addIntFields(TestIndex& testIndex, std::vector<FieldAndValues>& fieldsValues) {
    std::vector<int64_t> docs;

    int idx = -1;
    for (auto& fv : fieldsValues) {
      idx++;
      fv.testField.startIndexing();
      auto id = fv.ids->intVal();
      if (id.has_value()) {
        docs.push_back((*id << 32) | idx);  // combination of docid and field number
      }
    }

    Inverter& inverter = testIndex.getInverter();

    std::make_heap(docs.begin(), docs.end(), std::greater());  // use std::greater() to turn this into a min heap

    while (docs.size() > 0) {
      // std::cout << "first=" << *docs.begin() << " last=" << docs.back() << std::endl;
      std::pop_heap(docs.begin(), docs.end(), std::greater());
      auto min = docs.back();
      docs.pop_back();
      // std::cout << "min=" << min << std::endl;

      auto doc = (int32_t)(min>>32);
      idx = (int32_t)min;


      auto val = fieldsValues[idx].values->intVal();
      if (val.has_value()) {
        inverter.setDoc(doc);
        fieldsValues[idx].testField.add(doc, *val);
        inverter.finishDoc();  // will we ever need this, or just remove it?

        auto nextDoc = fieldsValues[idx].ids->intVal();
        if (nextDoc.has_value()) {
          docs.push_back((*nextDoc << 32) | idx);
          std::push_heap(docs.begin(), docs.end(), std::greater());
        }
      }
    }
  }

  void verifyField(FieldAndValues& fv) {
    fv.ids->init();
    fv.values->init();
    fv.testField.startReading();

    for(;;) {
      auto seqId = fv.ids->intVal();
      auto seqVal = fv.values->intVal();
      if (!seqId.has_value() || !seqVal.has_value()) break;
      auto doc = fv.testField.nextDoc();
      auto val = fv.testField.val();
      EXPECT_EQ(seqId, doc);
      EXPECT_EQ(seqVal, val);
    }
    auto doc = fv.testField.nextDoc();
    EXPECT_EQ(doc, -1);  // for now...
  }

  void verifyIntFields(TestIndex& testIndex, std::vector<FieldAndValues>& fieldsValues) {
    unused(testIndex);
    for (auto& fv : fieldsValues) {
      verifyField(fv);
    }
  }

};


TEST_F(IntColTest, basic) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_i");
    f.startIndexing();
    f.add(0, 5);
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(5, f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }

  // now test sparse
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_i");
    f.startIndexing();
    f.add(100000, 5);
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(100000, f.nextDoc());
    ASSERT_EQ(5, f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }
}

// test min/max column metadata
TEST_F(IntColTest, minMaxValues) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_i");
  f.startIndexing();
  
  f.add(0, 100);
  f.add(1, -50);
  f.add(2, 200);
  f.add(3, 0);
  f.add(4, -100);
  f.add(5, 150);
  
  testIndex.flush();
  f.startReading();
  f.nextSegment();
  
  ASSERT_EQ(-100, f.colReader->getMin());
  ASSERT_EQ(200, f.colReader->getMax());
  
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(100, f.val());
  ASSERT_EQ(1, f.nextDoc());
  ASSERT_EQ(-50, f.val());
  ASSERT_EQ(2, f.nextDoc());
  ASSERT_EQ(200, f.val());
  ASSERT_EQ(3, f.nextDoc());
  ASSERT_EQ(0, f.val());
  ASSERT_EQ(4, f.nextDoc());
  ASSERT_EQ(-100, f.val());
  ASSERT_EQ(5, f.nextDoc());
  ASSERT_EQ(150, f.val());
  ASSERT_EQ(-1, f.nextDoc());
}

TEST_F(IntColTest, minMaxSingleValue) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_i");
  f.startIndexing();
  
  // Add a single value
  f.add(0, 42);
  
  testIndex.flush();
  f.startReading();
  f.nextSegment();
  
  // Min and max should both be 42
  ASSERT_NE(f.colReader, nullptr);
  ASSERT_EQ(42, f.colReader->getMin());
  ASSERT_EQ(42, f.colReader->getMax());
}

// Blocks whose value range crosses 2^31 (or the full int64 span) used to
// overflow the writer's signed 32-bit delta math; deltas are unsigned and
// both writer and readers must zero-extend them (see IntColWriter::addBlock).
TEST_F(IntColTest, wideRangeBlock) {
  // range just over 2^31, gcd 1: compressed 32-bit-delta path
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_i");
    f.startIndexing();
    f.add(0, 2000000000);
    f.add(1, -2000000000);
    f.add(2, 1500000001);  // odd value forces gcd=1
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(2000000000, f.val());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(-2000000000, f.val());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(1500000001, f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }

  // range exceeding int64: uncompressed 64-bit path
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_i");
    f.startIndexing();
    f.add(0, std::numeric_limits<int64_t>::max());
    f.add(1, std::numeric_limits<int64_t>::min());
    f.add(2, 7);
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(std::numeric_limits<int64_t>::max(), f.val());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(std::numeric_limits<int64_t>::min(), f.val());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(7, f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }

  // range exceeding int64 but compressible thanks to a large gcd (2^62):
  // delta * gcd exceeds int64 on decode and must be done in unsigned math
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_i");
    f.startIndexing();
    f.add(0, int64_t(1) << 62);
    f.add(1, -(int64_t(1) << 62));
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(int64_t(1) << 62, f.val());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(-(int64_t(1) << 62), f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }
}

TEST_F(IntColTest, linearPackFormatCorners) {
  auto check = [](std::span<const int64_t> expected) {
    RAMDir dir;
    auto file = dir.createFile("numeric");
    OutputStream out(file.get());
    out.writeStr("odd");
    IntColWriter writer(out);
    for (int64_t value : expected) writer.addInt64(value);
    auto data = writer.finish();
    out.close();
    dir.finishFile(*file);

    auto in = dir.openFile("numeric");
    InputStream input(in->getInputStream());
    IntColReader reader(
        input, data.columnLoc, data.columnMetaOff, data.numValues);
    EXPECT_EQ(reader.numValues(), (int64_t)expected.size());
    EXPECT_EQ(reader.numBlocks(),
              ((int64_t)expected.size() + IntColReader::BLOCK_SIZE - 1) /
                  IntColReader::BLOCK_SIZE);

    IntColReader::SparseValues sparse(reader);
    IntColReader::BulkValues bulk(reader);
    for (int64_t i = 0; i < (int64_t)expected.size(); i++) {
      EXPECT_EQ(sparse.valueAt(i), expected[(size_t)i]) << "rank=" << i;
      EXPECT_EQ(bulk.valueAt(i), expected[(size_t)i]) << "rank=" << i;
    }
    for (int64_t block = 0; block < reader.numBlocks(); block++) {
      int64_t start = block * IntColReader::BLOCK_SIZE;
      int64_t end = std::min<int64_t>(
          start + IntColReader::BLOCK_SIZE, expected.size());
      auto [minIt, maxIt] = std::minmax_element(
          expected.begin() + start, expected.begin() + end);
      NumBlockZone zone = reader.blockZone(block);
      EXPECT_EQ(zone.min, *minIt);
      EXPECT_EQ(zone.max, *maxIt);
    }
    constexpr int64_t kLeaf = NumColumnFormat::LEAF_ZONE_SIZE;
    EXPECT_EQ(reader.numLeafZones(),
              ((int64_t)expected.size() + kLeaf - 1) / kLeaf);
    for (int64_t leaf = 0; leaf < reader.numLeafZones(); leaf++) {
      int64_t start = leaf * kLeaf;
      int64_t end = std::min<int64_t>(start + kLeaf, expected.size());
      auto [minIt, maxIt] = std::minmax_element(
          expected.begin() + start, expected.begin() + end);
      NumBlockZone zone = reader.leafZone(leaf);
      EXPECT_EQ(zone.min, *minIt) << "leaf=" << leaf;
      EXPECT_EQ(zone.max, *maxIt) << "leaf=" << leaf;
    }
    return reader.blockInfo(0);
  };

  auto bits0 = check(std::array<int64_t, 1>{42});
  EXPECT_EQ(bits0.bits(), 0);
  EXPECT_EQ(bits0.scaledSlope, 0);

  auto flatWidth = [&](uint8_t bits) {
    int64_t max = (int64_t)((1ULL << bits) - 1);
    return check(std::array<int64_t, 5>{0, max, 1, max, 0});
  };
  EXPECT_EQ(flatWidth(1).bits(), 1);
  EXPECT_EQ(flatWidth(32).bits(), 32);
  EXPECT_EQ(flatWidth(33).bits(), 33);
  EXPECT_EQ(flatWidth(57).bits(), 57);

  auto raw = check(std::array<int64_t, 5>{
      0, (int64_t)(1ULL << 58), 1, (int64_t)(1ULL << 58), 0});
  EXPECT_EQ(raw.bits(), NumColumnFormat::RAW_BITS);

  auto gcd = check(std::array<int64_t, 5>{100, 190, 130, 160, 100});
  EXPECT_EQ(gcd.gcd, 30);
  EXPECT_EQ(gcd.scaledSlope, 0);

  std::array<int64_t, 257> rising;
  std::array<int64_t, 257> falling;
  for (int64_t i = 0; i < (int64_t)rising.size(); i++) {
    rising[(size_t)i] = 1000 + i * 17 + (i == 128 ? 3 : 0);
    falling[(size_t)i] = 1000 + ((int64_t)falling.size() - 1 - i) * 17
        + (i == 128 ? 3 : 0);
  }
  EXPECT_GT(check(rising).scaledSlope, 0);
  EXPECT_LT(check(falling).scaledSlope, 0);

  std::vector<int64_t> tail(IntColReader::BLOCK_SIZE + 17);
  for (int64_t i = 0; i < (int64_t)tail.size(); i++) {
    tail[(size_t)i] = i * 5 + i % 7;
  }
  check(tail);

  auto extremes = check(std::array<int64_t, 2>{
      std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::max()});
  EXPECT_EQ(extremes.gcd, std::numeric_limits<uint64_t>::max());

  auto wrapping = check(std::array<int64_t, 4>{
      std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::min() + 2,
      std::numeric_limits<int64_t>::min() + 3});
  EXPECT_EQ(wrapping.gcd, 1);
  EXPECT_EQ(wrapping.baseBits,
            (uint64_t)std::numeric_limits<int64_t>::max());

  // Counts hugging every leaf/frame/block boundary: the leaf-zone oracle in
  // check() proves the partial-leaf and partial-block geometry.
  std::vector<int64_t> ramp;
  for (int64_t count : {1, 127, 128, 511, 512, 513, 4095, 4096, 4097,
                        3 * 4096 + 700}) {
    ramp.resize((size_t)count);
    for (int64_t i = 0; i < count; i++) {
      ramp[(size_t)i] =
          (int64_t)(((uint64_t)i * 2654435761u) % 100000) - 50000;
    }
    check(ramp);
  }
}

TEST_F(IntColTest, wrappingBaseBitPattern) {
  // Literal normalized quotients q=[0,0,2], min=INT64_MIN, gcd=1. The fit
  // lowers the intercept to -1 and therefore needs an unsigned wrapping base.
  std::array<char, 16> payload{};
  LinearPack::Writer writer(payload.data(), 1);
  writer.append(1);
  writer.append(0);
  writer.append(1);
  writer.finish(false);

  NumBlockInfo info;
  info.setPayload(0, 1);
  info.baseBits = (uint64_t)std::numeric_limits<int64_t>::min() +
      std::numeric_limits<uint64_t>::max();
  info.gcd = 1;
  info.scaledSlope = 1 << NumColumnFormat::SLOPE_SHIFT;

  NumColumn column(payload.data(), (const char*)&info, 3);
  EXPECT_EQ(column.valueAt(0), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(column.valueAt(1), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(column.valueAt(2), std::numeric_limits<int64_t>::min() + 2);

  int64_t decoded[NumColumn::BULK_SIZE];
  uint32_t count;
  EXPECT_EQ(column.decodeFrame(1, decoded, count), 0);
  ASSERT_EQ(count, 3);
  EXPECT_EQ(decoded[0], std::numeric_limits<int64_t>::min());
  EXPECT_EQ(decoded[1], std::numeric_limits<int64_t>::min());
  EXPECT_EQ(decoded[2], std::numeric_limits<int64_t>::min() + 2);
}

TEST_F(IntColTest, wideRangeUnalignedAdvance) {
  // raw64 (>32-bit-residual) blocks decode 128-value sub-blocks. An advance
  // into the middle of a sub-block must copy from the 128-aligned base, not
  // the requested rank (regression: every value shifted by index minus base).
  TestIndex testIndex;
  TestField f(testIndex, "foo_i");
  f.startIndexing();
  const int32_t N = 400;
  auto expected = [](int32_t doc) {
    return (int64_t)doc * (int64_t(1) << 40) + doc % 3;  // gcd 1, range > 2^32
  };
  for (int32_t doc = 0; doc < N; doc++) f.add(doc, expected(doc));
  testIndex.flush();

  f.startReading();
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(expected(0), f.val());
  for (int32_t target : {5, 130, 131, 259, 300, 399}) {
    ASSERT_EQ(target, f.iter->advance(target));
    ASSERT_EQ(expected(target), f.iter->value()) << "doc " << target;
  }
}

TEST_F(IntColTest, basic2) {
  TestIndex testIndex;
  std::vector<FieldAndValues> fvs;
  fvs.emplace_back(FieldAndValues{TestField(testIndex, "foo_i"),
                                  std::make_unique<IntSeq>(0,4),
                                  std::make_unique<IntSeq>(0,4)
  });
  addIntFields(testIndex, fvs);
  testIndex.flush();
  testIndex.initReader();
  verifyIntFields(testIndex, fvs);
}

TEST_F(IntColTest, basicMerge) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_i");
  f.startIndexing();
  f.add(0, 5);
  testIndex.flush();
  f.startIndexing();
  f.add(0, 7);
  testIndex.flush();

  testIndex.iw->mergeSegments();

  f.startReading();

  ASSERT_EQ(2, testIndex.reader->maxDoc());
  ASSERT_EQ(1, testIndex.reader->segments().size());
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(5, f.val());
  ASSERT_EQ(1, f.nextDoc());
  ASSERT_EQ(7, f.val());
  ASSERT_EQ(-1, f.nextDoc());

  // test merging of fields that are only in one segment or another
  TestField f2(testIndex, "foo2_i");
  f2.startIndexing();
  f2.add(5, 77);

  testIndex.flush();
  testIndex.iw->mergeSegments();

  f2.startReading();
  ASSERT_EQ(8, testIndex.reader->maxDoc());
  ASSERT_EQ(1, testIndex.reader->segments().size());
  f.startReading();
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(5, f.val());
  ASSERT_EQ(1, f.nextDoc());
  ASSERT_EQ(7, f.val());
  ASSERT_EQ(-1, f.nextDoc());

  ASSERT_EQ(7, f2.nextDoc());
  ASSERT_EQ(77, f2.val());
  ASSERT_EQ(-1, f2.nextDoc());
}

TEST_F(IntColTest, rand) {
  TestIndex testIndex;

  std::vector<FieldAndValues> fvs;
  int nFields = 10;
  int maxDoc = 65536 * 10;

  for (int i=0; i<nFields; i++) {
    int maxGap=rng.rint(1000)+1;
    // TODO: test a variety of patterns on both docids and values
    fvs.emplace_back(FieldAndValues{TestField(testIndex, "field_"+std::to_string(i)+"_i"),
                      std::make_unique<IncreasingRandomInts>(rng(), 1000000000, 0, maxDoc, maxGap),
                      std::make_unique<RandomInts>(rng(), 1000)});
  }

  addIntFields(testIndex, fvs);
  testIndex.flush();
  testIndex.initReader();
  verifyIntFields(testIndex, fvs);
}

TEST_F(IntColTest, textLen) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, "now is the time");
    f.add(3, "");
    f.add(5, "hi");
    testIndex.flush();
    f.startReading();

    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(4, f.val());
    ASSERT_EQ(3, f.nextDoc());
    ASSERT_EQ(0, f.val());
    ASSERT_EQ(5, f.nextDoc());
    ASSERT_EQ(1, f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }

  // test for dense (all-docs-set)
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(0, "now is the time");
    f.add(1, "");
    f.add(2, "hi");
    testIndex.flush();
    f.startReading();

    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(4, f.val());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(0, f.val());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(1, f.val());
    ASSERT_EQ(-1, f.nextDoc());
  }


}

TEST_F(IntColTest, testMono) {
  {
    RAMDir dir;
    auto file = dir.createFile("mono");
    OutputStream out(file.get());
    MemPool pool;
    MonoWriter w(pool, out);
    w.addInt64(10);
    w.addInt64(25); // +5 over line
    w.addInt64(28); // -2 over line
    w.addInt64(40);
    // total range of 7 for deltas from expected means bits should be 3.
    int nVals = w.finish();
    out.close();
    dir.finishFile(*file);

    auto in = dir.openFile("mono");
    InputStream is(in->getInputStream());
    MonoReader r(is, w.blockLoc.offset(), w.metaOff, nVals);

    ASSERT_EQ(nVals, r.numValues());
    ASSERT_EQ(10, r.valueAt(0));
    ASSERT_EQ(25, r.valueAt(1));
    ASSERT_EQ(28, r.valueAt(2));
    ASSERT_EQ(40, r.valueAt(3));
  }

  // test a single value (special case because you can't take the slope)
  {
    RAMDir dir;
    auto file = dir.createFile("mono");
    OutputStream out(file.get());
    MemPool pool;
    MonoWriter w(pool, out);
    w.addInt64(123);
    // total range of 7 for deltas from expected means bits should be 3.
    int nVals = w.finish();
    out.close();
    dir.finishFile(*file);

    auto in = dir.openFile("mono");
    InputStream is(in->getInputStream());
    //   MonoReader(InputStream& columnIS, int64_t loc, int64_t metaOff, int64_t nValues) : nValues(nValues)

    MonoReader r(is, w.blockLoc.offset(), w.metaOff, nVals);

    ASSERT_EQ(nVals, r.numValues());
    ASSERT_EQ(123, r.valueAt(0));
  }
}

TEST_F(IntColTest, basicDelete) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_i");
  f.startIndexing();
  f.add(0, 10);
  f.add(1, 20);
  testIndex.deleteDoc(1);
  testIndex.flush();
  f.startReading();
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(10, f.val());
  ASSERT_EQ(-1, f.nextDoc());
}

TEST_F(IntColTest, testMonoRepeatedValues) {
  // Test case with repeated values [1, 1, 3]
  RAMDir dir;
  auto file = dir.createFile("mono");
  OutputStream out(file.get());
  MemPool pool;
  MonoWriter w(pool, out);
  
  // Add the problematic sequence
  w.addInt64(1);
  w.addInt64(1);
  w.addInt64(3);
  
  int nVals = w.finish();
  out.close();
  dir.finishFile(*file);
  
  auto in = dir.openFile("mono");
  InputStream is(in->getInputStream());
  MonoReader r(is, w.blockLoc.offset(), w.metaOff, nVals);
  
  ASSERT_EQ(nVals, r.numValues());
  ASSERT_EQ(1, r.valueAt(0)) << "First value should be 1";
  ASSERT_EQ(1, r.valueAt(1)) << "Second value should be 1";
  ASSERT_EQ(3, r.valueAt(2)) << "Third value should be 3";
}

// valuesAt resolves one descriptor for the adjacent pair, so the pair that
// straddles a block boundary takes a different path than the rest. Check every
// rank across three blocks; nothing else calls valuesAt directly.
TEST_F(IntColTest, monoValuesAtSpansBlockBoundaries) {
  RAMDir dir;
  auto file = dir.createFile("mono");
  OutputStream out(file.get());
  MemPool pool;
  MonoWriter writer(pool, out);

  const int64_t n = 2 * (int64_t)MonoReader::BLOCK_SIZE + 37;
  std::vector<int64_t> expected((size_t)n);
  int64_t acc = 0;
  for (int64_t i = 0; i < n; i++) {
    acc += 1 + i % 5;  // increasing with a non-constant step
    expected[(size_t)i] = acc;
    writer.addInt64(acc);
  }
  int64_t count = (int64_t)writer.finish();
  out.close();
  dir.finishFile(*file);

  auto in = dir.openFile("mono");
  InputStream input(in->getInputStream());
  MonoReader reader(input, writer.blockLoc.offset(), writer.metaOff, count);

  EXPECT_EQ(reader.valuesAt(0),
            std::make_pair((int64_t)0, expected[0]));
  for (int64_t i = 1; i < n; i++) {
    EXPECT_EQ(reader.valuesAt(i),
              std::make_pair(expected[(size_t)i - 1], expected[(size_t)i]))
        << "rank " << i;
  }
}

TEST_F(IntColTest, monoFractionalSlope) {
  RAMDir dir;
  auto file = dir.createFile("mono");
  OutputStream out(file.get());
  MemPool pool;
  MonoWriter writer(pool, out);

  std::vector<int64_t> expected(MonoReader::BLOCK_SIZE);
  for (int64_t i = 0; i < (int64_t)expected.size(); i++) {
    expected[(size_t)i] = i * 15 / 2;
    writer.addInt64(expected[(size_t)i]);
  }
  int64_t count = (int64_t)writer.finish();
  out.close();
  dir.finishFile(*file);

  auto in = dir.openFile("mono");
  InputStream input(in->getInputStream());
  NumBlockInfo info;
  memcpy(&info,
         input.ptr(writer.blockLoc.offset() + writer.metaOff), sizeof(info));
  EXPECT_GT(info.scaledSlope, 7 * (int64_t)(1LL << NumColumnFormat::SLOPE_SHIFT));
  EXPECT_LT(info.scaledSlope, 8 * (int64_t)(1LL << NumColumnFormat::SLOPE_SHIFT));
  EXPECT_LE(info.bits(), 1);

  MonoReader reader(
      input, writer.blockLoc.offset(), writer.metaOff, count);
  for (int64_t i = 0; i < count; i++) {
    EXPECT_EQ(reader.valueAt(i), expected[(size_t)i]);
  }
}

TEST_F(IntColTest, testMonoBig) {
  std::vector<int64_t> vals;

  for (int iter=0; iter<2; iter++) {
    vals.clear();
    auto nVals = rng.rint(1u,MonoReader::BLOCK_SIZE * 3 + 10);
    size_t bits;
    int64_t val = 0;
    int64_t maxVal = 0;

    RAMDir dir;
    auto file = dir.createFile("mono");
    OutputStream out(file.get());
    out.writeStr("SOMETHING");
    MemPool pool;
    MonoWriter w(pool, out);

    for (auto i = 0u; i < nVals; i++) {
      // when we go to a new block, pick a new max bit width
      if (i % MonoReader::BLOCK_SIZE == 0) {
        rng = Rng(rng());
        bits = rng.rint(0ul, sizeof(int32_t)+10);
        maxVal = 1 << bits;
      }
      int64_t delta = rng.rint(0l, maxVal);
      val += delta;
      vals.push_back(val);
      w.addInt64(val);
    }

    int outVals = w.finish();
    ASSERT_EQ(outVals, nVals);
    out.close();
    dir.finishFile(*file);

    auto in = dir.openFile("mono");
    InputStream is(in->getInputStream());
    MonoReader r(is, w.blockLoc.offset(), w.metaOff, nVals);

    // confirm stateless value retrieval
    for (auto i = 0u; i < nVals; i++) {
      ASSERT_EQ(vals[i], r.valueAt(i));
    }

    // confirm stateful (iterator) value retrieval
    MonoReader::BulkValues bulk(r);

    /*
    // temp stateful in-order valueAt
    for (auto i = 0u; i < nVals; i++) {
      ASSERT_EQ(vals[i], bulk.valueAt(i)) << "Mismatch at index " << i;
    }
    */

    for (auto i = 0u; i < nVals; i++) {
      bulk.advance(i);
      ASSERT_EQ(i, bulk.index());
      ASSERT_EQ(vals[i], bulk.value()) << "Mismatch at index " << i;
      ASSERT_EQ(vals[i], bulk.valueAt(i));
    }

    // confirm stateful (iterator) random access
    for (auto i = 0u; i < nVals; i++) {
      auto idx = rng.rint(0u, nVals);
      ASSERT_EQ(vals[idx], bulk.valueAt(idx)) << "Mismatch at index " << idx;
    }

  }
}
