// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <string_view>
#include "boost/unordered/unordered_flat_map.hpp"

namespace luxir {

// This is a simple callback mechanism by name meant for use by tests.
// If we need callbacks in non-test code, we should use a more robust signal-slot library.
// Because this is only used for tests, listeners are currently cleared before each test.
// If you want the duration of a listener to be shorter, you can either unlisten or use
// a scope_guard:
//    auto cleaner = luxir::scope_guard([](){ luxir::Signal::unlisten("mergeStart");});
//
// LISTENER LIFETIME: emit() copies the matched listener and invokes it WITHOUT
// holding the internal lock, so a blocking listener (e.g. one that waits on a
// latch to reproduce a timing window) cannot deadlock other threads' emits.
// The consequence is that unlisten()/clear()/replacing a listener does NOT
// synchronize with an in-flight callback: a callback can still be running -
// or even start, if an emit copied it just before removal - after it was
// removed.  So do not remove or replace a listener, or destroy anything it
// captures by reference (latches, locals), while an emit for that signal can
// still be delivered.  The usual safe pattern is to join the threads that emit
// before unlisten().
class Signal {
  using callback_type = std::function<void*(void*, void*, void*)>;
  using map_type = boost::unordered_flat_map<std::string_view, callback_type>;
  static std::unique_ptr<map_type> callbacks;

  static void* emit_(std::string_view name, void* a=nullptr, void* b=nullptr, void* c=nullptr);
public:
  Signal() = delete;
  static void* emit(std::string_view name, void* a=nullptr, void* b=nullptr, void* c=nullptr) {
    // This is the only place where we access callbacks without a lock.  If the variable is
    // cached in a register (i.e. if the code is in a relatively tight loop), then callbacks
    // may not fire until that code is left.  This is a reasonable tradeoff for performance and to
    // minimize the impact on production code given that this is only used in tests.
    if (!callbacks) {
      [[likely]]
      return nullptr;
    }
    // call non-inline method to do the best to minimize code bloat.
    return emit_(name, a, b, c);
  }

  static void listen(std::string_view name, callback_type&& callback);
  static void unlisten(std::string_view name);
  static void clear();
};

} // namespace luxir
