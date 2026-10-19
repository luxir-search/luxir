// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/Scrubber.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/store/VerifiedDirectory.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/reader/VectorAuxReader.h"
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

TEST_F(ChecksumVerifyTest, scrubPacingDoesNotAccumulateCredit) {
  using namespace std::chrono_literals;
  auto start = Scrubber::Clock::time_point{};
  auto due = Scrubber::nextDeadline(start, start, 256, 1024);
  EXPECT_EQ(start + 250ms, due);
  EXPECT_EQ(start + 500ms, Scrubber::nextDeadline(due, start + 100ms, 256, 1024));
  EXPECT_EQ(start + 500ms, Scrubber::nextDeadline(due, due, 256, 1024));
  EXPECT_EQ(start + 10s + 250ms, Scrubber::nextDeadline(due, start + 10s, 256, 1024));
  EXPECT_EQ(start + 10s, Scrubber::nextDeadline(due, start + 10s, 256, 0));
}

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

TEST_F(ChecksumVerifyTest, readOnlyVerifiesMagicBeforeParsing) {
  populate();
  FileDescriptor damaged;
  std::filesystem::path dir;
  {
    LuxirNode node(config);
    dir = dirOf(node, "main");
    damaged = node.getCollection("main")->getShard()->getSnapshots().snapshot()->files.front();
  }
  {
    std::fstream file(dir / damaged.name, std::ios::in | std::ios::out | std::ios::binary);
    file.put('!');
  }
  config.read_only = true;
  config.store.verify_on_open = "full";
  {
    LuxirNode reader(config);
    expectUnavailable(reader, "main", damaged);
  }
  FSDirectory marked(dir, false);
  std::array<std::string, 1> suspects{damaged.name};
  CorruptionMarker::write(marked, suspects, "damaged magic header");
  config.store.verify_on_open = "none";
  LuxirNode reader(config);
  expectUnavailable(reader, "main", damaged);
}

