
#include <gtest/gtest.h>
#include <iostream>

#include "solux/index/PostingsWriter.h"

TEST(PostingsWriter_test, test_basic) {
  RAMDir dir;
  MemPool pool;
  PostingsWriter writer(dir, "gen1");
  writer.startField("field1");

}
