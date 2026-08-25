#pragma once

#include <stdexcept>

#include "InputStream.h"

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

  seg_location() noexcept : x(0) {}

  seg_location(uint32_t fnum, uint64_t offset) noexcept {
    x = offset + ((uint64_t)fnum << OFFSET_BITS);
  }

  uint32_t filenum() const noexcept { return x >> OFFSET_BITS; }
  uint64_t offset() const noexcept { return x & OFFSET_MASK; }
  uint64_t raw() const noexcept { return x; }
  bool isNull() const noexcept { return x == 0; }

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
};

class RAMFile;

// A filesystem output failure may leave the current append-only value partially
// written. Indexing code distinguishes this from a document error
// and aborts the in-progress segment instead of reusing its streams.
class FileIOException : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class File {
  friend class OutputStream;

protected:
  std::string name_;

  virtual void flush(OutputStream &os, bool deferNewBuff) = 0;

  virtual void close(OutputStream &os) = 0;

public:
  explicit File(const std::string_view &name) : name_(name) {}

  const std::string& name() { return name_; }

  virtual size_t size() = 0;

  /// Appends the input RAMFile by stealing its buffers. OutputStream::appendFile
  /// is the normal entry point because it also repairs the attached stream.
  virtual void destructiveAppend(RAMFile &in) = 0;

  virtual ~File() = default;
};


class OutputStream {
  friend class File;

  friend class RAMFile;
  friend class FSFile;
  friend class FSFile;

  // the associated File object controls the lifetime of the buffer
  char *pos = nullptr;
  char *start = nullptr;
  char *end = nullptr;
  size_t flushedSize = 0; // number of bytes that have been flushed to the source
private:
  File *target;
public:
  // just used by postings writer to know what field number this stream is associated with.
  uint16_t streamNumber;

  seg_location slocation() const {
    return seg_location(streamNumber, size());
  }

public:
  // By not requiring the File target up-front, we can directly include OutputStream instances in other
  // classes even if file creation is deferred.
  explicit OutputStream(File* target = nullptr) : target(target) {}

  size_t buffered() const noexcept { return start == nullptr ? 0 : (size_t)(pos - start); }

  bool hasFlushedBytes() const noexcept { return flushedSize != 0; }

  // A stream is relocatable while all of its bytes are still owned by its
  // current output buffer.  Keep this predicate at the stream boundary so a
  // RAM-delegating File can broaden it later without changing segment finalize.
  bool isRelocatable() const noexcept { return !hasFlushedBytes(); }

  size_t reserved() const noexcept {
    return pos == nullptr ? 0 : (size_t)(end - pos);
  }
  uint32_t reserve(uint32_t needed) {
    if (reserved() < needed) {
      flush();  // TODO: pass down needed amount?
    }
    assert(reserved() >= needed);
    return reserved();
  }

  char *ptr() const noexcept { return pos; } // the current position in the buffer
  size_t size() const noexcept { return flushedSize + buffered(); }

  File *getFile() const noexcept{ return target; }

  // don't call flush before close... it would needlessly create a new memory buffer
  void flush(bool deferNewBuff = false) { target->flush(*this, deferNewBuff); }

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

