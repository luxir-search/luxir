// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <oneapi/tbb/task_arena.h>
#include "test/LuxirTest.h"
#include "test/TaskSpawnCounter.h"
#include "luxir/util/SharedLazyCell.h"
#include "luxir/util/SharedLazyMap.h"
#include "luxir/util/StrRef.h"
#include "luxir/util/log.h"
#include "luxir/util/proto.h"
#include "luxir/util/thread.h"

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

  // An invalid continuation run backs up at most three bytes, so no fitted
  // term is ever shorter than MIN_FITTED_LEN.
  std::string cont(300, '\x80');
  EXPECT_EQ(cont.substr(0, PackedTerm::MIN_FITTED_LEN), PackedTerm::truncate(cont));
  EXPECT_FALSE(PackedTerm::mayBeFitted(std::string(PackedTerm::MIN_FITTED_LEN - 1, 'a')));
  EXPECT_TRUE(PackedTerm::mayBeFitted(PackedTerm::truncate(cont)));
  EXPECT_TRUE(PackedTerm::mayBeFitted(PackedTerm::truncate(acc)));
}

TEST_F(UtilTest, packedTermHash128Format) {
  PackedTerm::TermBuffer scratch;
  // Pinned independently: XXH3_128bits(300 ASCII 'a') canonical hex is
  // 5e563f2cb28d2005c1d17581d8eb7e21, which is 5l24htbmlaabor09i3b372fj5 in base36.
  std::string value(300, 'a');
  EXPECT_EQ(std::string(230, 'a') + "5l24htbmlaabor09i3b372fj5",
            *PackedTerm::fitTerm(TermPolicy::HASH128, value, scratch));
  EXPECT_EQ(value.substr(0, 255), *PackedTerm::fitTerm(TermPolicy::TRUNCATE, value, scratch));
  EXPECT_FALSE(PackedTerm::fitTerm(TermPolicy::REJECT, value, scratch));
  for (auto policy : {TermPolicy::HASH128, TermPolicy::TRUNCATE, TermPolicy::REJECT}) {
    for (size_t size : {0u, 230u, 254u, 255u}) {
      auto shortValue = std::string_view(value).substr(0, size);
      auto term = PackedTerm::fitTerm(policy, shortValue, scratch);
      ASSERT_TRUE(term);
      EXPECT_EQ(shortValue, *term);
      EXPECT_EQ(shortValue.data(), term->data());
    }
  }
  // Backoff at byte 230 for 2-, 3-, and 4-byte UTF-8 codepoints.
  for (auto codepoint : {"\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80"}) {
    for (size_t split = 1; split < strlen(codepoint); split++) {
      std::string prefix(230 - split, 'a');
      value = prefix + codepoint + std::string(70, 'z');
      auto term = PackedTerm::fitTerm(TermPolicy::HASH128, value, scratch);
      ASSERT_TRUE(term);
      EXPECT_EQ(prefix.size() + 25, term->size());
      EXPECT_TRUE(PackedTerm::mayBeFitted(*term));
      EXPECT_EQ(prefix, term->substr(0, prefix.size()));
      EXPECT_EQ(prefix, PackedTerm::fitPrefix(TermPolicy::HASH128, value));
    }
  }
}

TEST_F(UtilTest, lazyMapBuildsOnRequestingThread) {
  // The builder runs creation itself rather than leaving it in a deque that
  // idle workers (or nobody) might pick up.
  SharedLazyMap<int, std::thread::id> map;
  oneapi::tbb::task_arena arena;
  arena.execute([&] {
    for (int key = 0; key < 100; key++) {
      auto builtOn = map.getOrCreate(key, [] { return std::make_shared<std::thread::id>(std::this_thread::get_id()); });
      ASSERT_EQ(std::this_thread::get_id(), *builtOn);
    }
  });
}

TEST_F(UtilTest, taskFanoutKeepsLastOnCaller) {
  auto caller = std::this_thread::get_id();
  std::array<std::thread::id, 4> threads, madeOn;
  std::array<int, 4> values;
  int64_t spawns;
  {
    TaskGroupRunner runner;
    luxir::test::TaskSpawnCounter counter;
    task_group_fanout(runner.group(), threads.size(),
        [&, state = std::make_unique<int>(0)](size_t i) mutable {
      madeOn[i] = std::this_thread::get_id();
      int value = ++*state;
      return [&, i, value]() {
        threads[i] = std::this_thread::get_id();
        values[i] = value;
      };
    });
    EXPECT_EQ(caller, threads.back());
    runner.join();
    spawns = counter.count();
  }
  EXPECT_EQ(3, spawns);
  for (auto thread : madeOn) EXPECT_EQ(caller, thread);
  EXPECT_EQ((std::array<int, 4>{1, 2, 3, 4}), values);

  std::vector<int> order;
  task_group_fanout(nullptr, 4, [&](size_t i) {
    return [&, i]() { order.push_back((int)i); };
  });
  EXPECT_EQ((std::vector<int>{0, 1, 2, 3}), order);
}

