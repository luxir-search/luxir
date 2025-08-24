#pragma once
#include <cstdint>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "solux/util/solux_util.h"

namespace solux {
template<typename SkinnyType=uint8_t, typename KeyType=int64_t, typename ValType=int64_t>
class SkinnyCounter {
public:
  size_t max;
  std::vector<SkinnyType> counts;
  boost::unordered_flat_map<KeyType, ValType> overflow;

  /// "max" is the maximum number of values to expect, from 0 to max-1
  explicit SkinnyCounter(size_t max) : max(max) {
    counts.resize(max);
  }

  void SOLUX_INLINE increment(KeyType key) {
    assert(key >=0 && key < max);
    if (++counts[key] == 0) {
      overflow[key] += std::numeric_limits<SkinnyType>::max() + 1;
    };
  }

  void SOLUX_INLINE increment(KeyType key, ValType val) {
    assert(key >=0 && key < max && val >= 0);
    size_t tot = (size_t)counts[key] + val;
    if (tot > std::numeric_limits<SkinnyType>::max()) {
      counts[key] = 0;
      overflow[key] += tot;
    } else {
      counts[key] = (SkinnyType)tot;
    }
  }

  /// Merge another SkinnyCounter into this one.  They must have the same "max".
  void SOLUX_NOINLINE merge(const SkinnyCounter& other) {
    assert(other.max == max);
    for (size_t i=0; i<max; i++) {
      increment(i, other.counts[i]);
    }

    // now merge the overflow maps
    for (auto kv : other.overflow) {
      overflow[kv.first] += kv.second;
    }
  }

};

using SkinnyCounter8 = SkinnyCounter<uint8_t, int64_t, int64_t>;

}