  // Append a relocatable stream as one affine region.  The source starts at
  // offset zero, so aligning its new base preserves every within-file
  // alignment.  Returns false if the source has spilled or the combined file
  // would exceed seg_location's 44-bit offset space.
  bool tryAppendRelocatable(OutputStream& source, size_t alignment,
                            uint64_t& baseOffset) {
    assert(this != &source);
    assert(alignment > 0);
    if (!source.isRelocatable()) return false;

    uint64_t current = (uint64_t) size();
    uint64_t sourceBytes = (uint64_t) source.buffered();
    uint64_t padding = (alignment - current % alignment) % alignment;
    if (current > seg_location::maxOffset()
        || padding > seg_location::maxOffset() - current
        || sourceBytes > seg_location::maxOffset() - current - padding) {
      return false;
    }

    align(alignment);
    baseOffset = (uint64_t) size();
    write(source.start, source.buffered());
    source.pos = source.start;
    return true;
  }

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
    while (val > 0x7f) {
      write((char) (val | 0x80));
      val >>= 7;
    }
    write((char) val);
  }

  // Write a maximum of 9 bytes in vint format.  Note that this is inefficient for negative numbers.
  void writeVlong(uint64_t val) {
    // TODO: optimize this
    while (val > 0x7f) {
      write((char) (val | 0x80));
      val >>= 7;
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


// TODO: make a RAMDelegatingFile that starts out in RAM and after a certain size spills to (and delegates to) another
// type of File.
class RAMFile : public File {
  friend class OutputStream;

  friend class RAMInputFile;
  friend class FSFile;

  using element_type = std::pair<std::unique_ptr<char[]>, size_t>;

  std::vector<element_type> buffers;
  size_t fileSize = 0;

  void newBuffer(size_t size) {
    buffers.emplace_back(new char[size], size);
  }

  void flush(OutputStream &os, bool deferNewBuff) override {
    auto thisBufferSize = os.start == nullptr ? 0 : (size_t)(os.pos - os.start);
    fileSize += thisBufferSize;
    os.flushedSize = fileSize;

    if (!buffers.empty() && os.start == buffers.back().first.get()) {
      buffers.back().second = thisBufferSize;  // truncate to actually used space
    }

    if (!deferNewBuff) {
      // doubling strategy up to 1MiB
      auto prevBufferSize = buffers.empty() ? START_BUFFER_SIZE / 2 : (uint32_t)buffers.back().second;
      auto bufSize = std::max(START_BUFFER_SIZE, std::min(prevBufferSize << 1, 0x100000u));
      newBuffer(bufSize);
      auto&[ptr, sz] = buffers.back();
      os.start = ptr.get();
      os.pos = os.start;
      os.end = os.start + sz;
    } else {
      os.start = os.pos = os.end = nullptr;
    }
  }

  // TODO: this is currently redundant with Directory.finish
  void close(OutputStream &os) override {
    flush(os, true);
  }

public:
  constexpr static uint32_t START_BUFFER_SIZE = 1024;  // size of first allocated buffer (subsequent buffers may be bigger)... mostly for testing.

  RAMFile(std::string_view name) : File(name) {
  }

  ~RAMFile() override = default;

  /// only valid after flush or close
  size_t size() override {
    return fileSize;
  }

  /// copies size() bytes to the destination
  size_t copyTo(void *dest) {
    char *ptr = (char *) dest;
    for (const auto&[data, sz] : buffers) {
      assert(ptr - (char *) dest <= fileSize);
      memcpy(ptr, data.get(), sz);
      ptr += sz;
    }
    assert(ptr - (char *) dest == fileSize);
    return ptr - (char *) dest;
  }

  /// Frees up memory early (optional)
  /// This invalidates further use of the file or associated output stream.
  void clear() {
    buffers.clear();
  }

  /// appends the input RAMFile by stealing its buffers.
  /// TODO: if further writes will happen to this file, we could pass the OutputStreams as well and
  /// set our OutputStream to the other file's OutputStream (or somehow avoid extra flushes and allocations).
  /// NOTE: Caller must update any attached OutputStream's size tracking after this operation.
  void destructiveAppend(RAMFile &in) override {
    if (this == &in) return; // no-op
    auto otherSize = in.size();
    for (auto& pair : in.buffers) {
      buffers.emplace_back(std::move(pair));
    }
    fileSize += otherSize;
    in.fileSize = 0;
    in.buffers.clear();
  }
};

inline void OutputStream::appendFile(RAMFile& file) {
  flush(true);
  target->destructiveAppend(file);
  flushedSize = target->size();
  target->flush(*this, false);
}

class InputFile {
public:
  virtual ~InputFile() = default;

  virtual size_t size() = 0;

  // Reads all of the file and returns a pointer to the data, which should be valid as long as the InputFile is valid.
  virtual std::string_view read() = 0;

  virtual InputStream getInputStream() = 0;

  friend std::ostream &operator<<(std::ostream &out, InputFile &inf) {
    out << "InputFile: at" << &inf << " stream=" << inf.getInputStream();
    return out;
  }

};

class RAMInputFile : public InputFile {
  std::unique_ptr<char[]> data;
  size_t sz;
public:
  RAMInputFile(std::unique_ptr<char[]> fileData, size_t size) : data(std::move(fileData)), sz(size) {}

  size_t size() override {
    return sz;
  }

  std::string_view read() override {
    return std::string_view(data.get(), sz);
  }

  InputStream getInputStream() override {
    return InputStream(data.get(), data.get() + sz);
  }

};


} // end namespace
