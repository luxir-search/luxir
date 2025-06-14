
#include <gtest/gtest.h>
#include <iostream>
#include <vector>
#include "test/SoluxTest.h"
#include "solux/util/screaming.h"
#include "solux/index/ScreamingBuilder.h"

using namespace solux;

class ScreamingTest : public solux::SoluxTest {
public:

  // Utility class to make it easier to test screaming bitset
  class BldBase {
  public:
    Rng rng;

    size_t numBytes{};
    std::string resultStr;
    const char* ptrToEnd{};
    std::vector<screaming::BitSet::Bits::word_type> buf;  // used by obs
    screaming::BitSet::Bits obs{};  // 64K small bit set
    std::unique_ptr<screaming::BitSet> bitset;
    uint64_t addHash = 1;
    int base = 0; // the base of the current bucket
    int curr = -1;  // the current value added
    int nAdds = 0;

    explicit BldBase(const Rng& rng = SoluxTest::rng) : rng(rng) {
      buf.resize(screaming::BitSet::Bits::fixedNumWords);
      obs = screaming::BitSet::Bits(&buf[0]);
    }

    virtual ~BldBase() = default;

    virtual void virtAdd(int val) = 0;

    virtual void finishBuild() = 0;


    void add(int val) {
      if (val <= curr || val == 0x7fffffff) return;
      curr = val;
      addHash = addHash * 31 + val;
      virtAdd(val);
      nAdds++;
    }

    bool nextBucket(int nbuckets = 1) {
      int64_t newBase = base + 0x00010000 * (int64_t)nbuckets;
      if (newBase > INT_MAX) {
        return false;
      }
      base = (int)newBase;
      return true;
    }

    int bucketMax() const {
      return base | 0x0ffff;
    }

    /*
    int leftInBucket() const {
      return bucketMax() - curr;
    }
     */


/*
    void addInBucket() {
      if (leftInBucket() > 0) {
        add(curr + 1 + rng.rint(leftInBucket()));
      }
    }
*/
    void addSmallBucket() {
      // either small or big type of sparse buckets to better test boundaries
      int card = rng.rbool() ? rng.rint(1,5) : (int)(screaming::BitSet::BUCKET_SPARSE_MAX - rng.rint(3));
      addSmallBucket(card);
    }

    // This also works for buckets that are slightly bigger than a sparse bucket (to test that boundary)
    void addSmallBucket(int cardinality) {
      if (base < 0) return;
      auto card = cardinality;
      int r = rng.rint(100);
      if (r<5) {  // 5% of the time, start near first value
        add(base + rng.rint(1,3));
        --card;
        if (r<10) --card;  // account for last value
      }

      for (int i=0; i<card; i++) {
        auto gap = rng.rint(1, 65536/cardinality);
        add(curr + gap);
      }

      if (r>=5 && r < 10) { // 5% of the time, end around highest value
        add(bucketMax() - rng.rint(3));
      }

      nextBucket();
    }


    // TODO: add a mostly full bucket...
    void addBucket() {
      int r = rng.rint(100);
      if (r<50) {
        addSmallBucket();
      } else {
        if (r < 60) {
          // just over the number of entries to transition to a bitmap
          int card = (int) (screaming::BitSet::BUCKET_SPARSE_MAX + 1 + rng.rint(3));
          addSmallBucket(card);
        } else {
          // random bucket
          addMidBucket();
        }
      }
    }

    void addMidBucket() {
      if (base < 0) return;
      // fill bitset word at a time
      for (auto& elem : buf) {
        elem = rng() & rng() & rng();   // ~12.5% of the bits set, to speed up processing
      }

      // since zero words have special handling in bitsets, zero out a random word or two (esp near beginning or end)
      if (rng.rbool()) {
        buf[rng.rint(2)] = 0;
      }
      if (rng.rbool()) {
        buf[buf.size() - 1 - rng.rint(2)] = 0;
      }

      // now iterate over bits and add to our builder
      int val = -1;
      for(;val < (int)screaming::BitSet::Bits::fixedSize - 1;) {
        val = (int)obs.nextSetBit(val + 1);
        if (val == (int)screaming::BitSet::Bits::MAX_INDEX) break;
        add(base + val);
      }

      nextBucket();
    }




    bool verifyIterator() const {
      int card = 0;
      uint64_t itHash = 1;
      screaming::BitSet::Iterator it(*bitset);
      for(;;) {
        auto val = it.next();
        if (val == screaming::BitSet::END) break;
        itHash = itHash * 31 + val;
        card++;
      }
      EXPECT_EQ(card, nAdds);
      // std::cout << "nAdds=" << nAdds << std::endl;
      EXPECT_EQ(addHash, itHash);
      return addHash == itHash;
    }

