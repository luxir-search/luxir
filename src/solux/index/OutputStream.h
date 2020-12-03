#pragma once


class File;

class OutputStream;

class File {

  friend class OutputStream;

  std::string name;
  virtual void flush(OutputStream& os)=0;
  virtual void close(OutputStream& os)=0;
public:
  virtual ~File() = default;

  friend class OutputStream;
};

//
// Maybe templatize this...
// We need the ability to get a new buffer somehow, and flush a buffer (potentially changing buffers)
//
class OutputStream {
  friend class File;
  friend class RAMFile;

  // the associated File object controls the lifetime of the Buffer object
  char* pos = nullptr;
  char* start = nullptr;
  char* end = nullptr;
  size_t flushed_size = 0; // number of bytes that have been flushed to the source
  File& target;

public:
  OutputStream(File& file) : target(file) {}
  size_t buffered() { return pos - start; }
  size_t reserved() { return end - pos; }  // the amount of space left in the buffer
  char* ptr() { return pos; } // the current position in the buffer
  size_t size() { return flushed_size + buffered(); }
  void flush() { target.flush(*this); }
  void close() { target.close(*this); }

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
  void unsafeWrite(void* data, size_t len) {
    assert(len <= reserved());
    memcpy(pos, data, len);
    pos += len;
  }

  void write(void* data, size_t len) {
    // TODO: optimize this for the case that File can handle non-full buffers.
    while (len > 0) {
      size_t toWrite = std::min(len, reserved());
      unsafeWrite(data, toWrite);
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

};

class RAMFile : public File {
  // TODO: for small files, start off with smaller buffer sizes and combine them into the largest buffer size
  // for example, 1K, 1K, 2K, 4K, 8K, 16K, 32K, 64K (all previous buffers can be combined into a single 64K buffer)
  // Growing to larger buffers is important for avoiding memory fragmentation (i.e. get past the threshold where mmap is used)
  // TODO: extract a buffer class that does this... it will be useful elsewhere.
  constexpr static uint32_t BUFFER_SIZE = 8192;
  friend class OutputStream;

  std::vector< std::unique_ptr<char[]> > buffers;
  size_t fileSize;
  uint32_t blockSize;

  void newBuffer(size_t size) {
    // buffers.emplace_back( std::make_unique<char[]>(size) );  // make_unique zeroes memory, which we don't need
    buffers.emplace_back( std::unique_ptr<char[]>( new char[size]) );
  }

  virtual void flush(OutputStream &os) override {
    fileSize += os.end - os.start;
    os.flushed_size = fileSize;
    os.start = os.pos;
    if (os.pos >= os.end) {
      assert(os.pos == os.end);
      newBuffer(BUFFER_SIZE);
      os.start = buffers.back().get();
      os.pos = os.start;
      os.end = os.start + BUFFER_SIZE;
    }
  }

  void trim() {
    auto lastSize = fileSize % BUFFER_SIZE;
    if (lastSize == 0) {
      if (buffers.size()*BUFFER_SIZE > fileSize) {
        // last buffer must be empty
        buffers.pop_back();
      }
    } else {
      std::unique_ptr<char[]> lastBuffer = std::unique_ptr<char[]>( new char[lastSize] );
      std::copy(buffers.back().get(), buffers.back().get() + lastSize, lastBuffer.get());
      buffers.back().swap(lastBuffer);
    }

    assert (buffers.size() * BUFFER_SIZE + lastSize == fileSize);
  }

  virtual void close(OutputStream &os) override {
    flush(os);
    trim();
  }

public:

  ~RAMFile() override = default;
};


/***
class RAMDir : public Directory {
public:
  typedef RAMFile FileType;

 // TODO: want ordered map to replicate things like s3... ability to get everything with a prefix.
 // actually, since we will be producing files in sorted order, a simple vector may be very efficient!
 // Can check if new file is greater than last entry and then insertion is very fast!
 // TODO: emulate directories with object stores, or emulate object stores with directories?

 // TODO: try out sso_stringview? API would need to pass in a ByteBlockPool to copy anything large though.
 // Use copy-on-write on a vector impl of the file list (or a simple mutex)?  Some way to add / remove multiple files at once would be nice.
 // could give a mutable view to a writer that is committed at the end... but would it be implementable for
 // other directory types?
 // Have something like a commit set?  A list of adds/deletes?
 // For actual file system directory, we prob want to keep a separate list of opened files?

 // Hmmm, but could string view become invalidated by subsequent parallel operations on the Dir?  Don't allow that!

  std::unordered_map<std::string, std::shared_ptr<FileType>> fileMap;
public:

  // needs to be shared_ptr because it's shared with the fileMap?
  // or perhaps make this by-copy?
  std::shared_ptr<OutputStream> createOutput(std::string name) override {
    std::shared_ptr<OutputStreamType> make_shared<OutputStreamType>();
    fileMap[name] = std::make_shared<OutputStreamType>();


  }
};
***/


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


// For input stream, we want to be able to create a new one without having to necessarily call any virtual functions.
// This probably means being able to create an input stream from another input stream?
class Directory {
public:
  virtual std::shared_ptr<File> createFile(std::string name) = 0;
};
