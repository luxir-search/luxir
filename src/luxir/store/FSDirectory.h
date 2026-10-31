// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include "Directory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

class RAMDelegatingFile;

/// InputFile backed by mmap. The mapping remains valid after its fd is closed
/// and after the underlying file is unlinked.
class MMapInputFile : public InputFile {
  // The kernel reads at most max(read_ahead_kb, max_sectors_kb) pages per
  // MADV_WILLNEED call (128 KiB on common devices) and drops the rest of the
  // range, so advice goes out in chunks of this size.
  static constexpr size_t ADVISE_BYTES = 128 * 1024;
  // A prefetch of at least this many bytes checks residency first: mincore
  // costs a few ns per mapped page, advice on a resident page tens of ns.
  static constexpr size_t CHECK_BYTES = 64 * 1024;
  static constexpr size_t CHECK_PAGES = 2048;

  char* data_;
  size_t sz_;
  // mincore reports every page of a file the process can neither own nor
  // write as resident (to close a page-cache side channel), so residency is
  // only checked for files it reports truthfully.
  bool checkResidency_;

public:
  // The caller retains ownership of fd. O_DIRECT affects pread, not mmap.
  static std::shared_ptr<InputFile> map(int fd, const std::filesystem::path& path) {
    struct stat st;
    if (fstat(fd, &st) < 0) {
      throw FileIOException("MMapInputFile: fstat failed for " + path.string() + ": " + strerror(errno));
    }
    auto size = (size_t)st.st_size;
    if (!size) return std::make_shared<MMapInputFile>(nullptr, 0);
    void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
      throw FileIOException("MMapInputFile: mmap failed for " + path.string() + ": " + strerror(errno));
    }
    bool checkResidency = st.st_uid == geteuid() || faccessat(AT_FDCWD, path.c_str(), W_OK, AT_EACCESS) == 0;
    try {
      return std::make_shared<MMapInputFile>((char*)mapped, size, checkResidency);
    } catch (...) { munmap(mapped, size); throw; }
  }

  MMapInputFile(char* data, size_t size, bool checkResidency = false)
    : data_(data), sz_(size), checkResidency_(checkResidency) {}

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
    char* start = data_ + offset / pageSize * pageSize;
    char* end = data_ + offset + std::min(length, sz_ - offset);
    if (!checkResidency_ || (size_t)(end - start) < CHECK_BYTES) return advise(start, end);
#if defined(__APPLE__)
    char vec[CHECK_PAGES];
#else
    unsigned char vec[CHECK_PAGES];
#endif
    while (start < end) {
      size_t pages = std::min(CHECK_PAGES, ((size_t)(end - start) + pageSize - 1) / pageSize);
      if (mincore(start, pages * pageSize, vec) != 0) return advise(start, end);
      for (size_t i = 0; i < pages;) {
        if (vec[i] & 1) { i++; continue; }
        size_t run = i + 1;
        while (run < pages && !(vec[run] & 1)) run++;
        advise(start + i * pageSize, std::min(end, start + run * pageSize));
        i = run;
      }
      start += pages * pageSize;
    }
  }

  std::string_view read() override {
    return {data_, sz_};
  }

  InputStream getInputStream() override {
    return {data_, data_ + sz_};
  }

private:
  static void advise(char* start, char* end) {
    for (; start < end; start += ADVISE_BYTES) {
      (void)madvise(start, std::min(ADVISE_BYTES, (size_t)(end - start)), MADV_WILLNEED);
    }
  }
};


/// Sequential checksum of a file through its own descriptor: pread in 1 MiB
/// chunks, never the mmap path (see Directory::openChecksum). The fd keeps
/// the inode readable after an unlink. With O_DIRECT the chunks come from the
/// media and leave the page cache alone; a filesystem that refuses it (tmpfs)
/// or a read it rejects falls back to buffered reads of the same fd offset.
/// macOS has no O_DIRECT; F_NOCACHE keeps the reads out of its page cache
/// without alignment rules.
class FSChecksumReader : public ChecksumReader {
  std::filesystem::path path;
  int fd;
  uint64_t size_;
  bool direct;

  static constexpr size_t kChunk = 1 << 20;
  static constexpr size_t kAlignment = 4096; // covers every common logical block size

