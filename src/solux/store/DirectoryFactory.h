#pragma once

#include "Directory.h"

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


} // namespace solux
