#pragma once

#include <algorithm>
#include "DirectoryFactory.h"

namespace luxir {

/// Directory wrapper that serves reads and rejects every mutation.
///
/// This is the enforcement point for --read-only: the node opens a data
/// directory it does not own (no write.lock), so nothing may write to it.
/// Making that a wrapper rather than a flag inside each backend means the
/// guarantee holds for any Directory implementation, and a mutator added to
/// the interface later cannot silently escape it - the override is missing and
/// the compiler says so.
///
/// Requests are refused at the transport before reaching here; these throws are
/// the backstop for paths that hold a Directory directly (schema persistence,
/// merges, commit) rather than the expected way for a user to see an error.
class ReadOnlyDirectory : public Directory {
  std::shared_ptr<Directory> delegate_;
  std::string name_;

  [[noreturn]] void reject(std::string_view operation) const {
    throw ReadOnlyError("read-only directory '" + name_ + "': " + std::string(operation) +
                        " rejected");
  }

public:
  ReadOnlyDirectory(std::shared_ptr<Directory> delegate, std::string name)
      : delegate_(std::move(delegate)), name_(std::move(name)) {}

  void listFiles(std::vector<std::string>& target) override {
    delegate_->listFiles(target);
  }

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    return delegate_->openFile(name, expectSynced);
  }

  std::unique_ptr<File> createFile(std::string_view name) override {
    reject(std::string("createFile(") + std::string(name) + ")");
  }

  bool deleteFile(std::string_view name) override {
    reject(std::string("deleteFile(") + std::string(name) + ")");
  }

  void deletePrefix(std::string_view prefix) override {
    reject(std::string("deletePrefix(") + std::string(prefix) + ")");
  }

  void finishFile(File& file) override {
    reject(std::string("finishFile(") + std::string(file.name()) + ")");
  }

  void renameFile(std::string_view from, std::string_view to) override {
    reject(std::string("renameFile(") + std::string(from) + " -> " + std::string(to) + ")");
  }

  void sync(std::span<const std::string> filenames) override {
    unused(filenames);
    reject("sync");
  }

  void clear() override {
    reject("clear");
  }
};


/// Factory wrapper that hands out ReadOnlyDirectory instances.
///
/// Pair it with a backend factory opened read-only (see FSDirFactory), which
/// suppresses the open-time side effects a wrapper cannot reach: taking
/// write.lock, creating the data directory, resetting trash/.
class ReadOnlyDirFactory : public DirectoryFactory {
  std::unique_ptr<DirectoryFactory> delegate_;

public:
  explicit ReadOnlyDirFactory(std::unique_ptr<DirectoryFactory> delegate)
      : delegate_(std::move(delegate)) {}

  std::shared_ptr<Directory> create(std::string_view collectionName) override {
    // Backends materialize storage for an unknown collection on create() (and
    // are entitled to), so refuse before delegating rather than after.  For one
    // that already exists, create() only opens what is there.
    auto existing = delegate_->listCollections();
    if (std::find(existing.begin(), existing.end(), collectionName) == existing.end()) {
      throw ReadOnlyError("read-only data directory: collection '" +
                          std::string(collectionName) + "' does not exist and cannot be created");
    }
    return std::make_shared<ReadOnlyDirectory>(delegate_->create(collectionName),
                                               std::string(collectionName));
  }

  std::vector<std::string> listCollections() override {
    return delegate_->listCollections();
  }

  void remove(std::string_view collectionName) override {
    throw ReadOnlyError("read-only data directory: cannot remove collection '" +
                        std::string(collectionName) + "'");
  }
};

} // namespace luxir
