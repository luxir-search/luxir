// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <fstream>
#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "luxir/server/TenantCatalog.h"
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
  TenantCatalog::initialize(writer, true);
  TenantCatalog(writer).reserve("main");

  FSDirFactory reader(tempDir.path(), /*unowned=*/true);
  EXPECT_EQ(TenantCatalog::collections(reader), std::vector<CollectionId>{CollectionId::of("main")});
  // The reader must not have left a lock behind either.
  EXPECT_NO_THROW({ FSDirFactory alsoUnowned(tempDir.path(), /*unowned=*/true); });
}

TEST(DirLockTest, unownedFactoryRequiresAnExistingDataDir) {
  TempDir tempDir;
  EXPECT_THROW(FSDirFactory(tempDir.path() / "nope", /*unowned=*/true), ReadOnlyError);
  // Layout validation belongs to the engine.
  FSDirFactory reader(tempDir.path(), true);
  EXPECT_THROW(TenantCatalog::initialize(reader, false), ReadOnlyError);
}

// The decorator is the enforcement point, so it must reject every mutator on the
// Directory interface while leaving reads intact - on any backend.
TEST(DirLockTest, readOnlyDecoratorRejectsEveryMutator) {
  TempDir tempDir;
  {
    FSDirFactory writer(tempDir.path());
    auto dir = writer.directory(StorageRoot::AUTHORITATIVE, "data");
    auto file = dir->createFile("hello");
    dir->finishFile(*file);
  }

  ReadOnlyDirFactory reader(std::make_unique<FSDirFactory>(tempDir.path(), /*unowned=*/true));
  auto dir = reader.directory(StorageRoot::AUTHORITATIVE, "data");

  EXPECT_TRUE(dir->openFile("hello") != nullptr);
  EXPECT_TRUE(dir->readPublished("hello"));
  std::vector<Directory::FileInfo> files;
  EXPECT_NO_THROW(dir->listFiles(files));
  ASSERT_EQ(1, files.size());
  EXPECT_EQ("hello", files[0].name);

  EXPECT_THROW(dir->createFile("other"), ReadOnlyError);
  EXPECT_THROW(dir->deleteFile("hello"), ReadOnlyError);
  EXPECT_THROW(dir->deletePrefix("hel"), ReadOnlyError);
  EXPECT_THROW(dir->publishFile("hello", {}, Directory::PublishMode::REPLACE), ReadOnlyError);
  EXPECT_THROW(dir->clear(), ReadOnlyError);
  std::vector<std::string> syncNames{"hello"};
  EXPECT_THROW(dir->sync(syncNames), ReadOnlyError);
  EXPECT_THROW(dir->deleteDurably(syncNames), ReadOnlyError);
  EXPECT_THROW(reader.removeTree(StorageRoot::AUTHORITATIVE, "data"), ReadOnlyError);
  auto absent = reader.directory(StorageRoot::AUTHORITATIVE, "absent");
  EXPECT_THROW(absent->publishFile("root", {}, Directory::PublishMode::REPLACE), ReadOnlyError);

  // finishFile needs a File, which only a writable directory can hand out.
  FSDirFactory writer2(tempDir.path());
  auto writableDir = writer2.directory(StorageRoot::AUTHORITATIVE, "data");
  auto staged = writableDir->createFile("staged");
  EXPECT_THROW(dir->finishFile(*staged), ReadOnlyError);

  // Nothing above reached the filesystem.
  files.clear();
  writableDir->listFiles(files);
  ASSERT_EQ(1, files.size());
  EXPECT_EQ("hello", files[0].name);
}

// The decorator carries the guarantee for any backend, not just the fs one.
TEST(DirLockTest, readOnlyDecoratorWorksOverRam) {
  ReadOnlyDirFactory reader(std::make_unique<RAMDirFactory>());
  EXPECT_THROW(reader.directory(StorageRoot::AUTHORITATIVE, "main")->createFile("data"), ReadOnlyError);
  EXPECT_THROW(reader.removeTree(StorageRoot::AUTHORITATIVE, "main"), ReadOnlyError);
}

TEST(DirLockTest, refusesDataFromAnotherLayout) {
  TempDir tempDir;
  std::filesystem::create_directories(tempDir.path() / "t");
  std::ofstream(tempDir.path() / "t" / "LAYOUT") << "tenant/collection/incarnation 1\n";
  { FSDirFactory writer(tempDir.path()); EXPECT_THROW(TenantCatalog::initialize(writer, true), std::runtime_error); }
  FSDirFactory reader(tempDir.path(), true);
  EXPECT_THROW(TenantCatalog::initialize(reader, false), std::runtime_error);
}

TEST(DirLockTest, resumesAnInterruptedLayoutMarker) {
  TempDir tempDir;
  auto temp = tempDir.path() / ".luxir-publish-0-0";
  std::ofstream(temp) << "partial";
  auto live = tempDir.path() / (".luxir-publish-" + std::to_string(getpid()) + "-0");
  std::ofstream(live) << "active";
  FSDirFactory writer(tempDir.path());
  TenantCatalog::initialize(writer, true);
  EXPECT_TRUE(std::filesystem::exists(tempDir.path() / "LAYOUT"));
  EXPECT_FALSE(std::filesystem::exists(temp));
  EXPECT_TRUE(std::filesystem::exists(live));
}

TEST(DirLockTest, refusesTheEarlierCollectionsDirectory) {
  TempDir tempDir;
  std::filesystem::create_directories(tempDir.path() / "c" / "default" / "main");
  { FSDirFactory writer(tempDir.path()); EXPECT_THROW(TenantCatalog::initialize(writer, true), std::runtime_error); }
  FSDirFactory reader(tempDir.path(), true);
  EXPECT_THROW(TenantCatalog::initialize(reader, false), std::runtime_error);
}
