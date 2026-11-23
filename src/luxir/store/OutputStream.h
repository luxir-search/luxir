// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory_resource>

#include "StorageMemory.h"

#include <stdexcept>
#include <xxhash.h>

#include "HeldRange.h"

namespace luxir {

class OutputStream;

// A segment-global position (consists of a file number and a file offset)
// It's made into its own type to enhance type safety so it won't accidentally
// mix with a plain offset/location.
class seg_location {
  uint64_t x;
  static constexpr uint8_t FILENUM_BITS = 20;
  static constexpr uint8_t OFFSET_BITS = sizeof(uint64_t)*8 - FILENUM_BITS;
  static constexpr uint64_t OFFSET_MASK = (~uint64_t(0)) >> FILENUM_BITS;

public:
  static constexpr uint64_t maxOffset() noexcept { return OFFSET_MASK; }
  static constexpr uint32_t maxFilenum() noexcept { return (1u << FILENUM_BITS) - 1; }

  seg_location() noexcept : x(0) {}

  seg_location(uint32_t fnum, uint64_t offset) noexcept {
    x = offset + ((uint64_t)fnum << OFFSET_BITS);
  }

  uint32_t filenum() const noexcept { return x >> OFFSET_BITS; }
  uint64_t offset() const noexcept { return x & OFFSET_MASK; }
  uint64_t raw() const noexcept { return x; }
  bool isNull() const noexcept { return x == 0; }
  bool operator==(const seg_location&) const = default;

  seg_location addOffset(uint64_t bytes) const {
    if (bytes > maxOffset() - offset()) {
      throw std::length_error("segment offset exceeds seg_location");
    }
    return {filenum(), offset() + bytes};
  }

  static seg_location fromRaw(uint64_t raw) noexcept {
    seg_location location;
    location.x = raw;
    return location;
  }

  // return filenum, offset pair
  std::pair< uint32_t, uint64_t> decode() const noexcept {
    return {x >> OFFSET_BITS, x & OFFSET_MASK};
  }

  void write(OutputStream& os) const; // defined after OutputStream

  static seg_location read(InputStream& is) {
    uint32_t fnum = is.readVint();
    uint64_t off = is.readVlong();
    return {fnum, off};
  }

  static seg_location checkedRead(InputStream& is) {
    uint32_t fnum = is.checkedVint();
    uint64_t off = is.checkedVlong();
    if (fnum > maxFilenum() || off > maxOffset()) throw std::runtime_error("index data has an invalid segment location");
    return {fnum, off};
  }
};

class RAMFile;

struct FileDescriptor {
  std::string name;
  uint64_t size = 0;
  uint64_t xxh3 = 0;
  bool operator==(const FileDescriptor&) const = default;
};

class File {
  friend class OutputStream;

protected:
  std::string name_;

  virtual void flush(OutputStream& os, bool deferNewBuff, size_t minCapacity) = 0;

  virtual void close(OutputStream &os) = 0;

  virtual void appendFile(OutputStream& out, RAMFile& file);

  // Lend the unused part of dest's buffer to source. Source keeps its own
  // byte count. Return the buffer before moving dest's cursor or refilling it.
  static void borrowBuffer(OutputStream& source, OutputStream& dest, size_t appended = 0) noexcept;
  static void returnBuffer(OutputStream& source, OutputStream& dest) noexcept;

public:
  explicit File(const std::string_view &name) : name_(name) {}

  const std::string& name() { return name_; }

  virtual size_t size() = 0;
  // Valid after close and Directory::finishFile.
  virtual uint64_t digest() const = 0;
  FileDescriptor descriptor() { return {name_, size(), digest()}; }

  /// Appends the input RAMFile by stealing its buffers. OutputStream::appendFile
  /// is the normal entry point because it also repairs the attached stream.
  virtual void destructiveAppend(RAMFile &in) = 0;

  virtual ~File() = default;
};


class OutputStream {
  friend class File;

  friend class RAMFile;
  friend class SizedRAMFile;
  friend class FSFile;

  // the associated File object controls the lifetime of the buffer
  char *pos = nullptr;
  char *start = nullptr;
  char *end = nullptr;
  size_t flushedSize = 0; // number of bytes that have been flushed to the source
  File* target;

public:
  // By not requiring the File target up-front, we can directly include OutputStream instances in other
  // classes even if file creation is deferred.
  explicit OutputStream(File* target = nullptr) : target(target) {}

  size_t buffered() const noexcept { return start == nullptr ? 0 : (size_t)(pos - start); }

