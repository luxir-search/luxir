// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <numbers>
#include <span>
#include <vector>

namespace luxir {

// Mergeable quantile sketch: Dunning's merging t-digest with the k1
// (arcsine) scale function.
//
// Values are buffered and folded into weighted centroids, sorted by mean,
// whose sizes shrink toward both tails: a centroid may span at most one unit
// of k(q) = compression / (2 pi) * asin(2q - 1). The digest therefore keeps
// at most about compression centroids, and its quantile error is smallest at
// the extremes. Error is rank error, bounded roughly by
// q(1 - q) * pi / compression per centroid before interpolation; at the
// default compression of 100 a median is typically within 0.2% of rank.
// min and max are tracked exactly.
//
// While fewer than bufferLimit() values have been added in total the digest
// holds them raw and answers exactly: quantile() interpolates linearly
// between the two closest ranks, and cdf() is the fraction of values <= x.
// Equal finite inputs have identical bits after normalizing signed zero.
// Sorting only raw doubles and stably merging fixed runs makes compression
// independent of the sort implementation.
class TDigest {
public:
  static constexpr double DEFAULT_COMPRESSION = 100.0;
  static constexpr double MIN_COMPRESSION = 20.0;
  static constexpr double MAX_COMPRESSION = 1000.0;

  struct Centroid {
    double mean;
    double weight;
  };

  double compression;
  std::pmr::vector<Centroid> centroids; // sorted by mean; empty while exact
  std::pmr::vector<double> buffer;      // raw values not yet folded in
  std::pmr::vector<Centroid> scratch;
  double centroidWeight = 0.0;
  double min = std::numeric_limits<double>::infinity();
  double max = -std::numeric_limits<double>::infinity();
  bool sortedBuffer = true;

  static size_t bufferLimitFor(double compression) {
    return (size_t)(5.0 * compression);
  }

  // Resident bytes a digest of this compression may hold.
  static size_t maxBytes(double compression) {
    size_t limit = bufferLimitFor(compression);
    return sizeof(TDigest) + 4 * limit * sizeof(double)
        + 6 * limit * sizeof(Centroid);
  }

