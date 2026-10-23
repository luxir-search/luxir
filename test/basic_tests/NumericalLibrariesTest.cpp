// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <future>
#include <omp.h>
#include <oneapi/tbb/info.h>
#include "luxir/util/NumericalLibraries.h"

namespace luxir {

TEST(NumericalLibrariesTest, ConcurrentBuildBudgetsRestoreCallingTasks) {
  int expected = std::getenv("LUXIR_VECTOR_BUILD_THREADS")
      ? std::atoi(std::getenv("LUXIR_VECTOR_BUILD_THREADS"))
      : std::max(1, oneapi::tbb::info::default_concurrency());
  EXPECT_EQ(expected, vectorBuildThreads());
  std::array<std::future<bool>, 4> work;
  for (int i = 0; i < (int)work.size(); i++) {
    work[i] = std::async(std::launch::async, [i, expected] {
      omp_set_num_threads(i + 2);
      try {
        VectorBuildThreads budget;
        if (omp_get_max_threads() != expected) return false;
        int observed = 0;
        #pragma omp parallel reduction(max:observed)
        observed = omp_get_num_threads();
        if (observed < 1 || observed > expected) return false;
        throw 1;
      } catch (int) {
        return omp_get_max_threads() == i + 2;
      }
      return false;
    });
  }
  for (auto& task : work) EXPECT_TRUE(task.get());
}

}