  size_t reserved() const noexcept {
    return pos == nullptr ? 0 : (size_t)(end - pos);
  }
  uint32_t reserve(uint32_t needed) {
    if (reserved() < needed) {
      target->flush(*this, false, needed);
      if (reserved() < needed) throw FileIOException("output reservation exceeds stream buffer capacity");
    }
    return (uint32_t)std::min(reserved(), (size_t)UINT32_MAX);
  }

  char *ptr() const noexcept { return pos; } // the current position in the buffer
  size_t size() const noexcept { return flushedSize + buffered(); }

  File *getFile() const noexcept{ return target; }

  // don't call flush before close... it would needlessly create a new memory buffer
  void flush(bool deferNewBuff = false) { target->flush(*this, deferNewBuff, 0); }

  void close() {
    target->close(*this);
    target = nullptr;
  }

  void setFile(File *fileTarget) noexcept {
    assert(target == nullptr);
    target = fileTarget;
  }

  // Flushes both sides, destructively appends file, updates size accounting,
  // and leaves this stream ready for continued writes.
  void appendFile(RAMFile& file);

  // pretend that numBytes have been written and move ptr() that many bytes forward in the buffer.
  // requires reserved() >= numBytes as a prerequisite.
  void advance(size_t numBytes) {
    assert(reserved() >= numBytes);
    pos += numBytes;
  }

  /// align the output to the given alignment (based on the total size of the file, not the current buffer)
  /// we use a non-zero fill value to better catch bugs (which zeroes can obscure)
  void align(size_t alignment, uint8_t fill = 0x11) {
    while((size() % alignment) != 0) {
      write(fill);
    }
  }

  void write(char b) {
    if (pos == end) [[unlikely]] {
      flush();
    }
    *pos = b;
    ++pos;
  }

  // Write without bounds checking.  Assumes reserved() >= 1
  void unsafeWrite(char b) {
    assert(pos < end);
    *pos = b;
    ++pos;
  }

  // Write without bounds checking.  Assumes len <= reserved().
  void unsafeWrite(const void *data, size_t len) {
    assert(len <= reserved());
    if (len != 0) {
      // ubsan doesn't like it when we pass memcpy(nullptr,...,0)
      memcpy(pos, data, len);
    }
    pos += len;
  }

  void write(const void *data, size_t len) {
    // TODO: optimize this for the case that File can handle non-full buffers or doesn't keep a copy of the buffer (i.e. we can avoid a copy)
    char *in = (char *) data;
    while (len > 0) {
      size_t toWrite = std::min(len, reserved());
      unsafeWrite(in, toWrite);
      in += toWrite;
      len -= toWrite;
      if (len > 0) {
        flush();
      }
    }
  }

  // Write 4 byte integer in little endian format
  void writeInt(int32_t val) {
    write((void*)&val, sizeof(val));
  }

  // Write 8 byte long integer in little endian format
  void writeLong(int64_t val) {
    write((void*)&val, sizeof(val));
  }

  // Write a maximum of 5 bytes in vint format.  Note that this is inefficient for negative numbers.
  void writeVint(uint32_t val) {
    // In the case that we don't have to write full buffers before flushing, we should simply reserve
    // and then bump the pointer.
    // This should be a template property of File that is propagated here? Or maybe File implementations
    // that can't accept partial writes should just do their own buffering when they get an incomplete write?
    // TODO: optimize this.

    // Make sure val is unsigned here since high bit may be set.
    while (val > VARINT_PAYLOAD_MASK) {
      write((char) (val | VARINT_CONTINUATION));
      val >>= VARINT_PAYLOAD_BITS;
    }
    write((char) val);
  }

  // Write a uint64_t as a varint.
  void writeVlong(uint64_t val) {
    // TODO: optimize this
    while (val > VARINT_PAYLOAD_MASK) {
      write((char) (val | VARINT_CONTINUATION));
      val >>= VARINT_PAYLOAD_BITS;
    }
    write((char) val);
  }

  void writePackedTerm(PackedTerm term) {
    write(term.ptr() , term.memorySize());
  }

  // Writes a prefix-length encoded string.
  void writeStr(const char *data, uint32_t len) {
    writeVint(len);
    write((void *) data, len);
  }

  // Writes a prefix-length encoded string.
  void writeStr(std::string_view sv) {
    writeStr(sv.data(), sv.length());
  }

  // This is a convenience method that writes the bytes without the length prefix.
  // It is used for writing byte arrays that are not strings.
  void writeBytes(std::string_view sv) {
    write(sv.data(), sv.length());
  }

