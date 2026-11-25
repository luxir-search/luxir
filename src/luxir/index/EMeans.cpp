// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
//
// Translated from the e-means reference implementation in FAISS:
// contrib/e_means (base.py, e_means.py, e_means_spherical.py) and the e-means
// helpers accumulate_removal_stats and update_removal_axes in
// faiss/utils/distances.cpp, at facebookresearch/faiss commit 6074b69e9
// (2026-11-24). The original code carries this notice:
//
//   Copyright (c) Meta Platforms, Inc. and affiliates.
//
//   MIT License
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to
//   permit persons to whom the Software is furnished to do so, subject to
//   the following conditions:
//
//   The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

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

// Constants and fitted laws of the reference (base.py, recipe "canonical_v1").
// The shipped fits differ from the paper's Table 2; these are the ones the
// reference code runs.

// Cosine schedule floor: the steps approach this fraction of alpha0 (Eq. 5).
constexpr double ALPHA_MIN_FACTOR = 0.1;
// Batch updates between revival checks (Algorithm 1, line 9).
constexpr int64_t CHECK_UNUSED_EVERY = 5;
// Initial EMA mass N_j; the paper uses 1e-10.
constexpr float INIT_USAGE = 1.0f / 65536;
// Mass revival: clusters below this fraction of the mean mass.
constexpr float UNUSED_THRESHOLD = 0.1f;
// Mass revival hurts at d <= 2 (reference measurements); it is off there.
constexpr int32_t NO_REVIVAL_MAX_D = 2;
// Removal-cost revival: centroids whose removal cost is zero or below this
// fraction of the median.
constexpr double REMOVAL_THRESHOLD = 0.3;
// The Oja axis sums decay as one scalar; they are rescaled below this.
constexpr double MIN_AXIS_SCALE = 0x1.0p-32;
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

void validate(int64_t n, int64_t k, int32_t dims, double epochs) {
  if (k < 1 || n < k) throw std::invalid_argument("EMeans: need n >= k >= 1");
  if (dims < 1) throw std::invalid_argument("EMeans: need dims >= 1");
  if (!(epochs > 0) || !(epochs <= 1000)) {
    throw std::invalid_argument("EMeans: epochs must be in (0, 1000]");
  }
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

int64_t chunkRowsFor(int64_t batch, int32_t d) {
  return std::min(batch, std::max<int64_t>(
      (int64_t)(ASSIGN_CHUNK_BYTES / ((size_t)d * sizeof(float))), MIN_ASSIGN_CHUNK_ROWS));
}

bool revives(const EMeans::Params& params, int64_t k, int32_t d) {
  return k >= 2 && (params.revival == EMeans::Revival::REMOVAL_COST || d > NO_REVIVAL_MAX_D);
}

double uniform01(SplitMix64& rng) {
  return (double)(rng() >> 11) * 0x1.0p-53;
}

double gaussian(SplitMix64& rng) {
  double u = std::max(uniform01(rng), std::numeric_limits<double>::min());
  return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * std::numbers::pi * uniform01(rng));
}

void shuffle(std::vector<int64_t>& order, SplitMix64& rng) {
  for (int64_t i = (int64_t)order.size() - 1; i > 0; i--) {
    std::swap(order[(size_t)i], order[(size_t)rng.rint(i + 1)]);
  }
}

// The unit direction of a row; zero stays zero.  As the reference's
// normalize_into, the row is prescaled by its largest magnitude, and the
// scale stays in double, so tiny (even subnormal) rows normalize exactly
// rather than overflowing a float inverse.
void normalizeRow(const float* in, float* out, int32_t d) {
  float largest = 0;
  for (int32_t i = 0; i < d; i++) largest = std::max(largest, std::abs(in[i]));
  if (!(largest > 0)) {
    std::fill_n(out, d, 0.0f);
    return;
  }
  double inv = 1.0 / (double)largest;
  double norm2 = 0;
  for (int32_t i = 0; i < d; i++) {
    double w = (double)in[i] * inv;
    norm2 += w * w;
  }
  double scale = inv / std::sqrt(norm2);
  for (int32_t i = 0; i < d; i++) out[i] = (float)((double)in[i] * scale);
}

