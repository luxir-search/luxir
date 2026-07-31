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

class TrackingArrDocSet : public ArrDocSet {
  bool& destroyed;

public:
  explicit TrackingArrDocSet(bool& destroyed)
    : ArrDocSet(std::vector<int32_t>{2, 5}), destroyed(destroyed) {}

  ~TrackingArrDocSet() override {
    destroyed = true;
  }
};

TEST_F(DocSetTest, domainHandleCopiesOwnTheirDocSet) {
  bool destroyed = false;
  DomainHandle copy;
  {
    DomainHandle original(
        std::make_unique<TrackingArrDocSet>(destroyed));
    ASSERT_TRUE(original.isDeliverable());
    ASSERT_TRUE(original.get()->get(5));

    copy = original;
    original = {};
    EXPECT_FALSE(destroyed);
    EXPECT_TRUE(copy.get()->get(2));
  }

  EXPECT_FALSE(destroyed);
  copy = {};
  EXPECT_TRUE(destroyed);
}

TEST_F(DocSetTest, borrowedDomainRequiresPinBeforeDelivery) {
  auto docs = std::make_shared<ArrDocSet>(std::vector<int32_t>{3});
  std::weak_ptr<ArrDocSet> weak = docs;
  auto borrowed = DomainHandle::borrowed(docs.get());
  EXPECT_FALSE(borrowed.isDeliverable());

  auto pinned = std::move(borrowed).pinnedWith(docs);
  docs.reset();
  EXPECT_TRUE(pinned.isDeliverable());
  EXPECT_FALSE(weak.expired());
  EXPECT_TRUE(pinned.get()->get(3));

  pinned = {};
  EXPECT_TRUE(weak.expired());
}

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

TEST_F(DocSetTest, intersectArrayLeadWithBitset) {
  ArrDocSet array({3, 7, 11});
  RAMBitDocSet bits(64);
  for (int32_t doc : {1, 3, 5, 7, 9, 13}) bits.mutableBits().set(doc);
  std::array<DocSet*, 2> sets{&bits, &array};

  auto result = DocSet::intersect(sets);

  ASSERT_EQ(result->type, DocSet::ARRAY);
  EXPECT_EQ(collect(*result, 64), (std::vector<int32_t>{3, 7}));
}

TEST_F(DocSetTest, intersectBitsetLeadWithArray) {
  RAMBitDocSet bits(100000);
  for (int32_t doc = 0; doc < 500; doc++) bits.mutableBits().set(doc * 2);
  std::vector<int32_t> arrayDocs;
  for (int32_t doc = 0; doc < 2000; doc++) arrayDocs.emplace_back(doc * 3);
  ArrDocSet array(std::move(arrayDocs));
  std::array<DocSet*, 2> sets{&array, &bits};

  auto result = DocSet::intersect(sets);

  ASSERT_EQ(result->type, DocSet::ARRAY);
  auto matches = collect(*result, 100000);
  EXPECT_EQ(result->card(), 167);
  EXPECT_EQ(matches.front(), 0);
  EXPECT_EQ(matches.back(), 996);
}

TEST_F(DocSetTest, intersectMixedThreeWay) {
  ArrDocSet small({4, 8, 12, 16, 20});
  RAMBitDocSet bits(64);
  for (int32_t doc : {2, 4, 8, 10, 16, 20, 30}) bits.mutableBits().set(doc);
  ArrDocSet large({1, 4, 7, 8, 11, 16, 19, 20, 25});
  std::array<DocSet*, 3> sets{&bits, &large, &small};

  auto result = DocSet::intersect(sets);

  ASSERT_EQ(result->type, DocSet::ARRAY);
  EXPECT_EQ(collect(*result, 64), (std::vector<int32_t>{4, 8, 16, 20}));
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

TEST_F(DocSetTest, builderAddWindowWordsMasksFinalPartialWord) {
  DocSetBuilder builder(130);
  std::array<uint64_t, 2> words{};
  words[0] = (1ULL << 0) | (1ULL << 1) | (1ULL << 63);
  words[1] = (1ULL << 0) | (1ULL << 4) | (1ULL << 10);

  builder.addWindowWords(words.data(), 3, 72, 5);
  auto set = builder.build();

  ASSERT_EQ(set->type, DocSet::ARRAY);
  EXPECT_EQ(set->card(), 5);
  EXPECT_EQ(collect(*set, 130), (std::vector<int32_t>{3, 4, 66, 67, 71}));
}

TEST_F(DocSetTest, builderAddWindowWordsPromotesAtAddBoundary) {
  DocSetBuilder builder(96);
  builder.add(1);
  builder.add(2);

  std::array<uint64_t, 1> words{};
  words[0] = (1ULL << 0) | (1ULL << 10);
  builder.addWindowWords(words.data(), 10, 21, 2);
  auto set = builder.build();

  ASSERT_EQ(set->type, DocSet::BITSET);
  EXPECT_EQ(set->card(), 4);
  EXPECT_EQ(collect(*set, 96), (std::vector<int32_t>{1, 2, 10, 20}));
}

TEST_F(DocSetTest, builderAddSortedPromotesOnSpanOverflow) {
  DocSetBuilder builder(96);
  std::array<int32_t, 2> first{1, 2};
  std::array<int32_t, 3> second{10, 20, 95};

  builder.addSorted(first);
  builder.addSorted(second);
  auto set = builder.build();

  ASSERT_EQ(set->type, DocSet::BITSET);
  EXPECT_EQ(set->card(), 5);
  EXPECT_EQ(collect(*set, 96),
            (std::vector<int32_t>{1, 2, 10, 20, 95}));
}

}  // namespace solux::test
