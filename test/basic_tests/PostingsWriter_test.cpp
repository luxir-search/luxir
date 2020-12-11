
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/PostingsWriter.h"


TEST(PostingsWriter, test_basic) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter writer(dir, "gen1");
  writer.startField("field1");
  std::string t1 = "term1";
  TermRef term1(pool,t1.data(),t1.size());
  writer.startTerm(term1);
  writer.startDoc(7);
  writer.addPositionDelta(5);
  writer.addPositionDelta(3);
  writer.addPositionDelta(10);
  writer.endDoc(7);
  writer.endTerm(term1);
  writer.endField("field1");
  writer.finish();
}
