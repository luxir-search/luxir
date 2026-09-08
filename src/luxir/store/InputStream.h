// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/StrRef.h"
#include "luxir/util/encoding.h"

namespace luxir {

class InputStream {
  const char *pos = nullptr;  // these initializations just to suppress maybe-uninitialized warnings with -O3
  const char *start = nullptr;
  const char *end = nullptr;
  // Future?
  // size_t offset; // the position of "start" in the file?
  //                // Or perhaps InputStream should always be a 0 based view, and reads should go back to the File?
  // File& source;
public:
  InputStream() = default;

  InputStream(const char *start, const char *end) : pos(start), start(start), end(end) {
    assert(end>=start);
  }

  /// get a pointer to the current position
  const char *ptr() const noexcept { return pos; }

  /// get a pointer at the given offset (from the start of the file)
  const char *ptr(int64_t offset) const {
    const char* p = start + offset;
    assert(offset >= 0 && p <= end);  // it's OK for p==end since positioning at end is fine, just not reading.
    return p;
  }

  int64_t offset() const noexcept { return pos - start; }

  int64_t left() const noexcept { return end - pos; }  // how much data is left to read

  int64_t size() const noexcept { return end - start; }

  void seek(int64_t offset) {
    pos = start + offset;
    assert(start <= pos && pos <= end);
  }

  void relativeSeek(int64_t offset) {
    pos += offset;
    assert(start <= pos && pos <= end);
  }

  void skip(int64_t len) {
    pos += len;
    assert(start <= pos && pos <= end && len >= 0);
  }

  char readByte() {
    assert(pos < end);
    return *pos++;
  }

  void read(void *dest, int32_t len) {
    memcpy(dest, pos, len);
    pos += len;
    assert(pos <= end);
  }


  inline static uint32_t readVint(const char *&pos, const char *end) {
    char b = *pos++;
    uint32_t val = b & 0x7f;
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = *pos++;
      val |= uint32_t(b & 0x7f) << shift;
    }
    assert(pos <= end);
    return val;
  }

  inline static uint64_t readVlong(const char *&pos, const char *end) {
    char b = *pos++;
    uint64_t val = b & 0x7f;
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = *pos++;
      val |= uint64_t(b & 0x7f) << shift;
    }
    assert(pos <= end);
    return val;
  }

  inline static uint32_t readStrLen(const char *&pos, const char *end) {
    return readVint(pos, end);
  }

  // read signed 4 byte little endian integer
  int32_t readInt() {
    assert(pos + sizeof(uint32_t) <= end);
    uint32_t val = loadUnaligned<uint32_t>(pos);
    pos += sizeof(uint32_t);
    return val;
  }

  // read signed 8 byte little endian integer
  int64_t readLong() {
    assert(pos + sizeof(uint64_t) <= end);
    int64_t val = loadUnaligned<int64_t>(pos);
    pos += sizeof(int64_t);
    return val;
  }

  uint32_t readVint() {
    char b = readByte();
    uint32_t val = b & 0x7f;
    // TODO: try replacing with a loop to 4 (to avoid running long if data is bad)
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = readByte();
      val |= uint32_t(b & 0x7f) << shift;
    }
    return val;
  }

  uint64_t readVlong() {
    char b = readByte();
    uint64_t val = b & 0x7f;
    // TODO: try a loop to 8 (to avoid running long if data is bad)
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = readByte();
      val |= uint64_t(b & 0x7f) << shift;
    }
    return val;
  }

  // Read a zig-zag encoded signed long.
  int64_t readZlong() {
    return zigzagDecode(readVlong());
  }

  uint32_t readStrLen() {
    return readVint();
  }

  std::string_view readStr() {
    size_t strLen = readStrLen();
    auto strStart = pos;
    skip(strLen);
    return {strStart, strLen};
  }

  void skipStr() {
    auto len = readStrLen();
    pos += len;
    assert(pos <= end);
  }

  // The returned PackedTerm points into this InputStream.
  // NOTE: if we create other implementations that read chunk-at-a-time, then this PackedTerm
  // could either be split or later invalidated by more reads on the InputStream.  This won't
  // happen for memory-mapped files or for RAMDir.
  PackedTerm readPackedTerm() {
    PackedTerm term(const_cast<char *>(pos));
    pos += term.memorySize();
    assert(pos <= end);
    return term;
  }

  PackedTerm readPackedTerm(int64_t location) {
    return PackedTerm(const_cast<char *>(start + location));
  }

  template<class T> T readVal() {
    return T::read(*this);
  }

  friend std::ostream &operator<<(std::ostream &out, const InputStream &is) {
    // TODO: print out some of the bytes before and after the current position?
    out << "{sz=" << (is.end - is.start) << " left=" << is.left()
        << " start=" << (void *) is.start << " pos=" << (void *) is.pos << " end=" << (void *) is.end
        << '}';
    return out;
  }


};

} // end namespace
