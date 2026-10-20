// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <sys/resource.h>
#include <unistd.h>
#include "StrColReader.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

/// Read-ahead for the rows of a fixed-size column that may not be resident
/// (an index larger than the page cache).
///
/// Touching a cold mmap page is a synchronous major fault, so a loop over N
/// cold rows waits for N serial device reads (CPU prefetch never faults).
/// Advising the rows ahead of the loop (InputFile::prefetch) submits the
/// reads early, so the device serves many at once, and reads only the rows'
/// pages, where a fault on a file mapping reads a read-around window (128 KiB
/// by default) that a sparse loop mostly never uses.
///
/// Advice on resident pages still costs a syscall, so a Gate turns it on and
/// off from the calling thread's own counters (getrusage RUSAGE_THREAD).
/// Off, it turns on once major faults reach 1/ON_RATIO of the units consumed
/// since the last check.  On, faults stop (advised pages fault in as minor
/// faults), but ru_inblock counts exactly the bytes this thread caused to be
/// read, so the counter around a batch of advice gives the batch's cold
/// pages; advice turns off when they fall below 1/OFF_RATIO of the batch's
/// units.  The ratios put the break-even where measured on NVMe: advice costs
/// about 0.5-2 us per row, a serial fault about 100 us, so advice pays from
/// roughly 0.5% cold rows.  Without RUSAGE_THREAD (macOS) there is no signal
/// and no read-ahead.
///
/// Two front ends share the gate: scanAt() for a scan that reads every row in
/// order (a unit is half of SCAN_WINDOW_BYTES), and sparseAt() for an
/// ascending sparse sequence whose units (rows or document runs) the caller
/// queues on demand, at most SPARSE_AHEAD_UNITS ahead.  Off, sparse checks
/// wait for SPARSE_CHECK_UNITS units, which a shared gate gathers across
/// short sequences; probes cover each sequence's start.
class RowReadAhead {
public:
  /// Counter state of one thread, carried across consecutive uses on that
  /// thread so that short uses still add up to a check.  Never share a Gate
  /// between threads: its counters are per thread.
  class Gate {
    friend class RowReadAhead;
    bool on = false;
    // Advice reads nothing that ru_inblock sees (a filesystem that submits
    // from its own threads, or no task I/O accounting): stay on once on.
    bool blind = false;
    long faults = 0;    // thread major faults at the last check
    long inblock = 0;   // thread input blocks (512 bytes) at the last check
    int64_t units = 0;  // units consumed since the last check

  public:
    Gate() { sample(faults, inblock); }
    bool active() const { return on; }
  };

  /// Off switch for benchmarks that measure plain faults.
  static inline bool enabled = true;

private:
  static constexpr int64_t ON_RATIO = 128;
  static constexpr int64_t OFF_RATIO = 256;
  static constexpr int64_t SCAN_WINDOW_BYTES = 8 << 20;
  static constexpr int64_t SPARSE_AHEAD_UNITS = 1024;
  static constexpr size_t SPARSE_BATCH_BYTES = 8 << 20;
  static constexpr int64_t SPARSE_CHECK_UNITS = 256;
  static constexpr int64_t NEVER = std::numeric_limits<int64_t>::max();

  Gate& gate;
  InputFile* file;
  size_t base;        // file offset of row 0
  size_t stride;      // bytes per row
  int64_t rows;
  int64_t due = 0;    // position of the next step; NEVER when disabled
  int64_t ahead = 0;  // scan: rows advised so far; sparse: units queued so far
  int64_t last = 0;   // position at the previous step
  int64_t count = 0;  // sparse: units in the sequence
  size_t runStart = 0;
  size_t runEnd = 0;
  size_t batchBytes = 0;

  static size_t pageSize() {
    static const size_t size = (size_t)sysconf(_SC_PAGESIZE);
    return size;
  }

  static void sample(long& faults, long& inblock) {
    faults = inblock = 0;
#ifdef RUSAGE_THREAD
    struct rusage usage;
    if (getrusage(RUSAGE_THREAD, &usage) == 0) {
      faults = usage.ru_majflt;
      inblock = usage.ru_inblock;
    }
#endif
  }

  // Off: turn on when faults since the last check reach 1/ON_RATIO of the
  // units consumed.  A fault on a block filesystem reads synchronously in
  // this thread, so faults without input blocks mean the counter is dead
  // (one alone can be a page another thread was reading, or a zswap load).
  void checkFaults() {
    long faults, inblock;
    sample(faults, inblock);
    long newFaults = faults - gate.faults;
    if (newFaults > 0 && newFaults * ON_RATIO >= gate.units) {
      gate.on = true;
      if (newFaults >= 2 && inblock == gate.inblock) gate.blind = true;
    }
    gate.faults = faults;
    gate.inblock = inblock;
    gate.units = 0;
  }

  // Issues the advice that queue() adds and returns the pages it read.
  template <class Queue>
  int64_t adviseBatch(Queue&& queue) {
    long faults, before, after;
    sample(faults, before);
    queue();
    flush();
    sample(faults, after);
    gate.faults = faults;
    gate.inblock = after;
    gate.units = 0;
    return (int64_t)(after - before) * 512 / (int64_t)pageSize();
  }

