// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <filesystem>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include "Directory.h"

namespace luxir {

class RAMDelegatingFile;

/// InputFile backed by mmap. The mapping remains valid after its fd is closed
/// and after the underlying file is unlinked.
class MMapInputFile : public InputFile {
  char* data_;
  size_t sz_;

public:
  MMapInputFile(char* data, size_t size) : data_(data), sz_(size) {}

  ~MMapInputFile() override {
    if (data_ && sz_ > 0) {
      munmap(data_, sz_);
    }
  }

  MMapInputFile(const MMapInputFile&) = delete;
  MMapInputFile& operator=(const MMapInputFile&) = delete;

  size_t size() override { return sz_; }

  void prefetch(size_t offset, size_t length) override {
    if (offset >= sz_ || length == 0) return;
    static const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
    size_t start = offset / pageSize * pageSize;
    (void)madvise(data_ + start, offset - start + std::min(length, sz_ - offset), MADV_WILLNEED);
  }

  std::string_view read() override {
    return {data_, sz_};
  }

  InputStream getInputStream() override {
    return {data_, data_ + sz_};
  }
};


/// File backed by the filesystem. Opens the fd on first flush and writes
/// incrementally. Reuses a single write buffer.
class FSFile : public File {
  friend class OutputStream;
  friend class FSDirectory;
  friend class RAMDelegatingFile;

  std::filesystem::path path_;
  std::filesystem::path tmpPath_;  // write to temp, rename on finish
  int fd_ = -1;
  size_t fileSize_ = 0;
  XXH3_state_t hash;

  // Single reusable write buffer
  std::unique_ptr<char[]> buf_;
  size_t bufCapacity_ = 0;

  static constexpr size_t START_BUFFER_SIZE = 1024;
  static constexpr size_t MAX_BUFFER_SIZE = 0x20000; // 128 KiB

  void openFd() {
    if (fd_ >= 0) return;
    fd_ = ::open(tmpPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
      throw FileIOException("FSFile: failed to create file: " + tmpPath_.string() + ": " + strerror(errno));
    }
  }

  void writeToFd(const char* data, size_t len) {
    XXH3_64bits_update(&hash, data, len);
    while (len > 0) {
      auto n = ::write(fd_, data, len);
      if (n < 0) {
        throw FileIOException("FSFile: write failed for: " + path_.string() + ": " + strerror(errno));
      }
      if (n == 0) {
        throw FileIOException("FSFile: write returned zero for: " + path_.string());
      }
      data += n;
      len -= (size_t)n;
    }
  }

  void flush(OutputStream& os, bool deferNewBuff) override {
    auto written = os.start == nullptr ? 0 : (size_t)(os.pos - os.start);

    if (written > 0) {
      openFd();
      writeToFd(os.start, written);
      fileSize_ += written;
    }
    os.flushedSize = fileSize_;

    if (!deferNewBuff) {
      size_t needed = std::max(START_BUFFER_SIZE, std::min(bufCapacity_ > 0 ? bufCapacity_ << 1 : START_BUFFER_SIZE, MAX_BUFFER_SIZE));
      if (bufCapacity_ < needed) {
        buf_ = std::make_unique_for_overwrite<char[]>(needed);
        bufCapacity_ = needed;
      }
      os.start = buf_.get();
      os.pos = os.start;
      os.end = os.start + bufCapacity_;
    } else {
      os.start = os.pos = os.end = nullptr;
    }
  }

  void close(OutputStream& os) override {
    flush(os, true);
  }

public:
  FSFile(std::string_view name, const std::filesystem::path& path)
      : File(name), path_(path), tmpPath_(path.string() + ".tmp") {
    XXH3_64bits_reset(&hash);
  }

  ~FSFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  size_t size() override { return fileSize_; }
  uint64_t digest() const override { return XXH3_64bits_digest(&hash); }

  void closeFd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void destructiveAppend(RAMFile& in) override {
    auto otherSize = in.size();
    if (otherSize == 0) return;

    openFd();
    for (const auto& buffer : in.buffers) {
      writeToFd(buffer.data.get(), buffer.used);
    }
    fileSize_ += otherSize;
    in.fileSize = 0;
    in.allocatedSize = 0;
    in.buffers.clear();
  }
};


/// FS output that retains its complete byte image in RAM until it either
/// exceeds a safety threshold or segment finalize decides it must survive.
/// The final FSFile always uses the original name, so spilling never changes
/// the physical filenum or future object-store key.
class RAMDelegatingFile : public File {
  friend class FSDirectory;