TEST_F(ChecksumVerifyTest, verifiedReaderSurvivesUnlinkAndViewLifetime) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a", "embedding_v", std::vector<float>{1, 0}),
                          flatdoc("id", "b", "embedding_v", std::vector<float>{0, 1})}, UpdateMessage::NO_COMMIT).success);
  h.commit({"*"});
  ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
  auto& registry = node.getCollection("main")->getShard()->getSnapshots();
  // Drop the writer's reusable readers so preparation must open through the view.
  auto snapshot = CommitSnapshot::fromBytes(registry.snapshot()->bytes);
  for (const auto& target : {std::shared_ptr<Directory>(std::make_shared<FSDirectory>(path)),
                            std::shared_ptr<Directory>(std::make_shared<RAMDir>())}) {
    CheckedDirectory dir(target, CheckedDirMode::THROW);
    std::vector<std::string> names;
    for (const auto& descriptor : snapshot->files) {
      auto input = registry.dir.openFile(descriptor.name);
      auto bytes = input->read();
      auto file = dir.createFile(descriptor.name);
      OutputStream out(file.get()); out.write(bytes.data(), bytes.size()); out.close();
      dir.finishFile(*file);
      names.push_back(descriptor.name);
    }
    std::shared_ptr<IndexReader> reader;
    {
      FileVerification verification;
      verification.open(dir, snapshot->files);
      verification.run();
      VerifiedDirectory source(dir, verification);
      EXPECT_THROW(source.openFile(names.front(), true), std::runtime_error);
      dir.sync(names);
      for (const auto& name : names) ASSERT_TRUE(dir.deleteFile(name));
      ReaderManager readers(dir);
      reader = readers.prepare(*snapshot, source);
    }
    EXPECT_EQ(1, reader->liveDocs());
    for (const auto& segment : reader->segments()) for (const auto& aux : segment.auxReaders()) {
      if (auto vector = std::dynamic_pointer_cast<VectorAuxReader>(aux)) {
        EXPECT_NE(nullptr, vector->getFaissIndex());
      }
    }
    registry.publish(snapshot, std::move(reader));
    auto req = localReq(node.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("id", "b").withStats().fields({"id", "embedding_v"});
    req->execute();
    ASSERT_OK(req);
    EXPECT_EQ(1, req->getMatchCount());
    EXPECT_EQ(1u, req->getDocs().size());
  }
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
    EXPECT_EQ(0u, clean.errors);
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

TEST_F(ChecksumVerifyTest, writerRecoveryPreservesSuspectFiles) {
  populate();
  std::filesystem::path dir;
  FileDescriptor suspect;
  {
    LuxirNode node(config);
    dir = dirOf(node, "main");
    suspect = *std::ranges::max_element(node.getCollection("main")->getShard()->getSnapshots().snapshot()->files, {}, &FileDescriptor::size);
  }
  {
    FSDirectory old(dir, false);
    auto manifest = Manifest::load(old);
    auto torn = old.createFile(Manifest::name(manifest.generation + 1));
    OutputStream out(torn.get()); out.write((char)0); out.close(); old.finishFile(*torn);
    std::array<std::string, 2> suspects{suspect.name, "s0_absent"};
    CorruptionMarker::write(old, suspects, "pending scrub hit");
    flip(dir, suspect);
  }
  LuxirNode node(config);
  expectUnavailable(node, "main", suspect);
  auto storage = node.collections().storage().collection(CollectionId::of("main"));
  auto selected = storage.current();
  ASSERT_TRUE(selected);
  EXPECT_NE(dir.filename().string(), *selected);
  EXPECT_EQ(std::vector<std::string>{suspect.name}, CorruptionMarker::read(*storage.open(*selected)));
  EXPECT_NO_THROW(node.getCollection("other"));
}

TEST_F(ChecksumVerifyTest, readOnlyScrubSkipsRetiredFiles) {
  populate();
  config.store.scrub_max_bytes_per_sec = 0;
  LuxirNode writer(config);
  config.read_only = true;
  LuxirNode reader(config);
  auto snapshot = reader.getCollection("main")->getShard()->getSnapshots().snapshot();
  CollectionHelper h(writer, "main");
  ASSERT_TRUE(h.deleteById("b", UpdateMessage::COMMIT).success);
  auto dir = dirOf(reader, "main");
  ASSERT_TRUE(std::ranges::any_of(snapshot->files, [&](const auto& file) { return !std::filesystem::exists(dir / file.name); }));
  EXPECT_EQ(0u, reader.getScrubber().scrub());
  EXPECT_EQ(0u, reader.getScrubber().stats().mismatches);
  EXPECT_EQ(0u, reader.getScrubber().stats().errors);
  EXPECT_EQ(1, reader.getCollection("main")->getReaderManager().getReader()->liveDocs());
}

TEST_F(ChecksumVerifyTest, scrubFailsOnMissingHeldFileOnWriter) {
  populate();
  config.store.scrub_max_bytes_per_sec = 0;
  LuxirNode node(config);
  auto collection = node.getCollection("main");
  auto file = collection->getShard()->getSnapshots().snapshot()->files.front();
  auto dir = dirOf(node, "main");
  std::filesystem::remove(dir / file.name);
  EXPECT_EQ(1u, node.getScrubber().scrub());
  EXPECT_EQ(1u, node.getScrubber().stats().mismatches);
  EXPECT_EQ(0u, node.getScrubber().stats().errors);
  EXPECT_EQ("missing file: " + file.name, collection->getUnavailableReason());
  EXPECT_THROW(node.getCollection("main"), CollectionUnavailableError);
  EXPECT_EQ(std::vector<std::string>{file.name}, CorruptionMarker::read(*collection->getShard()->getDirectory()));
}

// ELOOP is deterministic regardless of the process's filesystem privileges.
TEST_F(ChecksumVerifyTest, scrubRetriesOtherIOErrorsWithoutFailingTheCollection) {
  populate();
  config.store.scrub_max_bytes_per_sec = 0;
  LuxirNode node(config);
  ASSERT_EQ(1, node.getCollection("main")->getReaderManager().getReader()->liveDocs());
  auto snapshot = node.getCollection("main")->getShard()->getSnapshots().snapshot();
  auto file = dirOf(node, "main") / snapshot->files.front().name;
  auto saved = file.string() + ".saved";
  std::filesystem::rename(file, saved);
  std::filesystem::create_symlink(file.filename(), file);
  EXPECT_EQ(0u, node.getScrubber().scrub());
  EXPECT_EQ(0u, node.getScrubber().stats().mismatches);
  EXPECT_EQ(1u, node.getScrubber().stats().errors);
  EXPECT_EQ(0u, count(file.parent_path(), CorruptionMarker::kPrefix));
  EXPECT_EQ(1, node.getCollection("main")->getReaderManager().getReader()->liveDocs());
  std::filesystem::remove(file);
  std::filesystem::rename(saved, file);
  EXPECT_EQ(0u, node.getScrubber().scrub());
  EXPECT_EQ(1u, node.getScrubber().stats().errors);
}

TEST_F(ChecksumVerifyTest, startupReportsOtherIOErrorsWithoutRecordingCorruption) {
  populate();
  config.store.verify_on_open = "full";
  std::filesystem::path file;
  {
    LuxirNode node(config);
    file = dirOf(node, "main") / node.getCollection("main")->getShard()->getSnapshots().snapshot()->files.front().name;
  }
  std::filesystem::rename(file, file.string() + ".saved");
  std::filesystem::create_symlink(file.filename(), file);
  LuxirNode node(config);
  auto entry = node.collections().get(CollectionId::of("main"));
  EXPECT_NE(std::string::npos, entry->getUnavailableReason().find("could not verify file: " + file.filename().string()));
  EXPECT_THROW(node.getCollection("main"), CollectionUnavailableError);
  EXPECT_NO_THROW(node.getCollection("other"));
  EXPECT_EQ(0u, count(file.parent_path(), CorruptionMarker::kPrefix));
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
  auto other = dirOf(node, "other") / node.getCollection("other")->getShard()->getSnapshots().snapshot()->files.front().name;
  auto saved = other.string() + ".saved";
  std::filesystem::rename(other, saved);
  std::filesystem::create_symlink(other.filename(), other);
  node.getScrubber().scrub();
  std::filesystem::remove(other);
  std::filesystem::rename(saved, other);
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
  EXPECT_EQ(1, scrub["errors"].get<int64_t>());
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
    auto reader = dir->openChecksum("s0_a", true);
    dir->deleteFile("s0_a");
    EXPECT_EQ(bytes, reader->map()->read()); // mmap also accepts the O_DIRECT descriptor
    EXPECT_TRUE(reader->checksum().matches(good));

    FileDescriptor missing{"s0_none", 1, 1};
    auto alsoBad = write(*dir, "s0_c");
    alsoBad.xxh3 ^= 1;
    std::vector<FileDescriptor> files{bad, alsoBad};
    try {
      verifyFiles(*dir, files);
      FAIL() << "verified a damaged file";
    } catch (const ChecksumMismatchError& e) {
      EXPECT_NE(std::string::npos, std::string(e.what()).find("checksum mismatch: s0_b: expected")) << e.what();
      EXPECT_NE(std::string::npos, std::string(e.what()).find("(and 1 more files)")) << e.what();
    }
    FileVerification verification;
    EXPECT_THROW(verification.open(*dir, std::span(&missing, 1)), ChecksumMismatchError);
    std::vector<FileDescriptor> okay{write(*dir, "s0_d")};
    EXPECT_NO_THROW(verifyFiles(*dir, okay));
  }
}

}