TEST_F(UtilTest, taskFanoutEmptyAndUnaryStayOnCaller) {
  int completed = 0;
  auto caller = std::this_thread::get_id();
  int64_t spawns;
  {
    TaskGroupRunner runner;
    luxir::test::TaskSpawnCounter counter;
    task_group_fanout(runner.group(), 0, [&](size_t) {
      ADD_FAILURE() << "empty fan-out invoked its factory";
      return [&]() { completed++; };
    });
    EXPECT_EQ(0, completed);
    task_group_fanout(runner.group(), 1, [&](size_t i) {
      EXPECT_EQ(0u, i);
      EXPECT_EQ(caller, std::this_thread::get_id());
      return [&]() {
        EXPECT_EQ(caller, std::this_thread::get_id());
        completed++;
      };
    });
    runner.join();
    spawns = counter.count();
  }
  EXPECT_EQ(1, completed);
  EXPECT_EQ(0, spawns);
}

TEST_F(UtilTest, taskGroupRunnerForksAreNotCounted) {
  std::atomic<int> completed{0};
  int64_t spawns;
  {
    TaskGroupRunner runner;
    luxir::test::TaskSpawnCounter counter;
    runner.run(true, [&] { completed.fetch_add(1); });
    runner.join();
    spawns = counter.count();
  }
  EXPECT_EQ(1, completed.load());
  EXPECT_EQ(0, spawns);
}

