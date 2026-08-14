#pragma once

#include <bit>
#include <cstdint>

namespace luxir {

// Sortable encodings for IEEE-754 values, following Lucene's NumericUtils:
// the float/double is bit-cast to a same-width signed integer, then the
// non-sign bits are flipped for negative values so the result orders the
// same as the original floating point value when compared as a signed
// integer.  -0.0 sorts before +0.0, NaN sorts above +Inf.
// The bit-flip is an involution, so encode and decode share the helpers.

inline int64_t sortableDoubleBits(int64_t bits) {
  return bits ^ ((bits >> 63) & 0x7fffffffffffffffLL);
}

inline int32_t sortableFloatBits(int32_t bits) {
  return bits ^ ((bits >> 31) & 0x7fffffff);
}

inline int64_t doubleToSortableInt64(double val) {
  // Canonicalize NaN (any payload, any sign), like Java's doubleToLongBits.
  // A NaN with the sign bit set would otherwise sort below -Inf.
  int64_t bits = val != val ? 0x7ff8000000000000LL : std::bit_cast<int64_t>(val);
  return sortableDoubleBits(bits);
}

inline double sortableInt64ToDouble(int64_t encoded) {
  return std::bit_cast<double>(sortableDoubleBits(encoded));
}

// The result sign-extends to int64_t without disturbing sort order, which is
// how FLOAT fields store it in the int column (the per-block min/gcd
// compression squeezes the 32-bit range back down).
inline int32_t floatToSortableInt32(float val) {
  int32_t bits = val != val ? 0x7fc00000 : std::bit_cast<int32_t>(val);
  return sortableFloatBits(bits);
}

inline float sortableInt32ToFloat(int32_t encoded) {
  return std::bit_cast<float>(sortableFloatBits(encoded));
}

} // end namespace
