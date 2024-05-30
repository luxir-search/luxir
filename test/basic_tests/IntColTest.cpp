#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include <vector>

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

  // TODO: force reopen of IndexReader since that is what mergeSegments uses?
  testIndex.iw->mergeSegments();

  f.startReading();

  ASSERT_EQ(2, testIndex.reader->numDocs());
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
  ASSERT_EQ(8, testIndex.reader->numDocs());
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
    MonoReader r(pool, is, w.metaLoc.offset(), w.blockLoc.offset(), nVals);

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
    MonoReader r(pool, is, w.metaLoc.offset(), w.blockLoc.offset(), nVals);

    ASSERT_EQ(nVals, r.numValues());
    ASSERT_EQ(123, r.valueAt(0));
  }
}

TEST_F(IntColTest, testMonoBig) {
  for (int iter=0; iter<1; iter++) {
    RAMDir dir;
    auto file = dir.createFile("mono");
    OutputStream out(file.get());
    MemPool pool;
    MonoWriter w(pool, out);
    int32_t deltaMax = rng() & std::numeric_limits<int32_t>::max();
    switch (rng.rint(4)) {
      case 0:
        deltaMax &= 0xff;
        break;
      case 1:
        deltaMax &= 0xfff;
        break;
      case 2:
        deltaMax &= 0xfffff;
        break;
      default:
        break;
    }
    if (deltaMax==0) deltaMax=1;
    auto num = rng.rint(MonoReader::BLOCK_SIZE * 3);
    if (rng.rint(100) < 20) {
      num = MonoReader::BLOCK_SIZE * rng.rint(1,3) + rng.rint(3)-1;  // sometimes test exactly block size +-1
    }

    Rng rand = rng;

    for (auto i = 0u; i < num; i++) {
      w.addInt64(rand.rint(deltaMax));
    }

    int nVals = w.finish();
    out.close();
    dir.finishFile(*file);

    auto in = dir.openFile("mono");
    InputStream is(in->getInputStream());
    MonoReader r(pool, is, w.metaLoc.offset(), w.blockLoc.offset(), nVals);

    rand = rng;  // reset the rng so we can produce the same sequence of numbers.
    for (auto i = 0u; i < num; i++) {
      ASSERT_EQ(rand.rint(deltaMax), r.valueAt(i));
    }
  }
}