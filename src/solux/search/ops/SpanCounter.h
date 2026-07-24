#pragma once

#include <cstdint>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>

#include "solux/util/solux_util.h"

namespace solux {

class SpanCounter {
public:
  static constexpr int SPAN_BITS = 16;
  static constexpr int64_t SPAN_SIZE = (int64_t)1 << SPAN_BITS;
  static constexpr uint32_t SPAN_MASK = (uint32_t)(SPAN_SIZE - 1);
  using SpanMap = boost::unordered_flat_map<uint16_t, uint16_t>;

  size_t maxOrd;
  std::vector<SpanMap> spans;
  boost::unordered_flat_map<uint64_t, uint64_t> overflow;

  explicit SpanCounter(size_t maxOrd) : maxOrd(maxOrd) {
    // Keep at least span 0 so it always exists.
    spans.resize((size_t)((maxOrd >> SPAN_BITS) + 1));
  }

  void SOLUX_INLINE increment(int64_t ord) {
    assert(ord >= 0 && (size_t)ord < maxOrd);
    uint16_t& c =
        spans[(size_t)(ord >> SPAN_BITS)][(uint16_t)((uint64_t)ord & SPAN_MASK)];
    if (++c == 0) {
      // The saturated slot remains present at zero, and overflow restores the
      // 65536 low-bit wrap.
      overflow[(uint64_t)ord] += (uint64_t)SPAN_SIZE;
    }
  }

  void SOLUX_INLINE increment(int64_t ord, int64_t val) {
    assert(ord >= 0 && (size_t)ord < maxOrd && val >= 0);
    uint16_t& c =
        spans[(size_t)(ord >> SPAN_BITS)][(uint16_t)((uint64_t)ord & SPAN_MASK)];
    uint64_t tot = (uint64_t)c + (uint64_t)val;
    uint64_t carry = tot & ~(uint64_t)SPAN_MASK;
    c = (uint16_t)tot;
    if (carry) {
      overflow[(uint64_t)ord] += carry;
    }
  }

  int64_t total(int64_t ord) const {
    int64_t t = 0;
    const auto& m = spans[(size_t)(ord >> SPAN_BITS)];
    auto it = m.find((uint16_t)((uint64_t)ord & SPAN_MASK));
    if (it != m.end()) {
      t = (int64_t)it->second;
    }
    auto oit = overflow.find((uint64_t)ord);
    if (oit != overflow.end()) {
      t += (int64_t)oit->second;
    }
    return t;
  }

  size_t distinct() const {
    size_t n = 0;
    for (const auto& m : spans) {
      n += m.size();
    }
    return n;
  }

  // Approximate heap footprint: the span directory, the per-span open-addressed
  // tables (slots + one control byte each), and the overflow table. Used only
  // by the SOLUX_FACET_BYTES measurement hook.
  size_t bytesUsed() const {
    size_t b = spans.capacity() * sizeof(SpanMap);
    for (const auto& m : spans) {
      b += m.bucket_count() * (sizeof(SpanMap::value_type) + 1);
    }
    b += overflow.bucket_count() * (sizeof(std::pair<uint64_t, uint64_t>) + 1);
    return b;
  }

  void SOLUX_NOINLINE merge(const SpanCounter& other) {
    assert(other.maxOrd == maxOrd);
    for (size_t s = 0; s < other.spans.size(); s++) {
      uint64_t base = (uint64_t)s << SPAN_BITS;
      for (const auto& [low, cnt] : other.spans[s]) {
        increment((int64_t)(base | (uint64_t)low), (int64_t)cnt);
      }
    }
    // The span slot is only the low bits. Add the other's overflow multiple
    // separately so every part of its true total is folded exactly once.
    for (const auto& [ord, cnt] : other.overflow) {
      overflow[ord] += cnt;
    }
  }

  template<typename Accept>
  void foldOverflow(Accept&& accept) {
    for (auto& [ord, ovf] : overflow) {
      uint16_t& slot =
          spans[(size_t)(ord >> SPAN_BITS)][(uint16_t)(ord & SPAN_MASK)];
      int64_t total = (int64_t)ovf + (int64_t)slot;
      slot = 0;
      accept((int64_t)ord, total);
    }
  }

  template<typename Accept>
  void forEachCount(Accept&& accept) const {
    for (size_t s = 0; s < spans.size(); s++) {
      uint64_t base = (uint64_t)s << SPAN_BITS;
      for (const auto& [low, cnt] : spans[s]) {
        accept((int64_t)(base | (uint64_t)low), (int64_t)cnt);
      }
    }
  }

  void releaseStorage() {
    spans.clear();
    spans.shrink_to_fit();
  }
};

}
