// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Signal.h"

namespace luxir {

std::unique_ptr<Signal::map_type> Signal::callbacks;
// protected by mutex
std::mutex Signal_mutex;

void* Signal::emit_(std::string_view name, void* a, void* b, void* c) {
    // Copy the callback out under the lock, then invoke it WITHOUT the lock
    // held.  Holding Signal_mutex across the callback would let a blocking
    // listener (e.g. a test that waits on a latch to reproduce a timing
    // window) stall every other thread's emit on the global mutex - a
    // deadlock when the blocked thread is the one expected to release it.
    // The std::function copy keeps the callback valid even if another thread
    // unlistens/clears while it runs.
    callback_type cb;
    {
      std::lock_guard<std::mutex> lock(Signal_mutex);
      if (!callbacks) {
        return nullptr;
      }
      auto it = callbacks->find(name);
      if (it == callbacks->end()) {
        return nullptr;
      }
      cb = it->second;
    }
    return cb(a, b, c);
}

void Signal::listen(std::string_view name, callback_type&& callback) {
  std::lock_guard<std::mutex> lock(Signal_mutex);
  if (!callbacks) {
    callbacks = std::make_unique<map_type>();
  }
  (*callbacks)[name] = callback;
}

void Signal::unlisten(std::string_view name) {
  std::lock_guard<std::mutex> lock(Signal_mutex);
  if (!callbacks) {
    return;
  }
  callbacks->erase(name);
  if (callbacks->empty()) {
    callbacks.reset();
  }
}

void Signal::clear() {
  std::lock_guard<std::mutex> lock(Signal_mutex);
  callbacks.reset();
}

} // namespace luxir