template <class Fn>
void parallelRows(int64_t n, int64_t grain, Fn fn) {
  oneapi::tbb::this_task_arena::isolate([&] {
    oneapi::tbb::parallel_for(
        oneapi::tbb::blocked_range<int64_t>(0, n, (size_t)std::max<int64_t>(1, grain)),
        [&](const oneapi::tbb::blocked_range<int64_t>& r) { fn(r.begin(), r.end()); });
  });
}

double squaredDistance(const float* a, const float* b, int32_t d) {
  double sum = 0;
  for (int32_t i = 0; i < d; i++) {
    double diff = (double)(a[i] - b[i]);
    sum += diff * diff;
  }
  return sum;
}

class Run {
  // A revival during the closing pass: centroid target moved, splitting
  // source, once `born` closing-pass rows had been assigned.
  struct Event {
    int32_t target;
    int32_t source;
    int64_t born;
  };

  int64_t n;
  const float* x;
  int32_t d;
  int32_t k;
  EMeans::Params params;
  faiss::IndexFlat& assigner;
  EMeans::Result result;
  bool revive;
  // Removal-cost revival searches two neighbors and tracks removal costs.
  bool removalCost;
  int32_t neighbors;
  SplitMix64 rng;

  // EMA state: positions c_j = S_j / N_j and masses N_j.
  std::vector<float> centroids;
  std::vector<float> usage;
  // Unit directions of the positions, for spherical assignment.
  std::vector<float> readout;
  // One batch's per-cluster sums s_j and counts n_j.  Float sums and int32
  // row indices are exact enough below 2^24 rows per batch (the reference
  // widens above that); the builder's sample stays far below it.
  std::vector<float> batchSums;
  std::vector<float> batchCounts;
  // Closing pass sums and counts (Algorithm 1, line 12).
  std::vector<float> passSums;
  std::vector<double> passCounts;
  // Revival statistics, k rows of d + 1: the Oja axis sums U_j / axisScale
  // and then the removal cost R_j; the batch's sum of p y and of d2 - d1;
  // and the unit axes u_j.
  std::vector<float> removalStats;
  std::vector<float> batchRemoval;
  std::vector<float> axes;
  double axisScale = 1;
  // One assignment chunk: gathered rows, search results, each row's label,
  // second label and distances, and its rows grouped by cluster (bucket j
  // ends at bucketEnd[j]).
  std::vector<float> chunk;
  std::vector<float> searchDistances;
  std::vector<faiss::idx_t> searchLabels;
  std::vector<int32_t> rowLabel;
  std::vector<float> rowMargin;
  std::vector<int32_t> bucketEnd;
  std::vector<int32_t> rowsByCluster;
  std::vector<int64_t> order;
  // Closing pass: each row's label and second label, when each centroid last
  // moved (in rows assigned; -1 if it did not), and the revivals.
  std::vector<int32_t> closingLabel;
  std::vector<int32_t> closingSecond;
  std::vector<int64_t> movedAt;
  std::vector<Event> events;
  int64_t closingDone = 0;

public:
  Run(int64_t n, const float* x, int32_t k, const EMeans::Params& params,
      faiss::IndexFlat& assigner)
    : n(n), x(x), d((int32_t)assigner.d), k(k), params(params), assigner(assigner),
      revive(revives(params, k, (int32_t)assigner.d)),
      removalCost(revive && params.revival == EMeans::Revival::REMOVAL_COST),
      neighbors(removalCost ? 2 : 1), rng(params.seed) {}

