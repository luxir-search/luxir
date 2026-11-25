// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "luxir/index/EMeans.h"

#include <faiss/Clustering.h>
#include <faiss/IndexFlat.h>
#include <faiss/utils/distances.h>

#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <random>
#include <set>
#include <vector>

using namespace luxir;

namespace {

// n points around `blobs` centers drawn at `spread` (unit noise); unit rows
// when `unit`.  label, when given, receives each row's blob.
std::vector<float> blobData(int64_t n, int32_t d, int32_t blobs, uint32_t seed, bool unit = false,
                            float spread = 4, std::vector<int32_t>* label = nullptr) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss;
  std::vector<float> centers((size_t)blobs * d);
  for (float& c : centers) c = spread * gauss(rng);
  std::vector<float> x((size_t)n * d);
  for (int64_t i = 0; i < n; i++) {
    int32_t b = (int32_t)(rng() % blobs);
    if (label) label->push_back(b);
    for (int32_t t = 0; t < d; t++) x[(size_t)i * d + t] = centers[(size_t)b * d + t] + gauss(rng);
  }
  if (unit) faiss::fvec_renorm_L2((size_t)d, (size_t)n, x.data());
  return x;
}

std::vector<float> centroidsOf(faiss::IndexFlat& index) {
  return std::vector<float>(index.get_xb(), index.get_xb() + index.ntotal * index.d);
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

bool allFinite(const std::vector<float>& v) {
  for (float f : v) {
    if (!std::isfinite(f)) return false;
  }
  return true;
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

TEST(EMeansTest, matchesLloydLossAndRepeats) {
  int32_t d = 16, k = 64;
  auto x = blobData(16000, d, 200, 1);
  faiss::IndexFlatL2 lloydIndex(d);
  double lloyd = lloydLoss(x, lloydIndex, k);
  for (auto revival : {EMeans::Revival::REMOVAL_COST, EMeans::Revival::MASS}) {
    SCOPED_TRACE((int)revival);
    faiss::IndexFlatL2 index(d);
    auto result = EMeans::train(16000, x.data(), k, {.revival = revival}, index);
    ASSERT_EQ(index.ntotal, k);
    auto [loss, used] = evaluate(x, index);
    EXPECT_LT(loss, lloyd * 1.01) << "lloyd " << lloyd;
    EXPECT_EQ(used, (size_t)k);
    EXPECT_GT(result.recipe.batchSize, 0);
    // Same seed, same centroids.
    faiss::IndexFlatL2 again(d);
    EMeans::train(16000, x.data(), k, {.revival = revival}, again);
    EXPECT_EQ(centroidsOf(again), centroidsOf(index));
  }
}

TEST(EMeansTest, sphericalCentroidsAreUnitAndMatchLloyd) {
  int32_t d = 24, k = 32;
  auto x = blobData(8000, d, 80, 3, /*unit=*/true);
  faiss::IndexFlatIP lloydIndex(d);
  double lloyd = lloydLoss(x, lloydIndex, k);
  faiss::IndexFlatIP index(d);
  EMeans::train(8000, x.data(), k, {.spherical = true}, index);
  for (int32_t j = 0; j < k; j++) {
    EXPECT_NEAR(faiss::fvec_norm_L2sqr(index.get_xb() + j * d, d), 1.0f, 1e-4);
  }
  auto [loss, used] = evaluate(x, index);
  EXPECT_LT(loss, lloyd * 1.01) << "lloyd " << lloyd;
  EXPECT_EQ(used, (size_t)k);
}

TEST(EMeansTest, zeroRowsKeepCentroidsFinite) {
  // Spherical training on all-zero rows, and on unit rows around a common
  // direction mixed with zero rows.  Mass revival splits zero donors into
  // subnormal rows, which must still normalize to finite directions.
  int32_t d = 128, k = 90;
  int64_t n = 8192;
  for (auto revival : {EMeans::Revival::MASS, EMeans::Revival::REMOVAL_COST})
  for (double zeros : {1.0, 0.5, 0.2}) {
    for (uint32_t seed : {1u, 2u, 3u}) {
      SCOPED_TRACE(fmt::format("revival {} zeros {} seed {}", (int)revival, zeros, seed));
      std::mt19937 rng(seed);
      std::normal_distribution<float> gauss;
      std::vector<float> x((size_t)n * d, 0.0f);
      for (int64_t i = 0; i < n; i++) {
        if (std::uniform_real_distribution<double>()(rng) < zeros) continue;
        float* row = x.data() + i * d;
        for (int32_t t = 0; t < d; t++) row[t] = (t == 0 ? 4.0f : 0.0f) + gauss(rng);
        faiss::fvec_renorm_L2((size_t)d, 1, row);
      }
      faiss::IndexFlatIP index(d);
      EMeans::train(n, x.data(), k, {.seed = seed, .spherical = true, .revival = revival}, index);
      auto centroids = centroidsOf(index);
      ASSERT_TRUE(allFinite(centroids));
      for (int32_t j = 0; j < k; j++) {
        float norm2 = faiss::fvec_norm_L2sqr(centroids.data() + (size_t)j * d, d);
        EXPECT_TRUE(norm2 == 0.0f || std::abs(norm2 - 1.0f) < 1e-4) << norm2;
      }
    }
  }
}

TEST(EMeansTest, separatedBlobsCloseAtTheirMeans) {
  // One centroid per far-apart blob: each closes at its blob's mean.
  int32_t d = 8, blobs = 6;
  std::vector<int32_t> label;
  auto x = blobData(6000, d, blobs, 4, false, 1000, &label);
  std::vector<double> means((size_t)blobs * d, 0.0), counts((size_t)blobs, 0.0);
  for (size_t i = 0; i < label.size(); i++) {
    counts[(size_t)label[i]] += 1;
    for (int32_t t = 0; t < d; t++) means[(size_t)label[i] * d + t] += x[i * d + t];
  }
  faiss::IndexFlatL2 index(d);
  EMeans::train(6000, x.data(), blobs, {}, index);
  std::set<int32_t> matched;
  for (int32_t j = 0; j < blobs; j++) {
    const float* c = index.get_xb() + j * d;
    for (int32_t b = 0; b < blobs; b++) {
      bool same = true;
      for (int32_t t = 0; t < d; t++) {
        same = same && std::abs(c[t] - means[(size_t)b * d + t] / counts[(size_t)b]) < 1e-3;
      }
      if (same) matched.insert(b);
    }
  }
  EXPECT_EQ(matched.size(), (size_t)blobs);
}

TEST(EMeansTest, revivalSplitsOnDuplicateHeavyData) {
  // Most rows are one repeated point, so randomly seeded centroids collapse
  // onto it; revival must still spread centroids over the other rows.
  int32_t d = 8, k = 32;
  auto x = blobData(8000, d, 64, 2);
  std::fill_n(x.begin(), 6000 * d, 1.0f);
  for (auto revival : {EMeans::Revival::REMOVAL_COST, EMeans::Revival::MASS}) {
    SCOPED_TRACE((int)revival);
    faiss::IndexFlatL2 index(d);
    auto result = EMeans::train(8000, x.data(), k, {.epochs = 3, .revival = revival}, index);
    EXPECT_GT(result.splits, 0);
    EXPECT_TRUE(allFinite(centroidsOf(index)));
    EXPECT_GT(evaluate(x, index).second, (size_t)k * 3 / 4);
  }
}

TEST(EMeansTest, closingRevivalsHandRowsOver) {
  // A single pass is all closing pass: centroids collapsed onto a repeated
  // point move while holding its rows, which the handover must reassign.
  int32_t d = 8, k = 32;
  auto x = blobData(8000, d, 64, 6);
  std::fill_n(x.begin(), 6000 * d, 1.0f);
  faiss::IndexFlatL2 index(d);
  auto result = EMeans::train(8000, x.data(), k, {.epochs = 1}, index);
  EXPECT_GT(result.splits, 0);
  EXPECT_GT(result.handedOver + result.reassigned, 0);
  EXPECT_TRUE(allFinite(centroidsOf(index)));
  EXPECT_GT(evaluate(x, index).second, (size_t)k * 3 / 4);
}

TEST(EMeansTest, edgeShapesAndWorkspace) {
  auto x = blobData(500, 2, 5, 4);
  // n == k copies the rows.
  faiss::IndexFlatL2 all(2);
  EMeans::train(500, x.data(), 500, {}, all);
  EXPECT_EQ(centroidsOf(all), x);
  // k == 1 with a fractional budget: the closing mean is the data mean.
  faiss::IndexFlatL2 one(2);
  EMeans::train(500, x.data(), 1, {.epochs = 1.5}, one);
  double mean0 = 0;
  for (int32_t i = 0; i < 500; i++) mean0 += x[(size_t)i * 2];
  EXPECT_NEAR(one.get_xb()[0], mean0 / 500, 1e-4);
  // The workspace covers the k x d tables, and the unit directions when spherical.
  int64_t l2 = EMeans::workspaceBytes(409600, 4096, 768, {});
  EXPECT_GT(l2, (int64_t)4 * 4096 * 768 * 4);
  EXPECT_EQ(EMeans::workspaceBytes(409600, 4096, 768, {.spherical = true}) - l2,
            (int64_t)4096 * 768 * 4);
}

TEST(EMeansTest, rejectsInvalidInput) {
  auto x = blobData(100, 4, 3, 5);
  faiss::IndexFlatL2 l2(4);
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {.spherical = true}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::train(7, x.data(), 8, {}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {.epochs = INFINITY}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {.epochs = 0}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {.epochs = 1e12}, l2), std::invalid_argument);
  EXPECT_THROW(EMeans::recipe(100, 0, 4, 5), std::invalid_argument);
  EXPECT_THROW(EMeans::recipe(100, 8, 0, 5), std::invalid_argument);
  x[42] = std::numeric_limits<float>::infinity();
  EXPECT_THROW(EMeans::train(100, x.data(), 8, {}, l2), std::invalid_argument);
}
