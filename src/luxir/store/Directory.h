// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include <string_view>
#include <gtl/btree.hpp>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>
#include "luxir/util/ApiError.h"
#include "luxir/util/log.h"
#include "luxir/util/Signal.h"
#include "OutputStream.h"

// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define DIR_DEBUG LOG_TRACE
// #define DIR_DEBUG LOG_DEBUG

namespace luxir {

/// Thrown when a mutation is attempted against storage opened read-only.
/// Read-only is a node-wide mode (--read-only), but it is enforced here so
/// that no path - present or future - can put bytes in a data directory this
/// process does not hold the write lock on.
class ReadOnlyError : public ApiError {
public:
  explicit ReadOnlyError(const std::string& message)
    : ApiError(ErrorKind::FAILED_PRECONDITION, "read_only", message) {}
};

// Size and xxh3 of a file's bytes, as a manifest FileDescriptor records them.
struct FileCheck {
  uint64_t size = 0;
  uint64_t xxh3 = 0;
  bool matches(const FileDescriptor& expected) const { return size == expected.size && xxh3 == expected.xxh3; }
};

// One file open for a sequential checksum pass; see Directory::openChecksum.
class ChecksumReader {
public:
  virtual ~ChecksumReader() = default;
  virtual uint64_t size() const = 0;
  // Maps this held file, without reopening its name. The InputFile outlives
  // this handle; callers verify before mapping potentially corrupt bytes.
  virtual std::shared_ptr<InputFile> map() = 0;
  virtual FileCheck checksum(const std::function<void(size_t)>& progress = {}) = 0;
};

// Implementations of Directory are thread safe.
class Directory {
public:
  enum class PublishMode { CREATE_ONLY, REPLACE };
  // Backend-defined value, compared only for equality.
  using PublicationVersion = std::string;
  struct PublishCondition {
    PublishMode mode;
    std::optional<PublicationVersion> version;
    PublishCondition(PublishMode mode) : mode(mode) {}
    static PublishCondition match(PublicationVersion version) {
      PublishCondition result(PublishMode::REPLACE);
      result.version = std::move(version);
      return result;
    }
  };
  struct Publication {
    std::string bytes;
    PublicationVersion version;
  };

  struct FileCreateOptions {
    std::function<void(int64_t)> ramBytesChanged;
    size_t ramSpillBytes = 0;
    bool ramDelegating = false;
    std::optional<uint64_t> expectedSize;
  };

  struct FileInfo {
    std::string name;
    uint64_t size = 0;
  };

  // What a backend can do beyond the bare interface. Callers that can fall back
  // (reuseFiles) check a flag instead of assuming the strongest behavior.
  using Capabilities = uint32_t;
  static constexpr Capabilities HARD_LINK = 1u << 0;       // linkFile shares bytes instead of copying
  static constexpr Capabilities ATOMIC_RENAME = 1u << 1;   // backing store supports atomic name replacement
  static constexpr Capabilities DURABLE_SYNC = 1u << 2;    // sync() makes the files crash-safe (fsync)
  static constexpr Capabilities MEMORY_MAPPED = 1u << 3;   // openFile returns stable mapped bytes
  static constexpr Capabilities REMOTE = 1u << 4;          // bytes may be fetched over a network

  // Unknown backends claim nothing, which selects the conservative fallbacks.
  virtual Capabilities capabilities() const { return 0; }

  // A local directory where a task may spill scratch bytes it does not keep
  // (see ScratchBytes), or empty to keep them in RAM.
  virtual std::string scratchPath() const { return {}; }

  // TODO: add a prefix option?
  // Appends; the appended range is sorted by name.  Sizes are a best-effort
  // snapshot and entries vanishing mid-scan are skipped, never thrown.
  virtual void listFiles(std::vector<FileInfo> &target) = 0;

  // Open a file for reading.  If expectSynced is true, the caller asserts that
  // this file should have been fsynced (e.g. it was referenced from a committed
  // index).  CheckedDirectory uses this flag to detect missing fsyncs.
  virtual std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) = 0;

  // The caller asserts that `files` were fsynced: a root about to be published
  // references them (Manifest::commit). CheckedDirectory verifies it.
  virtual void expectSynced(std::span<const FileDescriptor> files) { unused(files); }

