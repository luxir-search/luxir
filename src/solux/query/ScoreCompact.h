#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

namespace solux {

inline bool scoreCanReach(float score, double bound, float minCompetitiveScore,
                          double scoreBoundFactor) {
  return ((double) score + bound) * scoreBoundFactor >= (double) minCompetitiveScore;
}

inline uint32_t orderedFloatBits(float value) {
  uint32_t bits = std::bit_cast<uint32_t>(value);
  uint32_t sign = 0x80000000U;
  return (bits & sign) != 0 ? ~bits : bits ^ sign;
}

inline float floatFromOrderedBits(uint32_t ordered) {
  uint32_t sign = 0x80000000U;
  uint32_t bits = (ordered & sign) != 0 ? ordered ^ sign : ~ordered;
  return std::bit_cast<float>(bits);
}

template <typename CanReach>
inline float competitiveScoreThresholdBisection(uint32_t lo, uint32_t hi,
                                                CanReach&& canReach) {
  while (lo + 1 < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (canReach(floatFromOrderedBits(mid))) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return floatFromOrderedBits(hi);
}

inline float competitiveScoreThresholdReference(float minCompetitiveScore,
                                                double scoreBoundFactor, double bound) {
  auto canReach = [&](float score) {
    return scoreCanReach(score, bound, minCompetitiveScore, scoreBoundFactor);
  };

  if (canReach(std::numeric_limits<float>::lowest())) {
    return -std::numeric_limits<float>::infinity();
  }
  if (!canReach(std::numeric_limits<float>::max())) {
    return std::numeric_limits<float>::infinity();
  }

  uint32_t lo = orderedFloatBits(std::numeric_limits<float>::lowest());
  uint32_t hi = orderedFloatBits(std::numeric_limits<float>::max());
  return competitiveScoreThresholdBisection(lo, hi, canReach);
}

inline float competitiveScoreThreshold(float minCompetitiveScore, double scoreBoundFactor,
                                       double bound, double seed) {
  auto canReach = [&](float score) {
    return scoreCanReach(score, bound, minCompetitiveScore, scoreBoundFactor);
  };

  if (canReach(std::numeric_limits<float>::lowest())) {
    return -std::numeric_limits<float>::infinity();
  }
  if (!canReach(std::numeric_limits<float>::max())) {
    return std::numeric_limits<float>::infinity();
  }

  uint32_t lo = orderedFloatBits(std::numeric_limits<float>::lowest());
  uint32_t hi = orderedFloatBits(std::numeric_limits<float>::max());
  double lowFloat = (double) std::numeric_limits<float>::lowest();
  double highFloat = (double) std::numeric_limits<float>::max();
  if (std::isnan(seed)) {
    return competitiveScoreThresholdBisection(lo, hi, canReach);
  }
  if (seed < lowFloat) {
    seed = lowFloat;
  } else if (seed > highFloat) {
    seed = highFloat;
  }

  uint32_t probe = orderedFloatBits((float) seed);
  if (probe < lo) {
    probe = lo;
  } else if (probe > hi) {
    probe = hi;
  }

  bool reaches = canReach(floatFromOrderedBits(probe));
  constexpr int32_t kMaxGallopDoublings = 8;
  if (reaches) {
    uint32_t upper = probe;
    uint32_t step = 1;
    for (int32_t i = 0; i < kMaxGallopDoublings; i++) {
      uint32_t distance = upper - lo;
      uint32_t lower = step >= distance ? lo : upper - step;
      if (!canReach(floatFromOrderedBits(lower))) {
        return competitiveScoreThresholdBisection(lower, upper, canReach);
      }
      if (lower == lo) {
        break;
      }
      upper = lower;
      if (step <= std::numeric_limits<uint32_t>::max() / 2) {
        step *= 2;
      }
    }
    return competitiveScoreThresholdBisection(lo, hi, canReach);
  }

  uint32_t lower = probe;
  uint32_t step = 1;
  for (int32_t i = 0; i < kMaxGallopDoublings; i++) {
    uint32_t distance = hi - lower;
    uint32_t upper = step >= distance ? hi : lower + step;
    if (canReach(floatFromOrderedBits(upper))) {
      return competitiveScoreThresholdBisection(lower, upper, canReach);
    }
    if (upper == hi) {
      break;
    }
    lower = upper;
    if (step <= std::numeric_limits<uint32_t>::max() / 2) {
      step *= 2;
    }
  }
  return competitiveScoreThresholdBisection(lo, hi, canReach);
}

inline float competitiveScoreThreshold(float minCompetitiveScore, double scoreBoundFactor,
                                       double bound) {
  double seed = (double) minCompetitiveScore / scoreBoundFactor - bound;
  return competitiveScoreThreshold(minCompetitiveScore, scoreBoundFactor, bound, seed);
}

inline int32_t compactByScoreThreshold(int32_t* docs, float* scores, int32_t size,
                                       float threshold) {
  int32_t write = 0;
  for (int32_t read = 0; read < size; read++) {
    bool keep = scores[(size_t) read] >= threshold;
    docs[(size_t) write] = docs[(size_t) read];
    scores[(size_t) write] = scores[(size_t) read];
    write += (int32_t) keep;
  }
  return write;
}

inline int32_t compactByScoreNotLessThanThreshold(int32_t* docs, float* scores, int32_t size,
                                                  float threshold) {
  int32_t write = 0;
  for (int32_t read = 0; read < size; read++) {
    bool keep = !(scores[(size_t) read] < threshold);
    docs[(size_t) write] = docs[(size_t) read];
    scores[(size_t) write] = scores[(size_t) read];
    write += (int32_t) keep;
  }
  return write;
}

} // namespace solux
