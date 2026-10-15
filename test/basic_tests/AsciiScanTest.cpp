// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "luxir/analysis/AsciiScan.h"

#include <cstdint>
#include <random>
#include <string>

using namespace luxir;

namespace {

size_t findNonAsciiRef(const char* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if ((unsigned char) p[i] >= 0x80) return i;
  }
  return n;
}

std::string lowerRef(const char* p, size_t n) {
  std::string out(p, n);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = (char) (c + 32);
  }
  return out;
}

// Every byte value at every position of every length up to a few SIMD widths,
// at every start alignment within a 32-byte stride, so each kernel's main
// loop, tail and lane boundaries all see each byte class.
void checkAt(const std::string& buf, size_t off, size_t n) {
  const char* p = buf.data() + off;
  size_t want = findNonAsciiRef(p, n);
  ASSERT_EQ(want, ascii::findNonAscii(p, n)) << "off=" << off << " n=" << n;
  ASSERT_EQ(want, ascii::findNonAsciiSwar(p, n)) << "off=" << off << " n=" << n;
  std::string want2 = lowerRef(p, n);
  std::string got(n, '\0');
  ascii::lowerCopy(got.data(), p, n);
  ASSERT_EQ(want2, got) << "off=" << off << " n=" << n;
  ascii::lowerCopySwar(got.data(), p, n);
  ASSERT_EQ(want2, got) << "off=" << off << " n=" << n;
}

}  // namespace

TEST(AsciiScanTest, EveryByteAtEveryPositionAndAlignment) {
  constexpr size_t maxLen = 80;
  std::string base(maxLen + 32, 'm');
  for (size_t i = 0; i < base.size(); i++) base[i] = "aZ09 _.Ax"[i % 9];
  for (int b = 0; b < 256; b++) {
    for (size_t off = 0; off < 32; off++) {
      for (size_t n = 0; n <= maxLen; n += (n < 40 ? 1 : 7)) {
        for (size_t pos = 0; pos < n; pos++) {
          std::string buf = base;
          buf[off + pos] = (char) b;
          checkAt(buf, off, n);
          if (::testing::Test::HasFatalFailure()) return;
        }
      }
    }
  }
}

TEST(AsciiScanTest, RandomBuffers) {
  std::mt19937_64 rng(7);
  for (int round = 0; round < 2000; round++) {
    size_t n = (size_t) (rng() % 300);
    std::string buf(n + 32, '\0');
    unsigned highOdds = (unsigned) (rng() % 4);  // 0: pure ASCII
    for (char& c : buf) {
      unsigned r = (unsigned) rng();
      c = (char) (highOdds && r % (64 >> highOdds) == 0 ? 0x80 | (r >> 8) : (r >> 8) & 0x7F);
    }
    checkAt(buf, (size_t) (rng() % 32), n);
    if (HasFatalFailure()) return;
  }
}