  // Clears direct when the descriptor reads through the page cache after all.
  static int openPath(const std::filesystem::path& path, bool& direct) {
#if defined(__APPLE__)
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0 && direct) direct = fcntl(fd, F_NOCACHE, 1) == 0;
#else
    int fd = ::open(path.c_str(), O_RDONLY | (direct ? O_DIRECT : 0));
    if (fd < 0 && direct && errno == EINVAL) fd = ::open(path.c_str(), O_RDONLY);
    // fcntl tells whether the direct open stuck (the fallback above cleared it).
    if (fd >= 0 && direct) {
      int flags = fcntl(fd, F_GETFL);
      direct = flags >= 0 && (flags & O_DIRECT);
    }
#endif
    return fd;
  }
  [[noreturn]] static void fail(const std::filesystem::path& path, const char* operation, int error) {
    auto message = "FSChecksumReader: " + std::string(operation) + " failed for " + path.string() + ": " + strerror(error);
    if (error == EIO) throw FileEIOError(message);
    throw FileIOException(message);
  }
  void useBuffered() {
#if defined(__APPLE__)
    if (fcntl(fd, F_NOCACHE, 0) < 0) fail(path, "fcntl", errno);
#else
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_DIRECT) < 0) fail(path, "fcntl", errno);
#endif
    direct = false;
  }

public:
  // Null when the file does not exist.
  static std::shared_ptr<FSChecksumReader> open(const std::filesystem::path& path, bool direct) {
    int fd = openPath(path, direct);
    if (fd < 0) {
      if (errno == ENOENT) return {};
      fail(path, "open", errno);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
      int error = errno;
      ::close(fd);
      fail(path, "fstat", error);
    }
    try {
      return std::make_shared<FSChecksumReader>(path, fd, (uint64_t)st.st_size, direct);
    } catch (...) { ::close(fd); throw; }
  }
  FSChecksumReader(std::filesystem::path path, int fd, uint64_t size, bool direct)
      : path(std::move(path)), fd(fd), size_(size), direct(direct) {}
  ~FSChecksumReader() override { ::close(fd); }
  FSChecksumReader(const FSChecksumReader&) = delete;
  FSChecksumReader& operator=(const FSChecksumReader&) = delete;

  uint64_t size() const override { return size_; }
  std::shared_ptr<InputFile> map() override { return MMapInputFile::map(fd, path); }
  bool isDirect() const { return direct; }

  FileCheck checksum(const std::function<void(size_t)>& progress) override {
    void* raw = nullptr;
    if (posix_memalign(&raw, kAlignment, kChunk) != 0) throw std::bad_alloc();
    std::unique_ptr<void, decltype(&std::free)> buffer(raw, &std::free);
    auto* data = (char*)raw;
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    uint64_t total = 0;
    // Reads past the recorded size too: a file that grew hashes to a mismatch
    // instead of silently verifying its first bytes.
    for (;;) {
      auto n = ::pread(fd, data, kChunk, (off_t)total);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (direct && errno == EINVAL) { useBuffered(); continue; }
        fail(path, "read", errno);
      }
      if (n == 0) break;
      XXH3_64bits_update(&state, data, (size_t)n);
      total += (uint64_t)n;
      if (progress) progress((size_t)n);
    }
    return {total, XXH3_64bits_digest(&state)};
  }
};


/// File backed by the filesystem. Opens the fd on first flush and writes
/// incrementally. Reuses a single write buffer.
class FSFile : public File {
  friend class OutputStream;
  friend class FSDirectory;
  friend class RAMDelegatingFile;

  std::filesystem::path path_;
  std::filesystem::path tmpPath_;  // write to temp, install create-only on finish
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
public:
  struct ParentSync {
    std::mutex mutex;
    bool durable = false;
  };
private:
  std::filesystem::path basePath_;
  std::filesystem::path durabilityRoot;
  std::shared_ptr<ParentSync> parents;
  inline static std::atomic<uint64_t> nextPublishTemp{0};
  // write.lock excludes other owning processes. Every install takes this lock
  // so a Replace cannot interleave a Match check and rename. No fsync holds it.
  inline static std::mutex publicationMutex;
  static constexpr std::string_view publishTempPrefix = ".luxir-publish-";

