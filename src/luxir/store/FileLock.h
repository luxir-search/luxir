#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace luxir {

/// Process-scoped exclusive lock backed by a filesystem file. Acquisition is
/// non-blocking: if another process (or open file description) already holds
/// the lock, the constructor throws. The lock is released when the fd is closed,
/// including on process exit or crash, so no stale-lock cleanup is needed.
class FileLock {
  int fd_ = -1;

  // Record who holds the lock, for humans inspecting the file. The flock is the
  // real enforcement; this content is best-effort and never disrupts locking.
  void writeDiagnostic() noexcept {
    try {
      char hostbuf[256] = {};
      std::string host = "unknown";
      if (::gethostname(hostbuf, sizeof(hostbuf) - 1) == 0) {
        host = hostbuf;
      }

      auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
      std::string line = std::format("pid={} host={} started={:%Y-%m-%dT%H:%M:%SZ}\n",
                                     ::getpid(), host, now);

      if (::ftruncate(fd_, 0) != 0) return;
      if (::write(fd_, line.data(), line.size()) < 0) return;
    } catch (...) {
      // best-effort diagnostic only
    }
  }

public:
  explicit FileLock(const std::filesystem::path& lockPath) {
    fd_ = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      throw std::runtime_error(std::format("FileLock: failed to open {}: {}",
                                           lockPath.string(),
                                           std::system_category().message(errno)));
    }

    if (::flock(fd_, LOCK_EX | LOCK_NB) < 0) {
      int error = errno;
      ::close(fd_);
      fd_ = -1;

      if (error == EWOULDBLOCK || error == EAGAIN) {
        throw std::runtime_error(std::format(
            "luxir: data directory {} is locked by another running instance "
            "(write.lock held). Only one instance may write to a data directory.",
            lockPath.parent_path().string()));
      }
      throw std::runtime_error(std::format("FileLock: failed to lock {}: {}",
                                           lockPath.string(),
                                           std::system_category().message(error)));
    }

    writeDiagnostic();
  }

  ~FileLock() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;
};

} // namespace luxir