TEST_F(UtilTest, taskFanoutInlineFailureDrainsSiblings) {
  for (bool failFactory : {false, true}) {
    SCOPED_TRACE(failFactory);
    std::promise<void> started, release;
    auto starting = started.get_future();
    auto released = release.get_future();
    std::atomic<bool> completed{false};
    oneapi::tbb::task_arena arena(2);
    arena.execute([&] {
      auto fail = [&] {
        EXPECT_EQ(std::future_status::ready, starting.wait_for(std::chrono::seconds(10)));
        release.set_value();
        throw std::runtime_error("stop");
      };
      EXPECT_THROW({
        TaskGroupRunner runner;
        task_group_fanout(runner.group(), 2, [&](size_t i) {
          if (failFactory && i == 1) fail();
          return [&, i]() {
            if (i == 0) {
              started.set_value();
              released.wait();
              completed.store(true);
            } else {
              fail();
            }
          };
        });
      }, std::runtime_error);
    });
    EXPECT_TRUE(completed.load());
  }
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

namespace {

enum class LazyBuildOutcome { VALUE, NULL_VALUE, THROW, NON_STD_THROW };

template <typename GetOrCreate>
void checkLazyWaiters(GetOrCreate&& getOrCreate, LazyBuildOutcome outcome, bool cacheNull = false) {
  constexpr int numWaiters = 8;
  std::latch started(1), attached(numWaiters);
  std::atomic<int> builds{0}, failures{0};
  std::array<std::shared_ptr<int>, numWaiters + 1> values;
  bool fail = outcome == LazyBuildOutcome::THROW || outcome == LazyBuildOutcome::NON_STD_THROW;
  auto error = outcome == LazyBuildOutcome::NON_STD_THROW
      ? std::make_exception_ptr(42)
      : std::make_exception_ptr(std::runtime_error("original build error"));
  Signal::listen("sharedLazyBuildWait", [&](void*, void*, void*) -> void* {
    attached.count_down();
    return nullptr;
  });
  auto create = [&]() -> std::shared_ptr<int> {
    builds.fetch_add(1);
    started.count_down();
    attached.wait();
    if (fail) std::rethrow_exception(error);
    if (outcome == LazyBuildOutcome::NULL_VALUE) return nullptr;
    return std::make_shared<int>(42);
  };
  auto request = [&](int i) {
    try {
      values[i] = getOrCreate(create);
    } catch (...) {
      EXPECT_EQ(error, std::current_exception());
      failures.fetch_add(1);
    }
  };
  {
    std::jthread builder([&] { request(0); });
    started.wait();
    std::vector<std::jthread> waiters;
    for (int i = 1; i <= numWaiters; i++) {
      waiters.emplace_back([&, i] {
        if (i == 1) {
          tbb::task_arena arena(2);
          arena.execute([&] {
            tbb::this_task_arena::isolate([&] {
              tbb::task_group group;
              group.run([&] { request(i); });
              group.wait();
            });
          });
        } else {
          request(i);
        }
      });
    }
  }
  Signal::unlisten("sharedLazyBuildWait");
  EXPECT_EQ(1, builds.load());
  EXPECT_EQ(fail ? numWaiters + 1 : 0, failures.load());
  auto value = getOrCreate([&] {
    builds.fetch_add(1);
    return std::make_shared<int>(43);
  });
  bool retry = fail || (outcome == LazyBuildOutcome::NULL_VALUE && !cacheNull);
  EXPECT_EQ(retry ? 2 : 1, builds.load());
  if (retry || outcome == LazyBuildOutcome::VALUE) {
    ASSERT_NE(nullptr, value);
    EXPECT_EQ(retry ? 43 : 42, *value);
  } else {
    EXPECT_EQ(nullptr, value);
  }
  for (const auto& returned : values) {
    if (outcome == LazyBuildOutcome::VALUE) EXPECT_EQ(value, returned);
    else EXPECT_EQ(nullptr, returned);
  }
}

template <typename GetOrCreate>
void checkCanceledBuilder(GetOrCreate&& getOrCreate) {
  tbb::task_group request;
  std::shared_ptr<int> result;
  request.run([&] {
    request.cancel();
    result = getOrCreate([] { return std::make_shared<int>(42); });
  });
  request.wait();
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(42, *result);
}

} // namespace

TEST_F(UtilTest, lazyCellConcurrentOutcomesAndRetry) {
  for (auto outcome : {LazyBuildOutcome::VALUE, LazyBuildOutcome::NULL_VALUE,
                       LazyBuildOutcome::THROW, LazyBuildOutcome::NON_STD_THROW}) {
    SharedLazyCell<int, true> cached;
    checkLazyWaiters([&](auto&& create) { return cached.getOrCreate(create); }, outcome, true);
    SharedLazyCell<int, false> retried;
    checkLazyWaiters([&](auto&& create) { return retried.getOrCreate(create); }, outcome);
  }
}

TEST_F(UtilTest, lazyMapConcurrentOutcomesAndRetry) {
  for (auto outcome : {LazyBuildOutcome::VALUE, LazyBuildOutcome::NULL_VALUE,
                       LazyBuildOutcome::THROW, LazyBuildOutcome::NON_STD_THROW}) {
    SharedLazyMap<int, int> map;
    checkLazyWaiters([&](auto&& create) { return map.getOrCreate(7, create); }, outcome);
  }
}

TEST_F(UtilTest, lazyCellDoesNotInheritCancellation) {
  SharedLazyCell<int> cell;
  checkCanceledBuilder([&](auto&& create) { return cell.getOrCreate(create); });
}

TEST_F(UtilTest, lazyMapDoesNotInheritCancellation) {
  SharedLazyMap<int, int> map;
  checkCanceledBuilder([&](auto&& create) { return map.getOrCreate(7, create); });
}

#ifndef NDEBUG
TEST_F(UtilTest, lazyMapRejectsRecursiveBuild) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SharedLazyMap<int, int> map;
  EXPECT_DEATH({
    map.getOrCreate(7, [&] { return map.getOrCreate(7, [] { return std::make_shared<int>(42); }); });
  }, "recursive shared build");
}
#endif

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

TEST_F(UtilTest, arenaCreateCleanupRegistrationFailure) {
  // The real protobuf allocator needs another block for its first cleanup node.
  static thread_local bool rejectBlock = false;
  google::protobuf::ArenaOptions options;
  options.block_alloc = [](size_t bytes) -> void* {
    if (rejectBlock) throw std::bad_alloc();
    return ::operator new(bytes);
  };
  options.block_dealloc = [](void* ptr, size_t bytes) { ::operator delete(ptr, bytes); };
  struct Owner {
    std::unique_ptr<char[]> bytes = std::make_unique<char[]>(4096);
    int& destroyed;
    explicit Owner(int& destroyed) : destroyed(destroyed) { rejectBlock = true; }
    ~Owner() { ++destroyed; }
  };

  int destroyed = 0;
  {
    google::protobuf::Arena arena(options);
    auto reset = scope_guard([] { rejectBlock = false; });
    EXPECT_THROW(arenaCreate<Owner>(arena, destroyed), std::bad_alloc);
    EXPECT_EQ(1, destroyed);
  }
  EXPECT_EQ(1, destroyed); // Failed registration must not leave a second cleanup.
}
