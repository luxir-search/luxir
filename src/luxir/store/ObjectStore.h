// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luxir {

/// A flat key/value store of immutable byte objects: the contract an S3-like
/// service offers, and all ObjectStoreDirectory needs from one. Names are flat
/// keys; "directories" are only a naming convention of the caller. Every
/// method is thread safe and throws std::runtime_error on failure.
class ObjectStore {
public:
  struct ObjectInfo {
    std::string name;  // the full key, relative to the store
    uint64_t size = 0;
  };

  virtual ~ObjectStore() = default;

  /// Every object whose name starts with `prefix`, sorted by name.
  virtual std::vector<ObjectInfo> list(std::string_view prefix) = 0;
  /// Size of the object, or nullopt when it does not exist.
  virtual std::optional<uint64_t> size(std::string_view name) = 0;
  /// Fills `out` from [offset, offset + out.size()). Throws on a short read
  /// or a missing object.
  virtual void readRange(std::string_view name, uint64_t offset, std::span<char> out) = 0;
  /// Replaces the object atomically: readers see the old or the new bytes,
  /// never a mix. With `ifAbsent` the put is a conditional create and fails
  /// when the object exists: it throws std::runtime_error whose message
  /// contains "already exists".
  virtual void put(std::string_view name, std::string_view bytes, bool ifAbsent = false) = 0;
  /// True when the object existed.
  virtual bool remove(std::string_view name) = 0;
  /// Server-side copy; replaces `to`. Throws when `from` does not exist.
  virtual void copy(std::string_view from, std::string_view to) = 0;

  /// The distinct names of the immediate "subdirectories" under `prefix`
  /// (`prefix` ends with '/'): "a/b/c" and "a/b/d" under "a/" give "b". The
  /// default lists everything under the prefix; a backend with a delimiter
  /// listing (S3) overrides it.
  virtual std::vector<std::string> listDirectories(std::string_view prefix);

  /// One line for logs.
  virtual std::string describe() const { return "object_store"; }
};

/// An in-memory ObjectStore with request accounting, for tests, demos and the
/// ram_object backend. Stores are shared by name so that a node restarted in
/// the same process finds its data again; see named().
class RamObjectStore : public ObjectStore {
public:
  struct Request {
    std::string op;    // list, size, read, put, remove, copy
    std::string name;  // the object, or the prefix of a list
    uint64_t offset = 0;
    uint64_t length = 0;  // bytes read or written
  };

private:
  mutable std::mutex mutex;
  std::map<std::string, std::string, std::less<>> objects;
  std::vector<Request> log;
  std::string label;
  std::atomic<uint64_t> requests_{0};
  std::atomic<uint64_t> bytesRead_{0};
  std::atomic<uint64_t> bytesWritten_{0};

  void record(std::string_view op, std::string_view name, uint64_t offset, uint64_t length);

public:
  explicit RamObjectStore(std::string label = "ram") : label(std::move(label)) {}

  /// The process-wide store of this name, created empty on first use.
  static std::shared_ptr<RamObjectStore> named(std::string_view name);
  /// Forgets the named store; existing holders keep theirs.
  static void drop(std::string_view name);

  std::vector<ObjectInfo> list(std::string_view prefix) override;
  std::optional<uint64_t> size(std::string_view name) override;
  void readRange(std::string_view name, uint64_t offset, std::span<char> out) override;
  void put(std::string_view name, std::string_view bytes, bool ifAbsent = false) override;
  bool remove(std::string_view name) override;
  void copy(std::string_view from, std::string_view to) override;
  std::string describe() const override { return "ram_object:" + label; }

  /// Every call counts as a request.
  uint64_t requests() const { return requests_.load(std::memory_order_relaxed); }
  uint64_t bytesRead() const { return bytesRead_.load(std::memory_order_relaxed); }
  uint64_t bytesWritten() const { return bytesWritten_.load(std::memory_order_relaxed); }
  /// Requests of one op ("read", "list", ...), optionally only on names starting with `prefix`.
  uint64_t requests(std::string_view op, std::string_view prefix = {}) const;
  /// The request log, in order.
  std::vector<Request> requestLog() const;
  /// Bytes read from objects whose name starts with `prefix`.
  uint64_t bytesRead(std::string_view prefix) const;
  void resetStats();
  /// Total bytes of the objects whose names start with `prefix`.
  uint64_t storedBytes(std::string_view prefix = {});
};

}  // namespace luxir
