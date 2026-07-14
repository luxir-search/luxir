#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace solux {

// Bounds-checked byte cursor over parser input.  Query strings come straight
// off the wire, so string parsers are an attack surface: this class is the
// ONLY way a parser touches its input bytes, concentrating every bounds check
// in one small, auditable place instead of scattering them across the
// grammar.  Every accessor is safe at and past the end: peeks return NUL,
// advances saturate, and slices clamp - parser logic can be wrong about
// positions without ever reading outside [data, data + size).
//
// NUL is not a terminator (the input is a view, not a C string, and may
// contain NUL bytes); atEnd() is the only end-of-input test.  peek() == '\0'
// at the end simply means "no structural character here", which is why the
// scanning helpers below are all explicitly bounded - a raw
// `while (cond(peek()))` loop would spin at the end if cond ignores NUL.
//
// The cursor has no notion of escapes or tokens; those are grammar decisions
// that stay in the parser.  Positions are plain byte offsets, freely saved
// and restored (backtracking is cheap and cannot dangle).
class Cursor {
  const char* base;
  size_t len;
  size_t pos = 0;

public:
  explicit Cursor(std::string_view input) : base(input.data()), len(input.size()) {}

  size_t position() const { return pos; }
  size_t size() const { return len; }
  bool atEnd() const { return pos >= len; }
  size_t remaining() const { return len - pos; }  // pos never exceeds len

  // Byte at the current position (+ahead), or NUL at/past the end.
  char peek() const { return pos < len ? base[pos] : '\0'; }
  char peekAt(size_t ahead) const { return ahead < remaining() ? base[pos + ahead] : '\0'; }

  // Move forward n bytes, saturating at the end.
  void advance(size_t n = 1) { pos += n < remaining() ? n : remaining(); }

  // Jump to an absolute position, clamped into [0, size()].  Restoring a
  // previously saved position() is always exact.
  void seek(size_t p) { pos = p < len ? p : len; }

  // View of [from, to), both ends clamped to the input.
  std::string_view slice(size_t from, size_t to) const {
    if (to > len) to = len;
    if (from > to) from = to;
    return {base + from, to - from};
  }

  bool startsWith(std::string_view s) const {
    return remaining() >= s.size() && slice(pos, pos + s.size()) == s;
  }

  // Consume s / c if it is next; returns whether it did.
  bool consume(std::string_view s) {
    if (!startsWith(s)) return false;
    pos += s.size();
    return true;
  }
  bool consume(char c) {
    if (pos < len && base[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }

  // Advance while pred(byte) holds; returns the consumed slice.
  template <typename P>
  std::string_view takeWhile(P&& pred) {
    size_t start = pos;
    while (pos < len && pred(base[pos])) ++pos;
    return {base + start, pos - start};
  }

  // Inter-token whitespace length `ahead` bytes from the current position:
  // ASCII space/tab/newline/return, or U+3000 (ideographic space, E3 80 80 -
  // the one non-ASCII whitespace two decades of Lucene needed).  0 = none.
  size_t wsLenAt(size_t ahead) const {
    if (ahead >= remaining()) return 0;
    char c = base[pos + ahead];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    size_t left = remaining() - ahead;
    if ((uint8_t)c == 0xE3 && left >= 3 && (uint8_t)base[pos + ahead + 1] == 0x80 &&
        (uint8_t)base[pos + ahead + 2] == 0x80) {
      return 3;
    }
    return 0;
  }

  // Inter-token whitespace length at the current position: ASCII
  // space/tab/newline/return, or U+3000 (ideographic space, E3 80 80 - the
  // one non-ASCII whitespace two decades of Lucene needed).  0 = none.
  size_t wsLen() const { return wsLenAt(0); }

  // Skip whitespace; returns whether any was skipped.
  bool skipWs() {
    bool any = false;
    while (size_t n = wsLen()) {
      pos += n;
      any = true;
    }
    return any;
  }
};

} // namespace solux
