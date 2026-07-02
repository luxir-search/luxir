#include <gtest/gtest.h>

#include <string>

#include "solux/index/Inverter.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

// Inverter::memSize() must account for RAM held OUTSIDE the inverter's pool -
// the heap term-hash tables, IdHandler's private idPool, and string-column
// RAMFiles - via the extraRamBytes counter handlers bump at their allocation
// sites. Without this, memSize()==pool.size() badly under-counts a real load.
class InverterMemSizeTest : public SoluxTest {};

// A text field's term-hash table lives on the heap (outside inverter.pool);
// indexing many distinct terms must grow extraRamBytes and lift memSize above
// pool.size().
TEST_F(InverterMemSizeTest, textTermHashAccounted) {
  TestIndex idx;
  TestField f(idx, "foo_w");
  f.startIndexing();
  Inverter& inv = idx.getInverter();
  ASSERT_EQ(0u, inv.extraRamBytes);
  ASSERT_EQ(inv.pool.size(), inv.memSize());

  for (int i = 0; i < 4000; i++) {
    f.add(i, "term" + std::to_string(i) + " shared common words here");
  }

  EXPECT_GT(inv.extraRamBytes, 0u) << "term-hash table not accounted";
  EXPECT_GT(inv.memSize(), inv.pool.size());
  EXPECT_EQ(inv.pool.size() + inv.extraRamBytes, inv.memSize());
}

// IdHandler owns a private idPool (not inverter.pool); indexing many ids must
// grow extraRamBytes even though almost nothing lands in inverter.pool.
TEST_F(InverterMemSizeTest, idPoolAccounted) {
  TestIndex idx;
  TestField f(idx, "id");
  f.startIndexing();
  Inverter& inv = idx.getInverter();

  size_t before = inv.extraRamBytes;
  for (int i = 0; i < 4000; i++) {
    f.add(i, "id" + std::to_string(i));
  }

  EXPECT_GT(inv.extraRamBytes, before) << "IdHandler idPool not accounted";
}

// A string column stores its values in a RAMFile outside inverter.pool.
TEST_F(InverterMemSizeTest, strColumnRamFileAccounted) {
  TestIndex idx;
  TestField f(idx, "foo_sc");  // _sc suffix = string column
  f.startIndexing();
  Inverter& inv = idx.getInverter();

  size_t before = inv.extraRamBytes;
  for (int i = 0; i < 4000; i++) {
    f.add(i, "column value payload number " + std::to_string(i));
  }

  EXPECT_GT(inv.extraRamBytes, before) << "string-column RAMFile not accounted";
}

// memSize grows monotonically as more docs are indexed (sanity: the counter
// tracks growth, not just a one-time bump).
TEST_F(InverterMemSizeTest, growsMonotonically) {
  TestIndex idx;
  TestField f(idx, "foo_w");
  f.startIndexing();
  Inverter& inv = idx.getInverter();

  size_t prev = inv.memSize();
  for (int batch = 0; batch < 5; batch++) {
    for (int i = 0; i < 2000; i++) {
      int docid = batch * 2000 + i;
      f.add(docid, "distinct" + std::to_string(docid) + " and some shared words");
    }
    size_t now = inv.memSize();
    EXPECT_GE(now, prev) << "memSize shrank at batch " << batch;
    prev = now;
  }
}
