#pragma once

#include <functional>
#include <span>
#include <stdexcept>
#include <vector>
#include <string_view>
#include <gtl/btree.hpp>
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
class ReadOnlyError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Implementations of Directory are thread safe.
class Directory {
public:
  struct FileCreateOptions {
    std::function<void(int64_t)> ramBytesChanged;
    size_t ramSpillBytes = 0;
    bool ramDelegating = false;
  };

  struct FileInfo {
    std::string name;
    uint64_t size = 0;
  };

  // TODO: add a prefix option?
  // Appends; the appended range is sorted by name.  Sizes are a best-effort
  // snapshot and entries vanishing mid-scan are skipped, never thrown.
  virtual void listFiles(std::vector<FileInfo> &target) = 0;

  // Open a file for reading.  If expectSynced is true, the caller asserts that
  // this file should have been fsynced (e.g. it was referenced from a committed
  // index).  CheckedDirectory uses this flag to detect missing fsyncs.
  virtual std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) = 0;

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

  virtual ~Directory() = default;
};


class RAMDir : public Directory {
public:
  using OutputFileType = RAMFile;
  using InputFileType = RAMInputFile;
  using InputReferenceType = std::shared_ptr<InputFileType>;

private:
  gtl::btree_map<std::string, InputReferenceType> files;
  std::mutex mutex;

public:
  RAMDir() = default;

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
    return std::make_unique<OutputFileType>(name);
  }

  void finishFile(File &file) override {
    DIR_DEBUG("DIR about to finishFile {} size={}", file.name(), file.size());

    auto &ramFile = dynamic_cast<OutputFileType &>(file);
    auto sz = ramFile.size();
    std::unique_ptr<char[]> singleBuffer = std::make_unique_for_overwrite<char[]>(sz);
    ramFile.copyTo(singleBuffer.get());
    auto inputFile = std::make_shared<RAMInputFile>(std::move(singleBuffer), sz);

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

  void clear() override {
    DIR_DEBUG("DIR about to clear all files");
    std::lock_guard<std::mutex> lock(mutex);
    files.clear();
  }

};


} // end namespace
