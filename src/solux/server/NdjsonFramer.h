#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>

namespace solux {

class NdjsonFramer {
  std::size_t maxRecordBytes_;
  std::string carry_;
  std::deque<std::string> records_;
  std::string current_;
  std::string errorMessage_;
  bool finished_ = false;

  void fail(std::string message) {
    if (!errorMessage_.empty()) return;
    errorMessage_ = std::move(message);
    carry_.clear();
    records_.clear();
    current_.clear();
  }

  void pushRecord(std::string_view record) {
    if (!record.empty() && record.back() == '\r') record.remove_suffix(1);
    if (record.empty()) return;
    if (record.size() > maxRecordBytes_) {
      fail("NDJSON record exceeds maximum size");
      return;
    }
    records_.emplace_back(record);
  }

public:
  explicit NdjsonFramer(std::size_t maxRecordBytes = 1024 * 1024)
    : maxRecordBytes_(maxRecordBytes) {}

  void feed(std::string_view bytes) {
    if (error() || finished_) return;
    current_.clear();
    carry_.append(bytes.data(), bytes.size());

    std::size_t start = 0;
    while (true) {
      std::size_t nl = carry_.find('\n', start);
      if (nl == std::string::npos) break;
      pushRecord(std::string_view(carry_).substr(start, nl - start));
      if (error()) return;
      start = nl + 1;
    }

    if (start > 0) carry_.erase(0, start);
    if (carry_.size() > maxRecordBytes_) {
      fail("NDJSON record exceeds maximum size");
    }
  }

  bool next(std::string_view& record) {
    if (records_.empty()) return false;
    current_ = std::move(records_.front());
    records_.pop_front();
    record = current_;
    return true;
  }

  bool finish(std::string_view& record) {
    if (error() || finished_) return false;
    current_.clear();
    finished_ = true;
    if (carry_.empty()) return false;
    current_ = std::move(carry_);
    carry_.clear();
    if (!current_.empty() && current_.back() == '\r') current_.pop_back();
    if (current_.empty()) return false;
    if (current_.size() > maxRecordBytes_) {
      fail("NDJSON record exceeds maximum size");
      return false;
    }
    record = current_;
    return true;
  }

  bool error() const { return !errorMessage_.empty(); }
  const std::string& message() const { return errorMessage_; }
};

}  // namespace solux
