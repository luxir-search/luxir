// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <expected>
#include <stdexcept>
#include <utility>
#include "luxir/util/StrRef.h"
#include "luxir/util/encoding.h"

namespace luxir {

class InputStream {
  const char *pos = nullptr;  // these initializations just to suppress maybe-uninitialized warnings with -O3
  const char *start = nullptr;
  const char *end = nullptr;
  [[noreturn]] static void invalidVlong(const char* reason) {
    throw std::runtime_error(std::string("invalid index vint: ") + reason);
  }
  [[noreturn]] static void truncated() {
    throw std::runtime_error("index data is truncated or in an unsupported format");
  }

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

  /// get a pointer at the given offset from the start of the stream
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
    uint32_t val = b & VARINT_PAYLOAD_MASK;
    for (int shift = VARINT_PAYLOAD_BITS; (b & VARINT_CONTINUATION) != 0; shift += VARINT_PAYLOAD_BITS) {
      b = *pos++;
      val |= uint32_t(b & VARINT_PAYLOAD_MASK) << shift;
    }
    assert(pos <= end);
    return val;
  }

  inline static uint64_t readVlong(const char *&pos, const char *end) {
    char b = *pos++;
    uint64_t val = b & VARINT_PAYLOAD_MASK;
    for (int shift = VARINT_PAYLOAD_BITS; (b & VARINT_CONTINUATION) != 0; shift += VARINT_PAYLOAD_BITS) {
      b = *pos++;
      val |= uint64_t(b & VARINT_PAYLOAD_MASK) << shift;
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
    uint32_t val = b & VARINT_PAYLOAD_MASK;
    // TODO: try replacing with a loop to 4 (to avoid running long if data is bad)
    for (int shift = VARINT_PAYLOAD_BITS; (b & VARINT_CONTINUATION) != 0; shift += VARINT_PAYLOAD_BITS) {
      b = readByte();
      val |= uint32_t(b & VARINT_PAYLOAD_MASK) << shift;
    }
    return val;
  }

  uint64_t readVlong() {
    char b = readByte();
    uint64_t val = b & VARINT_PAYLOAD_MASK;
    // TODO: try a loop to 8 (to avoid running long if data is bad)
    for (int shift = VARINT_PAYLOAD_BITS; (b & VARINT_CONTINUATION) != 0; shift += VARINT_PAYLOAD_BITS) {
      b = readByte();
      val |= uint64_t(b & VARINT_PAYLOAD_MASK) << shift;
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

  // The returned PackedTerm borrows the bytes backing this stream. A chunked
  // InputStream could split the term across chunks or invalidate it on a later read.
  PackedTerm readPackedTerm() {
    PackedTerm term(const_cast<char *>(pos));
    pos += term.memorySize();
    assert(pos <= end);
    return term;
  }

  PackedTerm readPackedTerm(int64_t location) const {
    return PackedTerm(const_cast<char *>(start + location));
  }

  template<class T> T readVal() {
    return T::read(*this);
  }

  // Check lengths before reading untrusted metadata. The fast reads above
  // rely on a validated layout and use debug assertions.
  void require(int64_t len) const {
    if (len < 0 || len > left()) truncated();
  }

  int32_t checkedInt() {
    require(sizeof(uint32_t));
    return readInt();
  }

private:
  enum class VintForm { PERMISSIVE, CANONICAL };

  std::expected<uint64_t, const char*> tryVlong(int maxBytes, uint64_t limit, VintForm form) {
    constexpr auto MAX_LAST_BYTE = UINT64_MAX >> ((MAX_VLONG_SIZE - 1) * VARINT_PAYLOAD_BITS);
    uint64_t value = 0;
    int bytes = std::min(maxBytes, MAX_VLONG_SIZE);
    for (int i = 0; i < bytes; ++i) {
      if (pos == end) return std::unexpected("truncated input");
      auto next = (uint8_t)*pos++;
      if (i == MAX_VLONG_SIZE - 1 && next > MAX_LAST_BYTE) return std::unexpected("vint overflow");
      auto payload = next & VARINT_PAYLOAD_MASK;
      value |= (uint64_t)payload << (i * VARINT_PAYLOAD_BITS);
      if (!(next & VARINT_CONTINUATION)) {
        if (value > limit) return std::unexpected("invalid vint: value exceeds limit");
        if (form == VintForm::CANONICAL && i && !payload) return std::unexpected("invalid vint: noncanonical form");
        return value;
      }
    }
    return std::unexpected("overlong vint");
  }

public:
  std::expected<uint64_t, const char*> readCanonicalVlong(uint64_t limit) {
    return tryVlong(MAX_VLONG_SIZE, limit, VintForm::CANONICAL);
  }

  uint64_t checkedVlong(int maxBytes = MAX_VLONG_SIZE) {
    auto value = tryVlong(maxBytes, UINT64_MAX, VintForm::PERMISSIVE);
    if (!value) invalidVlong(value.error());
    return *value;
  }

  std::expected<uint64_t, const char*> tryVint() {
    return tryVlong(MAX_VINT_SIZE, UINT32_MAX, VintForm::PERMISSIVE);
  }

  uint32_t checkedVint() {
    auto value = tryVint();
    if (!value) invalidVlong(value.error());
    return (uint32_t)*value;
  }

  PackedTerm tryPackedTerm() {
    if (left() < 1 || PackedTerm(const_cast<char *>(pos)).memorySize() > left()) return PackedTerm(nullptr);
    return readPackedTerm();
  }

  PackedTerm checkedPackedTerm() {
    auto term = tryPackedTerm();
    if (term.isNull()) truncated();
    return term;
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