  std::filesystem::path path_;
  RAMFile ramFile;
  std::unique_ptr<FSFile> delegate;
  std::function<void(int64_t)> ramBytesChanged;
  size_t spillBytes;
  bool bufferingEnabled = true;

  void accountChange(int64_t delta) noexcept {
    if (delta == 0 || !ramBytesChanged) return;
    try {
      ramBytesChanged(delta);
    } catch (...) {
      // File cleanup must not throw. The PostingsWriter accounting callback is
      // noexcept; this also contains any future callback implementation.
    }
  }

  void accountRamOperation(size_t before) noexcept {
    size_t after = ramFile.allocatedBytes();
    if (after >= before) {
      accountChange((int64_t) (after - before));
    } else {
      accountChange(-(int64_t) (before - after));
    }
  }

  void flushActiveToRam(OutputStream& os) {
    size_t before = ramFile.allocatedBytes();
    ramFile.flush(os, true);
    accountRamOperation(before);
  }

  void allocateRamBuffer(OutputStream& os) {
    size_t before = ramFile.allocatedBytes();
    // Make the threshold a write-time boundary, not a finalize-time check.
    // OutputStream flushes when this bounded active region fills and more data
    // remains, so a write that crosses the threshold spills before continuing.
    size_t remaining = spillBytes - ramFile.size();
    size_t previous = ramFile.buffers.empty()
        ? RAMFile::START_BUFFER_SIZE / 2 : ramFile.buffers.back().used;
    size_t capacity = std::max((size_t) RAMFile::START_BUFFER_SIZE,
        std::min(previous << 1, (size_t) 0x100000));
    capacity = std::min(capacity, remaining);
    ramFile.newBuffer(capacity);
    auto& buffer = ramFile.buffers.back();
    os.start = buffer.data.get();
    os.pos = os.start;
    os.end = os.start + buffer.capacity;
    accountRamOperation(before);
  }

  void materializeRam() {
    if (delegate != nullptr) return;
    delegate = std::make_unique<FSFile>(name_, path_);
    size_t before = ramFile.allocatedBytes();
    delegate->destructiveAppend(ramFile);
    accountRamOperation(before);
  }

  void spill(OutputStream& os, bool deferNewBuff, bool activeAlreadyFlushed) {
    if (!activeAlreadyFlushed) flushActiveToRam(os);
    materializeRam();
    os.flushedSize = delegate->size();
    os.start = os.pos = os.end = nullptr;
    bufferingEnabled = false;
    if (!deferNewBuff) delegate->flush(os, false);
  }

  void flush(OutputStream& os, bool deferNewBuff) override {
    if (delegate != nullptr) {
      delegate->flush(os, deferNewBuff);
      return;
    }

    flushActiveToRam(os);
    if (!bufferingEnabled || ramFile.size() >= spillBytes) {
      spill(os, deferNewBuff, true);
    } else if (!deferNewBuff) {
      allocateRamBuffer(os);
    }
  }

  void close(OutputStream& os) override {
    if (delegate != nullptr) {
      delegate->close(os);
    } else {
      flush(os, true);
    }
  }

  bool isRelocatable(const OutputStream& os) const override {
    return delegate == nullptr && bufferingEnabled
        && os.size() < spillBytes;
  }

  void appendRelocatableTo(OutputStream& source,
                           OutputStream& target) override {
    assert(isRelocatable(source));
    flushActiveToRam(source);
    ramFile.appendTo(target);
    size_t before = ramFile.allocatedBytes();
    ramFile.clear();
    accountRamOperation(before);
    source.flushedSize = 0;
    source.start = source.pos = source.end = nullptr;
  }

  void disableRamBuffering(OutputStream& os) override {
    if (delegate != nullptr) return;
    bufferingEnabled = false;
    spill(os, false, false);
  }

public:
  RAMDelegatingFile(std::string_view name, const std::filesystem::path& path,
                    size_t spillBytes,
                    std::function<void(int64_t)>&& ramBytesChanged)
      : File(name), path_(path), ramFile(name),
        ramBytesChanged(std::move(ramBytesChanged)), spillBytes(spillBytes) {
    assert(spillBytes > 0);
  }

  ~RAMDelegatingFile() override {
    accountChange(-(int64_t) ramFile.allocatedBytes());
  }

  uint64_t digest() const override {
    return delegate ? delegate->digest() : ramFile.digest();
  }

  size_t size() override {
    return delegate != nullptr ? delegate->size() : ramFile.size();
  }