  // `files` are committed: a manifest loaded from this directory lists them,
  // with their sizes (Manifest::load). A directory may use that in place of
  // asking its backend, e.g. a remote one skips a size request per openFile.
  // Committed names are never rewritten with other bytes; a directory that
  // remembers them forgets a name it deletes, renames or writes again.
  virtual void knownFiles(std::span<const FileDescriptor> files) { unused(files); }

  virtual std::unique_ptr<File> createFile(const std::string_view name) = 0;

  // Backends that support delayed materialization may honor these options.
  // Other backends retain their normal behavior through this default.
  virtual std::unique_ptr<File> createFile(
      const std::string_view name, FileCreateOptions options) {
    unused(options);
    return createFile(name);
  }

  // Returns true if file was found and deleted, false if not found.
  virtual bool deleteFile(const std::string_view name) = 0;

  // Removes the named files durably. Missing names are allowed.
  virtual void deleteDurably(std::span<const std::string> names) = 0;

  virtual void deletePrefix(const std::string_view prefix) {
    std::vector<FileInfo> files;
    listFiles(files);
    for (const auto &file : files) {
      if (file.name.starts_with(prefix)) {
        deleteFile(file.name);
      }
    }
  }

  // Make the file readable to others through the Directory.  Putting this on the Directory class
  // gives more flexibility in implementation without having every File have to point back to it's
  // owning Directory.
  // Installs a complete immutable file; an existing name is an error.
  virtual void finishFile(File &file) = 0;

  // Installs bytes atomically and durably, returning a fresh version even for identical bytes.
  // An empty result means the condition failed and nothing changed.
  // A throw leaves the outcome unknown: publication may have taken place.
  virtual std::optional<PublicationVersion> publishFile(
      std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) = 0;
  // Bytes and version from one observation. Only absence returns nullopt.
  virtual std::optional<Publication> readPublished(std::string_view name) = 0;

  // Makes the named files' bytes AND directory entries durable. RAM is volatile.
  virtual void sync(std::span<const std::string> names) = 0;

  // remove all files from the directory
  virtual void clear() = 0;

  // Sum of file sizes from a single listFiles snapshot.
  uint64_t totalBytes() {
    std::vector<FileInfo> files;
    listFiles(files);
    uint64_t total = 0;
    for (const auto &file : files) {
      total += file.size;
    }
    return total;
  }

  virtual Directory& underlying() { return *this; }
  // Install an immutable file without copying its bytes (same storage backend).
  virtual void linkFile(Directory& source, std::string_view name) = 0;

  // Installs a copy of `source`'s file `name` under the same name. The fallback
  // for backends that cannot link; the caller syncs it like any created file.
  void copyFile(Directory& source, std::string_view name) {
    auto input = source.openFile(name, true);
    if (!input) throw std::runtime_error("copy source missing: " + std::string(name));
    auto data = input->read();
    auto file = createFile(name);
    OutputStream out(file.get());
    out.write(data.data(), data.size());
    out.close();
    finishFile(*file);
  }

  // Only descriptors from a published snapshot authorize reuse; filenames alone
  // are not identities across collection incarnations. A hard link cannot
  // change bytes, so this compares descriptors only: whether the source bytes
  // still match them is the at-rest question verify-on-open and the scrub ask.
  std::vector<FileDescriptor> reuseFiles(Directory& source, std::span<const FileDescriptor> available,
                                        std::span<const FileDescriptor> wanted) {
    std::map<std::string_view, const FileDescriptor*> byName;
    for (const auto& file : available) byName.emplace(file.name, &file);
    std::vector<FileDescriptor> reused;
    for (const auto& file : wanted) {
      auto it = byName.find(file.name);
      if (it == byName.end() || *it->second != file) continue;
      // An existing (even unreadable) candidate must be verified by its caller.
      bool present = true;
      try { present = openFile(file.name) != nullptr; } catch (const std::exception&) {}
      if (present) continue;
      if (capabilities() & HARD_LINK) linkFile(source, file.name);
      else copyFile(source, file.name);
      reused.push_back(file);
    }
    return reused;
  }

