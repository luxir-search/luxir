// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "luxir/index/EMeans.h"

#include <faiss/Clustering.h>
#include <faiss/IndexFlat.h>
#include <faiss/utils/distances.h>

#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <vector>

using namespace luxir;

namespace {

// n points around `blobs` random centers, plus Gaussian noise; unit rows
// when `unit`.
std::vector<float> blobData(int64_t n, int32_t d, int32_t blobs, uint32_t seed, bool unit = false) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss;
  std::vector<float> centers((size_t)blobs * d);
  for (float& c : centers) c = 4 * gauss(rng);
  std::vector<float> x((size_t)n * d);
  for (int64_t i = 0; i < n; i++) {
    const float* c = centers.data() + (size_t)(rng() % blobs) * d;
    for (int32_t t = 0; t < d; t++) x[(size_t)i * d + t] = c[t] + gauss(rng);
  }
  if (unit) faiss::fvec_renorm_L2((size_t)d, (size_t)n, x.data());
  return x;
}

// Mean distance of x to its nearest centroid in index (1 - similarity for
// inner product), and how many centroids own at least one row.
std::pair<double, size_t> evaluate(const std::vector<float>& x, faiss::IndexFlat& index) {
  int64_t n = (int64_t)x.size() / index.d;
  std::vector<float> dist((size_t)n);
  std::vector<faiss::idx_t> label((size_t)n);
  index.search(n, x.data(), 1, dist.data(), label.data());
  double sum = 0;
  for (float v : dist) sum += index.metric_type == faiss::METRIC_L2 ? v : 1.0 - v;
  return {sum / (double)n, std::set<faiss::idx_t>(label.begin(), label.end()).size()};
}

// FAISS Lloyd k-means (25 iterations) on every row: n <= 256 * k, so it does
// not subsample.
double lloydLoss(const std::vector<float>& x, faiss::IndexFlat& index, int32_t k) {
  faiss::ClusteringParameters cp;
  cp.spherical = index.metric_type == faiss::METRIC_INNER_PRODUCT;
  faiss::Clustering lloyd((int)index.d, k, cp);
  lloyd.train((faiss::idx_t)(x.size() / index.d), x.data(), index);
  return evaluate(x, index).first;
}

}  // namespace

TEST(EMeansTest, recipeMatchesReferenceScalingLaws) {
  // Values from FAISS contrib/e_means/base.py _resolve_recipe.
  auto ivf = EMeans::recipe(409600, 4096, 768, 5);
  EXPECT_EQ(ivf.batchSize, 32768);
  EXPECT_NEAR(ivf.alpha0, 0.519099, 1e-5);
  auto lowDims = EMeans::recipe(65536, 256, 8, 5);  // quality law at d <= 16
  EXPECT_EQ(lowDims.batchSize, 512);
  EXPECT_NEAR(lowDims.alpha0, 0.100711, 1e-5);
  auto small = EMeans::recipe(100000, 316, 96, 3);
  EXPECT_EQ(small.batchSize, 4096);
  EXPECT_NEAR(small.alpha0, 0.518571, 1e-5);
}

TEST(EMeansTest, matchesLloydLossInFewerPasses) {
  int32_t d = 16, k = 64;
  auto x = blobData(16000, d, 200, 1);
  faiss::IndexFlatL2 lloydIndex(d);
  double lloyd = lloydLoss(x, lloydIndex, k);

  faiss::IndexFlatL2 index(d);
  auto result = EMeans::train(16000, x.data(), k, {.epochs = 5}, index);
  ASSERT_EQ(index.ntotal, k);
  auto [loss, used] = evaluate(x, index);
  EXPECT_LT(loss, lloyd * 1.01) << "lloyd " << lloyd;
  EXPECT_EQ(used, (size_t)k);
  EXPECT_GT(result.totalSteps, 5);

  // Same seed, same centroids.
  faiss::IndexFlatL2 again(d);
  EMeans::train(16000, x.data(), k, {.epochs = 5}, again);
  EXPECT_EQ(std::vector<float>(again.get_xb(), again.get_xb() + k * d),
            std::vector<float>(index.get_xb(), index.get_xb() + k * d));
}

TEST(EMeansTest, sphericalMatchesSphericalLloyd) {
  int32_t d = 24, k = 32;
  auto x = blobData(8000, d, 80, 3, /*unit=*/true);
  faiss::IndexFlatIP lloydIndex(d);
  double lloyd = lloydLoss(x, lloydIndex, k);

  faiss::IndexFlatIP index(d);
  EMeans::train(8000, x.data(), k, {.epochs = 5, .spherical = true}, index);
  for (int32_t j = 0; j < k; j++) {
    EXPECT_NEAR(faiss::fvec_norm_L2sqr(index.get_xb() + j * d, d), 1.0f, 1e-4);
  }
  auto [loss, used] = evaluate(x, index);
  EXPECT_LT(loss, lloyd * 1.01) << "lloyd " << lloyd;
  EXPECT_EQ(used, (size_t)k);
}

TEST(EMeansTest, revivesDeadClustersOnDuplicateHeavyData) {
  // Most rows are one repeated point, so randomly seeded centroids collapse
  // onto it; revival must still spread centroids over the other rows.
  int32_t d = 8, k = 32;
  auto x = blobData(8000, d, 64, 2);
  std::fill_n(x.begin(), 6000 * d, 1.0f);
  faiss::IndexFlatL2 index(d);
  auto result = EMeans::train(8000, x.data(), k, {.epochs = 3}, index);
  EXPECT_GT(result.splits, 0);
  EXPECT_GT(evaluate(x, index).second, (size_t)k * 3 / 4);
}

TEST(EMeansTest, edgeShapesAndOverrides) {
  auto x = blobData(500, 2, 5, 4);
  // n == k: every row is a centroid.
  faiss::IndexFlatL2 all(2);
  EMeans::train(500, x.data(), 500, {.epochs = 1}, all);
  EXPECT_NEAR(evaluate(x, all).first, 0.0, 1e-9);
  // k == 1 with a fractional budget: the closing mean is the data mean.
  faiss::IndexFlatL2 one(2);
  EMeans::train(500, x.data(), 1, {.epochs = 1.5}, one);
  double mean0 = 0;
  for (int32_t i = 0; i < 500; i++) mean0 += x[(size_t)i * 2];
  EXPECT_NEAR(one.get_xb()[0], mean0 / 500, 1e-4);
  // A given batch keeps the step law at that batch: the law's own batch here
  // is 2, and the step grows with the batch.
  faiss::IndexFlatL2 small(2);
  auto r = EMeans::train(500, x.data(), 5, {.epochs = 2, .batchSize = 64}, small);
  EXPECT_EQ(EMeans::recipe(500, 5, 2, 2).batchSize, 2);
  EXPECT_EQ(r.recipe.batchSize, 64);
  EXPECT_GT(r.recipe.alpha0, 10 * EMeans::recipe(500, 5, 2, 2).alpha0);
  EXPECT_EQ(EMeans::train(500, x.data(), 5, {.alpha0 = 0.25}, small).recipe.alpha0, 0.25);
}

TEST(EMeansTest, rejectsInvalidInput) {
  auto x = blobData(100, 4, 3, 5);
  faiss::IndexFlatL2 l2(4);
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {.spherical = true}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::train(7, x.data(), 8, {}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {.epochs = INFINITY}, l2), std::invalid_argument);
  x[42] = std::numeric_limits<float>::infinity();
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {}, l2), std::invalid_argument);
}
