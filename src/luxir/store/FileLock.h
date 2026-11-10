// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace luxir {

/// Admission lock backed by a filesystem file. Acquisition is non-blocking:
/// a conflicting holder makes the constructor throw. Closing the fd releases
/// the lock, including on process exit or crash, so no stale-lock cleanup is
/// needed. Shared storage runs one node per process: on NFS, flock is emulated
/// by process-scoped POSIX locks, so two holders in one process do not exclude
/// each other and closing either releases both.
class FileLock {
public:
  enum class Mode { EXCLUSIVE, SHARED };
private:
  int fd = -1;

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

      if (::ftruncate(fd, 0) != 0) return;
      if (::write(fd, line.data(), line.size()) < 0) return;
    } catch (...) {
      // best-effort diagnostic only
    }
  }

public:
  explicit FileLock(const std::filesystem::path& path, Mode mode = Mode::EXCLUSIVE) {
    fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("FileLock: cannot open " + path.string() + ": " + strerror(errno));
    if (::flock(fd, (mode == Mode::SHARED ? LOCK_SH : LOCK_EX) | LOCK_NB) < 0) {
      int error = errno;
      ::close(fd);
      if (error == EWOULDBLOCK || error == EAGAIN) {
        throw std::runtime_error("luxir: lock " + path.string() + " is held by another instance ("
            + (mode == Mode::SHARED ? "shared" : "exclusive") + " admission refused)");
      }
      throw std::runtime_error("FileLock: cannot lock " + path.string() + ": " + strerror(error));
    }
    // A shared lock file has several holders; only an exclusive holder describes itself.
    if (mode == Mode::EXCLUSIVE) writeDiagnostic();
  }

  ~FileLock() { ::close(fd); }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;
};

} // namespace luxir