    bool verifyIteratorSkips() {
      int card = 0;
      uint64_t itHash = 1;
      screaming::BitSet::Iterator it(*bitset);
      std::vector<screaming::BitSet::Iterator> iters;
      for (int i=0; i<8; i++) {
        iters.emplace_back(*bitset);
      }
      int last = it.val();
      for(;;) {
        int val = it.next();

        auto gap = (int64_t)val - last;  // this can exceed signed int
        int seekTarget = last + 1 + (int)rng.rint(gap);
        assert(seekTarget > last && seekTarget <= val);
        auto& skipIter = iters[rng.rint(iters.size())];
        if (skipIter.val() < last && rng.rbool()) {
          // mix in some calls to next()
          int n = skipIter.next();
          EXPECT_TRUE(n <= last);
        }
        // std::cout << "\tseeking target=" << seekTarget << " expected val=" << val << " last=" << last << std::endl;

        last = val;

        int seekResult = skipIter.advance(seekTarget);
        if (val != seekResult) {
          EXPECT_EQ(val, seekResult);
        }

        if (val == screaming::BitSet::END) break;

        int rank = it.rank();
        if (card != rank) {
          rank = it.rank(); // breakpoint here
          EXPECT_EQ(card, rank);
        }
        int rankResult = skipIter.rank();
        if (rankResult != rank) {
          rankResult = skipIter.rank(); // breakpoint here
          EXPECT_EQ(rank, rankResult);
        }

        itHash = itHash * 31 + val;
        card++;
      }
      EXPECT_EQ(card, nAdds);
      // std::cout << "nAdds=" << nAdds << std::endl;
      EXPECT_EQ(addHash, itHash);
      return addHash == itHash;
    }
  };


  class OutputStreamBuilder : public BldBase {
  public:

    MemPool pool;
    RAMFile ramFile = {""};
    OutputStream os{&ramFile};
    ScreamingBuilder builder{pool, os};

    explicit OutputStreamBuilder(const Rng& rng = SoluxTest::rng) : BldBase(rng) {
    }

    void virtAdd(int val) override {
      builder.add(val);
    }

    void finishBuild() override {
      numBytes = builder.flush();
      auto card = builder.cardinality();
      EXPECT_EQ(nAdds, card);
      os.close();
      numBytes = ramFile.size();
      resultStr.resize(numBytes);
      ramFile.copyTo(&resultStr[0]);
      EXPECT_EQ(numBytes, resultStr.size());
      ptrToEnd = resultStr.c_str() + resultStr.size();
      bitset = std::make_unique<screaming::BitSet>(ptrToEnd);
    }

    ~OutputStreamBuilder() override = default;
  };

  class SStreamBuilder : public BldBase {
  public:
    std::ostringstream out;
    screaming::StringStreamBuilder builder{out};
    std::string resultStr;

    explicit SStreamBuilder(const Rng& rng = SoluxTest::rng) : BldBase(rng) {
    }

    void virtAdd(int val) override {
      builder.add(val);
    }

    void finishBuild() override {
      numBytes = builder.flush();
      auto card = builder.cardinality();
      EXPECT_EQ(nAdds, card);
      resultStr = out.str();
      // ASSERT_EQ(numBytes, resultStr.size());   // can't use ASSERT_EQ in this context
      EXPECT_EQ(numBytes, resultStr.size());
      ptrToEnd = resultStr.c_str() + resultStr.size();
      bitset = std::make_unique<screaming::BitSet>(ptrToEnd);
    }

    ~SStreamBuilder() override = default;
  };


};

TEST_F(ScreamingTest, debug) {
}

