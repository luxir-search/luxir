#include <benchmark/benchmark.h>
#include <random>
#include "solux/util/random.h"
#include "solux/util/heap.h"
#include "solux/util/solux_util.h"
#include "gtest/gtest.h"

using namespace solux;


/*
 We get a good benefit to updating top, even when comparisons are extremely cheap!
 (and small heaps will be common when merging segments in solux)
NOTE: the third variant (using std::pop_heap instead of solux::update_heap_top when actually
 popping the heap is slower!  This means the code/alg could still be improved, even though
 it's a win over a pop_heap/push_heap pair.

BM_heap<HeapStd>/1                 283 ns          283 ns      2473990 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<HeapStd>/2                 867 ns          867 ns       811567 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<HeapStd>/4                2154 ns         2153 ns       323482 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<HeapStd>/8                4836 ns         4836 ns       145425 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<HeapStd>/16              10860 ns        10860 ns        64461 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<HeapStd>/32              27184 ns        27184 ns        25595 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<HeapStd>/64             141802 ns       141803 ns         4922 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<HeapStd>/128            355947 ns       355948 ns         1972 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<HeapStd>/256            844061 ns       844063 ns          824 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<HeapStd>/512           1896734 ns      1896738 ns          369 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<HeapStd>/1024          4214839 ns      4214853 ns          166 fp=86.595k heapSz=1024 inc=85.8993M
BM_heap<UpdateTop>/1               222 ns          222 ns      3145728 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<UpdateTop>/2               597 ns          597 ns      1167607 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<UpdateTop>/4              1329 ns         1329 ns       526949 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<UpdateTop>/8              2886 ns         2886 ns       243580 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<UpdateTop>/16             6703 ns         6703 ns       103145 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<UpdateTop>/32            16722 ns        16722 ns        42134 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<UpdateTop>/64           114205 ns       114205 ns         6178 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<UpdateTop>/128          299698 ns       299699 ns         2350 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<UpdateTop>/256          717777 ns       717779 ns          969 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<UpdateTop>/512         1687674 ns      1687678 ns          419 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<UpdateTop>/1024        3863422 ns      3863430 ns          183 fp=86.595k heapSz=1024 inc=85.8993M
BM_heap<UpdateTopOnly>/1           265 ns          265 ns      2634123 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<UpdateTopOnly>/2           635 ns          635 ns      1111284 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<UpdateTopOnly>/4          1519 ns         1519 ns       463298 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<UpdateTopOnly>/8          3642 ns         3642 ns       193078 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<UpdateTopOnly>/16         8686 ns         8686 ns        80553 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<UpdateTopOnly>/32        20335 ns        20335 ns        34324 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<UpdateTopOnly>/64       114431 ns       114431 ns         6039 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<UpdateTopOnly>/128      306944 ns       306945 ns         2273 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<UpdateTopOnly>/256      737429 ns       737431 ns          952 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<UpdateTopOnly>/512     1673131 ns      1673136 ns          421 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<UpdateTopOnly>/1024    3786159 ns      3786167 ns          184 fp=86.595k heapSz=1024 inc=85.8993M
 */



class HeapStd {
public:
  static const bool useIdx = false;
  static uint64_t calcResult(std::vector<uint32_t>& data, std::vector<uint32_t *> &dataPointers, uint32_t maxIncrement, uint64_t seed) {
    unused(data);
    Rng rng(seed);
    uint64_t ret = 0;

    using elem = uint32_t *;

    auto cmp = [](const elem& a, const elem& b) { return *b < *a; };  // reversed for a min-heap

    auto start = dataPointers.begin();
    auto end = dataPointers.end();

    std::make_heap(start, end, cmp);

    while (start != end) {
      std::pop_heap(start, end, cmp);
      end--;
      elem top = *end;

      uint32_t currV = *top;
      ret = ret * 31 + currV;

      *top += rng.rint(1u, maxIncrement);
      if (*top > currV) {  // no overflow, so reinsert
        end++;
        std::push_heap(start, end, cmp);
      }
      assert(std::is_heap(start,end,cmp));
    }

    return ret;
  }
};


class UpdateTop {
public:
  static constexpr bool useIdx = false;
  static uint64_t calcResult(std::vector<uint32_t>& data, std::vector<uint32_t *> &dataPointers, uint32_t maxIncrement, uint64_t seed) {
    unused(data);
    Rng rng(seed);
    uint64_t ret = 0;

    using elem = uint32_t*;

    auto cmp = [](const elem& a, const elem& b) { return *b < *a; };  // reversed for a min-heap

    auto start = dataPointers.begin();
    auto end = dataPointers.end();

    std::make_heap(start, end, cmp);

    while (start != end) {
      elem top = *start;
      uint32_t currV = *top;
      ret = ret * 31 + currV;
      *top = currV + rng.rint(1u, maxIncrement);

      if (*top > currV) { // no overflow
        // just update the top of the heap
        update_heap_top(start, end, cmp);
      } else {
        // exhausted, need to pop heap top.
        std::pop_heap(start, end, cmp);
        end--;
      }
      assert(std::is_heap(start,end,cmp));
    }

    return ret;
  }
};

class indirectPQ {
public:
  static constexpr bool useIdx = false;

