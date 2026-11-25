// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
//
// Translated from the e-means reference implementation in FAISS
// (contrib/e_means, facebookresearch/faiss commit 6074b69e9, 2026-11-24),
// Copyright (c) Meta Platforms, Inc. and affiliates, MIT License; see
// EMeans.cpp and NOTICE.

#pragma once

#include <cstdint>

namespace faiss {
struct IndexFlat;
}

namespace luxir {

/// e-means clustering (Bessa et al., "e-means: outperforming k-means (in
/// one-tenth the epochs)", arXiv:2611.09501), a port of the FAISS reference
/// NumpyEmeans at commit 6074b69e9.  Mini-batch k-means whose centroids are
/// exponential moving averages of per-batch cluster sums and counts, with a
/// cosine-annealed step, cluster revival every five batches, and closing
/// centroids taken as the means of the last pass's rows.  It reaches the loss
/// of Lloyd's algorithm in a few passes; each pass costs about one Lloyd
/// iteration, since both are dominated by assigning every row once.
///
/// Revival (the reference default, REMOVAL_COST) moves the centroids that are
/// cheap to remove, those whose EMA of (second-nearest - nearest) distance
/// over their rows is zero or below 0.3 of the median, to split the donors
/// with the longest Oja axes.  A split puts the two copies at the Gaussian
/// half-means along the donor's axis.  When a centroid moves during the last
/// pass, the rows it held before moving go to their second-nearest centroid
/// (or are assigned again), a donor's earlier rows go to the nearer of its
/// two copies, and the affected closing sums are rebuilt from those rows.
/// MASS, the reference's mass-based default, revives clusters below 0.1 of
/// the mean mass by splitting mass-sampled donors (no revival at d <= 2),
/// with the same splits.  The port also applies the closing handover there;
/// the reference does not.
///
/// Spherical mode (inner-product clustering) assigns by inner product against
/// unit centroid directions and returns unit centroids, as FAISS's spherical
/// k-means does.
class EMeans {
public:
  enum class Revival { REMOVAL_COST, MASS };

  struct Params {
    // Passes over the data; fractional values train on a prefix of the last
    // shuffled pass before the closing pass.
    double epochs = 5;
    uint64_t seed = 1234;
    bool spherical = false;
    Revival revival = Revival::REMOVAL_COST;
  };

  struct Recipe {
    int64_t batchSize = 0;
    double alpha0 = 0;
  };

  struct Result {
    Recipe recipe;
    int64_t splits = 0;
    // Closing-pass rows handed to their second-nearest centroid, and those
    // assigned again, because their centroid moved after assigning them.
    int64_t handedOver = 0;
    int64_t reassigned = 0;
  };

  /// The scaling-law batch size and initial step for n rows, k clusters,
  /// dims dimensions and the given epochs (the reference "canonical_v1"
  /// recipe: throughput batch law above 16 dims, quality law at or below).
  static Recipe recipe(int64_t n, int64_t k, int32_t dims, double epochs);

  /// Bytes train() allocates for n rows of dims into k clusters, including
  /// the assigner's copy of the centroids: callers charge it with the sample.
  static int64_t workspaceBytes(int64_t n, int64_t k, int32_t dims, const Params& params);

  /// Trains k centroids on the n rows of x (row-major, assigner.d wide).
  /// Each pass visits the rows in a fresh random order.  assigner must be a
  /// flat index whose metric is L2 (Euclidean) or inner product (spherical);
  /// it serves every batch assignment (a row-parallel subclass parallelizes
  /// them), and on return holds exactly the k trained centroids.  n == k
  /// copies the rows, as FAISS does.  Requires n >= k >= 1 and finite rows.
  static Result train(int64_t n, const float* x, int32_t k, const Params& params,
                      faiss::IndexFlat& assigner);
};

}  // namespace luxir
