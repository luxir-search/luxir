// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

namespace luxir {

/// Read-ahead for rows of a memory-mapped column that may not be resident
/// (an index larger than the page cache).
///
/// Touching a cold mmap page is a synchronous major fault, so a loop over N
/// cold rows waits for N serial device reads, and CPU prefetch instructions
/// do not help (they never fault).  MADV_WILLNEED on the rows' pages submits
/// the reads before the loop reaches them, so the device serves many at once,
/// and it reads only those pages: a plain fault on a file mapping reads a
/// read-around window (128 KiB by default) centered on the faulting page,
/// which for a sparse rescore is mostly bytes that are never used and evict
/// other cached data.
///
/// Advice costs a syscall even when the pages are cached, so it is adaptive:
/// it starts off unless a residency probe finds a cold page, and turns on once
/// the calling thread takes a major fault (see refresh()).  Ranges passed to
/// add() in address order coalesce; flush() issues them in chunks no larger
/// than the kernel's per-call WILLNEED limit (max(read_ahead_kb,
/// max_sectors_kb), 128 KiB on common devices), since a larger call reads
/// only its first chunk.
class PagePrefetcher {
  static constexpr size_t ADVISE_CHUNK_BYTES = 128 * 1024;

  uintptr_t runStart = 0;
  uintptr_t runEnd = 0;
  long majflt = 0;
  bool on = false;

  static size_t pageSize() {
    static const size_t size = (size_t)sysconf(_SC_PAGESIZE);
    return size;
  }

  static long threadMajorFaults() {
    struct rusage usage;
    if (getrusage(RUSAGE_THREAD, &usage) != 0) return 0;
    return usage.ru_majflt;
  }

public:
  PagePrefetcher() : majflt(threadMajorFaults()) {}

  PagePrefetcher(const PagePrefetcher&) = delete;
  PagePrefetcher& operator=(const PagePrefetcher&) = delete;

  ~PagePrefetcher() { flush(); }

  bool active() const { return on; }

  /// Turns advice on when the page holding p is not resident.
  void probe(const void* p) {
    if (on) return;
    unsigned char resident = 1;
    auto page = (uintptr_t)p & ~(uintptr_t)(pageSize() - 1);
    if (mincore((void*)page, pageSize(), &resident) == 0 && (resident & 1) == 0) on = true;
  }

  /// Turns advice on when this thread took a major fault since the last
  /// check.  Call between batches of reads on the same thread.
  void refresh() {
    if (on) return;
    long now = threadMajorFaults();
    if (now != majflt) on = true;
    majflt = now;
  }

  /// Queues [p, p + len) for read-ahead.
  void add(const void* p, size_t len) {
    auto mask = (uintptr_t)(pageSize() - 1);
    auto start = (uintptr_t)p & ~mask;
    auto end = ((uintptr_t)p + len + mask) & ~mask;
    if (runEnd != 0 && start <= runEnd && end >= runStart) {
      runStart = std::min(runStart, start);
      runEnd = std::max(runEnd, end);
      return;
    }
    flush();
    runStart = start;
    runEnd = end;
  }

  /// Issues the queued ranges.  Advisory: errors (e.g. memory that is not a
  /// file mapping) are ignored.
  void flush() {
    for (uintptr_t at = runStart; at < runEnd; at += ADVISE_CHUNK_BYTES) {
      (void)madvise((void*)at, std::min<size_t>(ADVISE_CHUNK_BYTES, runEnd - at), MADV_WILLNEED);
    }
    runStart = runEnd = 0;
  }
};

} // namespace luxir
