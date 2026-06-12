#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include <vector>

#include "solux/index/IntColWriter.h"

using namespace solux;
using namespace solux::test;

class IntColTest : public SoluxTest {
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

TEST_F(IntColTest, testMonoBig) {
  std::vector<int64_t> vals;

  for (int iter=0; iter<2; iter++) {
    vals.clear();
    auto nVals = rng.rint(1u,MonoReader::BLOCK_SIZE * 3 + 10);
    size_t bits;
    int64_t val = 0;
    int64_t maxVal;

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
