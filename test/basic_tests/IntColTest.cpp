#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
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
    EXPECT_EQ(doc, screaming::BitSet::END);
  }

  void verifyIntFields(TestIndex& testIndex, std::vector<FieldAndValues>& fieldsValues) {
    unused(testIndex);
    for (auto& fv : fieldsValues) {
      verifyField(fv);
    }
  }

};


TEST_F(IntColTest, basic) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_i");
  f.startIndexing();
  f.add(0, 5);
  testIndex.flush();
  f.startReading();
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(5, f.val());
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