// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "EMeans.h"

#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <faiss/IndexFlat.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "luxir/util/random.h"

namespace luxir {

namespace {

// Constants and fitted laws of the reference implementation (FAISS
// contrib/e_means/base.py, recipe "canonical_v1").  The shipped fits differ
// from the paper's Table 2; these are the ones the reference code runs.

// Cosine schedule floor: the last step is this fraction of alpha0 (Eq. 5).
constexpr double ALPHA_MIN_FACTOR = 0.1;
// Batch updates between dead-cluster checks (Algorithm 1, line 9).
constexpr int64_t CHECK_UNUSED_EVERY = 5;
// Initial EMA mass N_j; the paper uses 1e-10.
constexpr float INIT_USAGE = 1.0f / 65536;
// Relative coordinate perturbation when a donor is split.
constexpr float SPLIT_EPSILON = 1e-4f;
// Revive clusters whose mass is below this fraction of the mean mass.
constexpr float UNUSED_THRESHOLD = 0.1f;
// Revival hurts at d <= 2 (reference measurements); turn it off there.
constexpr int32_t NO_REVIVAL_MAX_D = 2;
// The quality batch law (8x smaller batches) wins at d <= 16.
constexpr int32_t QUALITY_BATCH_MAX_D = 16;

// Batch law B = C * k^bk * n^bn * T^bT (Eq. 7), before rounding and caps.
constexpr double BATCH_THROUGHPUT_C = 0.035134;
constexpr double BATCH_QUALITY_C = 0.00439175;
constexpr double BATCH_K_EXP = 0.134749;
constexpr double BATCH_N_EXP = 0.876473;
constexpr double BATCH_T_EXP = 0.813344;

// Step law alpha0 = C * k^ak * B^aB * n^an * T^aT (Eq. 6), clamped to [1e-4, 1].
constexpr double ALPHA_C = 0.8244;
constexpr double ALPHA_K_EXP = -0.4617;
constexpr double ALPHA_B_EXP = 0.9003;
constexpr double ALPHA_N_EXP = -0.4498;
constexpr double ALPHA_T_EXP = -0.1057;

// Rows assigned per search call: bounds the gathered-row scratch buffer.
constexpr size_t ASSIGN_CHUNK_BYTES = 32 << 20;
constexpr int64_t MIN_ASSIGN_CHUNK_ROWS = 1024;

int64_t roundPowerOfTwo(double value) {
  if (value <= 1) return 1;
  int64_t low = (int64_t)1 << (int)std::floor(std::log2(value));
  int64_t high = 2 * low;
  return value / (double)low < (double)high / value ? low : high;
}

// Rows of each pass: whole passes, a fractional prefix, then the closing pass.
std::vector<int64_t> epochRows(int64_t n, double epochs) {
  int64_t whole = (int64_t)std::floor(epochs);
  int64_t remainder = (int64_t)std::nearbyint((epochs - (double)whole) * (double)n);
  std::vector<int64_t> rows;
  if (whole == 0) {
    rows.push_back(std::max<int64_t>(remainder, 1));
    return rows;
  }
  rows.assign((size_t)(whole - 1), n);
  if (remainder > 0) rows.push_back(remainder);
  rows.push_back(n);
  return rows;
}

double scheduledAlpha(double alpha0, int64_t step, int64_t totalSteps) {
  double fraction = (double)std::min(step, totalSteps) / (double)std::max<int64_t>(1, totalSteps);
  double cosine = 0.5 * (1.0 + std::cos(std::numbers::pi * fraction));
  return alpha0 * (ALPHA_MIN_FACTOR + (1.0 - ALPHA_MIN_FACTOR) * cosine);
}

double stepLaw(int64_t n, int64_t k, double epochs, int64_t batch) {
  double alpha0 = ALPHA_C * std::pow((double)k, ALPHA_K_EXP) * std::pow((double)batch, ALPHA_B_EXP)
      * std::pow((double)n, ALPHA_N_EXP) * std::pow(epochs, ALPHA_T_EXP);
  return std::clamp(alpha0, 1e-4, 1.0);
}

double uniform01(SplitMix64& rng) {
  return (double)(rng() >> 11) * 0x1.0p-53;
}

void shuffle(std::vector<int64_t>& order, SplitMix64& rng) {
  for (int64_t i = (int64_t)order.size() - 1; i > 0; i--) {
    std::swap(order[(size_t)i], order[(size_t)rng.rint(i + 1)]);
  }
}

// Unit directions of rows; zero rows stay zero.
void normalizeRows(const float* in, float* out, int64_t rows, int32_t d) {
  for (int64_t r = 0; r < rows; r++) {
    const float* src = in + r * d;
    float* dst = out + r * d;
    double norm2 = 0;
    for (int32_t i = 0; i < d; i++) norm2 += (double)src[i] * src[i];
    float scale = norm2 > 0 ? (float)(1.0 / std::sqrt(norm2)) : 0.0f;
    for (int32_t i = 0; i < d; i++) dst[i] = src[i] * scale;
  }
}

template <class Fn>
void parallelRows(int64_t n, int64_t grain, Fn fn) {
  oneapi::tbb::this_task_arena::isolate([&] {
    oneapi::tbb::parallel_for(
        oneapi::tbb::blocked_range<int64_t>(0, n, (size_t)std::max<int64_t>(1, grain)),
        [&](const oneapi::tbb::blocked_range<int64_t>& r) { fn(r.begin(), r.end()); });
  });
}

class Run {
  int64_t n;
  const float* x;
  int32_t d;
  int32_t k;
  EMeans::Params params;
  faiss::IndexFlat& assigner;
  EMeans::Result result;
  float unusedThreshold;
  SplitMix64 rng;

