#pragma once

#include <functional>
#include <string_view>
#include "boost/unordered/unordered_flat_map.hpp"

namespace solux {

// This is a simple callback mechanism by name meant for use by tests.
// If we need callbacks in non-test code, we should use a more robust signal-slot library.
// Because this is only used for tests, listeners are currently cleared before each test.
// If you want the duration of a listener to be shorter, you can either unlisten or use
// a scope_guard:
//    auto cleaner = solux::scope_guard([](){ solux::Signal::unlisten("mergeStart");});
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

} // namespace solux
