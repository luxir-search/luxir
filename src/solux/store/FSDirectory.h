#pragma once

#include <algorithm>
#include <filesystem>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "Directory.h"

namespace solux {

/// InputFile backed by mmap. The fd is kept open so that even if the
/// underlying file is unlinked, the mapping (and fd) remain valid.
class MMapInputFile : public InputFile {
  int fd_;
  char* data_;
  size_t sz_;

public:
  MMapInputFile(int fd, char* data, size_t size) : fd_(fd), data_(data), sz_(size) {}

  ~MMapInputFile() override {
    if (data_ && sz_ > 0) {
      munmap(data_, sz_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  MMapInputFile(const MMapInputFile&) = delete;
  MMapInputFile& operator=(const MMapInputFile&) = delete;

  size_t size() override { return sz_; }

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

  std::filesystem::path path_;
  int fd_ = -1;
  size_t fileSize_ = 0;

  // Single reusable write buffer
  std::unique_ptr<char[]> buf_;
  size_t bufCapacity_ = 0;

  static constexpr size_t START_BUFFER_SIZE = 1024;
  static constexpr size_t MAX_BUFFER_SIZE = 0x20000; // 128 KiB

  void openFd() {
    if (fd_ >= 0) return;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
      throw std::runtime_error("FSFile: failed to create file: " + path_.string() + ": " + strerror(errno));
    }
  }

  void writeToFd(const char* data, size_t len) {
    while (len > 0) {
      auto n = ::write(fd_, data, len);
      if (n < 0) {
        throw std::runtime_error("FSFile: write failed for: " + path_.string() + ": " + strerror(errno));
      }
      data += n;
      len -= (size_t)n;
    }
  }

  void flush(OutputStream& os, bool deferNewBuff) override {
    auto written = (size_t)(os.pos - os.start);
    fileSize_ += written;
    os.flushedSize = fileSize_;

    if (written > 0) {
      openFd();
      writeToFd(os.start, written);
    }

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
  FSFile(std::string_view name, const std::filesystem::path& path) : File(name), path_(path) {}

  ~FSFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  size_t size() override { return fileSize_; }

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
    auto tmpBuf = std::make_unique_for_overwrite<char[]>(otherSize);
    in.copyTo(tmpBuf.get());
    writeToFd(tmpBuf.get(), otherSize);
    fileSize_ += otherSize;
    in.clear();
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

  void listFiles(std::vector<std::string>& target) override {
    auto startIdx = target.size();
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.is_regular_file()) {
        target.push_back(entry.path().filename().string());
      }
    }
    std::sort(target.begin() + (int64_t)startIdx, target.end());
  }

  std::shared_ptr<InputFile> openFile(std::string_view name) override {
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
      return std::make_shared<MMapInputFile>(fd, nullptr, 0);
    }

    void* mapped = mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
      ::close(fd);
      return {};
    }

    return std::make_shared<MMapInputFile>(fd, (char*)mapped, sz);
  }

  std::unique_ptr<File> createFile(std::string_view name) override {
    return std::make_unique<FSFile>(name, filePath(name));
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
    auto& fsFile = dynamic_cast<FSFile&>(file);
    // If nothing was ever flushed (empty file), still create it on disk
    if (fsFile.fd_ < 0) {
      fsFile.openFd();
    }
    fsFile.closeFd();
  }

  void clear() override {
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.is_regular_file()) {
        std::filesystem::remove(entry.path());
      }
    }
  }
};

} // namespace solux