  // EMA state: positions c_j = S_j / N_j and masses N_j.
  std::vector<float> centroids;
  std::vector<float> usage;
  // Centroids the current batch is assigned against.
  std::vector<float> readout;
  // One batch's per-cluster sums s_j and counts n_j.  Float sums and int32
  // row indices are exact enough below 2^24 rows per batch (the reference
  // widens above that); the builder's sample stays far below it.
  std::vector<float> batchSums;
  std::vector<float> batchCounts;
  // Closing pass sums and counts (Algorithm 1, line 12).
  std::vector<float> passSums;
  std::vector<double> passCounts;
  // Gathered rows of one assignment chunk, their labels, and the chunk's
  // rows grouped by cluster (bucket j is rowsByCluster[bucketStart[j]..]).
  std::vector<float> chunk;
  std::vector<float> distances;
  std::vector<faiss::idx_t> labels;
  std::vector<int32_t> bucketStart;
  std::vector<int32_t> rowsByCluster;
  std::vector<int64_t> order;

public:
  Run(int64_t n, const float* x, int32_t k, const EMeans::Params& params,
          faiss::IndexFlat& assigner)
    : n(n), x(x), d((int32_t)assigner.d), k(k), params(params), assigner(assigner),
      unusedThreshold(d <= NO_REVIVAL_MAX_D ? 0.0f : UNUSED_THRESHOLD), rng(params.seed) {}

