// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "luxir/analysis/AsciiScan.h"

#include <cstdint>
#include <memory>
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

// Checks every kernel against the references on buf[off, off + n).
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

// The kernels classify each byte on its own, and findNonAscii then reports the
// first hit, so every byte value on every lane of every length up to a few SIMD
// widths covers each kernel's main loop, tails and lane boundaries. Loads are
// unaligned, so a few starts suffice (an aligned-load prologue would need all 32
// within a stride). Each buffer ends where its allocation does, so ASan reports
// a read past n.
TEST(AsciiScanTest, EveryByteOnEveryLane) {
  constexpr size_t maxLen = 80;
  std::string want, got;
  for (size_t off : {0, 1, 31}) {
    for (size_t n = 0; n <= maxLen; n++) {
      auto storage = std::make_unique<char[]>(off + n);
      char* p = storage.get() + off;
      got.resize(n);
      // A cycle through all byte values puts every value on every lane over its
      // rotations.
      for (int r = 0; r < 256; r++) {
        for (size_t i = 0; i < n; i++) p[i] = (char) (i + r);
        want = lowerRef(p, n);
        ascii::lowerCopy(got.data(), p, n);
        if (got != want) FAIL() << "lowerCopy off=" << off << " n=" << n << " r=" << r;
        ascii::lowerCopySwar(got.data(), p, n);
        if (got != want) FAIL() << "lowerCopySwar off=" << off << " n=" << n << " r=" << r;
        if (r < 128) {  // every ASCII value on every lane, and no hit
          for (size_t i = 0; i < n; i++) p[i] = (char) ((i + r) & 0x7F);
          if (ascii::findNonAscii(p, n) != n || ascii::findNonAsciiSwar(p, n) != n) {
            FAIL() << "findNonAscii off=" << off << " n=" << n << " r=" << r;
          }
        }
      }
      // ASCII before pos and high bytes from pos on: each high value at pos is
      // the first hit.
      for (size_t pos = 0; pos <= n; pos++) {
        for (size_t i = 0; i < n; i++) p[i] = (char) (i < pos ? (i * 37 + n) % 128 : 0x80 | i);
        for (int h = 0x80; h < 0x100; h++) {
          if (pos < n) p[pos] = (char) h;
          size_t a = ascii::findNonAscii(p, n), b = ascii::findNonAsciiSwar(p, n);
          if (a != pos || b != pos) {
            FAIL() << "off=" << off << " n=" << n << " pos=" << pos << " h=" << h
                   << ": " << a << ", swar " << b;
          }
          if (pos == n) break;
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
