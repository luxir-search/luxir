#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "solux/search/DocSet.h"

namespace solux::test {

class DocSetTest : public SoluxTest {
public:
  static std::vector<int32_t> collect(DocSet& d, int32_t maxDoc) {
    std::vector<int32_t> out;
    for (int32_t i = 0; i < maxDoc; i++) {
      if (d.get(i)) out.push_back(i);
    }
    return out;
  }
};

TEST_F(DocSetTest, unionTwoArrays) {
  ArrDocSet a({1, 3, 5, 7});
  ArrDocSet b({2, 3, 6, 7, 9});
  std::array<DocSet*, 2> sets{&a, &b};
  auto u = DocSet::union_(sets);
  ASSERT_EQ(u->type, DocSet::ARRAY);
  EXPECT_EQ(((ArrDocSet*)u.get())->docs().size(), 7u);
  EXPECT_EQ(collect(*u, 10), (std::vector<int32_t>{1, 2, 3, 5, 6, 7, 9}));
}

TEST_F(DocSetTest, unionThreeArrays) {
  ArrDocSet a({1, 4});
  ArrDocSet b({2, 4, 8});
  ArrDocSet c({3, 4, 9});
  std::array<DocSet*, 3> sets{&a, &b, &c};
  auto u = DocSet::union_(sets);
  ASSERT_EQ(u->type, DocSet::ARRAY);
  EXPECT_EQ(collect(*u, 10), (std::vector<int32_t>{1, 2, 3, 4, 8, 9}));
}

TEST_F(DocSetTest, unionTwoBitsets) {
  RAMBitDocSet a(64);
  a.mutableBits().set(2);
  a.mutableBits().set(5);
  RAMBitDocSet b(64);
  b.mutableBits().set(5);
  b.mutableBits().set(7);
  std::array<DocSet*, 2> sets{&a, &b};
  auto u = DocSet::union_(sets);
  ASSERT_EQ(u->type, DocSet::BITSET);
  EXPECT_EQ(collect(*u, 64), (std::vector<int32_t>{2, 5, 7}));
}

TEST_F(DocSetTest, unionMixed) {
  ArrDocSet a({1, 3, 10});
  RAMBitDocSet b(64);
  b.mutableBits().set(3);
  b.mutableBits().set(20);
  std::array<DocSet*, 2> sets{&a, &b};
  auto u = DocSet::union_(sets);
  ASSERT_EQ(u->type, DocSet::BITSET);
  EXPECT_EQ(collect(*u, 64), (std::vector<int32_t>{1, 3, 10, 20}));
}

TEST_F(DocSetTest, intersectTwoBitsets) {
  RAMBitDocSet a(64);
  a.mutableBits().set(2);
  a.mutableBits().set(5);
  a.mutableBits().set(7);
  RAMBitDocSet b(64);
  b.mutableBits().set(5);
  b.mutableBits().set(7);
  b.mutableBits().set(9);
  std::array<DocSet*, 2> sets{&a, &b};
  auto u = DocSet::intersect(sets);
  ASSERT_EQ(u->type, DocSet::BITSET);
  EXPECT_EQ(collect(*u, 64), (std::vector<int32_t>{5, 7}));
}

TEST_F(DocSetTest, intersectThreeBitsets) {
  RAMBitDocSet a(64);
  for (int d : {1, 3, 5, 7}) a.mutableBits().set(d);
  RAMBitDocSet b(64);
  for (int d : {3, 5, 9}) b.mutableBits().set(d);
  RAMBitDocSet c(64);
  for (int d : {5, 7, 9}) c.mutableBits().set(d);
  std::array<DocSet*, 3> sets{&a, &b, &c};
  auto u = DocSet::intersect(sets);
  ASSERT_EQ(u->type, DocSet::BITSET);
  EXPECT_EQ(collect(*u, 64), (std::vector<int32_t>{5}));
}

TEST_F(DocSetTest, bitsetResultsHaveCardinality) {
  RAMBitDocSet a(64);
  for (int d : {1, 3, 5, 7}) a.mutableBits().set(d);
  RAMBitDocSet b(64);
  for (int d : {3, 5, 9}) b.mutableBits().set(d);
  std::array<DocSet*, 2> sets{&a, &b};
  EXPECT_EQ(DocSet::union_(sets)->card(), 5);
  EXPECT_EQ(DocSet::intersect(sets)->card(), 2);
}

}  // namespace solux::test