  static uint64_t calcResult(std::vector<uint32_t>& data, std::vector<uint32_t *> &dataPointers, uint32_t maxIncrement, uint64_t seed) {
    unused(data);
    // TODO: which constructor is used seems to affect timing (to the tune of ~3%, but consistently)... the seemingly more complex
    // constructor pq(data, dataPointers, true) is the faster one.
    // constexpr auto mycomp = [](const uint32_t& a, const uint32_t& b) { return b < a; }; // reversed comparator for min-heap
    // IndirectPQ<uint32_t, decltype(mycomp)> pq(data, dataPointers, true);
    IndirectPQ<uint32_t, std::greater<>> pq(dataPointers);
    // IndirectPQ<uint32_t, decltype(mycomp)> pq(dataPointers);

    Rng rng(seed);
    uint64_t ret = 0;

    while (pq.size() > 0) {
      uint32_t currV = pq.top();
      ret = ret * 31 + currV;
      pq.top() = currV + rng.rint(1u, maxIncrement);
      if (pq.top() > currV) { // no overflow
        pq.updateTop();
      } else {
        pq.removeTop();
      }
    }
    return ret;
  }
};



class UpdateTopIdx {
public:
  static constexpr bool useIdx = true;
  static uint64_t calcResult(std::vector<uint32_t>& data, std::vector<uint32_t> &idxArr, uint32_t maxIncrement, uint64_t seed) {
    Rng rng(seed);
    uint64_t ret = 0;

    uint32_t* dataArr = &data[0];
    uint32_t* arr = &idxArr[0];

    auto cmp = [dataArr](int32_t a, int32_t b) {
      return dataArr[b] < dataArr[a];
    };  // reversed for a min-heap

    uint32_t end = idxArr.size();  // current size of queue
    std::make_heap(arr, arr+end, cmp);

    while (end > 0) {
      uint32_t topIdx = arr[0];
      uint32_t currV = dataArr[topIdx];
      ret = ret * 31 + currV;
      dataArr[topIdx] = currV + rng.rint(1u, maxIncrement);

      if (dataArr[topIdx]  > currV) { // no overflow
        // just update the top of the heap
        update_heap_top(arr, arr+end, cmp);
      } else {
        // exhausted, need to pop heap top.
        std::pop_heap(arr, arr+end, cmp);
        end--;
      }
      assert(std::is_heap(arr,arr+end,cmp));
    }

    return ret;
  }
};


class UpdateTopOnly { // use our update_heap_top instead of pop_heap
public:
  static constexpr bool useIdx = false;
  static uint64_t calcResult(std::vector<uint32_t>& data, std::vector<uint32_t *> &dataPointers, uint32_t maxIncrement, uint64_t seed) {
    unused(data);
    Rng rng(seed);
    uint64_t ret = 0;

    using elem = uint32_t*;

    auto cmp = [](const elem& a, const elem& b) { return *b < *a; };  // reversed for a min-heap

    auto start = dataPointers.begin();
    auto end = dataPointers.end();

    std::make_heap(start, end, cmp);

    while (start != end) {
      elem top = *start;
      uint32_t currV = *top;
      ret = ret * 31 + currV;
      *top = currV + rng.rint(1u, maxIncrement);

      if (*top > currV) { // no overflow
        // just update the top of the heap
        update_heap_top(start, end, cmp);
      } else {
        // exhausted, need to pop heap top.
        end--;
        *start = *end;
        update_heap_top(start, end, cmp);
      }
      assert(std::is_heap(start,end,cmp));
    }

    return ret;
  }
};

template <class HeapImpl>
static void BM_heap(benchmark::State& state) {
  uint64_t result = 1;
  Rng rng(1);
  uint32_t maxsz = state.range(0); // number of iterators / streams to merge
  uint32_t maxinc = 0xffffffffu / 50;  // average of 100 "advances" before exhausting
  std::vector<uint32_t> origData(maxsz);
  std::vector<uint32_t*> origHeap(maxsz);
  std::vector<uint32_t> origDataIdx(maxsz);

  std::vector<uint32_t> data(maxsz);
  std::vector<uint32_t*> heap(maxsz);
  std::vector<uint32_t> dataIdx(maxsz);
  for (uint32_t i=0; i<maxsz; i++) {
    origData[i] = rng.rint(0u, maxinc);
    origHeap[i] = &data[i];
    origDataIdx[i] = i;
  }

  HeapImpl heapImpl;

  for (auto _ : state) {
    data = origData;  // reset data
    if constexpr (heapImpl.useIdx) {
      dataIdx = origDataIdx;
      result = heapImpl.calcResult(data, dataIdx, maxinc, 2);
    } else {
      heap = origHeap;  // reset data
      result = heapImpl.calcResult(data, heap, maxinc, 2);
    }
    benchmark::DoNotOptimize(result);
    benchmark::ClobberMemory();
  }
  ASSERT_TRUE(result != 0);

  state.counters["fp"] = double(result % 100000);
  state.counters["heapSz"] = maxsz;
  state.counters["inc"] = maxinc;
}


// #define RUN_DISABLED_BENCHMARKS
#ifdef RUN_DISABLED_BENCHMARKS
BENCHMARK(BM_heap<HeapStd>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<UpdateTop>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<indirectPQ>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<UpdateTopIdx>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<UpdateTopOnly>)->RangeMultiplier(2)->Range(1, 1<<10);
#else
inline void hackety_hack() {
  solux::unused(hackety_hack);
  solux::unused(BM_heap<HeapStd>);
  solux::unused(BM_heap<UpdateTop>);
  solux::unused(BM_heap<indirectPQ>);
  solux::unused(BM_heap<UpdateTopIdx>);
  solux::unused(BM_heap<UpdateTopOnly>);
}
#endif