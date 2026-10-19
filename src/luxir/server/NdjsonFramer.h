// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace luxir {

// Fixed-capacity storage that framed records point into.  Bytes never move
// once written, so whoever holds a record's chunk keeps the record alive.
struct NdjsonChunk {
  std::unique_ptr<char[]> data;
  std::size_t capacity = 0;
  std::size_t size = 0;
};

// Splits an NDJSON body into records without copying them: the body is read
// straight into the current chunk (writable()/commit()) and next() returns
// views into it.  Only bytes still pending when a chunk fills - normally one
// incomplete record - are copied, into the next chunk.  Records are found on
// demand, so an oversized record fails in stream order, after the records
// before it.
class NdjsonFramer {
  // A read gets at least this much space; less starts a new chunk.
  static constexpr std::size_t kMinReadBytes = 4096;

  std::size_t maxRecordBytes_;
  std::size_t chunkBytes_;
  std::shared_ptr<NdjsonChunk> chunk_;
  std::size_t start_ = 0;  // first byte of the next record in chunk_
  std::size_t scan_ = 0;   // where the newline search resumes
  std::string errorMessage_;
  bool finished_ = false;

  // The chunk stays: a read in flight may still be writing into it.
  void fail(std::string message) {
    if (errorMessage_.empty()) errorMessage_ = std::move(message);
  }

  // Moves the pending bytes into a new chunk.  Doubling past the pending size
  // keeps a record larger than a chunk linear to accumulate.
  void startChunk() {
    std::size_t pending = chunk_ ? chunk_->size - start_ : 0;
    auto next = std::make_shared<NdjsonChunk>();
    next->capacity = std::max(chunkBytes_, 2 * pending + kMinReadBytes);
    next->data = std::make_unique_for_overwrite<char[]>(next->capacity);
    if (pending > 0) std::memcpy(next->data.get(), chunk_->data.get() + start_, pending);
    next->size = pending;
    scan_ -= start_;
    start_ = 0;
    chunk_ = std::move(next);
  }

  static std::string_view trimCr(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
  }

public:
  explicit NdjsonFramer(std::size_t maxRecordBytes = 1024 * 1024, std::size_t chunkBytes = 1024 * 1024)
    : maxRecordBytes_(maxRecordBytes), chunkBytes_(std::max(chunkBytes, kMinReadBytes)) {}

  // Space for the next read of at most maxBytes; report what was read with
  // commit().  Call only once next() has returned false, with no read in flight.
  std::span<char> writable(std::size_t maxBytes) {
    if (!chunk_ || chunk_->capacity - chunk_->size < kMinReadBytes) startChunk();
    return {chunk_->data.get() + chunk_->size, std::min(maxBytes, chunk_->capacity - chunk_->size)};
  }
  void commit(std::size_t bytes) { chunk_->size += bytes; }

  // Copies bytes in, for a caller that already holds them.
  void feed(std::string_view bytes) {
    while (!bytes.empty()) {
      auto out = writable(bytes.size());
      std::memcpy(out.data(), bytes.data(), out.size());
      commit(out.size());
      bytes.remove_prefix(out.size());
    }
  }

  // The chunk holding the records returned so far from it; it changes only in
  // writable().
  const std::shared_ptr<NdjsonChunk>& chunk() const { return chunk_; }

  // The next complete record, skipping empty lines and stripping a trailing
  // '\r'.  The view lives as long as chunk().
  bool next(std::string_view& record) {
    while (chunk_ && !error() && !finished_) {
      const char* base = chunk_->data.get();
      const void* nl = std::memchr(base + scan_, '\n', chunk_->size - scan_);
      if (nl == nullptr) {
        scan_ = chunk_->size;
        if (scan_ - start_ > maxRecordBytes_) fail("NDJSON record exceeds maximum size");
        return false;
      }
      std::size_t end = (std::size_t)((const char*)nl - base);
      std::string_view line = trimCr(std::string_view(base + start_, end - start_));
      start_ = scan_ = end + 1;
      if (line.empty()) continue;
      if (line.size() > maxRecordBytes_) {
        fail("NDJSON record exceeds maximum size");
        return false;
      }
      record = line;
      return true;
    }
    return false;
  }

  // The unterminated final record at end of body, once next() is exhausted.
  bool finish(std::string_view& record) {
    if (error() || finished_) return false;
    finished_ = true;
    if (!chunk_) return false;
    std::string_view line = trimCr(std::string_view(chunk_->data.get() + start_, chunk_->size - start_));
    start_ = scan_ = chunk_->size;
    if (line.empty()) return false;
    if (line.size() > maxRecordBytes_) {
      fail("NDJSON record exceeds maximum size");
      return false;
    }
    record = line;
    return true;
  }

  bool error() const { return !errorMessage_.empty(); }
  const std::string& message() const { return errorMessage_; }
};

}  // namespace luxir