  void destructiveAppend(RAMFile& in) override {
    if (delegate != nullptr) {
      delegate->destructiveAppend(in);
      return;
    }

    uint64_t combined = (uint64_t) ramFile.size() + (uint64_t) in.size();
    if (!bufferingEnabled || combined > spillBytes) {
      materializeRam();
      bufferingEnabled = false;
      delegate->destructiveAppend(in);
      return;
    }

    size_t before = ramFile.allocatedBytes();
    ramFile.destructiveAppend(in);
    accountRamOperation(before);
  }
};


/// Filesystem-backed Directory using mmap for reads and buffered writes.
class FSDirectory : public Directory {
  std::filesystem::path basePath_;

  std::filesystem::path filePath(std::string_view name) const {
    return basePath_ / name;
  }

public:
  explicit FSDirectory(const std::filesystem::path& path) : basePath_(path) {
    std::filesystem::create_directories(basePath_);
  }

  void listFiles(std::vector<FileInfo>& target) override {
    auto startIdx = target.size();
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      // A file may be deleted between readdir and stat; skip on any error.
      std::error_code ec;
      if (!entry.is_regular_file(ec) || ec) {
        continue;
      }
      auto size = entry.file_size(ec);
      if (ec) {
        continue;
      }
      target.push_back({entry.path().filename().string(), (uint64_t)size});
    }
    std::sort(target.begin() + (int64_t)startIdx, target.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    unused(expectSynced);
    auto path = filePath(name);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      return {};
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
      ::close(fd);
      return {};
    }

    auto sz = (size_t)st.st_size;
    if (sz == 0) {
      ::close(fd);
      return std::make_shared<MMapInputFile>(nullptr, 0);
    }

    void* mapped = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
      ::close(fd);
      return {};
    }

    ::close(fd);
    try {
      return std::make_shared<MMapInputFile>((char*)mapped, sz);
    } catch (...) {
      munmap(mapped, sz);
      throw;
    }
  }

  std::unique_ptr<File> createFile(std::string_view name) override {
    return std::make_unique<FSFile>(name, filePath(name));
  }

  std::unique_ptr<File> createFile(
      std::string_view name, FileCreateOptions options) override {
    if (!options.ramDelegating) return createFile(name);
    if (options.ramSpillBytes == 0) {
      throw std::invalid_argument("RAM-delegating file requires a spill threshold");
    }
    return std::make_unique<RAMDelegatingFile>(
        name, filePath(name), options.ramSpillBytes,
        std::move(options.ramBytesChanged));
  }

  bool deleteFile(std::string_view name) override {
    auto path = filePath(name);
    return std::filesystem::remove(path);
  }

  void deletePrefix(std::string_view prefix) override {
    std::vector<std::string> toDelete;
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.is_regular_file()) {
        std::string name = entry.path().filename().string();
        if (name.starts_with(prefix)) {
          toDelete.push_back(std::move(name));
        }
      }
    }
    for (const auto& name : toDelete) {
      std::filesystem::remove(filePath(name));
    }
  }

  void finishFile(File& file) override {
    FSFile* fsFilePtr = dynamic_cast<FSFile*>(&file);
    if (fsFilePtr == nullptr) {
      auto& ramFile = dynamic_cast<RAMDelegatingFile&>(file);
      ramFile.materializeRam();
      fsFilePtr = ramFile.delegate.get();
    }
    FSFile& fsFile = *fsFilePtr;
    // If nothing was ever flushed (empty file), still create it on disk
    if (fsFile.fd_ < 0) {
      fsFile.openFd();
    }
    fsFile.closeFd();
    // Atomic rename from temp to final path. Existing mmap readers of the old
    // inode are unaffected because the mapping keeps the old inode alive.
    std::filesystem::rename(fsFile.tmpPath_, fsFile.path_);
  }

  void renameFile(std::string_view from, std::string_view to) override {
    std::filesystem::rename(filePath(from), filePath(to));
  }

  // Fsync the given files.  Use "." to fsync the directory itself
  // (to ensure renames/creates are durable).
  void sync(std::span<const std::string> filenames) override {
    for (auto& name : filenames) {
      int fd;
      if (name == ".") {
        fd = ::open(basePath_.c_str(), O_RDONLY);
      } else {
        fd = ::open(filePath(name).c_str(), O_RDONLY);
      }
      if (fd < 0) {
        throw std::runtime_error("FSDirectory::sync: failed to open " + name + ": " + strerror(errno));
      }
      if (::fsync(fd) < 0) {
        ::close(fd);
        throw std::runtime_error("FSDirectory::sync: fsync failed for " + name + ": " + strerror(errno));
      }
      ::close(fd);
    }
  }

  void clear() override {
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.is_regular_file()) {
        std::filesystem::remove(entry.path());
      }
    }
  }
};

} // namespace luxir