  EMeans::Result run() {
    result.recipe = EMeans::recipe(n, k, d, params.epochs);
    int64_t batch = result.recipe.batchSize;
    if (batch > (int64_t)1 << 24) {
      // Float batch sums and int32 row indices need batches below 2^24 rows
      // (the reference widens to double above that).
      throw std::invalid_argument("EMeans: batch above 2^24 rows");
    }
    std::vector<int64_t> rowsPerEpoch = epochRows(n, params.epochs);
    int64_t totalSteps = 0;
    for (int64_t rows : rowsPerEpoch) totalSteps += (rows + batch - 1) / batch;

    order.resize((size_t)n);
    std::iota(order.begin(), order.end(), (int64_t)0);
    shuffle(order, rng);
    centroids.resize((size_t)k * d);
    for (int32_t j = 0; j < k; j++) {
      std::copy_n(x + order[(size_t)j] * d, d, centroids.data() + (size_t)j * d);
    }
    if (params.spherical) {
      readout.resize((size_t)k * d);
      parallelRows(k, 64, [&](int64_t b, int64_t e) {
        for (int64_t j = b; j < e; j++) {
          normalizeRow(centroids.data() + j * d, centroids.data() + j * d, d);
        }
      });
    }
    usage.assign((size_t)k, INIT_USAGE);
    batchSums.assign((size_t)k * d, 0.0f);
    batchCounts.assign((size_t)k, 0.0f);
    passCounts.resize((size_t)k);
    bucketEnd.resize((size_t)k);
    if (revive) {
      removalStats.assign((size_t)k * (d + 1), 0.0f);
      batchRemoval.assign((size_t)k * (d + 1), 0.0f);
      axes.resize((size_t)k * d);
      std::vector<double> draw((size_t)d);
      for (int32_t j = 0; j < k; j++) {
        float* axis = axes.data() + (size_t)j * d;
        double norm2 = 0;
        for (int32_t t = 0; t < d; t++) {
          draw[(size_t)t] = gaussian(rng);
          norm2 += draw[(size_t)t] * draw[(size_t)t];
        }
        double inv = norm2 > 0 ? 1.0 / std::sqrt(norm2) : 0.0;
        for (int32_t t = 0; t < d; t++) axis[t] = (float)(draw[(size_t)t] * inv);
      }
    }

    int64_t chunkRows = chunkRowsFor(batch, d);
    chunk.resize((size_t)chunkRows * d);
    searchDistances.resize((size_t)chunkRows * neighbors);
    searchLabels.resize((size_t)chunkRows * neighbors);
    rowLabel.resize((size_t)chunkRows);
    rowMargin.resize((size_t)chunkRows);
    rowsByCluster.resize((size_t)chunkRows);

    int64_t step = 0;
    for (size_t epoch = 0; epoch < rowsPerEpoch.size(); epoch++) {
      bool closing = epoch + 1 == rowsPerEpoch.size();
      int64_t rows = rowsPerEpoch[epoch];
      shuffle(order, rng);
      std::fill(passCounts.begin(), passCounts.end(), 0.0);
      if (closing) {
        passSums.assign((size_t)k * d, 0.0f);
        closingLabel.assign((size_t)rows, -1);
        if (removalCost) closingSecond.assign((size_t)rows, -1);
        movedAt.assign((size_t)k, -1);
      }
      for (int64_t begin = 0; begin < rows; begin += batch) {
        int64_t end = std::min(begin + batch, rows);
        accumulate(begin, end, chunkRows, closing);
        update(scheduledAlpha(result.recipe.alpha0, step, totalSteps), closing);
        closingDone = end;
        if (revive && (step + 1) % CHECK_UNUSED_EVERY == 0) reviveClusters(closing);
        step++;
      }
      if (closing && !events.empty()) handOver(rows, chunkRows);
    }
    finish();
    return result;
  }

private:
  // The centroids batch rows are assigned against: unit directions when
  // spherical, else the positions themselves.
  const float* current() {
    if (!params.spherical) return centroids.data();
    parallelRows(k, 64, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) {
        normalizeRow(centroids.data() + j * d, readout.data() + j * d, d);
      }
    });
    return readout.data();
  }

  // Nearest and (removal-cost revival) second-nearest centroid of searched
  // row i, as squared distances (-2 x similarity when spherical); ties go
  // to the lower label, as with one neighbor.
  void neighborsOf(int64_t i, int32_t& label, int32_t& second, float& d1, float& d2) const {
    auto dist = [&](int64_t at) {
      return params.spherical ? -2.0f * searchDistances[(size_t)at] : searchDistances[(size_t)at];
    };
    faiss::idx_t a = searchLabels[(size_t)(i * neighbors)];
    d1 = dist(i * neighbors);
    second = -1;
    d2 = d1;
    if (neighbors == 2) {
      faiss::idx_t b = searchLabels[(size_t)(i * 2 + 1)];
      if (b >= 0) {
        d2 = dist(i * 2 + 1);
        if (d1 == d2 && b < a) std::swap(a, b);
        second = (int32_t)b;
      }
    }
    if (a < 0 || a >= k) throw std::runtime_error("EMeans: assignment returned no centroid");
    label = (int32_t)a;
  }

  // Assigns rows [begin, end) of this pass's order against one centroid
  // snapshot and collects their per-cluster sums, counts and revival terms.
  void accumulate(int64_t begin, int64_t end, int64_t chunkRows, bool closing) {
    const float* snapshot = current();
    assigner.reset();
    assigner.add(k, snapshot);
    for (int64_t c = begin; c < end; c += chunkRows) {
      int64_t m = std::min(chunkRows, end - c);
      parallelRows(m, 256, [&](int64_t b, int64_t e) {
        for (int64_t i = b; i < e; i++) {
          std::copy_n(x + order[(size_t)(c + i)] * d, d, chunk.data() + i * d);
        }
      });
      assigner.search(m, chunk.data(), neighbors, searchDistances.data(), searchLabels.data());
      // Group the chunk's rows by cluster (a counting sort that keeps row
      // order), then accumulate each cluster in parallel across clusters:
      // the same additions in the same order as a serial pass.
      std::fill(bucketEnd.begin(), bucketEnd.end(), 0);
      for (int64_t i = 0; i < m; i++) {
        int32_t label, second;
        float d1, d2;
        neighborsOf(i, label, second, d1, d2);
        rowLabel[(size_t)i] = label;
        float margin = d2 - d1;
        rowMargin[(size_t)i] = std::isfinite(margin) ? margin : 0.0f;
        if (closing) {
          closingLabel[(size_t)(c + i)] = label;
          if (removalCost) closingSecond[(size_t)(c + i)] = second;
        }
        bucketEnd[(size_t)label]++;
      }
      for (int32_t j = 1; j < k; j++) bucketEnd[(size_t)j] += bucketEnd[(size_t)j - 1];
      for (int64_t i = m - 1; i >= 0; i--) {
        rowsByCluster[(size_t)--bucketEnd[(size_t)rowLabel[(size_t)i]]] = (int32_t)i;
      }
      // bucketEnd[j] now starts bucket j; it ends at bucketEnd[j + 1].
      parallelRows(k, 16, [&](int64_t b, int64_t e) {
        for (int64_t j = b; j < e; j++) {
          int32_t from = bucketEnd[(size_t)j];
          int32_t to = j + 1 < k ? bucketEnd[(size_t)j + 1] : (int32_t)m;
          if (from == to) continue;
          batchCounts[(size_t)j] += (float)(to - from);
          float* sum = batchSums.data() + (size_t)j * d;
          const float* centroid = snapshot + (size_t)j * d;
          const float* axis = revive ? axes.data() + (size_t)j * d : nullptr;
          float* terms = revive ? batchRemoval.data() + (size_t)j * (d + 1) : nullptr;
          for (int32_t r = from; r < to; r++) {
            int32_t i = rowsByCluster[(size_t)r];
            const float* row = chunk.data() + (size_t)i * d;
            for (int32_t t = 0; t < d; t++) sum[t] += row[t];
            if (revive) {
              // accumulate_removal_stats: p = <y, u_j> with y = x - c_j.
              float p = 0;
              for (int32_t t = 0; t < d; t++) p += (row[t] - centroid[t]) * axis[t];
              for (int32_t t = 0; t < d; t++) terms[t] += p * (row[t] - centroid[t]);
              terms[d] += rowMargin[(size_t)i];
            }
          }
        }
      });
    }
  }

  // Algorithm 1, line 8, in the mass/position form of the reference:
  //   N_j <- (1 - alpha) N_j + alpha n_j
  //   c_j <- c_j + (alpha n_j / N_j) (s_j / n_j - c_j)
  // Clusters absent from the batch keep their position and decay in mass.
  // With revival, R_j decays the same way, U decays as one scalar, and the
  // touched clusters add their batch terms and renew their unit axes.
  void update(double alpha, bool closing) {
    bool replace = alpha >= 1.0;
    float decay = (float)(1.0 - alpha);
    double weight = 0;
    if (revive) {
      double scale = axisScale * (1.0 - alpha);
      if (!(scale > MIN_AXIS_SCALE)) {
        float factor = (float)scale;
        parallelRows(k, 64, [&](int64_t b, int64_t e) {
          for (int64_t j = b; j < e; j++) {
            float* u = removalStats.data() + (size_t)j * (d + 1);
            for (int32_t t = 0; t < d; t++) u[t] *= factor;
          }
        });
        scale = 1;
      }
      axisScale = scale;
      weight = alpha / scale;
    }
    parallelRows(k, 64, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) updateCluster(j, (float)alpha, decay, replace, closing, (float)weight);
    });
  }

  void updateCluster(int64_t j, float alpha, float decay, bool replace, bool closing, float weight) {
    float count = batchCounts[(size_t)j];
    passCounts[(size_t)j] += count;
    if (revive) {
      float* stats = removalStats.data() + (size_t)j * (d + 1);
      float* terms = batchRemoval.data() + (size_t)j * (d + 1);
      stats[d] = decay * stats[d] + alpha * terms[d];
      if (count > 0) {
        // update_removal_axes
        double norm2 = 0;
        for (int32_t t = 0; t < d; t++) {
          stats[t] += weight * terms[t];
          norm2 += (double)stats[t] * stats[t];
        }
        double norm = std::sqrt(norm2);
        if (norm > 0) {
          float* axis = axes.data() + (size_t)j * d;
          for (int32_t t = 0; t < d; t++) axis[t] = (float)(stats[t] / norm);
        }
        std::fill_n(terms, d + 1, 0.0f);
      }
    }
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
    float share = alpha * count / mass;
    float* c = centroids.data() + (size_t)j * d;
    float inv = 1.0f / count;
    if (share >= 1.0f) {
      for (int32_t t = 0; t < d; t++) c[t] = sum[t] * inv;
    } else {
      for (int32_t t = 0; t < d; t++) c[t] += share * (sum[t] * inv - c[t]);
    }
    usage[(size_t)j] = mass;
    std::fill_n(sum, d, 0.0f);
    batchCounts[(size_t)j] = 0;
  }

  double axisNorm(int32_t j) const {
    const float* u = removalStats.data() + (size_t)j * (d + 1);
    double norm2 = 0;
    for (int32_t t = 0; t < d; t++) norm2 += (double)u[t] * u[t];
    return axisScale * std::sqrt(norm2);
  }

  // (donor, moved centroid) pairs for this check.
  std::vector<std::pair<int32_t, int32_t>> chooseSplits() {
    std::vector<std::pair<int32_t, int32_t>> pairs;
    std::vector<int32_t> moved;
    std::vector<int32_t> donors;
    if (removalCost) {
      // Every centroid cheap to remove moves, cheapest first, each paired
      // with the next donor by Oja's gain (longest axis first).
      std::vector<double> cost((size_t)k);
      for (int32_t j = 0; j < k; j++) cost[(size_t)j] = removalStats[(size_t)j * (d + 1) + d];
      std::vector<double> sorted = cost;
      size_t mid = sorted.size() / 2;
      std::nth_element(sorted.begin(), sorted.begin() + (ptrdiff_t)mid, sorted.end());
      double median = sorted[mid];
      if (sorted.size() % 2 == 0) {
        median = 0.5 * (median + *std::max_element(sorted.begin(), sorted.begin() + (ptrdiff_t)mid));
      }
      std::vector<char> candidate((size_t)k, 0);
      for (int32_t j = 0; j < k; j++) {
        double r = cost[(size_t)j];
        if (r <= 0 || r < REMOVAL_THRESHOLD * median) {
          candidate[(size_t)j] = 1;
          moved.push_back(j);
        }
      }
      if (moved.empty()) return pairs;
      std::stable_sort(moved.begin(), moved.end(),
                       [&](int32_t a, int32_t b) { return cost[(size_t)a] < cost[(size_t)b]; });
      std::vector<float> gain((size_t)k);
      for (int32_t j = 0; j < k; j++) {
        if (usage[(size_t)j] > 0 && !candidate[(size_t)j]) {
          donors.push_back(j);
          gain[(size_t)j] = (float)axisNorm(j);
        }
      }
      std::stable_sort(donors.begin(), donors.end(),
                       [&](int32_t a, int32_t b) { return gain[(size_t)a] > gain[(size_t)b]; });
    } else {
      // Clusters below the mass threshold move, splitting donors drawn
      // without replacement in proportion to mass (Gumbel top-k).
      double total = 0;
      for (float u : usage) total += u;
      float threshold = (float)(total * UNUSED_THRESHOLD / k);
      std::vector<std::pair<double, int32_t>> keyed;
      for (int32_t j = 0; j < k; j++) {
        float u = usage[(size_t)j];
        if (u < threshold) {
          moved.push_back(j);
        } else if (u > 0) {
          double uniform = std::max(uniform01(rng), std::numeric_limits<double>::min());
          keyed.emplace_back(std::log((double)u) - std::log(-std::log(uniform)), j);
        }
      }
      size_t splits = std::min(moved.size(), keyed.size());
      std::partial_sort(keyed.begin(), keyed.begin() + (ptrdiff_t)splits, keyed.end(),
                        [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t s = 0; s < splits; s++) donors.push_back(keyed[s].second);
    }
    size_t splits = std::min(moved.size(), donors.size());
    for (size_t s = 0; s < splits; s++) pairs.emplace_back(donors[s], moved[s]);
    return pairs;
  }

  // Splits each donor into itself and the moved centroid, at the Gaussian
  // half-means c -/+ s u along the donor's axis, s = sqrt(2 |U| / (pi N)).
  void reviveClusters(bool closing) {
    for (auto [source, target] : chooseSplits()) {
      float* donor = centroids.data() + (size_t)source * d;
      float* child = centroids.data() + (size_t)target * d;
      const float* axis = axes.data() + (size_t)source * d;
      double norm = axisNorm(source);
      double mass = (double)usage[(size_t)source];
      bool valid = norm > 0 && mass > 0 && std::isfinite(norm) && std::isfinite(mass);
      double spread = valid ? std::sqrt(2.0 * (norm / mass) / std::numbers::pi) : 0.0;
      bool same = true;
      for (int32_t t = 0; t < d; t++) {
        double c = (double)donor[t];
        double offset = spread * (double)axis[t];
        donor[t] = (float)(c - offset);
        child[t] = (float)(c + offset);
        same = same && donor[t] == child[t];
      }
      if (same) {
        // Coincident copies: separate them by one ulp in the first coordinate.
        float next = std::nextafter(child[0], INFINITY);
        child[0] = std::isfinite(next) ? next : std::nextafter(child[0], -INFINITY);
      }
      float half = usage[(size_t)source] / 2;
      usage[(size_t)source] = half;
      usage[(size_t)target] = half;
      float* from = removalStats.data() + (size_t)source * (d + 1);
      for (int32_t t = 0; t <= d; t++) from[t] /= 2;
      std::copy_n(from, d + 1, removalStats.data() + (size_t)target * (d + 1));
      std::copy_n(axis, d, axes.data() + (size_t)target * d);
      if (closing) {
        movedAt[(size_t)target] = closingDone;
        events.push_back({target, source, closingDone});
      }
      result.splits++;
    }
  }

  // Closing handover after revivals in the last pass.  A row assigned before
  // its centroid moved takes its second-nearest centroid, unless that moved
  // after the row too; those rows are assigned again.  A donor's other rows
  // from before its first later split go to the nearer of that split's two
  // centroids.  The closing sums of every centroid involved are rebuilt from
  // the rows that end with it.
  void handOver(int64_t rows, int64_t chunkRows) {
    std::vector<char> stale((size_t)rows, 0);
    std::vector<int64_t> search;
    for (int64_t rank = 0; rank < rows; rank++) {
      int32_t label = closingLabel[(size_t)rank];
      if (rank >= movedAt[(size_t)label]) continue;
      stale[(size_t)rank] = 1;
      int32_t second = removalCost ? closingSecond[(size_t)rank] : -1;
      if (second >= 0 && movedAt[(size_t)second] <= rank) {
        closingLabel[(size_t)rank] = second;
        result.handedOver++;
      } else {
        search.push_back(rank);
      }
    }
    const float* now = current();
    assigner.reset();
    assigner.add(k, now);
    std::vector<float> distance((size_t)chunkRows);
    std::vector<faiss::idx_t> label((size_t)chunkRows);
    for (size_t s = 0; s < search.size(); s += (size_t)chunkRows) {
      int64_t m = std::min<int64_t>(chunkRows, (int64_t)(search.size() - s));
      for (int64_t i = 0; i < m; i++) {
        std::copy_n(x + order[(size_t)search[s + (size_t)i]] * d, d, chunk.data() + i * d);
      }
      assigner.search(m, chunk.data(), 1, distance.data(), label.data());
      for (int64_t i = 0; i < m; i++) {
        if (label[(size_t)i] < 0 || label[(size_t)i] >= k) {
          throw std::runtime_error("EMeans: assignment returned no centroid");
        }
        closingLabel[(size_t)search[s + (size_t)i]] = (int32_t)label[(size_t)i];
      }
    }
    result.reassigned += (int64_t)search.size();

    // Each donor's splits in the order they happened.
    std::vector<Event> splits = events;
    std::stable_sort(splits.begin(), splits.end(), [](const Event& a, const Event& b) {
      return a.source != b.source ? a.source < b.source : a.born < b.born;
    });
    parallelRows(rows, 4096, [&](int64_t b, int64_t e) {
      for (int64_t rank = b; rank < e; rank++) {
        if (stale[(size_t)rank]) continue;
        int32_t own = closingLabel[(size_t)rank];
        auto it = std::upper_bound(splits.begin(), splits.end(), std::pair<int32_t, int64_t>(own, rank),
            [](const std::pair<int32_t, int64_t>& key, const Event& ev) {
              return key.first != ev.source ? key.first < ev.source : key.second < ev.born;
            });
        if (it == splits.end() || it->source != own || movedAt[(size_t)it->target] != it->born) continue;
        const float* row = x + order[(size_t)rank] * d;
        double nearOwn = squaredDistance(row, now + (size_t)own * d, d);
        double nearChild = squaredDistance(row, now + (size_t)it->target * d, d);
        if (nearChild < nearOwn || (nearChild == nearOwn && it->target < own)) {
          closingLabel[(size_t)rank] = it->target;
        }
      }
    });

    std::vector<char> rebuild((size_t)k, 0);
    for (const Event& ev : events) rebuild[(size_t)ev.source] = rebuild[(size_t)ev.target] = 1;
    for (int64_t rank = 0; rank < rows; rank++) {
      if (stale[(size_t)rank]) rebuild[(size_t)closingLabel[(size_t)rank]] = 1;
    }
    std::vector<int64_t> start((size_t)k + 1, 0);
    for (int64_t rank = 0; rank < rows; rank++) {
      int32_t own = closingLabel[(size_t)rank];
      if (rebuild[(size_t)own]) start[(size_t)own + 1]++;
    }
    for (int32_t j = 0; j < k; j++) start[(size_t)j + 1] += start[(size_t)j];
    std::vector<int64_t> members((size_t)start[(size_t)k]);
    std::vector<int64_t> fill(start.begin(), start.end() - 1);
    for (int64_t rank = 0; rank < rows; rank++) {
      int32_t own = closingLabel[(size_t)rank];
      if (rebuild[(size_t)own]) members[(size_t)fill[(size_t)own]++] = rank;
    }
    parallelRows(k, 16, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) {
        if (!rebuild[(size_t)j]) continue;
        float* sum = passSums.data() + (size_t)j * d;
        std::fill_n(sum, d, 0.0f);
        for (int64_t at = start[(size_t)j]; at < start[(size_t)j + 1]; at++) {
          const float* row = x + order[(size_t)members[(size_t)at]] * d;
          for (int32_t t = 0; t < d; t++) sum[t] += row[t];
        }
        passCounts[(size_t)j] = (double)(start[(size_t)j + 1] - start[(size_t)j]);
      }
    });
  }

  // Closing centroids: the means of the last pass's rows; centroids without
  // rows keep their EMA position.
  void finish() {
    parallelRows(k, 64, [&](int64_t b, int64_t e) {
      for (int64_t j = b; j < e; j++) {
        float* c = centroids.data() + (size_t)j * d;
        double count = passCounts[(size_t)j];
        if (count > 0) {
          const float* sum = passSums.data() + (size_t)j * d;
          for (int32_t t = 0; t < d; t++) c[t] = (float)((double)sum[t] / count);
        }
        if (params.spherical) normalizeRow(c, c, d);
      }
    });
    assigner.reset();
    assigner.add(k, centroids.data());
  }
};

}  // namespace

