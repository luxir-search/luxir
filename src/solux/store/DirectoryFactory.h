#pragma once

#include <optional>
#include "Directory.h"
#include "FileLock.h"
#include "FSDirectory.h"
#include "solux/util/log.h"

namespace solux {

/// Creates Directory instances for collections. Owns shared resources
/// (e.g. I/O threads, dictionaries) that live under the base path.
/// Lives on SoluxNode - one per instance.
class DirectoryFactory {
public:
  virtual ~DirectoryFactory() = default;

  /// Create a Directory for the named collection.
  virtual std::shared_ptr<Directory> create(std::string_view collectionName) = 0;

  /// Return the names of collections that already exist on disk.
  virtual std::vector<std::string> listCollections() = 0;
};


/// Factory that creates RAMDir instances (one per collection).
class RAMDirFactory : public DirectoryFactory {
public:
  std::shared_ptr<Directory> create(std::string_view collectionName) override {
    (void)collectionName;
    return std::make_shared<RAMDir>();
  }

  std::vector<std::string> listCollections() override {
    return {};  // RAM has no persistence
  }
};


/// Factory that creates FSDirectory instances under basePath_/c/<name>.
/// Shared resources (dictionaries, etc.) live directly under basePath_.
class FSDirFactory : public DirectoryFactory {
  std::filesystem::path basePath_;
  std::filesystem::path collectionsPath_;  // basePath_/c
  std::optional<FileLock> lock_;

public:
  explicit FSDirFactory(std::filesystem::path path)
      : basePath_(std::move(path)), collectionsPath_(basePath_ / "c") {
    bool created = std::filesystem::create_directories(collectionsPath_);
    if (created) {
      LOG_INFO("Created data directory: {}", basePath_.string());
    } else {
      LOG_INFO("Using existing data directory: {}", basePath_.string());
    }
    lock_.emplace(basePath_ / "write.lock");
  }

  std::shared_ptr<Directory> create(std::string_view collectionName) override {
    return std::make_shared<FSDirectory>(collectionsPath_ / collectionName);
  }

  std::vector<std::string> listCollections() override {
    std::vector<std::string> result;
    for (const auto& entry : std::filesystem::directory_iterator(collectionsPath_)) {
      if (entry.is_directory()) {
        result.push_back(entry.path().filename().string());
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }
};


} // namespace solux