TEST_F(ScreamingTest, basic) {
  std::ostringstream ss;
  screaming::StringStreamBuilder builder(ss);
  builder.add(0xabcdef);
  auto nbytes = builder.flush();
  std::string result = ss.str();
  // std::cout << "Screaming bitset size = " << nbytes << std::endl;
  ASSERT_EQ(result.size(), nbytes);

  void* ptrToEnd = (void*)(result.c_str() + result.size());
  screaming::BitSet bs(ptrToEnd);

  screaming::BitSet::Iterator iter(bs);
  ASSERT_EQ(iter.val(), -1); // start off at -1
  ASSERT_EQ(iter.next(), 0xabcdef);
  ASSERT_EQ(iter.val(), 0xabcdef);
  ASSERT_EQ(iter.rank(), 0);
  ASSERT_EQ(iter.next(), screaming::BitSet::END);
  ASSERT_EQ(iter.val(), screaming::BitSet::END);

  screaming::BitSet::Iterator iter2(bs);
  ASSERT_EQ(iter2.advance(0x01cdef), 0xabcdef);
  ASSERT_EQ(iter2.val(), 0xabcdef);
  ASSERT_EQ(iter2.rank(), 0);
  ASSERT_EQ(iter2.advance(0xabcfff), screaming::BitSet::END);

  // every other bit set for a dense test
  std::ostringstream buf;
  screaming::StringStreamBuilder bld(buf);
  for (int i=0; i<65536; i+= 2) {
    bld.add(i);
  }
  nbytes = bld.flush();
  result = buf.str();
  ptrToEnd = (void*)(result.c_str() + result.size());
  bs = screaming::BitSet(ptrToEnd);
  iter = screaming::BitSet::Iterator(bs);
  ASSERT_EQ(result.size(), nbytes);
  for (int i=0; i<65536; i+=2) {
    ASSERT_EQ(iter.next(), i);
    ASSERT_EQ(iter.val(), i);
    ASSERT_EQ(iter.rank(), i>>1);
  }
  ASSERT_EQ(iter.next(), screaming::BitSet::END);
  ASSERT_EQ(iter.val(), screaming::BitSet::END);

  // code good at finding easily debuggable errors when something else fails.
  {
    for (int i=0; i<10; i++) {
      Rng rng(i);
      int sz = 1;
      // int sz = 4097;
      // std::cout << "trying seed " << i << " size " << sz << std::endl;
      SStreamBuilder set(rng);
      set.addSmallBucket(sz);
      set.addSmallBucket(4097);
      set.finishBuild();
      set.verifyIteratorSkips();
    }
  }


  {
    SStreamBuilder set;
    set.addMidBucket();
    set.addMidBucket();
    set.addMidBucket();
    set.finishBuild();
    set.verifyIterator();
    ASSERT_GT(set.nAdds, 0); // make sure the random bucket logic is actually working to add docs.
  }

  {
    OutputStreamBuilder set;
    set.addMidBucket();
    set.addSmallBucket();
    set.addMidBucket();
    set.addSmallBucket();
    set.nextBucket(1000);
    set.addMidBucket();
    set.addSmallBucket();
    set.finishBuild();
    set.verifyIterator();
  }
}

TEST_F(ScreamingTest, manyBuckets) {
  // This is to test that handling many bucket descriptors works
  SStreamBuilder set;
  set.addSmallBucket();
  set.addMidBucket();
  for (int i=0; i<32000; i++) {
    if ((i&0x0ff) == 0) {
      // skip a bucket once in a while
      set.nextBucket();
    } else {
      set.addSmallBucket(1 + rng.rint(3));
    }
  }
  set.addSmallBucket();
  set.addMidBucket();

  set.finishBuild();
  set.verifyIterator();
}

TEST_F(ScreamingTest, allBuckets) {
  // This is to test handling the maximum number of buckets
  OutputStreamBuilder set;
  for (int i=0; i<32768; i++) {
    if ((i&0x0ff) == 0) {
      // skip a bucket once in a while
      set.nextBucket();
    } else {
      set.addSmallBucket(1 + rng.rint(3));
    }
  }
  set.finishBuild();
  set.verifyIterator();
}


TEST_F(ScreamingTest, randomSets) {
  int iter=20;  // pump this up for more thorough testing
  for (int i=0; i<iter; i++) {
    int nBuckets = rng.rint(1,5);
    std::unique_ptr<BldBase> bldBase;
    if (rng.rbool()) {
      bldBase = std::make_unique<SStreamBuilder>();
    } else {
      bldBase = std::make_unique<OutputStreamBuilder>();
    }
    BldBase& set = *bldBase;

    for (int j=0; j<nBuckets; j++) {
      if (set.base < 0) break;  // base wrapped around... no more buckets

      // sometimes skip a few buckets
      if (rng.rbool()) {
        set.nextBucket(rng.rint(1000));
        if (set.base < 0) break;
      }

      set.addBucket();
    }

    // sometimes fill the last bucket
    if (rng.rbool() && set.base >= 0 && set.base < 0x7fff0000) {
      set.base = 0x7fff0000;
      set.addBucket();
    }

    set.finishBuild();
    // set.verifyIterator();
    set.verifyIteratorSkips();
  }
}

