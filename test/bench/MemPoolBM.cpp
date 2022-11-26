#include "benchmark/benchmark.h"
#include "test/SoluxTest.h"
#include <memory_resource>
#include <latch>
#include <google/protobuf/arena.h>
#include "tbb/task_group.h"

#include "solux/util/MemPool.h"
#include "solux/util/random.h"
#include "solux/util/solux_util.h"

using namespace solux;

/* solux::MemPool vs std::pmr::monotonic_buffer_resource vs protobuf3 Arena
   By default (linux/g++12), std::pmr::monotonic_buffer_resource starts with an initial allocation size of 1024+64.
   Subsequent allocation sizes multiply the large part (1024) by 1.5 and then add 64.
   For a fair comparison with MemPool, we start with the same allocation size.

   RESULTS:
     MemPool and std::pmr::monotonic_buffer_resource are the same speed on g++, but MemPool is faster on clang.
     Protobuf Arena does really well considering that it's allocation is thread safe!  Although it will allocate
     a new block for each thread that allocates from it.
     Pre-allocating the memory for the Arena does not help (this is testing *many* small allocations though)

g++: Release (NDEBUG) __OPTIMIZE__=1 __cplusplus=202100 __GNUC__=12 __VERSION__=12.2.0 _GLIBCXX_RELEASE=12 __GLIBCXX__=20220819 __linux__=1
------------------------------------------------------------
Benchmark                  Time             CPU   Iterations
------------------------------------------------------------
BM_AllocSmall_std           14696 ns        14696 ns        47707
BM_AllocSmall_std_mono       1384 ns         1384 ns       507392
BM_AllocSmall_MemPool        1413 ns         1413 ns       493202
BM_AllocSmall_Arena          1492 ns         1492 ns       473429
BM_AllocSmall_ArenaPreAlloc  1498 ns         1498 ns       467081

clang: Release (NDEBUG) __OPTIMIZE__=1 __cplusplus=202101 __clang__=1 __GNUC__=4 __VERSION__=Ubuntu Clang 15.0.5 _GLIBCXX_RELEASE=12 __GLIBCXX__=20220819 __linux__=1
------------------------------------------------------------
Benchmark                  Time             CPU   Iterations
------------------------------------------------------------
BM_AllocSmall_std           14670 ns        14670 ns        47230
BM_AllocSmall_std_mono       1354 ns         1354 ns       533164
BM_AllocSmall_MemPool         580 ns          580 ns      1210302
BM_AllocSmall_Arena          1214 ns         1214 ns       573933
*/