EMeans::Recipe EMeans::recipe(int64_t n, int64_t k, int32_t dims, double epochs) {
  validate(n, k, dims, epochs);
  bool quality = dims <= QUALITY_BATCH_MAX_D;
  double raw = (quality ? BATCH_QUALITY_C : BATCH_THROUGHPUT_C)
      * std::pow((double)k, BATCH_K_EXP) * std::pow((double)n, BATCH_N_EXP)
      * std::pow(epochs, BATCH_T_EXP);
  int64_t batch = roundPowerOfTwo(raw);
  batch = quality ? std::min(batch, n / 2) : std::min(batch, roundPowerOfTwo((double)n / 2));
  batch = std::clamp<int64_t>(batch, 1, n);
  return {.batchSize = batch, .alpha0 = stepLaw(n, k, epochs, batch)};
}

int64_t EMeans::workspaceBytes(int64_t n, int64_t k, int32_t dims, const Params& params) {
  validate(n, k, dims, params.epochs);
  // n == k: the assigner's copy, and the unit rows when spherical.
  if (n == k) return (int64_t)sizeof(float) * k * dims * (params.spherical ? 2 : 1);
  bool revival = revives(params, k, dims);
  int64_t neighbors = revival && params.revival == Revival::REMOVAL_COST ? 2 : 1;
  int64_t kd = k * dims;
  // Positions, batch sums, closing sums and the assigner's copy, plus the
  // unit directions when spherical.
  int64_t tables = 4 + (params.spherical ? 1 : 0);
  int64_t bytes = (int64_t)sizeof(float) * tables * kd;
  // Revival statistics, batch terms and axes.
  if (revival) bytes += (int64_t)sizeof(float) * (2 * k * (dims + 1) + kd);
  // Per-cluster masses, counts and bookkeeping, and the revival choice.
  bytes += k * 80;
  // The visiting order and the closing pass's labels, and the handover's
  // stale flags and row lists.
  bytes += n * (int64_t)(sizeof(int64_t) + neighbors * sizeof(int32_t) + 17);
  int64_t chunkRows = chunkRowsFor(recipe(n, k, dims, params.epochs).batchSize, dims);
  bytes += chunkRows * ((int64_t)sizeof(float) * dims
                        + neighbors * (int64_t)(sizeof(float) + sizeof(faiss::idx_t)) + 24);
  return bytes;
}

