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

  const std::string& what() const {
    return message;
  }
};

// A generic error holder to propagate errors and return values.  Currently designed to be cheap to copy if no errors.
// Making a copy if there are errors will normally need dynamic memory allocation.
class ErrorHolder {
public:
  std::unique_ptr<SoluxError> error;

  ErrorHolder() = default;

  void setException(const std::exception& e) {
    error = std::make_unique<SoluxError>();
    error->error_code = 1;
    error->eptr = std::current_exception();
    std::string msg = std::string("Unexpected exception: ").append(e.what());
    // std::stacktrace coming in gcc14 (but not sure if it will provide exception backtrace)
    // msg += "\n" + boost::stacktrace::to_string(boost::stacktrace::stacktrace());  // not in a released version of boost yet
    LOG_ERROR(msg);
    error->message = std::move(msg);
  }

  ErrorHolder(const ErrorHolder& other) {
    if (other.error) {
      error = std::make_unique<SoluxError>(*other.error);
    }
  }

  bool ok() {
    return !error;
  }

  const char* what() {
    if (error) {
      return error->what().c_str();
    } else {
      return "(ok)";
    }
  }
};


}