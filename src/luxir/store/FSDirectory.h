// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <mutex>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include "Directory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

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

  HeldRange acquire(ByteRange range) override {
    checkRange(range, sz_);
    return {data_ + range.offset, range.length};
  }
  bool holdsFree() const override { return true; }

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

  std::filesystem::path path_;
  std::filesystem::path tmpPath_;  // write to temp, install create-only on finish
  int fd_ = -1;
  size_t fileSize_ = 0;
  XXH3_state_t hash;
  ByteRateLimiter* rateLimiter_;  // null = unpaced

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
    // A paced write charges one chunk at a time, so no single wait is long.
    size_t chunk = rateLimiter_ ? rateLimiter_->chunk() : len;
    size_t paid = 0;
    while (len > 0) {
      if (rateLimiter_ && paid == 0) {
        paid = std::min(chunk, len);
        rateLimiter_->pace(paid);
      }
      auto n = ::write(fd_, data, rateLimiter_ ? paid : len);
      if (n < 0) {
        throw FileIOException("FSFile: write failed for: " + path_.string() + ": " + strerror(errno));
      }
      if (n == 0) {
        throw FileIOException("FSFile: write returned zero for: " + path_.string());
      }
      data += n;
      len -= (size_t)n;
      if (rateLimiter_) paid -= (size_t)n;
    }
  }

  void flush(OutputStream& os, bool deferNewBuff, size_t minCapacity) override {
    auto written = os.start == nullptr ? 0 : (size_t)(os.pos - os.start);

    if (written > 0) {
      openFd();
      writeToFd(os.start, written);
      fileSize_ += written;
    }
    os.flushedSize = fileSize_;
    os.start = os.pos = os.end = nullptr;

    if (!deferNewBuff) {
      size_t needed = std::max(START_BUFFER_SIZE, std::min(bufCapacity_ > 0 ? bufCapacity_ << 1 : START_BUFFER_SIZE, MAX_BUFFER_SIZE));
      needed = std::max(needed, minCapacity);
      if (bufCapacity_ != needed) {
        buf_ = std::make_unique_for_overwrite<char[]>(needed);
        bufCapacity_ = needed;
      }
      os.start = buf_.get();
      os.pos = os.start;
      os.end = os.start + bufCapacity_;
    }
  }

  void close(OutputStream& os) override {
    flush(os, true, 0);
  }

