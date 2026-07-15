#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "solux/store/DirectoryFactory.h"

using namespace solux;

namespace {

class TempDir {
  std::filesystem::path path_;

public:
  TempDir() {
    std::string pathTemplate =
        (std::filesystem::temp_directory_path() / "solux_dir_lock_XXXXXX").string();
    if (::mkdtemp(pathTemplate.data()) == nullptr) {
      throw std::runtime_error("Failed to create temp directory");
    }
    path_ = std::move(pathTemplate);
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }
};

} // namespace

TEST(DirLockTest, rejectsSecondFSDirFactory) {
  TempDir tempDir;
  FSDirFactory first(tempDir.path());

  try {
    FSDirFactory second(tempDir.path());
    FAIL() << "Expected a second FSDirFactory to fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find(tempDir.path().string()), std::string::npos);
  }
}

TEST(DirLockTest, releasesLockOnDestruction) {
  TempDir tempDir;
  {
    FSDirFactory first(tempDir.path());
  }

  EXPECT_NO_THROW({ FSDirFactory second(tempDir.path()); });
}

TEST(DirLockTest, allowsMultipleRAMDirFactories) {
  RAMDirFactory first;
  RAMDirFactory second;
}
