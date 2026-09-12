// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>
#include <exception>
#include <execinfo.h>
#include <unwind.h>
#include <cxxabi.h>
#include "luxir/util/ApiError.h"
#include "luxir/util/log.h"

namespace luxir {

// Helper function to get stack trace
inline std::string getStackTrace() {
  constexpr int maxFrames = 20;
  struct Frames {
    void* addresses[maxFrames];
    int size = 0;
  } frames;
  // glibc's backtrace() dlopens libgcc_s even with a statically linked GCC
  // runtime. Capture through the linked unwinder; symbolization stays in glibc.
  _Unwind_Backtrace([](_Unwind_Context* context, void* data) {
    auto& frames = *static_cast<Frames*>(data);
    auto address = _Unwind_GetIP(context);
    if (address == 0) return _URC_END_OF_STACK;
    frames.addresses[frames.size++] = reinterpret_cast<void*>(address);
    return frames.size == maxFrames ? _URC_END_OF_STACK : _URC_NO_REASON;
  }, &frames);
  char** strings = backtrace_symbols(frames.addresses, frames.size);
  if (strings == nullptr) return {};

  std::string result;
  for (int i = 0; i < frames.size; i++) {
    // Try to demangle C++ names
    char* mangled_name = nullptr;
    char* offset_begin = nullptr;
    char* offset_end = nullptr;

    // Find parentheses and +address offset surrounding mangled name
    for (char* p = strings[i]; *p; ++p) {
      if (*p == '(') {
        mangled_name = p;
      } else if (*p == '+') {
        offset_begin = p;
      } else if (*p == ')' && offset_begin) {
        offset_end = p;
        break;
      }
    }

    if (mangled_name && offset_begin && offset_end && mangled_name < offset_begin) {
      *mangled_name++ = '\0';
      *offset_begin++ = '\0';
      *offset_end = '\0';

      int status;
      char* real_name = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
      if (status == 0) {
        result += "  ";
        result += strings[i];
        result += "(";
        result += real_name;
        result += "+";
        result += offset_begin;
        result += ")\n";
        free(real_name);
      } else {
        // Preserve addresses when a stripped/static binary has no symbol name.
        *(mangled_name - 1) = '(';
        *(offset_begin - 1) = '+';
        *offset_end = ')';
        result += "  ";
        result += strings[i];
        result += "\n";
      }
    } else {
      result += "  ";
      result += strings[i];
      result += "\n";
    }
  }

  free(strings);
  return result;
}

// The failure recorded on an update message: its client-facing classification
// plus the exception for callers that want to rethrow.  The message is the
// exception's own text - diagnostics such as stack traces go to the log, never
// to the client.
class LuxirError {
public:
  ErrorInfo info;
  std::exception_ptr eptr;

  LuxirError(const std::exception& e, ErrorKind fallback)
    : info(classifyException(e, fallback)), eptr(std::current_exception()) {}

  explicit LuxirError(ErrorInfo info) : info(std::move(info)) {}

  const std::string& what() const {
    return info.message;
  }
};

// A generic thread-safe error holder to propagate errors and return values.  Currently designed to be cheap if no errors.
class ErrorHolder {
private:
  // different threads may try to set an error state concurrently, so we need some level of concurrency control.
  std::atomic<LuxirError*> error;
public:
  ErrorHolder() = default;
  ErrorHolder(const ErrorHolder& other) = delete;
  ErrorHolder& operator=(const ErrorHolder& other) = delete;

  ~ErrorHolder() {
    if (errored()) {
      // If the relaxed load indicated there was an error, we need to do an actual acquire load to safely delete.
      delete error.load(std::memory_order_acquire);
    }
  }

  // Sets a new error only if no error is already set and returns nullptr if successful.
  // If there was an error previously set, no modification is made and the previous error is returned.
  // For thread safety, make all modifications to the LuxirError before setting it.
  LuxirError* setLuxirError(std::unique_ptr<LuxirError>&& e) {
    LuxirError* err = e.release();
    LuxirError* expected = nullptr;
    if (!error.compare_exchange_strong(expected, err)) {
      // another error is already set.
      e.reset(err);  // give ptr back to the unique_ptr
    }
    return expected;
  }

  // Sets a new error only if no error is already set and returns nullptr if successful.
  // If there was an error previously set, no modification is made and the previous error is returned.
  // An exception that is not an ApiError is classified as `fallback`; the update
  // graph's default is INTERNAL because a request-class failure inside it is
  // expected to announce itself with a typed throw.
  LuxirError* setException(const std::exception& e, ErrorKind fallback = ErrorKind::INTERNAL) {
    auto err = std::make_unique<LuxirError>(e, fallback);
    if (dynamic_cast<const ApiError*>(&e) != nullptr) {
      LOG_WARN("update rejected: {}", err->what());
    } else {
      LOG_ERROR("update failed: {}\nStack trace:\n{}", err->what(), getStackTrace());
    }
    return setLuxirError(std::move(err));
  }

  // Returns true if no error.
  // Checking if there is an error does not use memory barriers (i.e. you are not guaranteed to
  // instantly see that an error has occured on another thread.  This should normally be fine as
  // order between different threads is already undefined w/o additional synchronization.
  bool ok() const {
    // We don't really even need an atomic load here since we are just checking for non-zero (i.e. a partial read
    // wouldn't matter.) but I don't think we can do better w/o an atomic_ref.
    return error.load(std::memory_order_relaxed) == nullptr;
  }

  // Returns true if there was an error.
  bool errored() const {
    return !ok();
  }

  std::string_view what() const {
    LuxirError* err = error.load(std::memory_order_acquire);
    if (err) {
      return err->what();
    } else {
      return "(ok)";
    }
  }

  // The recorded failure; only meaningful when errored().
  const ErrorInfo& info() const {
    return error.load(std::memory_order_acquire)->info;
  }

  // Don't use this method to check for an error, use !ok() or errored() instead.
  LuxirError* getError() const {
    return error.load(std::memory_order_acquire);
  }
};


}
