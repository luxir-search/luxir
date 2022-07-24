#include "benchmark/benchmark.h"
#include "test/SoluxTest.h"
#include <memory_resource>
#include <latch>

#include "solux/util/MemPool.h"
#include "solux/util/random.h"
#include "solux/util/solux_util.h"

using namespace solux;

/* solux::MemPool vs std::pmr::monotonic_buffer_resource
   By default (linux/g++12), std::pmr::monotonic_buffer_resource starts with an initial allocation size of 1024+64.
   Subsequent allocation sizes multiply the large part (1024) by 1.5 and then add 64.
   For a fair comparison with MemPool, we should start of with the same allocation size.

   RESULTS:
     MemPool and std::pmr::monotonic_buffer_resource are the same speed on g++, but MemPool is faster on clang.

g++: Release (NDEBUG) __OPTIMIZE__=1 __cplusplus=202100 __GNUC__=12 __VERSION__=12.1.0 _GLIBCXX_RELEASE=12 __GLIBCXX__=20220513 __linux__=1
------------------------------------------------------------
Benchmark                  Time             CPU   Iterations
------------------------------------------------------------
BM_Alloc_std_mono       1432 ns         1432 ns       489553
BM_Alloc_MemPool        1452 ns         1452 ns       483181

clang: Release (NDEBUG) __OPTIMIZE__=1 __cplusplus=202101 __clang__=1 __GNUC__=4 __VERSION__=Ubuntu Clang 14.0.6 _GLIBCXX_RELEASE=12 __GLIBCXX__=20220513 __linux__=1
------------------------------------------------------------
Benchmark                  Time             CPU   Iterations
------------------------------------------------------------
BM_Alloc_std_mono       1379 ns         1379 ns       508991
BM_Alloc_MemPool         716 ns          716 ns       977285
*/

/*  std::allocator vs std::pmr::unsynchronized_pool_resource
    It looks like the unsynchronized pool resource is actually a little
    slower than the default allocator.  But notice the time vs CPU time!
    This was done inside WSL, so we should try to repo/verify outside as well.
    The threading was also handled by Taskflow, so we should verify that
    it's faster than just using normal threads.

-------------------------------------------------------------------
Benchmark                         Time             CPU   Iterations
-------------------------------------------------------------------
BM_AllocFree_default/1     10608454 ns        30434 ns         1000
BM_AllocFree_default/8     15147544 ns        86153 ns         1000
BM_AllocFree_default/16    16755401 ns       158202 ns         1000
BM_AllocFree_std_pool/1    12475539 ns        31116 ns         1000
BM_AllocFree_std_pool/8    16707314 ns        85468 ns         1000
BM_AllocFree_std_pool/16   18341609 ns       155103 ns         1000
BM_AllocSmall_std_mono         1442 ns         1442 ns       482492
BM_AllocSmall_MemPool          1473 ns         1473 ns       477944
 */


// wrapper to change the default size and alignment
template <size_t initSize, size_t defaultAlignment>
class pmr_resource : public std::pmr::monotonic_buffer_resource {
public:
  pmr_resource() : std::pmr::monotonic_buffer_resource(initSize) {
  }

  void *allocate(std::size_t bytes, std::size_t alignment = defaultAlignment) {
    return std::pmr::monotonic_buffer_resource::allocate(bytes, alignment);
  };
};

/*
// wrapper around standard allocator (since type isn't available in c++ standards?)
template <size_t initSize, size_t defaultAlignment>
class default_allocator : public std::pmr::memory_resource {
  std::allocator<> allocator;
public:
  default_allocator() {

  }

  void *allocate(std::size_t bytes, std::size_t alignment = defaultAlignment) {
    return allocator.allocate(bytes, alignment);
  };
  void *deallocate(void* ptr, std::size_t bytes, std::size_t alignment = defaultAlignment) {
    return allocator.deallocate(ptr, bytes, alignment);
  };
};
*/

  /*
// wrapper to change default initSize and align size. This doesn't inherit from me
template <size_t initSize, size_t defaultAlignSize>
class pmr_wrapper {
public:
  std::pmr::monotonic_buffer_resource
  void* allocate(std::size_t bytes, std::size_t alignment = defaultAlignSize) {

  }


};
*/

template <class Allocator>
static uint64_t smallAlloc(solux::Rng& rng) {
  Allocator allocator;
  auto info = rng();
  for (int i=0; i<500; i++) {   // 500 allocations should fit in first block
    auto sz = (rng()&0x003f)+1;  // up to 64 bytes
    char* ptr = (char*)allocator.allocate(sz);
    if (ptr == nullptr) {
      std::cout << "ERROR! null pointer!" << std::endl;
    }
    *ptr = (char)sz;  // use the memory
    // use the address of the memory to try and avoid optimization
    info += (uint64_t)ptr;
  }
  return info;
};


