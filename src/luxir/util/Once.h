// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <mutex>
#include <utility>

namespace luxir {

/// std::once_flag whose throwing calls are retried on every platform. Outside
/// Linux, libstdc++ builds std::call_once on pthread_once, which stays locked
/// when the callable throws, so the next call aborts (GCC bug 66146).
class OnceFlag {
  std::atomic<bool> done{false};
  std::mutex mutex;

  template <typename F>
  friend void callOnce(OnceFlag& flag, F&& f);
};

/// Runs f unless an earlier call returned normally; a call that throws leaves
/// the flag unset, so the next caller runs f again.
template <typename F>
void callOnce(OnceFlag& flag, F&& f) {
  if (flag.done.load(std::memory_order_acquire)) return;
  std::lock_guard lock(flag.mutex);
  if (flag.done.load(std::memory_order_relaxed)) return;
  std::forward<F>(f)();
  flag.done.store(true, std::memory_order_release);
}

}
