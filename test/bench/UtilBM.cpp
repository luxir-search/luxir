#include <benchmark/benchmark.h>
#include <random>
#include "luxir/util/random.h"
#include "luxir/util/heap.h"
#include "luxir/util/luxir_util.h"
#include "gtest/gtest.h"

using namespace luxir;


/*
 We get a good benefit to updating top, even when comparisons are extremely cheap!
 (and small heaps will be common when merging segments in luxir)
NOTE: the UpdateTopOnly variant that uses luxir::update_heap_top to pop as well is slower than
 using std::heap_pop (the UpdateTop variant).  This menas our update_heap_top could still be improved
 even though it's still a win over a pop_heap/push_heap pair.

--------------------------------------------------------------------------------------
Benchmark                            Time             CPU   Iterations UserCounters...
--------------------------------------------------------------------------------------
BM_heap<HeapStd>/1                 235 ns          235 ns      2940261 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<HeapStd>/2                 746 ns          746 ns       935688 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<HeapStd>/4                2214 ns         2214 ns       317943 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<HeapStd>/8                4555 ns         4555 ns       153293 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<HeapStd>/16              10600 ns        10600 ns        70455 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<HeapStd>/32              24197 ns        24197 ns        28803 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<HeapStd>/64             138199 ns       138200 ns         5002 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<HeapStd>/128            346165 ns       346166 ns         2035 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<HeapStd>/256            815782 ns       815784 ns          858 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<HeapStd>/512           1840102 ns      1840107 ns          380 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<HeapStd>/1024          4125422 ns      4125430 ns          170 fp=86.595k heapSz=1024 inc=85.8993M
BM_heap<UpdateTop>/1               212 ns          212 ns      3308968 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<UpdateTop>/2               592 ns          592 ns      1189410 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<UpdateTop>/4              1315 ns         1315 ns       533500 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<UpdateTop>/8              2831 ns         2831 ns       246493 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<UpdateTop>/16             6757 ns         6757 ns       104389 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<UpdateTop>/32            15884 ns        15884 ns        43840 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<UpdateTop>/64           109331 ns       109332 ns         6341 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<UpdateTop>/128          287359 ns       287359 ns         2432 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<UpdateTop>/256          692402 ns       692404 ns         1009 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<UpdateTop>/512         1571691 ns      1571693 ns          442 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<UpdateTop>/1024        3532082 ns      3532086 ns          194 fp=86.595k heapSz=1024 inc=85.8993M
BM_heap<indirectPQ>/1              213 ns          213 ns      3274524 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<indirectPQ>/2              615 ns          615 ns      1135151 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<indirectPQ>/4             1301 ns         1301 ns       532143 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<indirectPQ>/8             2904 ns         2904 ns       243741 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<indirectPQ>/16            6523 ns         6523 ns       105288 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<indirectPQ>/32           16476 ns        16476 ns        42395 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<indirectPQ>/64          111391 ns       111392 ns         6302 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<indirectPQ>/128         292012 ns       292012 ns         2402 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<indirectPQ>/256         694250 ns       694251 ns          998 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<indirectPQ>/512        1618223 ns      1618226 ns          434 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<indirectPQ>/1024       3757810 ns      3757817 ns          188 fp=86.595k heapSz=1024 inc=85.8993M
BM_heap<UpdateTopIdx>/1            301 ns          301 ns      2315344 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<UpdateTopIdx>/2            717 ns          717 ns       969175 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<UpdateTopIdx>/4           1613 ns         1613 ns       434508 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<UpdateTopIdx>/8           3783 ns         3783 ns       183220 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<UpdateTopIdx>/16          9019 ns         9019 ns        78243 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<UpdateTopIdx>/32         20946 ns        20946 ns        33148 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<UpdateTopIdx>/64        120482 ns       120482 ns         5835 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<UpdateTopIdx>/128       318579 ns       318577 ns         2201 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<UpdateTopIdx>/256       759204 ns       759206 ns          922 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<UpdateTopIdx>/512      1738472 ns      1738475 ns          398 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<UpdateTopIdx>/1024     3940907 ns      3940915 ns          177 fp=86.595k heapSz=1024 inc=85.8993M
BM_heap<UpdateTopOnly>/1           239 ns          239 ns      2956996 fp=60.846k heapSz=1 inc=85.8993M
BM_heap<UpdateTopOnly>/2           630 ns          630 ns      1092804 fp=44.171k heapSz=2 inc=85.8993M
BM_heap<UpdateTopOnly>/4          1471 ns         1471 ns       478788 fp=33.224k heapSz=4 inc=85.8993M
BM_heap<UpdateTopOnly>/8          3182 ns         3182 ns       218524 fp=82.394k heapSz=8 inc=85.8993M
BM_heap<UpdateTopOnly>/16         7776 ns         7776 ns        90064 fp=39.814k heapSz=16 inc=85.8993M
BM_heap<UpdateTopOnly>/32        18432 ns        18432 ns        37619 fp=28.994k heapSz=32 inc=85.8993M
BM_heap<UpdateTopOnly>/64       113990 ns       113990 ns         6191 fp=54.48k heapSz=64 inc=85.8993M
BM_heap<UpdateTopOnly>/128      300503 ns       300503 ns         2340 fp=1.982k heapSz=128 inc=85.8993M
BM_heap<UpdateTopOnly>/256      723013 ns       723014 ns          975 fp=78.669k heapSz=256 inc=85.8993M
BM_heap<UpdateTopOnly>/512     1683227 ns      1683230 ns          415 fp=4.785k heapSz=512 inc=85.8993M
BM_heap<UpdateTopOnly>/1024    3867436 ns      3867444 ns          181 fp=86.595k heapSz=1024 inc=85.8993M
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

class indexedPQ {
public:
  static constexpr bool useIdx = true;

  static uint64_t calcResult(std::vector<uint32_t>& data, std::vector<uint32_t> &idxArr, uint32_t maxIncrement, uint64_t seed) {
    unused(data);
    IndexedPQ<uint32_t, std::greater<>, uint32_t> pq(data, idxArr, false);

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

  state.counters["fp"] = double(result % 100000);  // TODO: the fingerprint is different between gcc and clang!!! Why?
  state.counters["heapSz"] = maxsz;
  state.counters["inc"] = maxinc;
}


// #define RUN_DISABLED_BENCHMARKS
#ifdef RUN_DISABLED_BENCHMARKS
BENCHMARK(BM_heap<HeapStd>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<UpdateTop>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<indirectPQ>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<indexedPQ>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<UpdateTopIdx>)->RangeMultiplier(2)->Range(1, 1<<10);
BENCHMARK(BM_heap<UpdateTopOnly>)->RangeMultiplier(2)->Range(1, 1<<10);
#else
inline void hackety_hack() {
  luxir::unused(hackety_hack);
  luxir::unused(BM_heap<HeapStd>);
  luxir::unused(BM_heap<UpdateTop>);
  luxir::unused(BM_heap<indirectPQ>);
  luxir::unused(BM_heap<indexedPQ>);
  luxir::unused(BM_heap<UpdateTopIdx>);
  luxir::unused(BM_heap<UpdateTopOnly>);
}
#endif