  explicit TDigest(double compression = DEFAULT_COMPRESSION,
                   std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : compression(compression), centroids(resource), buffer(resource), scratch(resource) {
    assert(compression >= MIN_COMPRESSION && compression <= MAX_COMPRESSION);
  }

  size_t bufferLimit() const { return bufferLimitFor(compression); }

  // Raw values only: quantiles and ranks are exact.
  bool exact() const { return centroids.empty(); }

  double count() const { return centroidWeight + (double)buffer.size(); }

  bool empty() const { return count() == 0.0; }

  // Non-finite values are the caller's responsibility to drop.
  void add(double x) {
    assert(std::isfinite(x));
    x += 0.0; // -0.0 becomes +0.0.
    min = std::min(min, x);
    max = std::max(max, x);
    buffer.push_back(x);
    sortedBuffer = false;
    if (buffer.size() >= bufferLimit()) compress({});
  }

  void merge(const TDigest& other) {
    if (other.empty()) return;
    if (empty()) {
      centroids.assign(other.centroids.begin(), other.centroids.end());
      buffer.assign(other.buffer.begin(), other.buffer.end());
      centroidWeight = other.centroidWeight;
      min = other.min;
      max = other.max;
      sortedBuffer = other.sortedBuffer;
      return;
    }
    min = std::min(min, other.min);
    max = std::max(max, other.max);
    if (other.exact()) {
      for (double x : other.buffer) {
        buffer.push_back(x);
        sortedBuffer = false;
        if (buffer.size() >= bufferLimit()) compress({});
      }
      return;
    }
    buffer.insert(buffer.end(), other.buffer.begin(), other.buffer.end());
    sortedBuffer = false;
    compress(other.centroids);
  }

  // Prepare for quantile() and cdf(): fold pending values into the
  // centroids, or sort the raw values of an exact digest.
  void finish() {
    if (exact()) {
      if (!sortedBuffer) std::sort(buffer.begin(), buffer.end());
      sortedBuffer = true;
    } else if (!buffer.empty()) {
      compress({});
    }
  }

  // q in [0, 1]. NaN when empty. Call finish() first.
  double quantile(double q) const {
    assert(q >= 0.0 && q <= 1.0);
    if (empty()) return std::numeric_limits<double>::quiet_NaN();
    if (q == 0.0) return min;
    if (q == 1.0) return max;
    if (exact()) {
      assert(sortedBuffer);
      double position = q * (double)(buffer.size() - 1);
      size_t lo = (size_t)std::floor(position);
      size_t hi = std::min(lo + 1, buffer.size() - 1);
      double fraction = position - (double)lo;
      return interpolate(buffer[lo], buffer[hi], fraction);
    }
    assert(buffer.empty());
    const auto& c = centroids;
    const size_t n = c.size();
    const double total = centroidWeight;
    if (n == 1) return c[0].mean;
    double index = q * total;
    if (index < 1.0) return min;
    // Half of the first centroid lies between min and its mean.
    if (c[0].weight > 1.0 && index < c[0].weight / 2.0) {
      return interpolate(min, c[0].mean,
          (index - 1.0) / (c[0].weight / 2.0 - 1.0));
    }
    if (index > total - 1.0) return max;
    if (c[n - 1].weight > 2.0 && total - index <= c[n - 1].weight / 2.0) {
      return interpolate(max, c[n - 1].mean,
          (total - index - 1.0) / (c[n - 1].weight / 2.0 - 1.0));
    }
    double weightSoFar = c[0].weight / 2.0;
    for (size_t i = 0; i + 1 < n; i++) {
      double dw = (c[i].weight + c[i + 1].weight) / 2.0;
      if (weightSoFar + dw > index) {
        // A singleton centroid is an exact sample: do not smear it.
        double leftUnit = 0.0;
        if (c[i].weight == 1.0) {
          if (index - weightSoFar < 0.5) return c[i].mean;
          leftUnit = 0.5;
        }
        double rightUnit = 0.0;
        if (c[i + 1].weight == 1.0) {
          if (weightSoFar + dw - index <= 0.5) return c[i + 1].mean;
          rightUnit = 0.5;
        }
        double z1 = index - weightSoFar - leftUnit;
        double z2 = weightSoFar + dw - index - rightUnit;
        return weightedAverage(c[i].mean, z2, c[i + 1].mean, z1);
      }
      weightSoFar += dw;
    }
    return interpolate(c[n - 1].mean, max,
                       (index - weightSoFar) / (c[n - 1].weight / 2.0));
  }

  // Fraction of the values <= x, in [0, 1]. NaN when empty. Call finish()
  // first.
  double cdf(double x) const {
    if (empty()) return std::numeric_limits<double>::quiet_NaN();
    if (exact()) {
      assert(sortedBuffer);
      auto end = std::upper_bound(buffer.begin(), buffer.end(), x);
      return (double)(end - buffer.begin()) / (double)buffer.size();
    }
    assert(buffer.empty());
    if (x < min) return 0.0;
    if (x >= max) return 1.0;
    const auto& c = centroids;
    const size_t n = c.size();
    const double total = centroidWeight;
    if (n == 1) return fractionBetween(min, max, x);
    if (x < c[0].mean) {
      // Linear from min (weight 1) to the first centroid's midpoint.
      double left = 1.0 + fractionBetween(min, c[0].mean, x)
          * std::max(0.0, c[0].weight / 2.0 - 1.0);
      return std::min(left, c[0].weight / 2.0) / total;
    }
    if (x >= c[n - 1].mean) {
      double right = max > c[n - 1].mean
          ? 1.0 + (1.0 - fractionBetween(c[n - 1].mean, max, x))
              * std::max(0.0, c[n - 1].weight / 2.0 - 1.0)
          : 0.0;
      return 1.0 - std::min(right, c[n - 1].weight / 2.0) / total;
    }
    double weightSoFar = c[0].weight / 2.0;
    for (size_t i = 0; i + 1 < n; i++) {
      if (x < c[i + 1].mean) {
        double dw = (c[i].weight + c[i + 1].weight) / 2.0;
        double leftUnit = c[i].weight == 1.0 ? 0.5 : 0.0;
        double rightUnit = c[i + 1].weight == 1.0 ? 0.5 : 0.0;
        double fraction = c[i + 1].mean > c[i].mean
            ? fractionBetween(c[i].mean, c[i + 1].mean, x) : 0.0;
        double below = weightSoFar + leftUnit
            + (dw - leftUnit - rightUnit) * fraction;
        return below / total;
      }
      weightSoFar += (c[i].weight + c[i + 1].weight) / 2.0;
    }
    return 1.0;
  }

private:
  static double fractionBetween(double a, double b, double x) {
    double span = b - a;
    if (std::isfinite(span)) return (x - a) / span;
    return (x / 2.0 - a / 2.0) / (b / 2.0 - a / 2.0);
  }

  static double interpolate(double a, double b, double fraction) {
    return std::lerp(a, b, std::clamp(fraction, 0.0, 1.0));
  }

  // (x1 * w1 + x2 * w2) / (w1 + w2), clamped to [x1, x2] against rounding.
  static double weightedAverage(double x1, double w1, double x2, double w2) {
    if (w1 + w2 <= 0.0) return interpolate(x1, x2, 0.5);
    return interpolate(x1, x2, w2 / (w1 + w2));
  }

  // Merge sorted runs in a fixed order: current centroids, extra, raw values.
  // Only raw doubles need sorting; equal values are bit-identical, so their
  // sort order cannot affect the fold.
  void compress(std::span<const Centroid> extra) {
    if (!extra.empty()) {
      scratch.clear();
      scratch.reserve(centroids.size() + extra.size());
      std::merge(centroids.begin(), centroids.end(), extra.begin(), extra.end(),
                 std::back_inserter(scratch),
                 [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });
      centroids.swap(scratch);
      for (const Centroid& centroid : extra) centroidWeight += centroid.weight;
    }
    if (!sortedBuffer) std::sort(buffer.begin(), buffer.end());
    const double total = centroidWeight + (double)buffer.size();

    const double normalizer = compression / (2.0 * std::numbers::pi);
    auto weightLimit = [&](double weightSoFar) {
      double q = std::clamp(weightSoFar / total, 0.0, 1.0);
      double kNext = normalizer * std::asin(2.0 * q - 1.0) + 1.0;
      return kNext >= normalizer * std::numbers::pi / 2.0
          ? total : total * (std::sin(kNext / normalizer) + 1.0) / 2.0;
    };
    size_t i = 0, j = 0;
    auto nextCentroid = [&]() {
      if (i < centroids.size() && (j == buffer.size() || centroids[i].mean <= buffer[j])) {
        return centroids[i++];
      }
      return Centroid{buffer[j++], 1.0};
    };
    scratch.clear();
    scratch.reserve((size_t)compression);
    Centroid current = nextCentroid();
    double weightSoFar = 0.0;
    double limit = weightLimit(weightSoFar);
    while (i < centroids.size() || j < buffer.size()) {
      Centroid next = nextCentroid();
      double proposed = current.weight + next.weight;
      if (weightSoFar + proposed <= limit) {
        current.mean = interpolate(current.mean, next.mean, next.weight / proposed);
        current.weight = proposed;
      } else {
        weightSoFar += current.weight;
        limit = weightLimit(weightSoFar);
        scratch.push_back(current);
        current = next;
      }
    }
    scratch.push_back(current);
    centroids.swap(scratch);
    buffer.clear();
    sortedBuffer = true;
    centroidWeight = total;
  }
};

} // namespace luxir
