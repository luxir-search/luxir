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
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

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

std::string layoutError(const std::filesystem::path& path, bool writable) {
  FSDirFactory factory(path, !writable);
  try {
    TenantCatalog::initialize(factory, writable);
  } catch (const std::runtime_error& error) {
    std::string message = error.what();
    EXPECT_TRUE(message.starts_with("data directory " + factory.describe() + ": ")) << message;
    EXPECT_NE(message.find(path.string()), std::string::npos) << message;
    return message;
  }
  ADD_FAILURE() << "layout was accepted";
  return {};
}

void expectLayoutError(const std::filesystem::path& path, std::initializer_list<std::string_view> parts) {
  for (bool writable : {true, false}) {
    auto message = layoutError(path, writable);
    for (auto part : parts) EXPECT_NE(message.find(part), std::string::npos) << message;
  }
}

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
  TenantCatalog(writer, "t0", "test", newUuid()).reserve("main");

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
  auto message = layoutError(tempDir.path(), false);
  EXPECT_NE(message.find("read-only; start a writer once to create it"), std::string::npos);
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
  auto temp = tempDir.path() / "t" / ".luxir-publish-0-0";
  std::ofstream(temp) << "partial";
  expectLayoutError(tempDir.path(), {"no LAYOUT marker but is not empty (first entries: \"t\")"});
  EXPECT_TRUE(std::filesystem::exists(temp)); // Refusal must not open and clean t/.
}

TEST(DirLockTest, resumesAnInterruptedLayoutMarker) {
  TempDir tempDir;
  auto temp = tempDir.path() / "node/tmp/interrupted";
  std::filesystem::create_directories(temp.parent_path());
  std::ofstream(temp) << "partial";
  FSDirFactory writer(tempDir.path());
  TenantCatalog::initialize(writer, true);
  EXPECT_TRUE(std::filesystem::exists(tempDir.path() / "LAYOUT"));
  EXPECT_FALSE(std::filesystem::exists(temp));
}

TEST(DirLockTest, exclusiveAndSharedAdmissionExcludeEachOther) {
  TempDir tempDir;
  {
    FSDirFactory exclusive(tempDir.path());
    EXPECT_THROW(FSDirFactory(tempDir.path(), false, FSDirFactory::Mode::SHARED, "A"), std::runtime_error);
  }
  auto diagnosticSize = std::filesystem::file_size(tempDir.path() / "node/write.lock");
  {
    FSDirFactory first(tempDir.path(), false, FSDirFactory::Mode::SHARED, "A");
    FSDirFactory second(tempDir.path(), false, FSDirFactory::Mode::SHARED, "B");
    EXPECT_FALSE(first.exclusive());
    EXPECT_FALSE(second.exclusive());
    EXPECT_THROW(FSDirFactory(tempDir.path()), std::runtime_error);
    EXPECT_THROW(FSDirFactory(tempDir.path(), false, FSDirFactory::Mode::SHARED, "A"), std::runtime_error);
    EXPECT_NO_THROW(FSDirFactory(tempDir.path(), true, FSDirFactory::Mode::SHARED, "A"));
    EXPECT_EQ(diagnosticSize, std::filesystem::file_size(tempDir.path() / "node/write.lock"));
    EXPECT_GT(std::filesystem::file_size(tempDir.path() / "node/A/write.lock"), 0u);
  }
  EXPECT_NO_THROW(FSDirFactory(tempDir.path()));
}

TEST(DirLockTest, sharedAdmissionRefusesReservedIdsAndFollowerPrivateState) {
  TempDir tempDir;
  for (auto id : {"write.lock", "tmp", "replication.json", ".luxir-publish.lock"}) {
    EXPECT_THROW(FSDirFactory(tempDir.path(), false, FSDirFactory::Mode::SHARED, id), std::invalid_argument);
  }
  std::filesystem::create_directories(tempDir.path() / "node/A");
  std::ofstream(tempDir.path() / "node/A/replication.json") << "follower";
  EXPECT_THROW(FSDirFactory(tempDir.path(), false, FSDirFactory::Mode::SHARED, "A"), std::invalid_argument);
  EXPECT_THROW(FSDirFactory(tempDir.path(), true, FSDirFactory::Mode::SHARED, "A"), std::invalid_argument);
}

