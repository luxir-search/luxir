#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>
#include "test/LuxirTest.h"
#include "luxir/util/SharedLazyMap.h"
#include "luxir/util/StrRef.h"
#include "luxir/util/log.h"
#include "luxir/util/proto.h"

using namespace luxir;

class UtilTest : public LuxirTest {
protected:
};

TEST_F(UtilTest, packedTermTruncate) {
  std::string atCap(PackedTerm::MAX_LEN, 'a');
  EXPECT_EQ(atCap, PackedTerm::truncate(atCap));            // at the cap: identity
  EXPECT_EQ(atCap, PackedTerm::truncate(atCap + "tail"));   // over: cut at MAX_LEN

  // A multi-byte sequence straddling the cap backs up to its boundary:
  // 130 x 2-byte e-acute = 260 bytes; byte 255 splits a sequence, so cut at 254.
  std::string acc;
  for (int i = 0; i < 130; i++) acc += "\xC3\xA9";
  EXPECT_EQ(acc.substr(0, 254), PackedTerm::truncate(acc));

  // An invalid continuation run longer than the cap cuts at MAX_LEN exactly.
  std::string cont(300, '\x80');
  EXPECT_EQ(cont.substr(0, PackedTerm::MAX_LEN), PackedTerm::truncate(cont));
}

TEST_F(UtilTest, lazyMap) {
  // test exception handling
  {
    SharedLazyMap<int, int> map;
    int count = 0;
    auto create = [&]() {
      auto v = std::make_shared<int>(42);
      count++;
      if (count % 2 == 1) {
        throw std::runtime_error("Test exception");
      }
      return v;
    };

    // exception the first time
    EXPECT_THROW({
      auto ptr = map.getOrCreate(5, create);
      }, std::runtime_error);

    // make sure we can still create it correctly the second time
    auto ptr = map.getOrCreate(5, create);
    EXPECT_TRUE(ptr.get() != nullptr);
  }


  {
    SharedLazyMap<int, std::vector<int>> map;
    std::atomic<int> totalCreations{0};

    const int numThreads = 16;
    const int numKeys = 10;
    const int accessesPerThread = 1000;
    const size_t vecSize = 20;
    std::vector<std::thread> threads;

    // Create function that allocates an expensive object
    auto createFunc = [&totalCreations](int key) {
      totalCreations++;
      auto vec = std::make_shared<std::vector<int>>();
      for (auto i = 1u; i <= vecSize; i++) {
        vec->resize(i);
        vec->back() = key;
        vec->front() += vec->size();
      }
      return vec;
    };

    // Launch threads with high contention on limited keys
    for (int t = 0; t < numThreads; t++) {
      threads.emplace_back([&, t]() {
        Rng rng(t);

        for (int i = 0; i < accessesPerThread; i++) {
          int key = rng.rint(numKeys);
          try {
            std::function<std::shared_ptr<std::vector<int>>()> creator = [&]() { return createFunc(key); };
            auto vec = map.getOrCreate(key, creator);
            // Verify the vector contains the expected values
            EXPECT_TRUE(vec.get() != nullptr);
            EXPECT_EQ(vec->size(), vecSize);
            EXPECT_EQ(vec->back(), key);
          }
          catch (std::exception& e) {
            LOG_ERROR("Client caught exception: {}", e.what());
          }
        }
      });
    }

    // Wait for all threads
    for (auto& t : threads) {
      t.join();
    }

    // Verify we created exactly numKeys objects
    EXPECT_EQ(totalCreations.load(), numKeys);
  }
}

TEST_F(UtilTest, lazyMapConditionalReplaceAndErase) {
  SharedLazyMap<int, int> map;
  auto original = map.getOrCreate(7, [] { return std::make_shared<int>(1); });
  auto wrong = std::make_shared<int>(1);
  auto tombstone = std::make_shared<int>(2);

  EXPECT_FALSE(map.replace(7, wrong, tombstone));
  EXPECT_EQ(original, map.get(7));
  EXPECT_TRUE(map.replace(7, original, tombstone));
  EXPECT_EQ(tombstone, map.get(7));
  EXPECT_FALSE(map.erase(7, original));
  EXPECT_TRUE(map.erase(7, tombstone));
  EXPECT_EQ(nullptr, map.get(7));
}

namespace {
// Counters for arenaCreate exception-safety test.  File-scope so the tracked
// types below don't need to reach into function locals.
int arenaDtors = 0;    // ~Tracked / ~Boom calls (the arena-managed object)
int memberDtors = 0;   // ~Member calls (a subobject built before any throw)

struct Member {
  ~Member() { memberDtors++; }
};
// Normal object: its dtor should run exactly once, at arena reset.
struct Tracked {
  Member m;
  ~Tracked() { arenaDtors++; }
};
// Throwing ctor: the Member is fully built, then the ctor throws.  ~Boom must
// NEVER run - the object was never fully constructed and (crucially) arenaCreate
// registers the cleanup only on success, so nothing is scheduled for reset.
struct Boom {
  Member m;
  Boom() { throw std::runtime_error("ctor boom"); }
  ~Boom() { arenaDtors++; }
};
}  // namespace

// arenaCreate constructs first and registers ~T() only on success (unlike
// protobuf's Arena::Create, which registers before construction and would run
// ~T() on half-constructed memory at reset).  Verify both halves.
TEST_F(UtilTest, arenaCreateExceptionSafe) {
  arenaDtors = 0;
  memberDtors = 0;
  auto* arena = createArena();

  // Success path: dtor registered, not run yet.
  auto* t = arenaCreate<Tracked>(*arena);
  EXPECT_NE(t, nullptr);
  EXPECT_EQ(arenaDtors, 0);
  EXPECT_EQ(memberDtors, 0);

  // Throwing ctor: throw propagates.  C++ unwinds the Member built before the
  // throw (memberDtors == 1), but ~Boom is never scheduled for cleanup.
  EXPECT_THROW(arenaCreate<Boom>(*arena), std::runtime_error);
  EXPECT_EQ(memberDtors, 1);   // Boom::m unwound during the failed construction
  EXPECT_EQ(arenaDtors, 0);    // ~Boom did NOT run

  // Reset runs ~Tracked exactly once (and its Member), nothing for Boom.
  releaseArena(arena);
  EXPECT_EQ(arenaDtors, 1);    // only ~Tracked, never ~Boom
  EXPECT_EQ(memberDtors, 2);   // Boom::m (unwind) + Tracked::m (reset)
}
