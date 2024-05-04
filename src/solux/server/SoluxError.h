#pragma once

#include <memory>
#include <string>
#include <exception>
#include <stacktrace>
#include "solux/util/log.h"

namespace solux {


// TODO: have subclasses that can have more specific info?
// Example: ids (outside of the error message) of the document(s) that caused the error, etc.
class SoluxError {
public:
  int64_t error_code;
  // perhaps a list of errors to provide more context as error propagation occurs?  Or we could just modify the error string.
  std::string message;  // guaranteed to be set in the event of an exception, no need to touch eptr just for that.
  std::exception_ptr eptr;

  SoluxError(const std::exception& e) : eptr(std::current_exception()) {
    message = std::string("Unexpected exception: ").append(e.what());
    error_code = 1;
  }

  const std::string& what() const {
    return message;
  }
};

// A generic error holder to propagate errors and return values.  Currently designed to be cheap if no errors.
class ErrorHolder {
  // different threads may try to set an error state concurrently, so we need some level of concurrency control.
  std::atomic<SoluxError*> error;
public:
  ErrorHolder() = default;
  ErrorHolder(const ErrorHolder& other) = delete;
  ErrorHolder& operator=(const ErrorHolder& other) = delete;

  ~ErrorHolder() {
    if (error) {
      delete error;
    }
  }

  // Sets a new error only if no error is already set and returns nullptr if successful.
  // If there was an error previously set, no modification is made and the previous error is returned.
  SoluxError* setSoluxError(std::unique_ptr<SoluxError>&& e) {
    SoluxError* err = e.release();
    SoluxError* expected = nullptr;
    if (!error.compare_exchange_strong(expected, err)) {
      // another error is already set.
      e.reset(err);  // give ptr back to the unique_ptr
    }
    return expected;
  }

  // Sets a new error only if no error is already set and returns nullptr if successful.
  // If there was an error previously set, no modification is made and the previous error is returned.
  SoluxError* setException(const std::exception& e) {
    auto err = std::make_unique<SoluxError>(e);
    // std::stacktrace coming in gcc14 (but not sure if it will provide exception backtrace)
    // msg += "\n" + boost::stacktrace::to_string(boost::stacktrace::stacktrace());  // not in a released version of boost yet
    LOG_ERROR(err->what());
    return setSoluxError(std::move(err));
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

  const char* what() {
    SoluxError* err = error.load();
    if (err) {
      return err->what().c_str();
    } else {
      return "(ok)";
    }
  }

  SoluxError* getError() {
    return error.load();
  }
};


}