TEST(DirLockTest, publicationTempsBelongToOnlyOneNode) {
  TempDir tempDir;
  FSDirFactory first(tempDir.path(), false, FSDirFactory::Mode::SHARED, "A");
  TenantCatalog::initialize(first, true);
  EXPECT_TRUE(TenantCatalog::empty(first)); // The publication lock is not store data.
  auto privateTemp = tempDir.path() / "node/A/tmp";
  auto stale = tempDir.path() / "node/B/tmp/interrupted";
  std::filesystem::create_directories(stale.parent_path());
  std::ofstream(stale) << "partial";
  auto dir = first.directory(StorageRoot::AUTHORITATIVE, "data");
  Signal::listen("publishOpened", [&](void*, void*, void*) -> void* {
    EXPECT_EQ(1, std::distance(std::filesystem::directory_iterator(privateTemp), std::filesystem::directory_iterator{}));
    EXPECT_TRUE(std::filesystem::is_empty(tempDir.path() / "data"));
    FSDirFactory reader(tempDir.path(), true, FSDirFactory::Mode::SHARED, "A");
    EXPECT_FALSE(std::filesystem::is_empty(privateTemp));
    FSDirFactory second(tempDir.path(), false, FSDirFactory::Mode::SHARED, "B");
    EXPECT_FALSE(std::filesystem::exists(stale));
    EXPECT_FALSE(std::filesystem::is_empty(privateTemp));
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("publishOpened"); });
  EXPECT_TRUE(dir->publishFile("root", {}, Directory::PublishMode::CREATE_ONLY));
  EXPECT_TRUE(std::filesystem::is_empty(privateTemp));
  std::vector<Directory::FileInfo> files;
  dir->listFiles(files);
  ASSERT_EQ(1u, files.size());
  EXPECT_EQ("root", files[0].name);
}

TEST(DirLockTest, refusesTheEarlierCollectionsDirectory) {
  TempDir tempDir;
  std::filesystem::create_directories(tempDir.path() / "c" / "default" / "main");
  expectLayoutError(tempDir.path(), {"no LAYOUT marker but is not empty (first entries: \"c\")"});
}

TEST(DirLockTest, refusesAnUnknownRootMarker) {
  TempDir tempDir;
  std::ofstream(tempDir.path() / "LAYOUT") << "something/else 7\n";
  expectLayoutError(tempDir.path(), {"unsupported layout \"something/else 7\"", "\"tenant/catalog/incarnation 2\"",
      "reindex into an empty directory or use luxir pull from a node running this build"});
}

TEST(DirLockTest, summarizesAnUnprintableRootMarker) {
  TempDir tempDir;
  std::ofstream(tempDir.path() / "LAYOUT") << std::string(100, 'x');
  expectLayoutError(tempDir.path(), {"unsupported layout \"<100 bytes>\""});
  std::ofstream(tempDir.path() / "LAYOUT") << "bad\tmarker";
  expectLayoutError(tempDir.path(), {"unsupported layout \"<10 bytes>\""});
}

TEST(DirLockTest, describesAnEmptyRootMarker) {
  TempDir tempDir;
  std::ofstream(tempDir.path() / "LAYOUT");
  expectLayoutError(tempDir.path(), {"unsupported layout \"<empty>\""});
}

TEST(DirLockTest, sanitizesUnrecognizedEntryNames) {
  TempDir tempDir;
  for (const auto& name : {std::string("bad\n"), std::string(100, 'x')}) {
    std::filesystem::create_directories(tempDir.path() / name);
  }
  expectLayoutError(tempDir.path(), {"first entries: \"<4 bytes>\", \"<100 bytes>\""});
}

TEST(DirLockTest, refusesAnUnrecognizedNonEmptyDirectory) {
  TempDir tempDir;
  std::filesystem::create_directories(tempDir.path() / "stray");
  expectLayoutError(tempDir.path(), {"no LAYOUT marker but is not empty (first entries: \"stray\")",
      "expected an empty directory or a Luxir data directory with a LAYOUT marker",
      "check the path or finish an interrupted copy"});
  for (bool writable : {true, false}) {
    auto message = layoutError(tempDir.path(), writable);
    EXPECT_EQ(message.find("reindex"), std::string::npos) << message;
  }
}
