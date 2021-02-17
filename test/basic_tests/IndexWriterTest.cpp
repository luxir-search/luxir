
#include <gtest/gtest.h>
#include <iostream>
#include <solux/index/Inverter.h>

#include "solux/index/IndexWriter.h"
#include "solux/search/IndexReader.h"
#include "test/SoluxTest.h"

using namespace std;
using namespace solux;

class IndexWriterTest : public SoluxTest {
public:
  std::string field = "text_w";
};


TEST_F(IndexWriterTest, singleSeg) {
  RAMDir dir;
  IndexWriter iw(dir);
  auto inverter = &iw.getInverter();
  auto segField = &inverter->getSegField(field);

  std::string doc1 = "now is the time for all good men";
  inverter->startDoc();
  inverter->index(*segField, doc1.data(), doc1.size());
  inverter->finishDoc();

  iw.flush();

  IndexReader r1(dir);
  ASSERT_EQ(1, r1.segments().size());
  ASSERT_EQ(1, r1.maxDoc());

  inverter = &iw.getInverter();
  segField = &inverter->getSegField(field);

  doc1 = "to come to the aid";
  inverter->startDoc();
  inverter->index(*segField, doc1.data(), doc1.size());
  inverter->finishDoc();
  doc1 = "of their country";
  inverter->startDoc();
  inverter->index(*segField, doc1.data(), doc1.size());
  inverter->finishDoc();

  iw.flush();

  IndexReader r2(dir);
  ASSERT_EQ(2, r2.segments().size());
  ASSERT_EQ(3, r2.maxDoc());
}
