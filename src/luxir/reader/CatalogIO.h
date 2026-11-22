// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <format>
#include <optional>

#include "FieldParts.h"
#include "ReaderErrors.h"
#include "luxir/store/IndexFormat.h"

namespace luxir::catalog_io {

static_assert(std::endian::native == std::endian::little);

using field_parts::FileSizes;

// Reports a format violation as corrupt input or invalid writer output.
class Errors {
  std::string_view prefix;  // The format being processed.
  SegmentReadContext context;  // Origin of input bytes, if there is one.
  bool writing;  // Writer misuse is a logic error.

public:
  Errors(std::string_view prefix, SegmentReadContext context, bool writing = false)
      : prefix(prefix), context(context), writing(writing) {}

  [[noreturn]] void invalid(std::string_view reason) const {
    if (writing) throw std::logic_error(std::format("{}: {}", prefix, reason));
    context.invalid(reason, prefix);
  }
};

// Catalog names and dictionary endpoints use PackedTerm's length byte followed by the bytes.
inline PackedTerm packedTerm(const char* lengthByte) noexcept {
  return PackedTerm(const_cast<char*>(lengthByte));
}

// Reads bounded catalog or field record bytes.
class Reader : public Errors {
  InputStream input;  // Unread bytes in the input span.

public:
  Reader(std::span<const char> bytes, const Errors& errors)
      : Errors(errors), input(bytes.data(), bytes.data() + bytes.size()) {}

  std::span<const char> remaining() const { return {input.ptr(), (size_t)input.left()}; }

  std::span<const char> take(uint64_t size) {
    if (size > (uint64_t)input.left()) invalid("truncated input");
    auto bytes = remaining().first((size_t)size);
    input.skip(size);
    return bytes;
  }

  uint8_t readByte() { return (uint8_t)take(1).front(); }

  uint64_t readVarUInt(uint64_t limit = UINT64_MAX) {
    auto value = input.readCanonicalVlong(limit);
    if (!value) invalid(value.error());
    return *value;
  }

  uint8_t readOptions(uint8_t allowed) {
    auto value = readByte();
    if (value & ~allowed) invalid("unknown mode bits");
    return value;
  }

  std::string_view readString(uint64_t size) {
    auto bytes = take(size);
    return {bytes.data(), bytes.size()};
  }

  PackedTerm readPackedTerm(const char* reason) {
    if (remaining().empty()) invalid(reason);
    auto term = packedTerm(input.ptr());
    if (term.memorySize() > remaining().size()) invalid(reason);
    take(term.memorySize());
    return term;
  }

  int32_t readLittleEndian32() { return loadUnaligned<int32_t>(take(sizeof(int32_t)).data()); }
  void requireEnd() const { if (input.left()) invalid("trailing bytes"); }
};

// Writes catalog or field record bytes.
class Writer : public Errors {
  OutputStream* output;  // Destination for the encoded bytes.

public:
  Writer(OutputStream* output, const char* errorPrefix)
      : Errors(errorPrefix, SegmentReadContext(), true), output(output) {}

  void writeBytes(std::string_view bytes) {
    if (!bytes.empty()) output->writeBytes(bytes);
  }

  void writeByte(uint8_t value) {
    output->write((char)value);
  }

  void writePackedTerm(PackedTerm term) {
    writeBytes({term.data() - 1, term.memorySize()});
  }

  void writeVarUInt(uint64_t value) {
    char bytes[MAX_VLONG_SIZE];
    writeBytes({bytes, (size_t)(writeVLong(bytes, value) - bytes)});
  }

  template <class T>
  void writeLittleEndian(T value) {
    writeBytes({reinterpret_cast<const char*>(&value), sizeof(value)});
  }

};

inline void writeLocation(Writer& out, seg_location location) {
  out.writeVarUInt(location.filenum());
  out.writeVarUInt(location.offset());
}

inline seg_location readLocation(Reader& in) {
  auto file = (uint32_t)in.readVarUInt(seg_location::maxFilenum());
  auto offset = in.readVarUInt(seg_location::maxOffset());
  return {file, offset};
}

inline void writeSlice(Writer& out, field_parts::Slice slice) {
  writeLocation(out, slice.location);
  out.writeVarUInt(slice.bytes);
}

// Bounds a present slice against the format and, when supplied, the file inventory.
inline void checkSlice(const Errors& errors, field_parts::Slice slice, std::optional<FileSizes> files,
                       const char* reason) {
  auto [file, start] = slice.location.decode();
  if (start < IndexFormat::HEADER.size() || slice.bytes > seg_location::maxOffset() - start
      || (files && (file >= files->size() || !(*files)[file] || start > (*files)[file]
                    || slice.bytes > (*files)[file] - start))) errors.invalid(reason);
}

// Reads the location and length. Extent checks are shared with derived slices.
inline field_parts::Slice readSlice(Reader& in) {
  auto location = readLocation(in);
  if (location.isNull()) in.invalid("missing slice location");
  return {location, in.readVarUInt(seg_location::maxOffset() - location.offset())};
}

} // namespace luxir::catalog_io