  // Implement write method for any type that has .write(OutputStream& os)
  template <class T> void writeVal(const T& val) {
    val.write(*this);
  }
};


inline void seg_location::write(OutputStream& os) const {
  os.writeVint(filenum());
  os.writeVlong(offset());
}


// Chunked in-memory output used by RAMDir and temporary writers.
class RAMFile : public File {
  friend class OutputStream;

  friend class RAMInputFile;
  friend class FSFile;

  struct BufferDeleter {
    std::pmr::memory_resource* resource;
    size_t size;
    void operator()(char* ptr) const { resource->deallocate(ptr, size); }
  };
  using BufferPtr = std::unique_ptr<char[], BufferDeleter>;

  struct Buffer {
    StorageCharge charge;
    BufferPtr data;
    size_t used;
    size_t capacity;
  };

  std::shared_ptr<StorageMemory> memory;
  std::pmr::memory_resource* upstream;
  std::vector<Buffer> buffers;
  size_t fileSize = 0;
  size_t allocatedSize = 0;

  void newBuffer(size_t size) {
    StorageCharge charge(memory, size);
    BufferPtr data((char*)upstream->allocate(size), BufferDeleter{upstream, size});
    buffers.push_back({std::move(charge), std::move(data), 0, size});
    allocatedSize += size;
  }

protected:
  size_t nextBufferCapacity(const OutputStream& os, size_t minCapacity) const {
    // Double up to 1 MiB unless a reservation needs a larger buffer.
    size_t previousSize = os.start ? os.buffered()
        : buffers.empty() ? START_BUFFER_SIZE / 2 : buffers.back().used;
    return std::max({minCapacity, (size_t)START_BUFFER_SIZE, std::min(previousSize * 2, size_t{1} << 20)});
  }

  void flushBuffer(OutputStream& os, size_t capacity) {
    auto thisBufferSize = os.start == nullptr ? 0 : (size_t)(os.pos - os.start);
    fileSize += thisBufferSize;
    os.flushedSize = fileSize;

    if (!buffers.empty() && os.start == buffers.back().data.get()) {
      buffers.back().used = thisBufferSize;
    }

    os.start = os.pos = os.end = nullptr;
    if (capacity != 0) {
      newBuffer(capacity);
      auto& buffer = buffers.back();
      os.start = buffer.data.get();
      os.pos = os.start;
      os.end = os.start + buffer.capacity;
    }
  }

  void flush(OutputStream& out, bool deferNewBuff, size_t minCapacity) override {
    flushBuffer(out, deferNewBuff ? 0 : nextBufferCapacity(out, minCapacity));
  }

  // TODO: this is currently redundant with Directory.finish
  void close(OutputStream &os) override {
    flushBuffer(os, 0);
  }

public:
  constexpr static uint32_t START_BUFFER_SIZE = 1024;  // size of first allocated buffer (subsequent buffers may be bigger)... mostly for testing.

  RAMFile(std::string_view name, std::shared_ptr<StorageMemory> memory = {},
          std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
      : File(name), memory(std::move(memory)), upstream(upstream) {
  }

  ~RAMFile() override = default;

  /// only valid after flush or close
  size_t size() override {
    return fileSize;
  }

  uint64_t digest() const override {
    auto* state = XXH3_createState();
    if (!state) throw std::bad_alloc();
    XXH3_64bits_reset(state);
    for (const auto& buffer : buffers) {
      XXH3_64bits_update(state, buffer.data.get(), buffer.used);
    }
    auto result = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return result;
  }

  size_t allocatedBytes() const noexcept { return allocatedSize; }

  /// copies size() bytes to the destination
  size_t copyTo(void *dest) {
    char *ptr = (char *) dest;
    for (const auto& buffer : buffers) {
      assert((size_t)(ptr - (char *) dest) <= fileSize);
      memcpy(ptr, buffer.data.get(), buffer.used);
      ptr += buffer.used;
    }
    assert((size_t)(ptr - (char *) dest) == fileSize);
    return ptr - (char *) dest;
  }

  /// Frees up memory early (optional)
  /// This invalidates further use of the file or associated output stream.
  void clear() {
    buffers.clear();
    fileSize = 0;
    allocatedSize = 0;
  }

  void appendTo(OutputStream& out) const {
    for (const auto& buffer : buffers) {
      out.write(buffer.data.get(), buffer.used);
    }
  }

  /// appends the input RAMFile by stealing its buffers.
  /// TODO: if further writes will happen to this file, we could pass the OutputStreams as well and
  /// set our OutputStream to the other file's OutputStream (or somehow avoid extra flushes and allocations).
  /// NOTE: Caller must update any attached OutputStream's size tracking after this operation.
  void destructiveAppend(RAMFile &in) override {
    if (this == &in) return; // no-op
    auto otherSize = in.size();
    // Charge unaccounted scratch buffers before moving any of them.
    if (memory) for (auto& buffer : in.buffers) {
      if (!buffer.charge.charged()) buffer.charge = StorageCharge(memory, buffer.capacity);
    }
    for (auto& buffer : in.buffers) buffers.emplace_back(std::move(buffer));
    fileSize += otherSize;
    allocatedSize += in.allocatedSize;
    in.fileSize = 0;
    in.allocatedSize = 0;
    in.buffers.clear();
  }
};


class InputFile {
protected:
  static void checkRange(ByteRange range, uint64_t size) {
    if (range.offset > size || range.length > size - range.offset) {
      throw std::out_of_range("InputFile range exceeds file size");
    }
  }

public:
  virtual ~InputFile() = default;

