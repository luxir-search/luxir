// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "luxir/server/Scrubber.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/util/Uuid.h"
#include <filesystem>
#include <fstream>

namespace luxir::test {

class ChecksumVerifyTest : public LuxirTest {
protected:
  std::filesystem::path path;
  LuxirConfig config;

  void SetUp() override {
    LuxirTest::SetUp();
    path = std::filesystem::temp_directory_path() / ("luxir-verify-" + newUuid());
    config.store.backend = "fs";
    config.store.data_dir = path.string();
  }
  void TearDown() override {
    std::filesystem::remove_all(path);
    LuxirTest::TearDown();
  }
  // Segment data, live docs and a vector overlay, in two collections.
  void populate() {
    LuxirNode node(config);
    for (auto name : {"main", "other"}) {
      CollectionHelper h(node, name);
      ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "embedding_v", std::vector<float>{1, 0}),
                              flatdoc("id", "b", "embedding_v", std::vector<float>{0, 1})}, UpdateMessage::NO_COMMIT).success);
      h.commit({"*"});
      ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
    }
  }
  static std::filesystem::path filePath(LuxirNode& node, const std::string& collection, const FileDescriptor& file) {
    auto incarnation = node.getCollection(collection)->getShard()->getSnapshots().snapshot()->id.incarnation;
    return std::filesystem::path(node.getConfig().store.data_dir) / "t" / "t0" / collection / incarnation / file.name;
  }
  // Flips one byte in the middle of the collection's largest file; same size.
  static FileDescriptor corrupt(LuxirNode& node, const std::string& collection) {
    auto snapshot = node.getCollection(collection)->getShard()->getSnapshots().snapshot();
    auto file = *std::ranges::max_element(snapshot->files, {}, &FileDescriptor::size);
    std::fstream out(filePath(node, collection, file), std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    out.seekg((std::streamoff)(file.size / 2)); out.read(&byte, 1);
    byte = (char)~byte;
    out.seekp((std::streamoff)(file.size / 2)); out.write(&byte, 1);
    return file;
  }
  static void expectUnavailable(LuxirNode& node, const std::string& collection, const FileDescriptor& file) {
    try {
      node.getCollection(collection);
      FAIL() << "corrupt collection served";
    } catch (const CollectionUnavailableError& e) {
      std::string message = e.what();
      EXPECT_NE(std::string::npos, message.find("checksum mismatch: " + file.name)) << message;
      EXPECT_NE(std::string::npos, message.find(std::format("expected size={} xxh3={:016x}", file.size, file.xxh3))) << message;
      EXPECT_NE(std::string::npos, message.find("actual size=")) << message;
    }
  }
};

TEST_F(ChecksumVerifyTest, fullVerifyOnOpenFailsOnlyTheCorruptCollection) {
  populate();
  config.store.verify_on_open = "full";
  FileDescriptor damaged;
  {
    // Every file a writer commits, including live docs and overlays, verifies.
    LuxirNode node(config);
    EXPECT_EQ(1, node.getCollection("main")->getReaderManager().getReader()->liveDocs());
    damaged = corrupt(node, "main");
  }
  LuxirNode node(config);
  expectUnavailable(node, "main", damaged);
  EXPECT_EQ(1, node.getCollection("other")->getReaderManager().getReader()->liveDocs());

  // A read-only node refuses the copy the same way.
  config.read_only = true;
  LuxirNode reader(config);
  expectUnavailable(reader, "main", damaged);
  EXPECT_NO_THROW(reader.getCollection("other"));
}

TEST_F(ChecksumVerifyTest, scrubMarksCorruptCollectionUnavailable) {
  populate();
  config.store.scrub_max_bytes_per_sec = 0;
  LuxirNode node(config);
  EXPECT_EQ(0u, node.getScrubber().scrub());
  auto clean = node.getScrubber().stats();
  EXPECT_EQ(1u, clean.passes);
  EXPECT_EQ(node.getCollection("main")->getShard()->getSnapshots().snapshot()->files.size()
            + node.getCollection("other")->getShard()->getSnapshots().snapshot()->files.size(), clean.files);
  auto damaged = corrupt(node, "main");
  EXPECT_EQ(1u, node.getScrubber().scrub());
  EXPECT_EQ(1u, node.getScrubber().stats().mismatches);
  expectUnavailable(node, "main", damaged);
  EXPECT_NO_THROW(node.getCollection("other"));
  // A flagged collection is skipped, not reported again.
  EXPECT_EQ(0u, node.getScrubber().scrub());
}

TEST_F(ChecksumVerifyTest, reuseRefusesALinkedCopyThatNoLongerMatches) {
  RAMDir source, target;
  auto write = [&](const std::string& name, std::string_view bytes) {
    auto file = source.createFile(name);
    OutputStream out(file.get()); out.write(bytes.data(), bytes.size()); out.close();
    source.finishFile(*file);
    return file->descriptor();
  };
  auto good = write("s0_a", "intact");
  auto bad = write("s0_b", "changed");
  bad.xxh3 ^= 1; // the manifest recorded other bytes
  std::vector<FileDescriptor> files{good, bad};
  std::vector<std::string> corrupt;
  auto reused = target.reuseFiles(source, files, files, &corrupt);
  EXPECT_EQ(std::vector<FileDescriptor>{good}, reused);
  ASSERT_EQ(1u, corrupt.size());
  EXPECT_NE(std::string::npos, corrupt[0].find("checksum mismatch: s0_b")) << corrupt[0];
  EXPECT_FALSE(target.openFile("s0_b")); // unlinked again, so a caller fetches it
  EXPECT_THROW(verifyFiles(source, files), ChecksumMismatchError);
  EXPECT_NO_THROW(verifyFiles(source, std::span(&good, 1)));
}

}
