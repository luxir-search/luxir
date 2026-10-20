// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
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
  static constexpr Capabilities ATOMIC_RENAME = 1u << 1;   // renameFile replaces the target atomically
  static constexpr Capabilities DURABLE_SYNC = 1u << 2;    // sync() makes the files crash-safe (fsync)
  static constexpr Capabilities MEMORY_MAPPED = 1u << 3;   // openFile returns stable mapped bytes
  static constexpr Capabilities REMOTE = 1u << 4;          // bytes may be fetched over a network

  // Unknown backends claim nothing, which selects the conservative fallbacks.
  virtual Capabilities capabilities() const { return 0; }

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
  virtual void finishFile(File &file) = 0;

  // Atomically replace `to` with the already-finished file `from` when the
  // backing store supports it. Offline derivative builders use this only
  // after reopening and validating the unique source name.
  virtual void renameFile(std::string_view from, std::string_view to) = 0;

  // Fsync the given files to ensure durability.
  // Use "." to fsync the directory itself (to persist renames/creates).
  // The default implementation is a no-op (e.g. for RAMDir).
  virtual void sync(std::span<const std::string> filenames) { unused(filenames); }

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
    constexpr size_t kChunk = 1 << 20;
    for (size_t offset = 0; offset < data.size(); offset += kChunk) {
      out.write(data.data() + offset, std::min(kChunk, data.size() - offset));
    }
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
  gtl::btree_map<std::string, InputReferenceType> files;
  std::mutex mutex;

public:
  explicit RAMDir(std::shared_ptr<StorageMemory> memory = std::make_shared<StorageMemory>()) : memory(std::move(memory)) {}
  // linkFile shares the immutable buffer; renames happen under the lock.
  Capabilities capabilities() const override { return HARD_LINK | ATOMIC_RENAME | MEMORY_MAPPED; }
  uint64_t storageBytes() const { return memory->bytes(); }

  // Only used by tests to clear (e.g. dir = RAMDir())
  void operator=(RAMDir&& other) {
    std::lock_guard<std::mutex> lock(mutex);
    files = std::move(other.files);
  }

  void listFiles(std::vector<FileInfo>& target) override {
    std::lock_guard<std::mutex> lock(mutex);
    target.reserve(target.size() + files.size());
    for (const auto&[name, ifile] : files) {
      target.push_back({name, (uint64_t)ifile->size()});
    }
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    unused(expectSynced);
    std::lock_guard<std::mutex> lock(mutex);

    auto find = files.find(name);
    if (find != files.end()) {
      // redefine DEBUG to TRACE level which shouldn't currently be logged!
      // #define INDEX_DEBUG LOG_TRACE
      DIR_DEBUG("DIR openFile: found file {} size={}", name, find->second->size());
      return find->second;
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

      // overwrite existing entries.
      // TODO: we take pains to add files in order, so we should try a hint to add it at the end of the list.
      files[file.name()] = std::move(inputFile);
    }
  }

  void renameFile(std::string_view from, std::string_view to) override {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = files.find(from);
    if (it == files.end()) {
      throw std::runtime_error("RAMDir::renameFile: source not found");
    }
    auto value = std::move(it->second);
    files.erase(it);
    files[std::string(to)] = std::move(value);
  }

  void linkFile(Directory& source, std::string_view name) override {
    auto input = std::dynamic_pointer_cast<RAMInputFile>(source.openFile(name, true));
    if (!input) throw std::runtime_error("RAM link source missing or incompatible");
    std::lock_guard lock(mutex);
    files[std::string(name)] = std::move(input);
  }

  void clear() override {
    DIR_DEBUG("DIR about to clear all files");
    std::lock_guard<std::mutex> lock(mutex);
    files.clear();
  }

};


} // end namespace
