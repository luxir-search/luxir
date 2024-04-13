#include "Signal.h"

namespace solux {

std::unique_ptr<Signal::map_type> Signal::callbacks;
// protected by mutex
std::mutex Signal_mutex;

void* Signal::emit_(std::string_view name, void* a, void* b, void* c) {
    std::lock_guard<std::mutex> lock(Signal_mutex);
    if (!callbacks) {
      return nullptr;
    }
    auto it = callbacks->find(name);
    if (it == callbacks->end()) {
      return nullptr;
    }
    return it->second(a, b, c);
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

} // namespace solux