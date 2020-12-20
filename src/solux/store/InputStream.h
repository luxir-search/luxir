#pragma once

#include "solux/util/StrRef.h"


class InputStream {
  const char* pos;
  const char* start;
  const char* end;
  // Future?
  // size_t offset; // the position of "start" in the file
  // File& source;
public:
  InputStream() {}
  InputStream(const char* start, const char* end) : pos(start), start(start), end(end) {}

  const char* ptr() { return pos; }
  uint64_t offset() { return pos - start; }

  void seek(uint64_t offset) {
    pos = start + offset;
    assert(pos <= end);
  }

  void relativeSeek(uint64_t offset) {
    pos += offset;
    assert(start <= pos && pos <= end);
  }

  void skip(uint64_t len) {
    pos += len;
    assert(pos <= end);
  }

  char readByte() {
    assert(pos < end);
    return *pos++;
  }

  void read(void* dest, uint32_t len) {
    memcpy(dest, pos, len);
    pos += len;
    assert(pos<=end);
  }


  inline static uint32_t readVint(const char*& pos, const char* end) {
    char b = *pos++;
    uint32_t val = b & 0x7f;
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = *pos++;
      val |= (b & 0x7f) << shift;
    }
    assert(pos <= end);
    return val;
  }

  inline static uint64_t readVlong(const char*& pos, const char* end) {
    char b = *pos++;
    uint32_t val = b & 0x7f;
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = *pos++;
      val |= (b & 0x7f) << shift;
    }
    assert(pos <= end);
    return val;
  }

  inline static uint32_t readStrLen(const char*& pos, const char* end) {
    return readVint(pos, end);
  }

  uint32_t readVint() {
    char b = readByte();
    uint32_t val = b & 0x7f;
    // TODO: try replacing with a loop to 4 (to avoid running long if data is bad)
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = readByte();
      val |= (b & 0x7f) << shift;
    }
    return val;
  }

  uint64_t readVlong() {
    char b = readByte();
    uint64_t val = b & 0x7f;
    // TODO: try a loop to 8 (to avoid running long if data is bad)
    for (int shift = 7; (b & 0x80) != 0; shift += 7) {
      b = readByte();
      val |= (b & 0x7f) << shift;
    }
    return val;
  }

  uint32_t readStrLen() {
    return readVint();
  }

  void skipStr() {
    auto len = readStrLen();
    pos += len;
    assert(pos <= end);
  }

  // The returned PackedTerm points into this InputStream
  PackedTerm readPackedTerm() {
    PackedTerm term(const_cast<char*>(pos));
    pos += term.memorySize();
    assert(pos <= end);
    return term;
  }



};