template <class Allocator>
inline void benchAlloc(benchmark::State& state) {
  uint64_t result = 0;
  for (auto _ : state) {
    result += smallAlloc<Allocator>(SoluxTest::rng);
    benchmark::DoNotOptimize(result);
    benchmark::ClobberMemory();
  }
};

static void BM_AllocSmall_std_mono(benchmark::State& state) {
  // benchAlloc<std::pmr::monotonic_buffer_resource>(state);
  benchAlloc<pmr_resource<32768,1>>(state);  // test monotonic_buffer_resource with same starting size as MemPool
}

static void BM_AllocSmall_MemPool(benchmark::State& state) {
  benchAlloc<MemPool>(state);
}


//////////////////////////////////////////////////////////////////////

template <class Allocator>
static uint64_t allocFree(int iterations, int taskno, Allocator& allocator) {
  // std::cout << "TASK " << taskno << " START iterations=" << iterations << std::endl;

  solux::Rng rng(taskno+1);
  auto info = rng();
  int max = 1024;
  char** buffers = (char**)allocator.allocate(sizeof(char*)*max);
  int nbuf = 0;

  for (int i=0; i<iterations; i++) {
    int nalloc = rng.rint(1, max-nbuf);
    for (int j = 0; j<nalloc; j++) {
      auto sz = ((rng() & 0x00f) + 1) * 16;  // only 16 different allocation sizes (which are multiples of 16)
      // should a good scenario for a pool allocator
      char *ptr = (char *) allocator.allocate(sz);
      if (ptr == nullptr) {
        std::cout << "ERROR! null pointer!" << std::endl;
      }
      *(int *) ptr = (int) sz;  // use the memory to store the size
      info += (uint64_t) ptr;
      buffers[nbuf++] = ptr;
    }

    // now do some deallocations
    nalloc = nbuf < 10 ? 0 : rng.rint(1,nbuf);
    for (int j = 0; j<nalloc; j++) {
      // random deallocs, or deallocs from the top?
      // could also pick a random point and deallocate randomly above that.
      int slot = rng.rint(0, nbuf);
      char* ptr = buffers[slot];
      buffers[slot] = buffers[--nbuf];
      int sz = *(int *) ptr;
      allocator.deallocate(ptr, sz);
    }
  }

  // now free everything!
  for (int slot=0; slot<nbuf; slot++) {
    char* ptr = buffers[slot];
    int sz = *(int *) ptr;
    allocator.deallocate(ptr, sz);
  }

  allocator.deallocate((char*)buffers, sizeof(char*)*max);

  benchmark::DoNotOptimize(info);
  benchmark::ClobberMemory();
  // std::cout << "\tTASK " << taskno << " END" << iterations << std::endl;

  return info;
};

template <typename Allocator>
inline void benchAllocFree(benchmark::State& state) {
  auto numThreads = state.range(0);
  uint64_t result = 0;
  int iterations = 1024;
  tf::Executor& exec = SoluxTest::executor();
  std::vector<std::unique_ptr<tf::Taskflow>> jobs;
  std::vector<std::unique_ptr<Allocator>> allocators;
  tf::Taskflow taskflow;
  auto doneTask = taskflow.emplace([&](){result++;});
  for (int i=0; i<numThreads; i++) {
    allocators.emplace_back(std::make_unique<Allocator>());
    auto task = taskflow.emplace(
            [&,i](){allocFree<Allocator>(iterations, i, *allocators[i]);}
    );
    task.precede(doneTask);
  }

  for (auto _ : state) {
    exec.run(taskflow).wait();
  }
};

BENCHMARK(BM_AllocSmall_std_mono);
BENCHMARK(BM_AllocSmall_MemPool);

// #define RUN_DISABLED_BENCHMARKS
#ifdef RUN_DISABLED_BENCHMARKS
// testing std::allocator vs std::pmr::unsynchronized_pool_resource
static void BM_AllocFree_default(benchmark::State& state) {
  benchAllocFree<std::allocator<char>>(state);
}
static void BM_AllocFree_std_pool(benchmark::State& state) {
  benchAllocFree<std::pmr::unsynchronized_pool_resource>(state);
}

BENCHMARK(BM_AllocFree_default)->Range(1,16)->RangeMultiplier(2);
BENCHMARK(BM_AllocFree_std_pool)->Range(1,16)->RangeMultiplier(2);

#else
inline void hackety_hack() {
  solux::unused(hackety_hack);
}
#endif