  virtual size_t size() = 0;

  virtual HeldRange acquire(ByteRange range) = 0;

  // Acquires a batch of ranges together, so a backend can merge them and
  // issue them at once. Reports every outcome through `done`, called exactly
  // once, possibly before returning, from any thread: one hold per range in
  // the order requested and a null error, or no holds and the error. Never
  // throws; `done` must not throw either. The holds follow the contract of
  // single acquires. `ranges` is valid only during the call. The default
  // acquires each range in turn and completes inline.
  virtual void acquireBatch(std::span<const ByteRange> ranges, AcquireDone done) noexcept {
    std::vector<HeldRange> holds;
    std::exception_ptr failure;
    try {
      holds.reserve(ranges.size());
      for (const auto& range : ranges) holds.push_back(acquire(range));
    } catch (...) {
      holds.clear();
      failure = std::current_exception();
    }
    done(std::move(holds), std::move(failure));
  }

  // Blocks until the batch completes; returns its holds in the order
  // requested or throws its error.
  std::vector<HeldRange> acquireAll(std::span<const ByteRange> ranges);
  // True when all acquisitions return null-owner views valid for the file's lifetime.
  virtual bool holdsFree() const { return false; }

  // Advisory asynchronous readahead; memory-backed inputs need no work.
  virtual void prefetch(size_t offset, size_t length) {}
};

class RAMInputFile : public InputFile {
  StorageCharge charge;
  std::unique_ptr<char[]> data;
  size_t sz;
public:
  RAMInputFile(std::unique_ptr<char[]> fileData, size_t size, StorageCharge charge = {})
      : charge(std::move(charge)), data(std::move(fileData)), sz(size) {}

  size_t size() override {
    return sz;
  }

  HeldRange acquire(ByteRange range) override {
    checkRange(range, sz);
    return {data.get() + range.offset, range.length};
  }
  bool holdsFree() const override { return true; }
};


class SizedRAMFile : public File {
  StorageCharge charge;
  std::unique_ptr<char[]> data;
  size_t expected, written = 0;
  uint64_t hash = 0;
  bool closed = false;

  void flush(OutputStream& out, bool defer, size_t /*minCapacity*/) override {
    written = out.size();
    out.flushedSize = written;
    out.start = out.pos = out.end = nullptr;
    if (!defer) {
      if (written == expected) throw FileIOException("RAM output exceeds expected size");
      out.start = out.pos = data.get() + written;
      out.end = data.get() + expected;
    }
  }
  void close(OutputStream& out) override {
    flush(out, true, 0);
    if (written != expected) throw FileIOException("RAM output is shorter than expected size");
    hash = XXH3_64bits(data.get(), written);
    closed = true;
  }
public:
  SizedRAMFile(std::string_view name, size_t size, std::shared_ptr<StorageMemory> memory)
      : File(name), charge(std::move(memory), size), data(std::make_unique_for_overwrite<char[]>(size)), expected(size) {}
  size_t size() override { return written; }
  uint64_t digest() const override { return hash; }
  void destructiveAppend(RAMFile&) override { throw FileIOException("appendFile requires chunked RAM output"); }
  std::shared_ptr<RAMInputFile> finish() {
    if (!closed || !data) throw FileIOException("RAM output must be closed exactly once before finish");
    return std::make_shared<RAMInputFile>(std::move(data), written, std::move(charge));
  }
};


} // end namespace
