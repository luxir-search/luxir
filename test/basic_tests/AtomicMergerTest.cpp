// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <gtest/gtest.h>
#include <iostream>

#include "test/LuxirTest.h"
#include "luxir/util/AtomicMerger.h"

using namespace std;

namespace luxir::test {

class AtomicMergerTest : public LuxirTest {
public:
  class Data : public MergeableData {
  public:
    int64_t value = 0;

    static Data* merge(Data* a, Data* b) {
      if (a->value < b->value)
      {
        std::swap(a,b);
      }

      a->value += b->value;
      return a;
    }
  };

  AtomicMerger<Data> dataMerger;
};


TEST_F(AtomicMergerTest, basic) {
  // auto count = 100'000'000;
  auto count = 1000;
  auto requestsPerThread = count / (int)std::thread::hardware_concurrency();
  count = requestsPerThread * std::thread::hardware_concurrency(); // round down to a multiple of threads
  std::atomic<int64_t> largestValue(0);

  // spin up a number of threads to test the AtomicMerger
  std::vector<std::thread> threads;
  int nThreads = (int)std::thread::hardware_concurrency();
  for (int i = 0; i < nThreads; ++i) {
    threads.emplace_back([this, requestsPerThread, &largestValue] {
      int64_t num = 0;
      for (int j = 0; j < requestsPerThread; ++j) {
        auto* data = dataMerger.obtain();
        data->value = 1;
        num = dataMerger.release(data);
      }
      // update the largest value seen so far
      int64_t currentLargest = largestValue.load(std::memory_order_relaxed);
      while (num > currentLargest) {
        if (largestValue.compare_exchange_strong(currentLargest, num, std::memory_order_relaxed)) {
          break; // successfully updated the largest value
        }
      }
    });
  }

  // wait for all the threads to finish
  for (auto& thread : threads) {
    thread.join();
  }
  // now check the largest value
  int64_t finalLargest = largestValue.load(std::memory_order_relaxed);
  EXPECT_EQ(finalLargest, count);
}

} // end namespace