  static PublicationVersion versionOf(int fd, std::span<const std::byte> bytes) {
    struct stat info;
    if (::fstat(fd, &info) < 0) throw FileIOException("Publication fstat failed: " + std::string(strerror(errno)));
#if defined(__APPLE__)
    auto modified = info.st_mtimespec;
#else
    auto modified = info.st_mtim;
#endif
    // Reused metadata must also match the content hash. Identical CAS bytes
    // are the same logical state: the engine includes its revision and op.
    return std::format("{}:{}:{}:{}:{}:{:016x}", info.st_dev, info.st_ino,
        modified.tv_sec, modified.tv_nsec, info.st_size, XXH3_64bits(bytes.data(), bytes.size()));
  }

  std::filesystem::path filePath(std::string_view name) const {
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\\0", 0, 3) != std::string_view::npos) {
      throw std::invalid_argument("invalid directory file name");
    }
    return basePath_ / name;
  }

public:
  // Listings hide scratch files. Owned factories remove other processes' temps.
  static bool isPublishTemp(std::string_view name) { return name.starts_with(publishTempPrefix); }

  void linkFile(Directory& source, std::string_view name) override {
    std::filesystem::create_directories(basePath_);
    auto& fs = dynamic_cast<FSDirectory&>(source.underlying());
    std::filesystem::create_hard_link(fs.filePath(name), filePath(name));
  }

  explicit FSDirectory(const std::filesystem::path& path, std::filesystem::path root = {},
                       std::shared_ptr<ParentSync> parents = std::make_shared<ParentSync>())
      : basePath_(std::filesystem::absolute(path).lexically_normal()),
        durabilityRoot(root.empty() ? basePath_ : std::filesystem::absolute(root).lexically_normal()),
        parents(std::move(parents)) {
    if (root.empty()) while (!std::filesystem::is_directory(durabilityRoot)) durabilityRoot = durabilityRoot.parent_path();
  }

  static void cleanPublicationTemps(const std::filesystem::path& path) {
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      auto name = entry.path().filename().string();
      if (!isPublishTemp(name)) continue;
      auto text = std::string_view(name).substr(publishTempPrefix.size());
      auto dash = text.find('-');
      if (dash == std::string_view::npos) continue;
      uint64_t pid = 0;
      auto [end, error] = std::from_chars(text.data(), text.data() + dash, pid);
      if (error == std::errc() && end == text.data() + dash && pid != (uint64_t)getpid()) {
        if (::unlink(entry.path().c_str()) < 0 && errno != ENOENT) throw FileIOException("Cannot remove publication temp: " + entry.path().string());
      }
    }
  }

  void listFiles(std::vector<FileInfo>& target) override {
    if (!std::filesystem::exists(basePath_)) return;
    auto startIdx = target.size();
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (isPublishTemp(entry.path().filename().string())) continue;
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
    // Only absence is a null result; an unreadable file is an error, never
    // evidence that the file does not exist.
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
      if (errno == ENOENT) return {};
      throw FileIOException("FSDirectory::openFile: open failed for " + path.string() + ": " + strerror(errno));
    }

    auto close = scope_guard([&] { ::close(fd); });
    return MMapInputFile::map(fd, path);
  }

  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    return FSChecksumReader::open(filePath(name), direct);
  }

  std::unique_ptr<File> createFile(std::string_view name) override {
    std::filesystem::create_directories(basePath_);
    return std::make_unique<FSFile>(name, filePath(name));
  }

  std::unique_ptr<File> createFile(
      std::string_view name, FileCreateOptions options) override {
    std::filesystem::create_directories(basePath_);
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
    if (!std::filesystem::exists(basePath_)) return;
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
    if (::link(fsFile.tmpPath_.c_str(), fsFile.path_.c_str()) < 0) {
      throw FileIOException("File install failed: " + fsFile.path_.string() + ": " + strerror(errno));
    }
    if (::unlink(fsFile.tmpPath_.c_str()) < 0) throw FileIOException("Cannot remove file temp: " + fsFile.tmpPath_.string());
  }

  std::optional<Publication> readPublished(std::string_view name) override {
    auto path = filePath(name);
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      if (errno == ENOENT) return {};
      throw FileIOException("Publication open failed: " + path.string() + ": " + strerror(errno));
    }
    auto close = scope_guard([&] { ::close(fd); });
    auto file = MMapInputFile::map(fd, path);
    auto bytes = file->read();
    return Publication{std::string(bytes), versionOf(fd, std::as_bytes(std::span(bytes)))};
  }

  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) override {
    if (isPublishTemp(name)) throw std::invalid_argument("reserved publication temp name");
    auto target = filePath(name);
    std::filesystem::create_directories(basePath_);
    std::filesystem::path temp;
    int fd;
    for (;;) {
      temp = filePath(std::format("{}{}-{}", publishTempPrefix, getpid(),
          nextPublishTemp.fetch_add(1, std::memory_order_relaxed)));
      fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
      if (fd >= 0) break;
      if (errno != EEXIST) throw FileIOException("Cannot create publication temp: " + temp.string() + ": " + strerror(errno));
    }
    auto cleanup = scope_guard([&] { ::close(fd); ::unlink(temp.c_str()); });
    // The open hook exposes the fd for real I/O failure injection in tests.
    Signal::emit("publishOpened", this, &name, &fd);
    size_t written = 0;
    while (written < bytes.size()) {
      auto count = ::write(fd, bytes.data() + written,
          std::min(bytes.size() - written, (size_t)std::numeric_limits<ssize_t>::max()));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw FileIOException("Publication write failed: " + temp.string() + ": " + (count ? strerror(errno) : "zero write"));
      written += (size_t)count;
    }
    Signal::emit("publishWritten", this, &name);
    if (::fsync(fd) < 0) throw FileIOException("Publication fsync failed: " + temp.string() + ": " + strerror(errno));
    std::string syncedName(name);
    Signal::emit("fsSynced", &basePath_, &syncedName);
    Signal::emit("publishSynced", this, &name);
    auto version = versionOf(fd, bytes);
    {
      std::lock_guard lock(publicationMutex);
      if (condition.version) {
        auto current = readPublished(name);
        if (!current || current->version != *condition.version) return {};
      }
      if (condition.mode == PublishMode::CREATE_ONLY) {
        if (::link(temp.c_str(), target.c_str()) < 0) {
          if (errno == EEXIST) return {};
          throw FileIOException("Publication link failed: " + target.string() + ": " + strerror(errno));
        }
      } else if (::rename(temp.c_str(), target.c_str()) < 0) {
        throw FileIOException("Publication rename failed: " + target.string() + ": " + strerror(errno));
      }
    }
    if (condition.mode == PublishMode::CREATE_ONLY) {
      if (::unlink(temp.c_str()) < 0) throw FileIOException("Publication temp removal failed: " + temp.string() + ": " + strerror(errno));
    }
    syncDirectory();
    return version;
  }

  // The factory shares parent durability across views of each path. A new
  // owner re-syncs the chain: an earlier mkdir may have lacked its parent sync.
  void syncDirectory() {
    std::filesystem::create_directories(basePath_);
    auto sync = [](const std::filesystem::path& path) {
      int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (fd < 0) throw FileIOException("Cannot open directory for fsync: " + path.string() + ": " + strerror(errno));
      auto close = scope_guard([&] { ::close(fd); });
      if (::fsync(fd) < 0) throw FileIOException("Directory fsync failed: " + path.string() + ": " + strerror(errno));
      Signal::emit("fsSynced", (void*)&path);
    };
    sync(basePath_);
    std::lock_guard lock(parents->mutex);
    if (!parents->durable) {
      for (auto path = basePath_; path != durabilityRoot && path != path.root_path();) {
        path = path.parent_path();
        sync(path);
      }
      parents->durable = true;
    }
  }

  void sync(std::span<const std::string> filenames) override {
    for (auto& name : filenames) {
      int fd = ::open(filePath(name).c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        throw std::runtime_error("FSDirectory::sync: failed to open " + name + ": " + strerror(errno));
      }
      if (::fsync(fd) < 0) {
        ::close(fd);
        throw std::runtime_error("FSDirectory::sync: fsync failed for " + name + ": " + strerror(errno));
      }
      ::close(fd);
      Signal::emit("fsSynced", &basePath_, (void*)&name);
    }
    syncDirectory();
  }

  void deleteDurably(std::span<const std::string> names) override {
    for (const auto& name : names) {
      if (::unlink(filePath(name).c_str()) < 0 && errno != ENOENT) {
        throw FileIOException("Cannot delete file: " + std::string(name) + ": " + strerror(errno));
      }
    }
    syncDirectory();
  }

  void clear() override {
    if (!std::filesystem::exists(basePath_)) return;
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.is_regular_file()) {
        std::filesystem::remove(entry.path());
      }
    }
  }
};

} // namespace luxir
