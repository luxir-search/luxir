// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "luxir/util/Clock.h"

namespace luxir {

/// Generates document ids for SchemaDef.id_mode GENERATE: 64-bit snowflakes
///
///   0 | 41 bits ms since EPOCH_MS | 10 bits writer lifetime | 12 bits sequence
///
/// encoded as 13 fixed-width Crockford base32 characters, so string order is
/// numeric order and, within one writer lifetime, creation order.
///
/// Uniqueness needs no coordination service:
/// - One writer per collection. Every writer open claims the collection's
///   STATE file, which durably bumps the writer epoch; a superseded writer
///   cannot publish, so ids of a stale writer never become visible.
/// - The worker bits are the low 10 bits of that epoch. Each writer lifetime
///   gets a fresh namespace, so a restart that loses the in-memory clock never
///   reuses its predecessor's (ms, sequence) pairs, whatever the wall clock does.
/// - The clock is logical: it never moves backwards. A wall clock that steps
///   back keeps issuing from the last millisecond; an exhausted sequence
///   advances to the next millisecond instead of wrapping or sleeping.
/// - Each commit records the clock (IndexInfo.id_clock_ms), and a reopened
///   writer, including a promoted incarnation's, starts strictly above it, so
///   committed history is never revisited.
///
/// A collision would need two writer lifetimes 1024 epochs apart to issue the
/// same millisecond, which only the uncommitted ids of a crashed lifetime can
/// reach, and only with the wall clock behind them for all 1024 lifetimes.
class IdGenerator {
public:
  static constexpr int64_t EPOCH_MS = 1767225600000;  // 2026-01-01T00:00:00Z
  static constexpr int TIME_BITS = 41;
  static constexpr int WORKER_BITS = 10;
  static constexpr int SEQ_BITS = 12;
  static constexpr uint64_t WORKER_MASK = (1ULL << WORKER_BITS) - 1;
  static constexpr uint64_t MAX_MS = (1ULL << TIME_BITS) - 1;
  static constexpr size_t ENCODED_LEN = 13;

private:
  // The last issued (ms << SEQ_BITS | seq); the next id is one more, or a
  // later millisecond's first sequence.
  std::atomic<uint64_t> state{0};
  uint64_t worker = 0;

public:
  IdGenerator() = default;
  IdGenerator(uint64_t writerEpoch, uint64_t floorMs) { reset(writerEpoch, floorMs); }

  // Starts strictly above floorMs, so committed history is never revisited
  // whatever the worker bits (a promoted incarnation restarts its epochs).
  void reset(uint64_t writerEpoch, uint64_t floorMs) {
    worker = writerEpoch & WORKER_MASK;
    uint64_t start = floorMs == 0 ? 0 : std::min(floorMs + 1, MAX_MS);
    state.store(start << SEQ_BITS, std::memory_order_relaxed);
  }

  /// The logical clock: every id issued so far is at or below this millisecond.
  uint64_t clockMs() const { return state.load(std::memory_order_relaxed) >> SEQ_BITS; }

  static uint64_t sinceEpoch(int64_t epochMillis) {
    return epochMillis > EPOCH_MS ? (uint64_t)(epochMillis - EPOCH_MS) : 0;
  }

  uint64_t next() { return next(currentEpochMillis()); }

  uint64_t next(int64_t nowEpochMillis) {
    uint64_t now = sinceEpoch(nowEpochMillis);
    uint64_t cur = state.load(std::memory_order_relaxed);
    uint64_t issued;
    do {
      // Sequence overflow carries into the millisecond.
      issued = (now > (cur >> SEQ_BITS)) ? now << SEQ_BITS : cur + 1;
    } while (!state.compare_exchange_weak(cur, issued, std::memory_order_relaxed));
    uint64_t ms = issued >> SEQ_BITS;
    if (ms > MAX_MS) throw std::overflow_error("generated id clock exhausted");
    return (ms << (WORKER_BITS + SEQ_BITS)) | (worker << SEQ_BITS) | (issued & ((1ULL << SEQ_BITS) - 1));
  }

  /// Writes exactly ENCODED_LEN characters.
  static std::string_view encode(uint64_t id, char* out) {
    static constexpr char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (size_t i = ENCODED_LEN; i-- > 0;) {
      out[i] = ALPHABET[id & 31];
      id >>= 5;
    }
    return {out, ENCODED_LEN};
  }
};

} // namespace luxir