TEST_F(ScreamingTest, ramFixedBitSet) {
  // Test with all bits initially false
  {
    screaming::RAMFixedBitSet bitset(129);
    EXPECT_EQ(bitset.size(), 129);
    
    // Check all bits are initially false
    for (int i = 0; i < 129; i++) {
      EXPECT_FALSE(bitset.get(i));
    }
    
    // Set some bits
    bitset.set(0);
    bitset.set(7);
    bitset.set(64);
    bitset.set(127);
    
    // Check set bits
    EXPECT_TRUE(bitset.get(0));
    EXPECT_TRUE(bitset.get(7));
    EXPECT_TRUE(bitset.get(64));
    EXPECT_TRUE(bitset.get(127));
    
    // Check unset bits
    EXPECT_FALSE(bitset.get(1));
    EXPECT_FALSE(bitset.get(63));
    EXPECT_FALSE(bitset.get(65));
    
    // Test nextSetBit
    EXPECT_EQ(bitset.nextSetBit(0), 0);
    EXPECT_EQ(bitset.nextSetBit(1), 7);
    EXPECT_EQ(bitset.nextSetBit(7), 7);
    EXPECT_EQ(bitset.nextSetBit(8), 64);
    EXPECT_EQ(bitset.nextSetBit(64), 64);
    EXPECT_EQ(bitset.nextSetBit(65), 127);
    EXPECT_EQ(bitset.nextSetBit(128), screaming::FixedBitSet::MAX_INDEX);
  }
  
  // Test with all bits initially true
  {
    screaming::RAMFixedBitSet bitset(100, true);
    EXPECT_EQ(bitset.size(), 100);
    
    // Check all bits are initially true
    for (int i = 0; i < 100; i++) {
      EXPECT_TRUE(bitset.get(i));
    }

    // Check that high bits in the last word are not set
    // 100 bits = 1 full word (64 bits) + 36 bits in the second word
    // The remaining 28 bits in the second word should not be set
    auto* words = bitset.ownedWords.get();
    int numWords = screaming::FixedBitSet::sizeInWords(100);
    EXPECT_EQ(numWords, 2);
    
    // First word should have all bits set
    EXPECT_EQ(words[0], ~uint64_t(0));
    
    // Second word should only have the lower 36 bits set
    // Create a mask with 36 bits set
    uint64_t expectedMask = (uint64_t(1) << 36) - 1;
    EXPECT_EQ(words[1], expectedMask);

    // All bits are set, so no clear bits
    EXPECT_EQ(bitset.nextClearBit(0), screaming::FixedBitSet::MAX_INDEX);
    EXPECT_EQ(bitset.nextClearBit(50), screaming::FixedBitSet::MAX_INDEX);
    EXPECT_EQ(bitset.nextClearBit(99), screaming::FixedBitSet::MAX_INDEX);
    
    // Clear a bit and test
    bitset.clear(50);
    EXPECT_EQ(bitset.get(50), false);
    bitset.clear(50); // test clearing again
    EXPECT_EQ(bitset.get(50), false);
    EXPECT_EQ(bitset.nextClearBit(0), 50);
    EXPECT_EQ(bitset.nextClearBit(50), 50);
    EXPECT_EQ(bitset.nextClearBit(51), screaming::FixedBitSet::MAX_INDEX);
    
    // Clear another bit
    bitset.clear(99);
    EXPECT_EQ(bitset.nextClearBit(51), 99);
    EXPECT_EQ(bitset.nextClearBit(99), 99);
  }
}

TEST_F(ScreamingTest, ramFixedBitSetRandom) {
  // pump this up after changes to the bitset code
  // there aren't many edge cases compared to screaming bitset though.
  const int numIterations = 10;
  
  for (int iter = 0; iter < numIterations; iter++) {
    // Random bitset size between 1 and 1000
    int32_t nbits = 1 + rng.rint(64 * 4);  // use up to 4 words should be enough for all edge cases
    bool initialVal = rng.rbool();
    
    screaming::RAMFixedBitSet bitset(nbits, initialVal);
    std::vector<bool> reference(nbits, initialVal);
    
    // Perform random operations
    int numOps = 50 + rng.rint(100);
    for (int op = 0; op < numOps; op++) {
      int32_t index = rng.rint(nbits);
      
      if (rng.rbool()) {
        // Set operation
        bitset.set(index);
        reference[index] = true;
      } else {
        // Clear operation
        bitset.clear(index);
        reference[index] = false;
      }
      
      // Verify the bit was set/cleared correctly
      EXPECT_EQ(bitset.get(index), reference[index]);
    }
    
    // Verify all bits match reference
    for (int32_t i = 0; i < nbits; i++) {
      EXPECT_EQ(bitset.get(i), reference[i]);
    }
    
    // Test nextSetBit against reference
    for (int32_t startIdx = 0; startIdx < nbits; startIdx += rng.rint(1, 20)) {
      int32_t expected = screaming::FixedBitSet::MAX_INDEX;
      for (int32_t j = startIdx; j < nbits; j++) {
        if (reference[j]) {
          expected = j;
          break;
        }
      }
      EXPECT_EQ(bitset.nextSetBit(startIdx), expected);
    }
    
    // Test nextClearBit against reference
    for (int32_t startIdx = 0; startIdx < nbits; startIdx += rng.rint(1, 20)) {
      int32_t expected = screaming::FixedBitSet::MAX_INDEX;
      for (int32_t j = startIdx; j < nbits; j++) {
        if (!reference[j]) {
          expected = j;
          break;
        }
      }
      EXPECT_EQ(bitset.nextClearBit(startIdx), expected);
    }
  }
}