/*  std::allocator vs std::pmr::unsynchronized_pool_resource
    It looks like the unsynchronized pool resource is actually a little
    slower than the default allocator.  But notice the time vs CPU time!
    This was done inside WSL, so we should try to repo/verify outside as well.

-------------------------------------------------------------------
Benchmark                         Time             CPU   Iterations
-------------------------------------------------------------------
BM_AllocFree_default/1     10513498 ns     10513488 ns           67
BM_AllocFree_default/8     12343269 ns     11600178 ns           59
BM_AllocFree_default/16    16736317 ns     14930504 ns           49
BM_AllocFree_std_pool/1    12306869 ns     12306774 ns           57
BM_AllocFree_std_pool/8    15114711 ns     14096988 ns           50
BM_AllocFree_std_pool/16   17871094 ns     16231363 ns           43
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

// wrapper for protobuf Arena so we can try different arena options. initSize is the size of the pre-allocated block
// which is normally 0.
template <size_t allocSize, size_t defaultAlignment, size_t initSize=0>
class arena_resource {
public:
  char startBuffer[initSize];
  google::protobuf::ArenaOptions options;
  google::protobuf::Arena arena;

  google::protobuf::ArenaOptions getOptions() {
    google::protobuf::ArenaOptions options;
    options.start_block_size = allocSize;
    if (initSize > 0) {
      options.initial_block = startBuffer;
      options.initial_block_size = initSize;
    }
    return options;
  }

  // constructor that uses arena options
  arena_resource() : options(getOptions()), arena(options) {
  }

  void *allocate(std::size_t bytes, std::size_t alignment = defaultAlignment) {
    return arena.AllocateAligned(bytes, alignment);
  }
};


// wrapper around standard allocator that keeps track of all it's pointers
class default_allocator {
  std::vector<std::unique_ptr<char[]>> pointers;
public:
  default_allocator() {
  }

  void *allocate(std::size_t bytes, std::size_t alignment = 1) {
    void *ptr = new(std::align_val_t(alignment)) char[bytes];
    pointers.emplace_back(std::unique_ptr<char[]>((char*)ptr));
    return &*pointers.back().get();
  };
};


template <class Allocator>
static char* alloc(Allocator& allocator, size_t bytes) {
  if constexpr (std::is_same_v<Allocator, MemPool>) {
    return (char*)allocator.alloc(bytes);
  } else {
    return (char*)allocator.allocate(bytes);
  }
}

template <class Allocator>
static uint64_t smallAlloc(solux::Rng& rng) {
  Allocator allocator;
  auto info = rng();
  for (int i=0; i<500; i++) {   // 500 allocations should fit in first block
    auto sz = (rng()&0x003f)+1;  // up to 64 bytes

    // If the Allocator is of type MemPool, use alloc method else use allocate method.
    char* ptr = alloc<Allocator>(allocator, sz);

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

static void BM_AllocSmall_std(benchmark::State& state) {
  benchAlloc<default_allocator>(state);
}

static void BM_AllocSmall_std_mono(benchmark::State& state) {
  // benchAlloc<std::pmr::monotonic_buffer_resource>(state);
  benchAlloc<pmr_resource<32768,1>>(state);  // test monotonic_buffer_resource with same starting size as MemPool
}

static void BM_AllocSmall_MemPool(benchmark::State& state) {
  benchAlloc<MemPool>(state);
}
static void BM_AllocSmall_Arena(benchmark::State& state) {
  benchAlloc<arena_resource<32768,1>>(state);
}
static void BM_AllocSmall_ArenaPreAlloc(benchmark::State& state) {
  benchAlloc<arena_resource<32768,1,32768>>(state);
}


//////////////////////////////////////////////////////////////////////

template <class Allocator>
static uint64_t allocFree(int iterations, int taskno, Allocator& allocator) {
  // std::cout << "TASK " << taskno << " START iterations=" << iterations << std::endl;

  solux::Rng rng(taskno+1);
  auto info = rng();
  int max = 1024;
  char** buffers = (char**) alloc<Allocator>(allocator, sizeof(char *) * max);

  int nbuf = 0;

  for (int i=0; i<iterations; i++) {
    int nalloc = rng.rint(1, max-nbuf);
    for (int j = 0; j<nalloc; j++) {
      auto sz = ((rng() & 0x00f) + 1) * 16;  // only 16 different allocation sizes (which are multiples of 16)
      // should a good scenario for a pool allocator
      char *ptr = (char *) alloc<Allocator>(allocator, sz);
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

// minimal work to test that threading isn't introducing too much overhead
template <class Allocator>
static uint64_t allocFreeDummy(int iterations, int taskno, Allocator& allocator) {
  char* ptr = (char*) allocator.alloc(1);
  uint64_t ret = (uint64_t)ptr;
  allocator.deallocate(ptr,1);
  return ret + taskno + iterations;
}

template <typename Allocator>
inline void benchAllocFree(benchmark::State& state) {
  auto numThreads = state.range(0);
  int iterations = 1024;

  std::vector<std::unique_ptr<Allocator>> allocators;
  for (int i=0; i<numThreads; i++) {
    allocators.emplace_back(std::make_unique<Allocator>());
  }

  for (auto _ : state) {
    tbb::task_group tasks;  // TODO: could create a flow graph outside of this loop to eliminate that overhead.
    for (int i=0; i<numThreads; i++) {
        tasks.run([&,i](){
          allocFree<Allocator>(iterations, i, *allocators[i]);
        });
    }
    tasks.wait();
  }
};

BENCHMARK(BM_AllocSmall_std);
BENCHMARK(BM_AllocSmall_std_mono);
BENCHMARK(BM_AllocSmall_MemPool);
BENCHMARK(BM_AllocSmall_Arena);
BENCHMARK(BM_AllocSmall_ArenaPreAlloc);

// #define RUN_DISABLED_BENCHMARKS
#ifdef RUN_DISABLED_BENCHMARKS
// testing std::allocator vs std::pmr::unsynchronized_pool_resource
static void BM_AllocFree_default(benchmark::State& state) {
  benchAllocFree<std::allocator<char>>(state);
}
static void BM_AllocFree_std_pool(benchmark::State& state) {
  benchAllocFree<std::pmr::unsynchronized_pool_resource>(state);
}

BENCHMARK(BM_AllocFree_default)->Range(1,16)->RangeMultiplier(2)->UseRealTime();
BENCHMARK(BM_AllocFree_std_pool)->Range(1,16)->RangeMultiplier(2)->UseRealTime();

#else
inline void hackety_hack() {
  solux::unused(hackety_hack);
}
#endif