  // A file opened for one sequential checksum, apart from the mmap read path:
  // no page faults on the request threads' address space, no SIGBUS on a bad
  // sector, and the handle keeps the bytes readable after the name is
  // unlinked. Null when the file does not exist. With `direct` an FS backend
  // reads the media rather than the page cache where the filesystem allows.
  virtual std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) = 0;

  // Size and xxh3 of the file's bytes, or nullopt when it does not exist.
  // `progress` runs after each chunk with its byte count; it may sleep to pace
  // the read or throw to abandon it.
  std::optional<FileCheck> checksum(std::string_view name, const std::function<void(size_t)>& progress = {},
                                    bool direct = false) {
    auto reader = openChecksum(name, direct);
    if (!reader) return std::nullopt;
    return reader->checksum(progress);
  }
  virtual ~Directory() = default;
};


class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using InputReferenceType = std::shared_ptr<InputFileType>;

private:
  std::shared_ptr<StorageMemory> memory;
  struct Entry {
    InputReferenceType file;
    uint64_t version;
  };
  gtl::btree_map<std::string, Entry> files;
  inline static std::atomic<uint64_t> nextVersion{0};
  std::atomic<bool> materialized{false};
  std::mutex mutex;

public:
  explicit RAMDir(std::shared_ptr<StorageMemory> memory = std::make_shared<StorageMemory>()) : memory(std::move(memory)) {}
  // linkFile shares the immutable buffer; publication replaces names under the lock.
  Capabilities capabilities() const override { return HARD_LINK | ATOMIC_RENAME | MEMORY_MAPPED; }
  uint64_t storageBytes() const { return memory->bytes(); }
  bool exists() const { return materialized.load(std::memory_order_acquire); }

  // Only used by tests to clear (e.g. dir = RAMDir())
  void operator=(RAMDir&& other) {
    std::lock_guard<std::mutex> lock(mutex);
    files = std::move(other.files);
    materialized.store(other.exists(), std::memory_order_release);
  }

  void listFiles(std::vector<FileInfo>& target) override {
    std::lock_guard<std::mutex> lock(mutex);
    target.reserve(target.size() + files.size());
    for (const auto&[name, ifile] : files) {
      target.push_back({name, (uint64_t)ifile.file->size()});
    }
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    unused(expectSynced);
    std::lock_guard<std::mutex> lock(mutex);

    auto find = files.find(name);
    if (find != files.end()) {
      // redefine DEBUG to TRACE level which shouldn't currently be logged!
      // #define INDEX_DEBUG LOG_TRACE
      DIR_DEBUG("DIR openFile: found file {} size={}", name, find->second.file->size());
      return find->second.file;
    } else {
      DIR_DEBUG("DIR openFile: file {} not found", name);
      return {};
    }
  }

  std::shared_ptr<ChecksumReader> openChecksum(std::string_view name, bool direct = false) override {
    unused(direct);
    auto file = openFile(name);
    if (!file) return {};
    // Hashes the shared buffer; the handle keeps it alive after a delete.
    class RAMChecksumReader : public ChecksumReader {
      std::shared_ptr<InputFile> file;
    public:
      explicit RAMChecksumReader(std::shared_ptr<InputFile> file) : file(std::move(file)) {}
      uint64_t size() const override { return file->size(); }
      std::shared_ptr<InputFile> map() override { return file; }
      FileCheck checksum(const std::function<void(size_t)>& progress) override {
        auto data = file->read();
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        constexpr size_t kChunk = 1 << 20;
        for (size_t offset = 0; offset < data.size(); offset += kChunk) {
          size_t length = std::min(kChunk, data.size() - offset);
          XXH3_64bits_update(&state, data.data() + offset, length);
          if (progress) progress(length);
        }
        return {(uint64_t)data.size(), XXH3_64bits_digest(&state)};
      }
    };
    return std::make_shared<RAMChecksumReader>(std::move(file));
  }

  bool deleteFile(const std::string_view name) override {
    std::lock_guard<std::mutex> lock(mutex);

    bool success = files.erase(name);
    DIR_DEBUG("DIR deleteFile {} success={}", name, success);
    return success;
  }

  void deletePrefix(const std::string_view prefix) override {
    std::lock_guard<std::mutex> lock(mutex);

    // Would erase range be more efficient here?  The number of items
    // to be erased is relatively small (number of files in a segment)
    for (auto it = files.lower_bound(prefix); it != files.end(); ) {
      if (!it->first.starts_with(prefix)) {
        break;
      }
      it = files.erase(it);
    }
  }

  std::unique_ptr<File> createFile(const std::string_view name) override {
    DIR_DEBUG("DIR about to createFile {}", name);
    return std::make_unique<OutputFileType>(name, memory);
  }

  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    if (options.expectedSize) {
      if (*options.expectedSize > SIZE_MAX) throw FileIOException("RAM file is too large");
      return std::make_unique<SizedRAMFile>(name, (size_t)*options.expectedSize, memory);
    }
    return createFile(name);
  }

  void finishFile(File &file) override {
    DIR_DEBUG("DIR about to finishFile {} size={}", file.name(), file.size());

    std::shared_ptr<RAMInputFile> inputFile;
    if (auto sized = dynamic_cast<SizedRAMFile*>(&file)) {
      inputFile = sized->finish();
    } else {
      auto& ramFile = dynamic_cast<OutputFileType&>(file);
      auto sz = ramFile.size();
      StorageCharge charge(memory, sz);
      auto singleBuffer = std::make_unique_for_overwrite<char[]>(sz);
      ramFile.copyTo(singleBuffer.get());
      inputFile = std::make_shared<RAMInputFile>(std::move(singleBuffer), sz, std::move(charge));
    }

    {
      std::lock_guard<std::mutex> lock(mutex);

      if (!files.emplace(file.name(), Entry{std::move(inputFile), ++nextVersion}).second) {
        throw FileIOException("File already exists: " + std::string(file.name()));
      }
      materialized.store(true, std::memory_order_release);
    }
  }

  std::optional<PublicationVersion> publishFile(std::string_view name, std::span<const std::byte> bytes, PublishCondition condition) override {
    if (condition.mode == PublishMode::CREATE_ONLY) {
      std::lock_guard lock(mutex);
      if (files.contains(name)) return {};
    }
    StorageCharge charge(memory, bytes.size());
    auto buffer = std::make_unique_for_overwrite<char[]>(bytes.size());
    if (!bytes.empty()) std::memcpy(buffer.get(), bytes.data(), bytes.size());
    auto input = std::make_shared<RAMInputFile>(std::move(buffer), bytes.size(), std::move(charge));
    Signal::emit("publishWritten", this, &name);
    Signal::emit("publishSynced", this, &name);
    std::lock_guard lock(mutex);
    if (condition.mode == PublishMode::CREATE_ONLY && files.contains(name)) return {};
    if (condition.version && (!files.contains(name) || PublicationVersion(std::to_string(files.find(name)->second.version)) != *condition.version)) return {};
    auto version = ++nextVersion;
    files[std::string(name)] = {std::move(input), version};
    materialized.store(true, std::memory_order_release);
    return std::to_string(version);
  }

  std::optional<Publication> readPublished(std::string_view name) override {
    std::lock_guard lock(mutex);
    auto it = files.find(name);
    if (it == files.end()) return {};
    return Publication{std::string(it->second.file->read()), std::to_string(files.find(name)->second.version)};
  }

  void sync(std::span<const std::string> names) override { unused(names); }

  void deleteDurably(std::span<const std::string> names) override {
    std::lock_guard lock(mutex);
    for (const auto& name : names) files.erase(name);
  }

  void linkFile(Directory& source, std::string_view name) override {
    auto input = std::dynamic_pointer_cast<RAMInputFile>(source.openFile(name, true));
    if (!input) throw std::runtime_error("RAM link source missing or incompatible");
    std::lock_guard lock(mutex);
    files[std::string(name)] = {std::move(input), ++nextVersion};
    materialized.store(true, std::memory_order_release);
  }

  void clear() override {
    DIR_DEBUG("DIR about to clear all files");
    std::lock_guard<std::mutex> lock(mutex);
    files.clear();
  }

};


} // end namespace