  EMeans::Result run() {
    result.recipe = EMeans::recipe(n, k, d, params.epochs);
    if (params.batchSize > 0) {
      // A given batch keeps the step law, evaluated at that batch.
      result.recipe.batchSize = std::min(params.batchSize, n);
      result.recipe.alpha0 = stepLaw(n, k, params.epochs, result.recipe.batchSize);
    }
    if (params.alpha0 > 0) result.recipe.alpha0 = params.alpha0;
    int64_t batch = result.recipe.batchSize;
    std::vector<int64_t> rowsPerEpoch = epochRows(n, params.epochs);
    for (int64_t rows : rowsPerEpoch) result.totalSteps += (rows + batch - 1) / batch;

    order.resize((size_t)n);
    std::iota(order.begin(), order.end(), (int64_t)0);
    shuffle(order, rng);
    centroids.resize((size_t)k * d);
    for (int32_t j = 0; j < k; j++) {
      std::copy_n(x + order[(size_t)j] * d, d, centroids.data() + (size_t)j * d);
    }
    if (params.spherical) normalizeRows(centroids.data(), centroids.data(), k, d);
    usage.assign((size_t)k, INIT_USAGE);
    readout.resize((size_t)k * d);
    batchSums.assign((size_t)k * d, 0.0f);
    batchCounts.assign((size_t)k, 0.0f);
    bucketStart.resize((size_t)k + 1);
    passCounts.resize((size_t)k);

    int64_t chunkRows = std::min(batch, std::max<int64_t>(
        (int64_t)(ASSIGN_CHUNK_BYTES / ((size_t)d * sizeof(float))), MIN_ASSIGN_CHUNK_ROWS));
    chunk.resize((size_t)chunkRows * d);
    distances.resize((size_t)chunkRows);
    labels.resize((size_t)chunkRows);
    rowsByCluster.resize((size_t)chunkRows);

    int64_t step = 0;
    for (size_t epoch = 0; epoch < rowsPerEpoch.size(); epoch++) {
      bool closing = epoch + 1 == rowsPerEpoch.size();
      int64_t rows = rowsPerEpoch[epoch];
      shuffle(order, rng);
      std::fill(passCounts.begin(), passCounts.end(), 0.0);
      if (closing) passSums.assign((size_t)k * d, 0.0f);
      double objective = 0;
      for (int64_t begin = 0; begin < rows; begin += batch) {
        int64_t end = std::min(begin + batch, rows);
        objective += accumulate(begin, end, chunkRows);
        update(scheduledAlpha(result.recipe.alpha0, step, result.totalSteps), closing);
        if ((step + 1) % CHECK_UNUSED_EVERY == 0) revive();
        step++;
      }
      if (closing) result.lastEpochObjective = objective;
    }
    finish();
    return result;
  }

private:
  // Assigns rows [begin, end) of this pass's order against one centroid
  // snapshot and collects their per-cluster sums and counts.
  double accumulate(int64_t begin, int64_t end, int64_t chunkRows) {
    const float* current = centroids.data();
    if (params.spherical) {
      parallelRows(k, 64, [&](int64_t b, int64_t e) {
        normalizeRows(centroids.data() + b * d, readout.data() + b * d, e - b, d);
      });
      current = readout.data();
    }
    assigner.reset();
    assigner.add(k, current);
    double objective = 0;
    for (int64_t c = begin; c < end; c += chunkRows) {
      int64_t m = std::min(chunkRows, end - c);
      parallelRows(m, 256, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) {
          std::copy_n(x + order[(size_t)(c + i)] * d, d, chunk.data() + i * d);
        }
      });
      assigner.search(m, chunk.data(), 1, distances.data(), labels.data());
      // Group the chunk's rows by cluster (a counting sort that keeps row
      // order), then sum each cluster's rows in parallel across clusters: the
      // same additions in the same order as a serial pass.
      std::fill(bucketStart.begin(), bucketStart.end(), 0);
      for (int64_t i = 0; i < m; i++) {
        faiss::idx_t label = labels[(size_t)i];
        if (label < 0 || label >= k) {
          throw std::runtime_error("EMeans: assignment returned no centroid");
        }
        objective += params.spherical ? 2.0 - 2.0 * (double)distances[(size_t)i]
                                      : (double)distances[(size_t)i];
        bucketStart[(size_t)label + 1]++;
      }
      for (int32_t j = 0; j < k; j++) bucketStart[(size_t)j + 1] += bucketStart[(size_t)j];
      for (int64_t i = 0; i < m; i++) {
        rowsByCluster[(size_t)bucketStart[(size_t)labels[(size_t)i]]++] = (int32_t)i;
      }
      // bucketStart[j] now ends bucket j; bucket j starts at bucketStart[j - 1].
      parallelRows(k, 16, [&](int64_t b, int64_t e) {
        for (int64_t j = b; j < e; j++) {
          int32_t from = j == 0 ? 0 : bucketStart[(size_t)j - 1];
          int32_t to = bucketStart[(size_t)j];
          if (from == to) continue;
          batchCounts[(size_t)j] += (float)(to - from);
          float* sum = batchSums.data() + (size_t)j * d;
          for (int32_t r = from; r < to; r++) {
            const float* row = chunk.data() + (size_t)rowsByCluster[(size_t)r] * d;
            for (int32_t t = 0; t < d; t++) sum[t] += row[t];
          }
        }
      });
    }
    return objective;
  }