EMeans::Result EMeans::train(int64_t n, const float* x, int32_t k, const Params& params,
                             faiss::IndexFlat& assigner) {
  int32_t d = (int32_t)assigner.d;
  validate(n, k, d, params.epochs);
  bool ip = assigner.metric_type == faiss::METRIC_INNER_PRODUCT;
  if (ip != params.spherical || (!ip && assigner.metric_type != faiss::METRIC_L2)) {
    throw std::invalid_argument("EMeans: assigner metric must be L2, or inner product when spherical");
  }
  // As FAISS's Clustering does: a non-finite row would poison its centroid.
  parallelRows(n, 4096, [&](int64_t begin, int64_t end) {
    for (int64_t i = begin * d; i < end * d; i++) {
      if (!std::isfinite(x[i])) throw std::invalid_argument("EMeans: input contains NaN or Inf");
    }
  });
  if (n == k) {
    // Every row is a centroid, as FAISS copies them.
    assigner.reset();
    if (!params.spherical) {
      assigner.add(k, x);
      return {};
    }
    std::vector<float> rows((size_t)n * d);
    for (int64_t j = 0; j < k; j++) normalizeRow(x + j * d, rows.data() + j * d, d);
    assigner.add(k, rows.data());
    return {};
  }
  return Run(n, x, k, params, assigner).run();
}

}  // namespace luxir
