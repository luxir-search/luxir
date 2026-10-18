// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <algorithm>
#include <iterator>
#include <iostream>
#include <vector>
#include <random>
#include "test/LuxirTest.h"
#include "luxir/util/screaming.h"
#include "luxir/index/ScreamingBuilder.h"

using namespace luxir;

namespace {
class ScratchResource : public std::pmr::memory_resource {
public:
  size_t live = 0;
  int remaining = -1;

private:
  void* do_allocate(size_t bytes, size_t alignment) override {
    if (remaining == 0) throw std::bad_alloc();
    if (remaining > 0) --remaining;
    auto* ptr = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    live += bytes;
    return ptr;
  }
  void do_deallocate(void* ptr, size_t bytes, size_t alignment) override {
    live -= bytes;
    std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
  }
  bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }

};
} // namespace

class ScreamingTest : public luxir::LuxirTest {
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
    std::vector<int32_t> added;  // every value, in order

    explicit BldBase(const Rng& rng = LuxirTest::rng) : rng(rng) {
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
      added.push_back(val);
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
        } else if (r < 75) {
          // random bucket
          addMidBucket();
        } else if (r < 90) {
          addRunBucket(1 + rng.rint(3000));
        } else {
          addFullBucket(rng.rint(3));
        }
      }
    }

    // nRuns runs of random length at random spots, one per equal slice of the bucket.
    void addRunBucket(int nRuns) {
      if (base < 0) return;
      int64_t slice = 65536 / nRuns;
      for (int64_t r = 0; r < nRuns; r++) {
        int64_t first = base + r * slice + rng.rint(slice / 2 + 1);
        int64_t len = 1 + rng.rint(slice / 2 + 1);
        for (int64_t v = first; v < first + len && v <= bucketMax(); v++) add((int)v);
      }
      nextBucket();
    }

    // The whole bucket, minus a few random holes.
    void addFullBucket(int holes = 0) {
      if (base < 0) return;
      std::vector<int> skip;
      for (int i = 0; i < holes; i++) skip.push_back(base + rng.rint(65536));
      for (int64_t v = base; v <= bucketMax(); v++) {
        if (std::find(skip.begin(), skip.end(), (int)v) == skip.end()) add((int)v);
      }
      nextBucket();
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
      // select() is the inverse of rank(), so we verify it as we walk: after each
      // rank() below, select(rank) must return the value we are sitting on.  This
      // covers both the stateless select() and the amortized Selector without
      // recording the values separately.  (Only used on modest bucket counts;
      // stateless select()'s linear bucket scan would be quadratic otherwise.)
      std::vector<int32_t> selScratch((size_t)bitset->nBuckets + 1);
      screaming::BitSet::Selector selector(*bitset, selScratch);
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

        // Inverse check: select(rank) must round-trip back to the current value.
        EXPECT_EQ(val, bitset->select(rank));
        EXPECT_EQ(val, selector.select(rank));

        itHash = itHash * 31 + val;
        card++;
      }
      EXPECT_EQ(card, nAdds);
      EXPECT_EQ(bitset->cardinality(), nAdds);
      EXPECT_EQ(selector.cardinality(), nAdds);
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
    // Junk written before the builder exists misaligns the stream start; the
    // builder must pad back to the format's 8-aligned set start.
    int prefixBytes;
    ScreamingBuilder builder;

    explicit OutputStreamBuilder(const Rng& rng = LuxirTest::rng, int prefix = 0)
        : BldBase(rng),
          prefixBytes([&] {
            for (int i = 0; i < prefix; i++) os.write((char)0x5A);
            return prefix;
          }()),
          builder(pool, os) {
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

    explicit SStreamBuilder(const Rng& rng = LuxirTest::rng) : BldBase(rng) {
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

TEST_F(ScreamingTest, stringBuilderReclaimsUnflushedScratch) {
  ScratchResource resource;
  std::ostringstream out;
  {
    screaming::StringStreamBuilder builder(out, &resource);
    // More descriptors than fit in the first scratch block.
    for (int i = 0; i < 300; ++i) builder.add(i << 16);
    EXPECT_GT(resource.live, 0u);
  }
  EXPECT_EQ(0u, resource.live);
}

TEST_F(ScreamingTest, stringBuilderReclaimsScratchAfterFailedFlush) {
  ScratchResource resource;
  std::ostringstream out;
  {
    screaming::StringStreamBuilder builder(out, &resource);
    for (int i = 0; i < 300; ++i) builder.add(i << 16);
    out.setstate(std::ios::badbit);
    EXPECT_THROW(out.exceptions(std::ios::badbit), std::ios_base::failure);
    EXPECT_THROW(builder.flush(), std::ios_base::failure);
  }
  EXPECT_EQ(0u, resource.live);
}

TEST_F(ScreamingTest, stringBuilderAllocationFailureUnwinds) {
  bool succeeded = false;
  for (int failAt = 0; failAt < 32 && !succeeded; ++failAt) {
    ScratchResource resource;
    resource.remaining = failAt;
    std::ostringstream out;
    try {
      screaming::StringStreamBuilder builder(out, &resource);
      for (int i = 0; i < 300; ++i) builder.add(i << 16);
      builder.flush();
      succeeded = true;
    } catch (const std::bad_alloc&) {}
    EXPECT_EQ(0u, resource.live) << "failure at allocation " << failAt;
  }
  EXPECT_TRUE(succeeded);
}

TEST_F(ScreamingTest, fixedRangeOr) {
  std::mt19937_64 random(0x5b83);
  for (int32_t size : {1, 63, 64, 65, 137, 4099}) {
    int32_t words = (int32_t) screaming::FixedBitSet::sizeInWords(size);
    for (int32_t sourceOffset = 0; sourceOffset < 8; sourceOffset++) {
      std::vector<uint64_t> source((size_t) (words + sourceOffset));
      for (auto& word : source) word = random();
      screaming::FixedBitSet bits(source.data() + sourceOffset, size);
      for (int32_t destinationOffset = 0; destinationOffset < 8; destinationOffset++) {
        for (int32_t trial = 0; trial < 20; trial++) {
          int32_t from = trial == 0 ? 0 : (int32_t) (random() % (uint32_t) (size + 1));
          int32_t to = trial < 2 ? size : from + (int32_t) (random() % (uint32_t) (size - from + 1));
          if (trial == 2 && size >= 65) {
            from = 1;
            to = 65;
          }
          std::vector<uint64_t> actual((size_t) (words + destinationOffset + 1));
          for (auto& word : actual) word = random();
          auto expected = actual;
          for (int32_t bit = from; bit < to; bit++) {
            if (bits.get(bit)) {
              expected[(size_t) (destinationOffset + ((bit - from) >> 6))]
                  |= 1ULL << ((bit - from) & 63);
            }
          }
          bits.orRange(std::span<uint64_t>(actual).subspan((size_t) destinationOffset), from, to);
          EXPECT_EQ(expected, actual);
        }
      }
    }
  }
}

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

// Serialized sets must keep dense-bucket words 8-aligned (BLOCK_ALIGN): the
// word scans compile with alignment assumptions, so a byte-misaligned words
// pointer is a fault, not a slow path (this was the 5M-corpus norms-merge
// segfault).  Interleave odd-cardinality sparse buckets (which force block
// padding) with dense buckets, over stream starts at every misalignment
// residue (which force the builder's leading stream pad), and verify both the
// explicit alignment of every dense block and full iterate/rank/select parity.
TEST_F(ScreamingTest, denseBucketAlignment) {
  for (int prefix = 0; prefix < 8; prefix++) {
    OutputStreamBuilder set(LuxirTest::rng, prefix);
    set.addSmallBucket(3);
    set.addMidBucket();
    set.addSmallBucket(5);
    set.addMidBucket();
    set.addSmallBucket(1);
    set.finishBuild();

    const screaming::BitSet& bs = *set.bitset;
    ASSERT_GT((int)bs.nBuckets, 0);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(bs.start)
                  & (screaming::BitSet::BLOCK_ALIGN - 1), 0u);
    int denseSeen = 0;
    for (int i = 0; i < bs.nBuckets; i++) {
      if (bs.bucketKind(i) == screaming::BitSet::DENSE) {
        denseSeen++;
        ASSERT_EQ(reinterpret_cast<uintptr_t>(bs.container(bs.descriptors[i]))
                      & (screaming::BitSet::BLOCK_ALIGN - 1), 0u);
      }
    }
    ASSERT_GE(denseSeen, 2);
    set.verifyIteratorSkips();
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
    set.verifyIteratorSkips();  // also round-trips select() against rank()
  }
}

TEST_F(ScreamingTest, select) {
  // Focused select() coverage: a sparse bucket, a dense (bitset) bucket, and a
  // gap, across multiple bucket bases.  verifyIteratorSkips() round-trips both the
  // stateless select() and the Selector against rank() for every value.
  OutputStreamBuilder set;
  set.addSmallBucket(1 + rng.rint(3));     // tiny sparse
  set.addSmallBucket();                     // near-max sparse
  set.nextBucket(3);                        // skip a few buckets (gap)
  set.addMidBucket();                       // dense bitset bucket
  set.addSmallBucket((int)(screaming::BitSet::BUCKET_SPARSE_MAX + 1 + rng.rint(3))); // just-dense
  set.finishBuild();
  ASSERT_TRUE(set.verifyIteratorSkips());
}

// Each bucket gets the smallest container: array, bitmap, runs, or nothing
// for a full bucket. Ties keep the array/bitmap.
TEST_F(ScreamingTest, containerKinds) {
  using BS = screaming::BitSet;
  auto addRange = [](BldBase& set, int first, int len) {
    for (int v = first; v < first + len; v++) set.add(v);
  };
  SStreamBuilder set;
  std::vector<BS::Kind> expected;
  auto bucket = [&](BS::Kind kind, auto fill) {
    fill();
    set.nextBucket();
    expected.push_back(kind);
  };
  bucket(BS::SPARSE, [&] { for (int i = 0; i < 100; i++) set.add(set.base + i * 7); });
  bucket(BS::SPARSE, [&] { addRange(set, set.base, 4); });                // run 8 bytes == array 8
  bucket(BS::RUN, [&] { addRange(set, set.base + 9, 5); });               // run 8 < array 16
  bucket(BS::SPARSE, [&] { addRange(set, set.base, 2); addRange(set, set.base + 9, 2); }); // 16 vs 8
  bucket(BS::RUN, [&] { addRange(set, set.base + 100, 3000); addRange(set, set.base + 5000, 1000); });
  bucket(BS::DENSE, [&] { for (int i = 0; i < 65536; i += 2) set.add(set.base + i); });
  // 2048 runs (8200 bytes) still beat the 8448 byte bitmap; 2049 exceed RUN_MAX
  bucket(BS::RUN, [&] { for (int i = 0; i < (int)BS::RUN_MAX; i++) addRange(set, set.base + i * 31, 3); });
  bucket(BS::DENSE, [&] { for (int i = 0; i <= (int)BS::RUN_MAX; i++) addRange(set, set.base + i * 31, 3); });
  bucket(BS::ALL_ONES, [&] { addRange(set, set.base, 65536); });
  bucket(BS::RUN, [&] { addRange(set, set.base, 65535); });               // full but the last value
  bucket(BS::RUN, [&] { addRange(set, set.base + 1, 65535); });           // full but the first value
  bucket(BS::RUN, [&] { addRange(set, set.base, 300); addRange(set, set.base + 301, 65235); });
  bucket(BS::ALL_ONES, [&] { addRange(set, set.base, 65536); });          // last bucket has no payload
  set.finishBuild();

  ASSERT_EQ((int)set.bitset->nBuckets, (int)expected.size());
  for (int i = 0; i < (int)expected.size(); i++) {
    EXPECT_EQ(expected[i], set.bitset->bucketKind(i)) << "bucket " << i;
  }
  ASSERT_TRUE(set.verifyIteratorSkips());

  // Every value of a run bucket and its neighbors, via advance from a fresh iterator.
  for (int32_t target : {(4 << 16) + 99, (4 << 16) + 100, (4 << 16) + 3100, (4 << 16) + 4999,
                         (4 << 16) + 5999, (4 << 16) + 6000, (11 << 16) + 300}) {
    BS::Iterator it(*set.bitset);
    auto lb = std::lower_bound(set.added.begin(), set.added.end(), target);
    ASSERT_EQ(*lb, it.advance(target));
    EXPECT_EQ((int32_t)(lb - set.added.begin()), it.rank());
  }
}

// Advance-driven intersection (leapfrog) and next()-driven union over sets
// whose buckets mix every container kind.
TEST_F(ScreamingTest, mixedKindIntersectionAndUnion) {
  using BS = screaming::BitSet;
  for (int trial = 0; trial < 8; trial++) {
    SStreamBuilder a(Rng(trial * 2)), b(Rng(trial * 2 + 1));
    for (BldBase* set : {(BldBase*)&a, (BldBase*)&b}) {
      for (int i = 0; i < 6; i++) set->addBucket();
      set->finishBuild();
    }

    std::vector<int32_t> expected;
    std::set_intersection(a.added.begin(), a.added.end(), b.added.begin(), b.added.end(),
                          std::back_inserter(expected));
    std::vector<int32_t> got;
    BS::Iterator ia(*a.bitset), ib(*b.bitset);
    int32_t x = ia.next(), y = ib.next();
    while (x != BS::END && y != BS::END) {
      if (x < y) {
        x = ia.advance(y);
      } else if (y < x) {
        y = ib.advance(x);
      } else {
        got.push_back(x);
        EXPECT_EQ(x, a.added[(size_t)ia.rank()]);
        EXPECT_EQ(x, b.added[(size_t)ib.rank()]);
        x = ia.next();
        y = ib.next();
      }
    }
    EXPECT_EQ(expected, got);

    expected.clear();
    std::set_union(a.added.begin(), a.added.end(), b.added.begin(), b.added.end(),
                   std::back_inserter(expected));
    got.clear();
    BS::Iterator ua(*a.bitset), ub(*b.bitset);
    x = ua.next();
    y = ub.next();
    while (x != BS::END || y != BS::END) {
      int32_t v = std::min(x, y);
      got.push_back(v);
      if (x == v) x = ua.next();
      if (y == v) y = ub.next();
    }
    EXPECT_EQ(expected, got);
  }
}

TEST_F(ScreamingTest, selectSingleValue) {
  // Boundary: a one-element set.
  std::ostringstream ss;
  screaming::StringStreamBuilder builder(ss);
  builder.add(0xabcdef);
  builder.flush();
  std::string result = ss.str();
  screaming::BitSet bs((void*)(result.c_str() + result.size()));
  ASSERT_EQ(bs.cardinality(), 1);
  ASSERT_EQ(bs.select(0), 0xabcdef);
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

// visitZeroes/visitOnes word-walkers: edge bits 0 and 63, all-zero and
// all-one words, and exact complementarity over a multi-word array.
TEST_F(ScreamingTest, visitZeroesAndOnes) {
  std::vector<uint64_t> words(4, ~0ULL);
  words[0] &= ~((1ULL << 0) | (1ULL << 3) | (1ULL << 5) | (1ULL << 63));
  words[1] = 0;
  words[3] = rng.rlong();
  int32_t nbits = (int32_t)(words.size() * 64);
  std::vector<bool> reference(nbits);
  for (int32_t b = 0; b < nbits; b++) {
    reference[b] = (words[b >> 6] >> (b & 63)) & 1;
  }

  std::vector<int32_t> got;
  screaming::FixedBitSet::visitZeroes(words.data(), (int32_t)words.size(),
                                      [&](int32_t b) { got.push_back(b); });
  std::vector<int32_t> expected;
  for (int32_t b = 0; b < nbits; b++) {
    if (!reference[b]) expected.push_back(b);
  }
  EXPECT_EQ(got, expected);

  got.clear();
  expected.clear();
  screaming::FixedBitSet::visitOnes(words.data(), (int32_t)words.size(),
                                    [&](int32_t b) { got.push_back(b); });
  for (int32_t b = 0; b < nbits; b++) {
    if (reference[b]) expected.push_back(b);
  }
  EXPECT_EQ(got, expected);
}