  // On: turn off when a batch of `units` units read fewer than 1/OFF_RATIO
  // of their worth of pages.
  void checkCold(int64_t coldPages, int64_t units) {
    if (!gate.blind && coldPages * OFF_RATIO < units) gate.on = false;
  }

  void flush() {
    if (runEnd > runStart) file->prefetch(runStart, runEnd - runStart);
    runStart = runEnd = 0;
  }

  void scanStep(int64_t rank);

public:
  RowReadAhead(Gate& gate, InputFile* file, size_t base, size_t stride, int64_t rows)
    : gate(gate), file(file), base(base), stride(stride), rows(rows) {
#ifndef RUSAGE_THREAD
    due = NEVER;
#endif
    if (!enabled || file == nullptr || stride == 0 || rows <= 0) due = NEVER;
  }

  /// A fixed-size column's values.
  RowReadAhead(Gate& gate, StrColReader& col)
    : RowReadAhead(gate, col.valuesFile(), col.valuesOffset(),
                   col.isFixedSize() ? (size_t)col.fixedValueSize() : 0, col.numValues()) {}

  // A shared gate counts the units consumed after the last step.
  ~RowReadAhead() { gate.units += std::max<int64_t>(0, count - last); }

  RowReadAhead(const RowReadAhead&) = delete;
  RowReadAhead& operator=(const RowReadAhead&) = delete;

  bool active() const { return gate.on; }

  /// Queues rows [first, end) for advice (clamped to the column).  Adjacent
  /// and overlapping ranges in ascending order coalesce into one prefetch.
  void queueRows(int64_t first, int64_t end) {
    first = std::max<int64_t>(first, 0);
    end = std::min(end, rows);
    if (first >= end) return;
    size_t mask = pageSize() - 1;
    size_t start = (base + (size_t)first * stride) & ~mask;
    size_t stop = (base + (size_t)end * stride + mask) & ~mask;
    if (runEnd != 0 && start <= runEnd && stop >= runStart) {
      batchBytes += stop > runEnd ? stop - runEnd : 0;
      runStart = std::min(runStart, start);
      runEnd = std::max(runEnd, stop);
      return;
    }
    batchBytes += stop - start;
    flush();
    runStart = start;
    runEnd = stop;
  }

  /// Turns advice on when any of the rows was not resident (and reads them).
  /// Pass rows away from the column's ends: their pages may be shared with
  /// neighbouring data.  The gate's fault baseline is left alone, so faults
  /// since the last check still count at the next one.
  void probe(std::initializer_list<int64_t> ranks) {
    if (due == NEVER || gate.on) return;
    long faults, before, after;
    sample(faults, before);
    for (int64_t rank : ranks) {
      queueRows(rank, rank + 1);
      flush();
    }
    sample(faults, after);
    if (after > before) gate.on = true;
  }

  /// Sequential: call with each rank the scan reaches, in order.
  LUXIR_INLINE void scanAt(int64_t rank) {
    if (rank >= due) scanStep(rank);
  }

  /// Sparse: pos of total units consumed so far; queue(k) queues unit k
  /// through queueRows(), and is called with ascending k.
  template <class Queue>
  LUXIR_INLINE void sparseAt(int64_t pos, int64_t total, Queue&& queue) {
    if (pos >= due) sparseStep(pos, total, queue);
  }

  template <class Queue>
  void sparseStep(int64_t pos, int64_t total, Queue& queue) {
    count = total;
    gate.units += pos - last;
    last = pos;
    if (!gate.on && gate.units < SPARSE_CHECK_UNITS) {
      due = pos + SPARSE_CHECK_UNITS - gate.units;
      return;
    }
    if (!gate.on) checkFaults();
    if (gate.on) {
      int64_t from = std::max(ahead, pos);
      int64_t target = std::min(count, pos + SPARSE_AHEAD_UNITS);
      batchBytes = 0;
      int64_t coldPages = adviseBatch([&]() {
        for (ahead = from; ahead < target && batchBytes < SPARSE_BATCH_BYTES; ahead++) queue(ahead);
      });
      checkCold(coldPages, ahead - from);
      if (ahead >= count) {
        due = NEVER;
        return;
      }
      if (gate.on) {
        due = pos + std::max<int64_t>(1, (ahead - pos) / 2);
        return;
      }
    }
    due = pos + SPARSE_CHECK_UNITS;
  }
};

inline void RowReadAhead::scanStep(int64_t rank) {
  int64_t half = std::max<int64_t>(1, SCAN_WINDOW_BYTES / 2 / (int64_t)stride);
  if (rank == 0) probe({rows / 4, rows / 2 + rows / 4});
  gate.units++;
  if (!gate.on) checkFaults();
  if (gate.on) {
    int64_t from = std::max(ahead, rank);
    int64_t target = std::min(rows, rank + 2 * half);
    if (from < target) checkCold(adviseBatch([&]() { queueRows(from, target); }), 1);
    ahead = target;
  }
  due = rank + half;
}

} // namespace luxir
