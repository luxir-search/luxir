#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace luxir {

// Bounds-checked byte cursor over parser input. Parser text comes straight
// off the wire, so this class is the ONLY way parsers touch their input bytes,
// concentrating every bounds check in one small, auditable place instead of
// scattering them across each grammar. Every accessor is safe at and past the
// end: peeks return NUL, movement saturates, and slices clamp.
//
// NUL is not a terminator (the input is a view, not a C string, and may
// contain NUL bytes); atEnd() is the only end-of-input test. peek() == '\0'
// at the end simply means "no structural character here", which is why the
// scanning helpers below are explicitly bounded.
//
// The cursor has no notion of escapes or tokens; those are grammar decisions
// that stay in the parser. Positions are plain byte offsets, freely saved and
// restored (backtracking is cheap and cannot dangle).
class Cursor {
  std::string_view input;
  size_t pos = 0;

public:
  explicit Cursor(std::string_view input) : input(input) {}

  size_t position() const { return pos; }
  size_t size() const { return input.size(); }
  bool atEnd() const { return pos >= input.size(); }
  size_t remaining() const { return input.size() - pos; }  // pos never exceeds size

  // Byte at the current position (+ahead), or NUL at/past the end.
  char peek() const { return pos < input.size() ? input[pos] : '\0'; }
  char peekAt(size_t ahead) const {
    return ahead < remaining() ? input[pos + ahead] : '\0';
  }

  // Move in either direction, saturating at the corresponding end.
  void advance(size_t n = 1) { pos += n < remaining() ? n : remaining(); }
  void retreat(size_t n = 1) { pos -= n < pos ? n : pos; }

  // Jump to an absolute position, clamped into [0, size()]. Restoring a
  // previously saved position() is always exact.
  void seek(size_t p) { pos = p < input.size() ? p : input.size(); }

  // View of [from, to), both ends clamped to the input.
  std::string_view slice(size_t from, size_t to) const {
    if (to > input.size()) to = input.size();
    if (from > to) from = to;
    return input.substr(from, to - from);
  }

  std::string errorContext(size_t at, size_t radius = 20) const {
    size_t from = at > radius ? at - radius : 0;
    std::string context;
    context.reserve(slice(from, at).size() + 6 + slice(at, at + radius).size());
    context.append(slice(from, at));
    context.append("<HERE>");
    context.append(slice(at, at + radius));
    return context;
  }

  bool startsWith(std::string_view s) const {
    return remaining() >= s.size() && slice(pos, pos + s.size()) == s;
  }

  bool startsWithAsciiIgnoreCase(std::string_view s) const {
    if (remaining() < s.size()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
      auto upper = [](char c) {
        return c >= 'a' && c <= 'z' ? (char)(c - ('a' - 'A')) : c;
      };
      if (upper(input[pos + i]) != upper(s[i])) return false;
    }
    return true;
  }

  // Consume s / c if it is next; returns whether it did.
  bool consume(std::string_view s) {
    if (!startsWith(s)) return false;
    pos += s.size();
    return true;
  }
  bool consume(char c) {
    if (pos < input.size() && input[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }

  // Advance while pred(byte) holds; returns the consumed slice.
  template <typename P>
  std::string_view takeWhile(P&& pred) {
    size_t start = pos;
    while (pos < input.size() && pred(input[pos])) ++pos;
    return slice(start, pos);
  }

  // Seek to the next occurrence at or after the current position. Leaves the
  // cursor at the occurrence, or at end when none exists. Empty text matches
  // the current position.
  bool findNext(std::string_view text) {
    if (text.empty()) return true;
    while (!atEnd()) {
      if (startsWith(text)) return true;
      advance();
    }
    return false;
  }

  // Inter-token whitespace length `ahead` bytes from the current position:
  // ASCII space/tab/newline/return, or U+3000 (ideographic space, E3 80 80 -
  // the one non-ASCII whitespace two decades of Lucene needed). 0 = none.
  size_t wsLenAt(size_t ahead) const {
    if (ahead >= remaining()) return 0;
    char c = input[pos + ahead];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    size_t left = remaining() - ahead;
    if ((uint8_t)c == 0xE3 && left >= 3 && (uint8_t)input[pos + ahead + 1] == 0x80 &&
        (uint8_t)input[pos + ahead + 2] == 0x80) {
      return 3;
    }
    return 0;
  }

  // Inter-token whitespace length at the current position: ASCII
  // space/tab/newline/return, or U+3000 (ideographic space, E3 80 80 - the
  // one non-ASCII whitespace two decades of Lucene needed). 0 = none.
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

} // namespace luxir
