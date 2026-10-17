// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/Scrubber.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/util/Uuid.h"
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>

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
  static std::filesystem::path dirOf(LuxirNode& node, const std::string& collection) {
    auto incarnation = node.getCollection(collection)->getShard()->getSnapshots().snapshot()->id.incarnation;
    return std::filesystem::path(node.getConfig().store.data_dir) / "t" / "t0" / collection / incarnation;
  }
  // Flips one byte in the middle of the collection's largest file; same size.
  // Flipping it again restores the file.
  static FileDescriptor flip(const std::filesystem::path& dir, const FileDescriptor& file) {
    std::fstream out(dir / file.name, std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    out.seekg((std::streamoff)(file.size / 2)); out.read(&byte, 1);
    byte = (char)~byte;
    out.seekp((std::streamoff)(file.size / 2)); out.write(&byte, 1);
    return file;
  }
  static FileDescriptor corrupt(LuxirNode& node, const std::string& collection) {
    auto snapshot = node.getCollection(collection)->getShard()->getSnapshots().snapshot();
    return flip(dirOf(node, collection), *std::ranges::max_element(snapshot->files, {}, &FileDescriptor::size));
  }
  static size_t count(const std::filesystem::path& dir, std::string_view prefix) {
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) if (entry.path().filename().string().starts_with(prefix)) n++;
    return n;
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

// A scrub hit fails the collection: the writer publishes nothing more, the
// suspect file is recorded, and only that file is re-verified at the next
// start, with the default verify-on-open=none, until it passes.
TEST_F(ChecksumVerifyTest, scrubFailsTheWriterAndTheFailureSurvivesRestart) {
  populate();
  config.store.scrub_max_bytes_per_sec = 0;
  std::filesystem::path dir;
  FileDescriptor damaged;
  {
    LuxirNode node(config);
    EXPECT_EQ(0u, node.getScrubber().scrub());
    auto clean = node.getScrubber().stats();
    EXPECT_EQ(1u, clean.passes);
    EXPECT_EQ(node.getCollection("main")->getShard()->getSnapshots().snapshot()->files.size()
              + node.getCollection("other")->getShard()->getSnapshots().snapshot()->files.size(), clean.files);
    EXPECT_GT(clean.bytes, 0u);
    EXPECT_GT(clean.lastPassEnd, 0u);
    // A stopped pass does nothing.
    std::stop_source stopped; stopped.request_stop();
    EXPECT_EQ(0u, node.getScrubber().scrub(stopped.get_token()));
    EXPECT_EQ(1u, node.getScrubber().stats().passes);

    CollectionHelper h(node, "main");
    dir = dirOf(node, "main");
    auto roots = count(dir, Manifest::kPrefix);
    damaged = corrupt(node, "main");
    EXPECT_EQ(1u, node.getScrubber().scrub());
    EXPECT_EQ(1u, node.getScrubber().stats().mismatches);
    expectUnavailable(node, "main", damaged);
    EXPECT_NO_THROW(node.getCollection("other"));
    // A failed collection is skipped, not reported again.
    EXPECT_EQ(0u, node.getScrubber().scrub());
    // The writer failed with it: no update, commit or merge publishes over the damaged bytes.
    EXPECT_FALSE(h.index(flatdoc("id", "c"), UpdateMessage::COMMIT).success);
    EXPECT_EQ(roots, count(dir, Manifest::kPrefix));
    EXPECT_EQ(1u, count(dir, CorruptionMarker::kPrefix));
  }
  {
    FSDirectory marked(dir, false);
    EXPECT_EQ(std::vector<std::string>{damaged.name}, CorruptionMarker::read(marked));
    // Default policy: only the recorded file is checked, and it still fails.
    LuxirNode node(config);
    expectUnavailable(node, "main", damaged);
    EXPECT_NO_THROW(node.getCollection("other"));
    EXPECT_EQ(1u, count(dir, CorruptionMarker::kPrefix));
  }
  flip(dir, damaged); // restored
  LuxirNode node(config);
  EXPECT_EQ(1, node.getCollection("main")->getReaderManager().getReader()->liveDocs());
  EXPECT_EQ(0u, count(dir, CorruptionMarker::kPrefix));
}

TEST_F(ChecksumVerifyTest, statsReportScrubCountersWhileEnabled) {
  populate();
  {
    LuxirNode node(config);
    HttpServer server(node, 1, 0); server.start();
    auto response = httpRequest(server.getPort(), http::verb::get, "/_stats");
    ASSERT_EQ(200, response.result_int()) << response.body();
    glz::generic_i64 json;
    ASSERT_FALSE(glz::read_json(json, response.body())) << response.body();
    EXPECT_FALSE(json.contains("scrub"));
  }
  config.store.scrub_interval_s = 3600;
  config.store.scrub_max_bytes_per_sec = 0;
  LuxirNode node(config);
  node.getScrubber().scrub();
  auto damaged = corrupt(node, "main");
  node.getScrubber().scrub();
  HttpServer server(node, 1, 0); server.start();
  auto response = httpRequest(server.getPort(), http::verb::get, "/_stats");
  ASSERT_EQ(200, response.result_int()) << response.body();
  glz::generic_i64 json;
  ASSERT_FALSE(glz::read_json(json, response.body())) << response.body();
  ASSERT_TRUE(json.contains("scrub")) << response.body();
  auto& scrub = json["scrub"];
  EXPECT_EQ(2, scrub["passes"].get<int64_t>());
  EXPECT_EQ(1, scrub["mismatches"].get<int64_t>());
  EXPECT_GT(scrub["files"].get<int64_t>(), 0);
  EXPECT_GT(scrub["bytes"].get<int64_t>(), 0);
  EXPECT_GT(scrub["last_pass_end"].get<int64_t>(), 0);
  auto collection = httpRequest(server.getPort(), http::verb::get, "/collections/main/_stats");
  EXPECT_EQ(503, collection.result_int()) << collection.body();
  EXPECT_NE(std::string::npos, collection.body().find(damaged.name)) << collection.body();
}

// The checksum primitive reads by descriptor, not through a mapping, on both
// backends, and a verification names what is wrong with each file.
TEST_F(ChecksumVerifyTest, checksumPrimitiveAndVerification) {
  std::filesystem::create_directories(path);
  FSDirectory fs(path);
  RAMDir ram;
  std::string bytes(3 * 1024 * 1024 + 7, 'x');
  for (size_t i = 0; i < bytes.size(); i += 4099) bytes[i] = (char)i;
  auto write = [&](Directory& dir, const std::string& name) {
    auto file = dir.createFile(name);
    OutputStream out(file.get()); out.write(bytes.data(), bytes.size()); out.close();
    dir.finishFile(*file);
    return file->descriptor();
  };
  for (Directory* dir : {(Directory*)&fs, (Directory*)&ram}) {
    auto good = write(*dir, "s0_a");
    auto bad = write(*dir, "s0_b");
    bad.xxh3 ^= 1; // the manifest recorded other bytes
    EXPECT_TRUE(dir->checksum("s0_a")->matches(good));
    EXPECT_TRUE(dir->checksum("s0_a", {}, true)->matches(good)); // direct where the filesystem allows
    EXPECT_FALSE(dir->checksum("s0_b")->matches(bad));
    EXPECT_FALSE(dir->checksum("s0_none"));
    size_t chunks = 0;
    dir->checksum("s0_a", [&](size_t) { chunks++; });
    EXPECT_EQ(4u, chunks);
    // The handle outlives the name.
    auto reader = dir->openChecksum("s0_a");
    dir->deleteFile("s0_a");
    EXPECT_TRUE(reader->checksum().matches(good));

    FileDescriptor missing{"s0_none", 1, 1};
    std::vector<FileDescriptor> files{bad, missing};
    try {
      verifyFiles(*dir, files);
      FAIL() << "verified a damaged file";
    } catch (const ChecksumMismatchError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("checksum mismatch: s0_b: expected")) << e.what();
      EXPECT_NE(std::string::npos, std::string(e.what()).find("(and 1 more files)")) << e.what();
    }
    FileVerification verification;
    verification.open(*dir, std::span(&missing, 1));
    verification.run();
    EXPECT_EQ("missing file: s0_none", verification.problem(0));
    std::vector<FileDescriptor> okay{write(*dir, "s0_c")};
    EXPECT_NO_THROW(verifyFiles(*dir, okay));
  }
}

}
