// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <oneapi/tbb/task_arena.h>

#include "luxir/index/DocBisection.h"

using namespace luxir;

namespace {

// Doc d belongs to cluster d % clusters and holds `perDoc` terms of that
// cluster's own `vocabulary` terms, picked by a fixed hash of (d, i).
DocBisection::ForwardIndex clustered(uint32_t docs, uint32_t clusters, uint32_t vocabulary, uint32_t perDoc) {
  DocBisection::ForwardIndex index;
  index.numTerms = clusters * vocabulary;
  index.offsets.push_back(0);
  for (uint32_t d = 0; d < docs; d++) {
    std::vector<uint32_t> terms;
    for (uint32_t i = 0; i < perDoc; i++) {
      uint32_t h = (d * 2654435761u) ^ (i * 40503u + 17u);
      h ^= h >> 13;
      terms.push_back((d % clusters) * vocabulary + (h * 2246822519u >> 7) % vocabulary);
    }
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    index.terms.insert(index.terms.end(), terms.begin(), terms.end());
    index.offsets.push_back((uint32_t)index.terms.size());
  }
  return index;
}

// Adjacent positions whose docs belong to different clusters.
int switches(const std::vector<uint32_t>& order, uint32_t clusters) {
  int out = 0;
  for (size_t i = 1; i < order.size(); i++) out += order[i] % clusters != order[i - 1] % clusters;
  return out;
}

}  // namespace

// Two interleaved clusters of 64 docs each: the first split separates them.
TEST(DocBisectionTest, separatesInterleavedClusters) {
  auto index = clustered(128, 2, 16, 6);
  auto order = DocBisection::order(index);
  std::vector<uint32_t> sorted = order;
  std::sort(sorted.begin(), sorted.end());
  std::vector<uint32_t> identity(128);
  std::iota(identity.begin(), identity.end(), 0u);
  EXPECT_EQ(identity, sorted);
  EXPECT_EQ(1, switches(order, 2));
}

// Large enough to count, score, sort and recurse in parallel: one thread and
// many give the same order, and the four clusters come out contiguous.
TEST(DocBisectionTest, orderDoesNotDependOnThreadCount) {
  auto index = clustered(40000, 4, 200, 12);
  std::vector<uint32_t> serial, parallel;
  oneapi::tbb::task_arena(1).execute([&] { serial = DocBisection::order(index); });
  oneapi::tbb::task_arena(8).execute([&] { parallel = DocBisection::order(index); });
  EXPECT_EQ(serial, parallel);
  EXPECT_EQ(3, switches(serial, 4));
  EXPECT_EQ(serial, DocBisection::order(index));
}

TEST(DocBisectionTest, fastLog2) {
  for (uint32_t x : {1u, 2u, 3u, 1000u, 4095u, 4096u, 4097u, 100000u, 1u << 30}) {
    EXPECT_NEAR(std::log2((double)x), DocBisection::fastLog2(x), 1e-3) << x;
  }
}
