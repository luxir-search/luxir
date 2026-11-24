// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace faiss {
struct IndexFlat;
}

namespace luxir {

/// e-means clustering (Bessa et al., "e-means: outperforming k-means (in
/// one-tenth the epochs)", arXiv:2611.09501; reference code in FAISS
/// contrib/e_means).  Mini-batch k-means whose centroids are exponential
/// moving averages of per-batch cluster sums and counts, with a cosine-annealed
/// step, dead-cluster revival by splitting donors, and closing centroids taken
/// as the means of the last epoch's assignments.  It reaches the loss of 25
/// Lloyd iterations in a few epochs; each epoch costs about one Lloyd
/// iteration, since both are dominated by assigning every row once.
///
/// Spherical mode (inner-product clustering) assigns by inner product against
/// unit-normalized centroid directions and returns unit centroids, as FAISS's
/// spherical k-means does.
class EMeans {
public:
  struct Params {
    // Passes over the data; fractional values train on a prefix of the last
    // shuffled pass before the closing pass.
    double epochs = 5;
    uint64_t seed = 1234;
    bool spherical = false;
    // 0 derives the batch size and initial step from the fitted scaling laws.
    int64_t batchSize = 0;
    double alpha0 = 0;
  };

  struct Recipe {
    int64_t batchSize = 0;
    double alpha0 = 0;
  };

  struct Result {
    Recipe recipe;
    int64_t totalSteps = 0;
    int64_t splits = 0;
    // Sum of assignment distances over the closing pass (moving centroids).
    double lastEpochObjective = 0;
  };

  /// The scaling-law batch size and initial step for n rows, k clusters,
  /// dims dimensions and the given epochs (the reference "canonical_v1"
  /// recipe: throughput batch law above 16 dims, quality law at or below).
  static Recipe recipe(int64_t n, int64_t k, int32_t dims, double epochs);

  /// Trains k centroids on the n rows of x (row-major, assigner.d wide).
  /// Each pass visits the rows in a fresh random order.  assigner must be a
  /// flat index whose metric is L2 (Euclidean) or inner product (spherical);
  /// it serves every batch assignment (a row-parallel subclass parallelizes
  /// them), and on return holds exactly the k trained centroids.
  /// Requires n >= k >= 1.
  static Result train(int64_t n, const float* x, int32_t k, const Params& params,
                      faiss::IndexFlat& assigner);
};

}  // namespace luxir
