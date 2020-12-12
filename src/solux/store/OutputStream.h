#include <utility>

#pragma once


class File;

class OutputStream;

class File {
protected:
  std::string name_;
public:
  explicit File(const std::string& name) : name_(name) {}
  const std::string& name() { return name_; }
  virtual void flush(OutputStream& os, bool last)=0;
  virtual void close(OutputStream& os)=0;
  virtual size_t size() = 0;

  virtual ~File() = default;

  // virtual std::string_view read()=0;

  friend class OutputStream;
};


class OutputStream {
  friend class File;
  friend class RAMFile;

  // the associated File object controls the lifetime of the Buffer object
  char* pos = nullptr;
  char* start = nullptr;
  char* end = nullptr;
  size_t flushedSize = 0; // number of bytes that have been flushed to the source
  File* target = nullptr;

  void clear() {
    pos = start = end = nullptr;
    flushedSize = 0;
  }

public:
  // By not requiring the File target up-front, we can directly include OutputStream instances in other
  // classes even if file creation is deferred.
  explicit OutputStream() {}

  // An initial buffer to use.  It's lifetime should exceed the lifetime of this OutputStream and associated File.
  explicit OutputStream(char* beginInitialBuffer, char* endInitialBuffer) {
    start = pos = beginInitialBuffer;
    end = endInitialBuffer;
  }

  size_t buffered() const noexcept { return pos - start; }
  size_t reserved() const noexcept { return end - pos; }  // the amount of space left in the buffer
  char* ptr() const noexcept { return pos; } // the current position in the buffer
  size_t size() const noexcept { return flushedSize + buffered(); }
  File* getFile() const noexcept { return target; }
  void flush(bool last=false) { target->flush(*this, last); }
  void close() {
    target->close(*this);
    target = nullptr;
  }

  void setFile(File* fileTarget) noexcept {
    assert(target == nullptr);
    target = fileTarget;
  }

  // pretend that numBytes have been written and move ptr() that many bytes forward in the buffer.
  // requires reserved() >= numBytes as a prerequisite.
  void advance(size_t numBytes) {
    assert(reserved() >= numBytes);
    pos += numBytes;
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
  void unsafeWrite(const void* data, size_t len) {
    assert(len <= reserved());
    memcpy(pos, data, len);
    pos += len;
  }

  void write(const void* data, size_t len) {
    // TODO: optimize this for the case that File can handle non-full buffers or doesn't keep a copy of the buffer (i.e. we can avoid a copy)
    char* in = (char*)data;
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

  // Write a maximum of 5 bytes in vint format.  Note that this is inefficient for negative numbers.
  void writeVint(uint32_t val) {
    // In the case that we don't have to write full buffers before flushing, we should simply reserve
    // and then bump the pointer.
    // This should be a template property of File that is propagated here? Or maybe File implementations
    // that can't accept partial writes should just do their own buffering when they get an incomplete write?
    // TODO: optimize this.

    // Make sure val is unsigned here since high bit may be set.
    while (val > 0x7f) {
      write((char)(val | 0x80));
      val >>= 7;
    }
    write((char)val);
  }

  // Write a maximum of 9 bytes in vint format.  Note that this is inefficient for negative numbers.
  void writeVlong(uint64_t val) {
    // TODO: optimize this
    while (val > 0x7f) {
      write((char)(val | 0x80));
      val >>= 7;
    }
    write((char)val);
  }

  void writeStr(const char* data, uint32_t len) {
    writeVint(len);
    write((void*)data, len);
  }

};


// TODO: make a RAMDelegatingFile that starts out in RAM and after a certain size spills to (and delegates to) another
// type of File.
class RAMFile : public File {
  friend class OutputStream;
  friend class RAMInputFile;

  using element_type = std::pair<std::unique_ptr<char[]>, size_t>;

  std::vector<element_type> buffers;
  size_t fileSize = 0;
  const char* firstBuffer = nullptr;
  uint32_t firstLen = 0;

  void newBuffer(size_t size) {
    // buffers.emplace_back( std::make_pair(std::unique_ptr<char[]>( new char[size]), size) );
    // buffers.emplace_back( std::unique_ptr<char[]>( new char[size]), size );
    buffers.emplace_back(  new char[size], size );
  }

public:
  constexpr static uint32_t START_BUFFER_SIZE = 1024;  // size of first allocated buffer (subsequent buffers may be bigger)... mostly for testing.

  RAMFile(const std::string& name) : File(name) {
  }

  // only valid after flush or close
  size_t size() override {
    return fileSize;
  }

  // copies size() bytes to the destination
  size_t copyTo(void* dest) {
    char* ptr = (char*)dest;
    memcpy(ptr, firstBuffer, firstLen);
    ptr += firstLen;
    for (const auto&[data, sz] : buffers) {
      memcpy(ptr, data.get(), sz);
      ptr += sz;
    }
    return ptr - (char*)dest;
  }

  void flush(OutputStream &os, bool last) override {
    auto thisBufferSize = os.pos - os.start;
    fileSize += thisBufferSize;
    os.flushedSize = fileSize;
    auto prevBufferSize = START_BUFFER_SIZE/2;  // set up for first buffer to be 1024
    if (buffers.size() == 0) {
      // If this is the first call to flush, remember whatever buffer is set by the output stream as the first element.
      firstBuffer = os.start;
      firstLen = thisBufferSize;
    } else {
      if (thisBufferSize == 0) return; // protection against multiple calls to flush
      prevBufferSize = buffers.back().second;
      buffers.back().second = thisBufferSize;  // truncate to actually used space
    }

    if (last) {
      os.clear();
    } else {
      // doubling strategy up to 1MB
      newBuffer(std::max(prevBufferSize << 1, 0x100000u));
      os.start = buffers.back().first.get();
      os.pos = os.start;
      os.end = os.start + buffers.back().second;
    }
  }

  // TODO: this is currently redundant with Directory.finish
  void close(OutputStream &os) override {
    if (os.pos > os.start) {
      flush(os, true);
    }
  }

public:

  ~RAMFile() override = default;
};

class InputFile {
public:
  virtual size_t size() = 0;

  // Reads all of the file and returns a pointer to the data, which should be valid as long as the InputFile is valid.
  virtual std::string_view read() = 0;
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
};


class InputStream;


class InputStream {
  char* pos;
  char* start;
  char* end;
  size_t offset; // the position of "start" in the file
  File& source;
public:
  // TODO: implement a clone type functionality that lucene has that can share the underlying buffer? (operator= or copy constructor)
  // This could be the mechanism to avoid a virtual call for positioning at a different spot in a memory mapped file?
};



// TODO: output and input should perhaps be different classes?
// hence this should probably be RAMOutputFile
// What connects them though... just filename?