public:
  FSFile(std::string_view name, const std::filesystem::path& path, ByteRateLimiter* rateLimiter = nullptr)
      : File(name), path_(path), tmpPath_(path.string() + ".tmp"), rateLimiter_(rateLimiter) {
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


class FSDirectory : public Directory {
public:
  static constexpr std::string_view kPublicationLock = ".luxir-publish.lock";
  // Per-path state shared by a factory's views of one directory.
  struct ParentSync {
    std::mutex mutex;
    bool durable = false;
    // Serializes this process's publications and deletions here: on NFS,
    // flock is emulated by process-scoped POSIX locks, which do not exclude threads.
    std::mutex publication;
  };
private:
  std::filesystem::path basePath_;
  std::filesystem::path durabilityRoot;
  std::shared_ptr<ParentSync> parents;
  std::filesystem::path publishTemps;

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

  // Callers hold parents->publication: NFS-emulated flock is process-scoped.
  // Deletion shares the lock with publication; fsyncs run outside it.
  // A stall past the NFSv4 lease between Match and rename can lose the lock
  // and overwrite a newer STATE. Files stay intact; acknowledged commits can
  // drop out of STATE. Retention does not close this revocation window.
  int lockDirectory() const {
    auto path = basePath_ / kPublicationLock;
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      if (errno == ENOENT) return -1;
      throw FileIOException("Cannot open publication lock: " + path.string() + ": " + strerror(errno));
    }
    while (::flock(fd, LOCK_EX) < 0) {
      if (errno != EINTR) {
        auto message = "Cannot lock publication directory: " + path.string() + ": " + strerror(errno);
        ::close(fd);
        throw FileIOException(message);
      }
    }
    return fd;
  }

  std::filesystem::path filePath(std::string_view name) const {
    if (name.empty() || name == "." || name == ".." || name == kPublicationLock || name.find_first_of("/\\\0", 0, 3) != std::string_view::npos) {
      throw std::invalid_argument("invalid directory file name");
    }
    return basePath_ / name;
  }

public:
  Capabilities capabilities() const override { return HARD_LINK | ATOMIC_RENAME | DURABLE_SYNC | MEMORY_MAPPED; }

  void linkFile(Directory& source, std::string_view name) override {
    std::filesystem::create_directories(basePath_);
    auto& fs = dynamic_cast<FSDirectory&>(source.underlying());
    std::filesystem::create_hard_link(fs.filePath(name), filePath(name));
  }

  explicit FSDirectory(const std::filesystem::path& path, std::filesystem::path root = {},
                       std::shared_ptr<ParentSync> parents = std::make_shared<ParentSync>(),
                       std::filesystem::path publishTemps = {})
      : basePath_(std::filesystem::absolute(path).lexically_normal()),
        durabilityRoot(root.empty() ? basePath_ : std::filesystem::absolute(root).lexically_normal()),
        parents(std::move(parents)),
        publishTemps(publishTemps.empty() ? durabilityRoot / "node/tmp" : std::move(publishTemps)) {
    if (root.empty()) while (!std::filesystem::is_directory(durabilityRoot)) durabilityRoot = durabilityRoot.parent_path();
  }

  void listFiles(std::vector<FileInfo>& target) override {
    if (!std::filesystem::exists(basePath_)) return;
    auto startIdx = target.size();
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.path().filename() == kPublicationLock) continue;
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

  std::shared_ptr<InputFile> openFile(std::string_view name, OpenOptions options = {}) override {
    unused(options);
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

  using Directory::createFile;

  std::unique_ptr<File> createFile(std::string_view name) override {
    return createFile(name, FileCreateOptions{});
  }

  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    std::filesystem::create_directories(basePath_);
    return std::make_unique<FSFile>(name, filePath(name), options.rateLimiter);
  }

  bool deleteFile(std::string_view name) override {
    auto path = filePath(name);
    std::lock_guard local(parents->publication);
    int fd = lockDirectory();
    if (fd < 0) return false;
    auto unlock = scope_guard([&] { ::close(fd); });
    return std::filesystem::remove(path);
  }

  void deletePrefix(std::string_view prefix) override {
    std::lock_guard local(parents->publication);
    int fd = lockDirectory();
    if (fd < 0) return;
    auto unlock = scope_guard([&] { ::close(fd); });
    std::vector<std::string> toDelete;
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.is_regular_file()) {
        std::string name = entry.path().filename().string();
        if (name == kPublicationLock) continue;
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
    auto hold = file->acquire({0, file->size()});
    return Publication{std::string(hold.getData(), hold.getLength()), versionOf(fd, std::as_bytes(hold.bytes()))};
  }

  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) override {
    auto target = filePath(name);
    std::filesystem::create_directories(basePath_);
    std::filesystem::create_directories(publishTemps);
    auto temp = publishTemps / newUuid();
    int fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) throw FileIOException("Cannot create publication temp: " + temp.string() + ": " + strerror(errno));
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
      std::lock_guard local(parents->publication);
      int directoryFd = lockDirectory();
      if (directoryFd < 0) throw FileIOException("Publication directory was removed: " + basePath_.string());
      auto unlock = scope_guard([&] { ::close(directoryFd); });
      if (condition.version) {
        auto current = readPublished(name);
        if (!current || current->version != *condition.version) return {};
        Signal::emit("publishMatched", this, &name);
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
    {
      std::lock_guard local(parents->publication);
      int fd = lockDirectory();
      if (fd < 0) return;
      auto unlock = scope_guard([&] { ::close(fd); });
      for (const auto& name : names) {
        if (::unlink(filePath(name).c_str()) < 0 && errno != ENOENT) {
          throw FileIOException("Cannot delete file: " + std::string(name) + ": " + strerror(errno));
        }
      }
    }
    syncDirectory();
  }

  void clear() override {
    std::lock_guard local(parents->publication);
    int fd = lockDirectory();
    if (fd < 0) return;
    auto unlock = scope_guard([&] { ::close(fd); });
    for (const auto& entry : std::filesystem::directory_iterator(basePath_)) {
      if (entry.path().filename() == kPublicationLock) continue;
      if (entry.is_regular_file()) {
        std::filesystem::remove(entry.path());
      }
    }
  }
};

} // namespace luxir
