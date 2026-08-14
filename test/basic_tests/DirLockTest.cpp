#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "luxir/store/DirectoryFactory.h"
#include "luxir/store/ReadOnlyDirectory.h"

using namespace luxir;

namespace {

class TempDir {
  std::filesystem::path path_;

public:
  TempDir() {
    std::string pathTemplate =
        (std::filesystem::temp_directory_path() / "luxir_dir_lock_XXXXXX").string();
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

TEST(DirLockTest, unownedFactoryOpensAlongsideTheWriter) {
  TempDir tempDir;
  FSDirFactory writer(tempDir.path());
  writer.create("main");

  FSDirFactory reader(tempDir.path(), /*unowned=*/true);
  EXPECT_EQ(reader.listCollections(), std::vector<std::string>{"main"});
  // The reader must not have left a lock behind either.
  EXPECT_NO_THROW({ FSDirFactory alsoUnowned(tempDir.path(), /*unowned=*/true); });
}

TEST(DirLockTest, unownedFactoryRequiresAnExistingDataDir) {
  TempDir tempDir;
  EXPECT_THROW(FSDirFactory(tempDir.path() / "nope", /*unowned=*/true), ReadOnlyError);
  // An empty temp dir has no collections dir yet, so it is not a data directory.
  EXPECT_THROW(FSDirFactory(tempDir.path(), /*unowned=*/true), ReadOnlyError);
}

// The decorator is the enforcement point, so it must reject every mutator on the
// Directory interface while leaving reads intact - on any backend.
TEST(DirLockTest, readOnlyDecoratorRejectsEveryMutator) {
  TempDir tempDir;
  {
    FSDirFactory writer(tempDir.path());
    auto dir = writer.create("main");
    auto file = dir->createFile("hello");
    dir->finishFile(*file);
  }

  ReadOnlyDirFactory reader(std::make_unique<FSDirFactory>(tempDir.path(), /*unowned=*/true));
  auto dir = reader.create("main");

  EXPECT_TRUE(dir->openFile("hello") != nullptr);
  std::vector<std::string> files;
  EXPECT_NO_THROW(dir->listFiles(files));
  EXPECT_EQ(files, std::vector<std::string>{"hello"});

  EXPECT_THROW(dir->createFile("other"), ReadOnlyError);
  EXPECT_THROW(dir->deleteFile("hello"), ReadOnlyError);
  EXPECT_THROW(dir->deletePrefix("hel"), ReadOnlyError);
  EXPECT_THROW(dir->renameFile("hello", "other"), ReadOnlyError);
  EXPECT_THROW(dir->clear(), ReadOnlyError);
  std::vector<std::string> syncNames{"hello"};
  EXPECT_THROW(dir->sync(syncNames), ReadOnlyError);
  EXPECT_THROW(reader.remove("main"), ReadOnlyError);
  EXPECT_THROW(reader.create("absent"), ReadOnlyError);

  // finishFile needs a File, which only a writable directory can hand out.
  FSDirFactory writer2(tempDir.path());
  auto writableDir = writer2.create("main");
  auto staged = writableDir->createFile("staged");
  EXPECT_THROW(dir->finishFile(*staged), ReadOnlyError);

  // Nothing above reached the filesystem.
  files.clear();
  writableDir->listFiles(files);
  EXPECT_EQ(files, std::vector<std::string>{"hello"});
}

// The decorator carries the guarantee for any backend, not just the fs one.
TEST(DirLockTest, readOnlyDecoratorWorksOverRam) {
  ReadOnlyDirFactory reader(std::make_unique<RAMDirFactory>());
  EXPECT_THROW(reader.create("main"), ReadOnlyError);
  EXPECT_THROW(reader.remove("main"), ReadOnlyError);
}
