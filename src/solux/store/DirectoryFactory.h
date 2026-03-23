#pragma once

#include "Directory.h"
#include "FSDirectory.h"

namespace solux {

/// Creates Directory instances. Owns shared resources (e.g. I/O threads)
/// that are shared across all directories.
/// Lives on SoluxNode - one per instance.
class DirectoryFactory {
public:
  virtual ~DirectoryFactory() = default;

  virtual std::shared_ptr<Directory> create() = 0;
};


/// Factory that creates RAMDir instances.
class RAMDirFactory : public DirectoryFactory {
public:
  std::shared_ptr<Directory> create() override {
    return std::make_shared<RAMDir>();
  }
};


/// Factory that creates FSDirectory instances.
class FSDirFactory : public DirectoryFactory {
  std::filesystem::path basePath_;
public:
  explicit FSDirFactory(std::filesystem::path path) : basePath_(std::move(path)) {}

  std::shared_ptr<Directory> create() override {
    return std::make_shared<FSDirectory>(basePath_);
  }
};


} // namespace solux
