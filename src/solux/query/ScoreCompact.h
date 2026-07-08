#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
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

inline float competitiveScoreThreshold(float minCompetitiveScore, double scoreBoundFactor,
                                       double bound) {
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