  // Algorithm 1, line 8, in the mass/position form of the reference:
  //   N_j <- (1 - alpha) N_j + alpha n_j
  //   c_j <- c_j + (alpha n_j / N_j) (s_j / n_j - c_j)
  // Clusters absent from the batch keep their position and decay in mass.
  void update(double alpha, bool closing) {
    bool replace = alpha >= 1.0;
    float decay = (float)(1.0 - alpha);
    parallelRows(k, 64, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) updateCluster(j, (float)alpha, decay, replace, closing);
    });
  }

  // Applies cluster j's batch statistics, then clears them for the next batch.
  void updateCluster(int64_t j, float alpha, float decay, bool replace, bool closing) {
    float count = batchCounts[(size_t)j];
    passCounts[(size_t)j] += count;
    float retained = replace ? 0.0f : usage[(size_t)j] * decay;
    if (count <= 0) {
      if (!replace) usage[(size_t)j] = retained;
      return;
    }
    float* sum = batchSums.data() + (size_t)j * d;
    if (closing) {
      float* pass = passSums.data() + (size_t)j * d;
      for (int32_t t = 0; t < d; t++) pass[t] += sum[t];
    }
    float mass = retained + alpha * count;
    float weight = alpha * count / mass;
    float* c = centroids.data() + (size_t)j * d;
    float inv = 1.0f / count;
    if (weight >= 1.0f) {
      for (int32_t t = 0; t < d; t++) c[t] = sum[t] * inv;
    } else {
      for (int32_t t = 0; t < d; t++) c[t] += weight * (sum[t] * inv - c[t]);
    }
    usage[(size_t)j] = mass;
    std::fill_n(sum, d, 0.0f);
    batchCounts[(size_t)j] = 0;
  }

  // Revives clusters whose mass fell below the threshold by splitting donors
  // drawn without replacement in proportion to their mass (Gumbel top-k).
  void revive() {
    if (unusedThreshold <= 0) return;
    double total = 0;
    for (float u : usage) total += u;
    float threshold = (float)(total * unusedThreshold / k);
    std::vector<int32_t> unused;
    std::vector<std::pair<double, int32_t>> donors;
    for (int32_t j = 0; j < k; j++) {
      float u = usage[(size_t)j];
      if (u < threshold) {
        unused.push_back(j);
      } else if (u > 0) {
        double uniform = std::max(uniform01(rng), std::numeric_limits<double>::min());
        donors.emplace_back(std::log((double)u) - std::log(-std::log(uniform)), j);
      }
    }
    size_t splits = std::min(unused.size(), donors.size());
    if (splits == 0) return;
    std::partial_sort(donors.begin(), donors.begin() + (ptrdiff_t)splits, donors.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t s = 0; s < splits; s++) {
      int32_t source = donors[s].second;
      int32_t target = unused[s];
      float* donor = centroids.data() + (size_t)source * d;
      float* child = centroids.data() + (size_t)target * d;
      bool same = true;
      for (int32_t t = 0; t < d; t++) {
        float offset = (t & 1) ? -SPLIT_EPSILON : SPLIT_EPSILON;
        float value = donor[t];
        donor[t] = value * (1.0f - offset);
        child[t] = value * (1.0f + offset);
        same = same && donor[t] == child[t];
      }
      if (same) {
        // A zero donor splits into identical points: separate them by one ulp.
        float next = std::nextafter(child[0], INFINITY);
        child[0] = std::isfinite(next) ? next : std::nextafter(child[0], -INFINITY);
      }
      float half = usage[(size_t)source] / 2;
      usage[(size_t)source] = half;
      usage[(size_t)target] = half;
    }
    result.splits += (int64_t)splits;
  }

  // Closing centroids: the means of the last pass's assignments; centroids it
  // never assigned keep their EMA position.
  void finish() {
    for (int32_t j = 0; j < k; j++) {
      double count = passCounts[(size_t)j];
      if (count <= 0) continue;
      float* c = centroids.data() + (size_t)j * d;
      const float* sum = passSums.data() + (size_t)j * d;
      float inv = (float)(1.0 / count);
      for (int32_t t = 0; t < d; t++) c[t] = sum[t] * inv;
    }
    if (params.spherical) normalizeRows(centroids.data(), centroids.data(), k, d);
    assigner.reset();
    assigner.add(k, centroids.data());
  }
};

}  // namespace

EMeans::Recipe EMeans::recipe(int64_t n, int64_t k, int32_t dims, double epochs) {
  bool quality = dims <= QUALITY_BATCH_MAX_D;
  double raw = (quality ? BATCH_QUALITY_C : BATCH_THROUGHPUT_C)
      * std::pow((double)k, BATCH_K_EXP) * std::pow((double)n, BATCH_N_EXP)
      * std::pow(epochs, BATCH_T_EXP);
  int64_t batch = roundPowerOfTwo(raw);
  batch = quality ? std::min(batch, n / 2) : std::min(batch, roundPowerOfTwo((double)n / 2));
  batch = std::clamp<int64_t>(batch, 1, n);
  return {.batchSize = batch, .alpha0 = stepLaw(n, k, epochs, batch)};
}

EMeans::Result EMeans::train(int64_t n, const float* x, int32_t k, const Params& params,
                             faiss::IndexFlat& assigner) {
  if (k < 1 || n < k) throw std::invalid_argument("EMeans: need n >= k >= 1");
  if (!(params.epochs > 0) || !std::isfinite(params.epochs)) {
    throw std::invalid_argument("EMeans: epochs must be positive and finite");
  }
  bool ip = assigner.metric_type == faiss::METRIC_INNER_PRODUCT;
  if (ip != params.spherical || (!ip && assigner.metric_type != faiss::METRIC_L2)) {
    throw std::invalid_argument("EMeans: assigner metric must be L2, or inner product when spherical");
  }
  // As FAISS's Clustering does: a non-finite row would poison its centroid.
  int64_t d = assigner.d;
  parallelRows(n, 4096, [&](int64_t begin, int64_t end) {
    for (int64_t i = begin * d; i < end * d; i++) {
      if (!std::isfinite(x[i])) throw std::invalid_argument("EMeans: input contains NaN or Inf");
    }
  });
  return Run(n, x, k, params, assigner).run();
}

}  // namespace luxir
