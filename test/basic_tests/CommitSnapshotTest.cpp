// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <latch>
#include "luxir/util/Signal.h"
#include <filesystem>
#include "test/TestUtils.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "luxir/store/Manifest.h"
#include "test/LuxirTest.h"

using namespace luxir;
using namespace luxir::test;
using namespace std::chrono_literals;

class CommitSnapshotTest : public LuxirTest {};

TEST_F(CommitSnapshotTest, slowTransferSurvivesDeletesAndMerge) {
  auto path = std::filesystem::temp_directory_path() / "luxir-snapshot-transfer";
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  config.store.checked_dir.sync = "throw";
  LuxirNode node(config);
  CollectionHelper h(node, "main");
  auto w = h.getIndexWriter();
  w->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(h.indexAll({flatdoc("id", "a"), flatdoc("id", "b"), flatdoc("id", "c")},
                        UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
  auto pin = w->snapshots.acquire();
  ASSERT_TRUE(h.deleteById("b", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(h.index(flatdoc("id", "d"), UpdateMessage::COMMIT, false, 1).success);
  ASSERT_EQ(1u, w->snapshots.readers.getReader()->segments().size());
  uint64_t bytes = 0;
  for (const auto& desc : pin->files) {
    auto file = w->snapshots.openFile(pin->id, desc.name);
    auto data = file->read();
    EXPECT_EQ(desc.xxh3, XXH3_64bits(data.data(), data.size()));
    EXPECT_TRUE(w->snapshots.touch(pin->id, data.size()));
    bytes += desc.size;
  }
  EXPECT_EQ(bytes, w->snapshots.stats().retainedBytes);
  auto names = pin->files;
  auto inFlight = w->snapshots.openFile(pin->id, names.front().name);
  w->snapshots.evictOldest();
  EXPECT_EQ(0u, w->snapshots.stats().retainedBytes);
  for (const auto& file : names) EXPECT_EQ(nullptr, w->dir.openFile(file.name));
  auto data = inFlight->read();
  EXPECT_EQ(names.front().xxh3, XXH3_64bits(data.data(), data.size()));
  expectValidInventory(w->dir, *readDurableIndexInfo(w->dir));
}

TEST_F(CommitSnapshotTest, budgetCountsUniqueRetiredBytesAndEvictsOldest) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  auto w = h.getIndexWriter();
  w->snapshots.setPolicy({60s, 0});
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto first = w->snapshots.acquire();
  auto second = w->snapshots.acquire();
  EXPECT_EQ(0u, w->snapshots.stats().retainedBytes); // current never counts
  auto desc = first->files.front();
  auto inFlight = w->snapshots.openFile(first->id, desc.name);
  w->snapshots.setPolicy({60s, UINT64_MAX});
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT, false, 1).success);
  uint64_t bytes = 0;
  for (const auto& file : first->files) bytes += file.size;
  EXPECT_EQ(bytes, w->snapshots.stats().retainedBytes); // not twice the bytes
  auto current = w->snapshots.acquire();
  w->snapshots.setPolicy({60s, bytes - 1});
  EXPECT_EQ(1u, w->snapshots.stats().budgetDrops);
  EXPECT_EQ(1u, w->snapshots.stats().pins);
  EXPECT_FALSE(w->snapshots.touch(first->id, 1));
  EXPECT_THROW(w->snapshots.openFile(second->id, desc.name), SnapshotExpiredError);
  auto data = inFlight->read(); // eviction never interrupts an already-open file
  EXPECT_EQ(desc.xxh3, XXH3_64bits(data.data(), data.size()));
  EXPECT_NE(nullptr, w->snapshots.openFile(current->id, current->files.front().name));
}

TEST_F(CommitSnapshotTest, idleExpiryIsLazyAndOnlyBytesRenew) {
  RAMDir dir;
  auto time = CommitSnapshotRegistry::Clock::now();
  CommitSnapshotRegistry snapshots(dir, {}, [&] { return time; });
  IndexWriter writer(snapshots);
  snapshots.setPolicy({100ms, UINT64_MAX});
  auto first = snapshots.acquire();
  writer.setSchema(Schema::createDefaultSchema());
  auto active = snapshots.acquire();
  time += 60ms;
  EXPECT_TRUE(snapshots.touch(first->id, 0));
  EXPECT_TRUE(snapshots.touch(active->id, 1));
  time += 40ms;
  EXPECT_EQ(1u, snapshots.stats().idleDrops);
  EXPECT_FALSE(snapshots.touch(first->id, 1));
  EXPECT_TRUE(snapshots.touch(active->id, 0));
  time += 60ms;
  EXPECT_EQ(2u, snapshots.stats().idleDrops);
  EXPECT_EQ(0u, snapshots.stats().pins);
}

TEST_F(CommitSnapshotTest, reacquireRenewsSharedReservation) {
  RAMDir dir;
  auto time = CommitSnapshotRegistry::Clock::now();
  CommitSnapshotRegistry snapshots(dir, {}, [&] { return time; });
  IndexWriter writer(snapshots);
  snapshots.setPolicy({100ms, UINT64_MAX});
  auto first = snapshots.acquire();
  time += 90ms;
  EXPECT_EQ(first, snapshots.acquire());
  time += 90ms;
  EXPECT_EQ(1u, snapshots.stats().pins);
  time += 10ms;
  EXPECT_EQ(0u, snapshots.stats().pins);
}

TEST_F(CommitSnapshotTest, currentAndSchemaOnlySnapshotsDoNotConsumeBudget) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  auto w = h.getIndexWriter();
  w->snapshots.setPolicy({60s, 0});
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto pin = w->snapshots.acquire();
  w->setSchema(Schema::createDefaultSchema());
  auto reader = w->snapshots.readers.getReader();
  EXPECT_NE(pin->id.index_gen, reader->commitId());
  EXPECT_EQ(0u, w->snapshots.stats().budgetDrops);
  EXPECT_EQ(0u, w->snapshots.stats().retainedBytes);
  EXPECT_THROW(w->snapshots.openFile(pin->id, "../write.lock"), ApiError);
  w->snapshots.evictOldest();
  EXPECT_THROW(w->snapshots.openFile(pin->id, pin->files.front().name), SnapshotExpiredError);
  EXPECT_EQ(1, reader->liveDocs());
}

TEST_F(CommitSnapshotTest, writerlessManagerInstallsExplicitSnapshots) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  auto w = h.getIndexWriter();
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto pin = w->snapshots.acquire();
  RAMDir follower;
  for (const auto& desc : pin->files) {
    auto bytes = w->snapshots.openFile(pin->id, desc.name)->read();
    auto file = follower.createFile(desc.name);
    OutputStream out(file.get());
    out.write(bytes.data(), bytes.size());
    out.close();
    follower.finishFile(*file);
  }
  CommitSnapshotRegistry installed(follower);
  auto& readers = installed.readers;
  auto snapshot = CommitSnapshot::fromBytes(pin->bytes);
  installed.publish(snapshot, readers.prepare(*snapshot));
  auto first = readers.getReader();
  EXPECT_EQ(1, first->liveDocs());
  w->setSchema(Schema::createDefaultSchema());
  snapshot = w->snapshots.readers.snapshot();
  installed.publish(snapshot, readers.prepare(*snapshot));
  auto second = readers.getReader(UINT64_MAX);
  EXPECT_NE(first->schema(), second->schema());
  EXPECT_EQ(&first->segments()[0], &second->segments()[0]);
  EXPECT_EQ(pin->schema->gen_, first->schema()->gen_);
  EXPECT_EQ(w->snapshots.readers.snapshot()->schema, second->schema());
  ASSERT_TRUE(h.index(flatdoc("id", "missing"), UpdateMessage::COMMIT).success);
  EXPECT_THROW(readers.prepare(*w->snapshots.readers.snapshot()), std::exception);
  EXPECT_EQ(second, readers.getReader()); // a failed install leaves the old view serving
}

TEST_F(CommitSnapshotTest, restartReclaimsFilesOfLostReservations) {
  RAMDir persisted;
  {
    LuxirNode node;
    CollectionHelper h(node, "main");
    auto w = h.getIndexWriter();
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    auto pin = w->snapshots.acquire();
    ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT, false, 1).success);
    ASSERT_GT(w->snapshots.stats().retainedBytes, 0u);
    // Capture the real directory before orderly shutdown releases reservations.
    std::vector<Directory::FileInfo> files;
    w->dir.listFiles(files);
    for (const auto& info : files) {
      auto input = w->dir.openFile(info.name);
      auto bytes = input->read();
      auto file = persisted.createFile(info.name);
      OutputStream out(file.get());
      out.write(bytes.data(), bytes.size());
      out.close();
      persisted.finishFile(*file);
    }
  }
  auto before = persisted.totalBytes();
  CommitSnapshotRegistry reopenedSnapshots(persisted);
  IndexWriter reopened(reopenedSnapshots);
  EXPECT_LT(persisted.totalBytes(), before);
  EXPECT_EQ(2, reopened.snapshots.readers.getReader()->liveDocs());
  EXPECT_EQ(0u, reopened.snapshots.stats().pins);
  expectValidInventory(persisted, *readDurableIndexInfo(persisted));
}

TEST_F(CommitSnapshotTest, droppedReservationCannotUnlinkRecreatedCollection) {
  auto path = std::filesystem::temp_directory_path() / "luxir-pin-recreation";
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  LuxirNode node(config);
  CollectionHelper old(node, "main");
  auto writer = old.getIndexWriter();
  ASSERT_TRUE(old.index(flatdoc("id", "old"), UpdateMessage::COMMIT).success);
  auto pin = writer->snapshots.acquire();
  auto name = pin->files.front().name;
  ASSERT_TRUE(old.index(flatdoc("id", "merged"), UpdateMessage::COMMIT, false, 1).success);
  std::latch collected(1), resume(1);
  Signal::listen("snapshotReservationDropped", [&](void* source, void*, void*) -> void* {
    if (source == &writer->snapshots) {
      collected.count_down();
      resume.wait();
    }
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("snapshotReservationDropped"); });
  std::thread release([&] { writer->snapshots.evictOldest(); });
  collected.wait();
  node.deleteCollection("main");
  CollectionHelper fresh(node, "main");
  auto result = fresh.index(flatdoc("id", "new"), UpdateMessage::COMMIT);
  resume.count_down();
  release.join();
  Signal::unlisten("snapshotReservationDropped");
  ASSERT_TRUE(result.success);
  auto current = fresh.getIndexWriter();
  EXPECT_NE(nullptr, current->dir.openFile(name));
  EXPECT_EQ(1, fresh.collection().getReaderManager().getReader()->liveDocs());
}

TEST_F(CommitSnapshotTest, twoRequestsShareOneReservation) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto& snapshots = h.collection().getShard()->getSnapshots();
  auto first = snapshots.acquire();
  auto id = first->id;
  auto name = first->files.front().name;
  first.reset(); // End of the snapshot request does not release the reservation.
  auto second = snapshots.acquire();
  EXPECT_EQ(id, second->id);
  EXPECT_EQ(1u, snapshots.stats().pins);
  EXPECT_NE(nullptr, snapshots.openFile(id, name));
  EXPECT_TRUE(snapshots.touch(second->id, 1));
  snapshots.evictOldest();
  EXPECT_THROW(snapshots.openFile(second->id, name), SnapshotExpiredError);
}

TEST_F(CommitSnapshotTest, concurrentBudgetEviction) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  auto& snapshots = h.collection().getShard()->getSnapshots();
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto first = snapshots.acquire();
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT, false, 1).success);
  std::latch start(2);
  std::thread release([&] { start.count_down(); start.wait(); snapshots.evictOldest(); });
  start.count_down();
  start.wait();
  snapshots.setPolicy({60s, 0});
  release.join();
  auto stats = snapshots.stats();
  EXPECT_EQ(0u, stats.pins);
  EXPECT_EQ(0u, stats.retainedBytes);
  EXPECT_LE(stats.budgetDrops, 1u);
  for (const auto& file : first->files) EXPECT_EQ(nullptr, snapshots.dir.openFile(file.name));
  expectValidInventory(snapshots.dir, *readDurableIndexInfo(snapshots.dir));
}

TEST_F(CommitSnapshotTest, fallbackDoesNotSweepNewestFiles) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  auto previous = writer->snapshots.snapshot();
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  auto newest = writer->snapshots.snapshot();
  writer->close();
  Manifest::write(writer->dir, previous->id.index_gen, *previous->bytes);
  auto name = Manifest::name(newest->id.index_gen);
  auto file = writer->dir.createFile(name);
  OutputStream out(file.get());
  out.writeInt(42); // Simulate a torn newest root with recoverable data still present.
  out.close();
  writer->dir.finishFile(*file);
  CommitSnapshotRegistry recovered(writer->dir);
  IndexWriter reopened(recovered);
  EXPECT_NE(previous->id.incarnation, recovered.snapshot()->id.incarnation);
  EXPECT_GT(recovered.snapshot()->id.index_gen, newest->id.index_gen);
  EXPECT_EQ(1, recovered.readers.getReader()->liveDocs());
  for (const auto& desc : newest->files) EXPECT_NE(nullptr, writer->dir.openFile(desc.name));
}

TEST_F(CommitSnapshotTest, segmentNamesSurviveEmptySnapshotAndRestart) {
  auto path = std::filesystem::temp_directory_path() / "luxir-empty-restart";
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  uint64_t oldSegment = 0;
  std::string incarnation;
  {
    LuxirNode node(config);
    CollectionHelper h(node, "main");
    ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    auto info = readDurableIndexInfo(h.getIndexWriter()->dir);
    oldSegment = info->segments.front().seg_id;
    incarnation = info->incarnation;
    ASSERT_TRUE(h.deleteById("a", UpdateMessage::COMMIT).success);
    auto empty = readDurableIndexInfo(h.getIndexWriter()->dir);
    ASSERT_TRUE(empty->segments.empty());
    EXPECT_GE(empty->last_seg_id, oldSegment);
  }
  LuxirNode reopened(config);
  CollectionHelper h(reopened, "main");
  ASSERT_TRUE(h.index(flatdoc("id", "b"), UpdateMessage::COMMIT).success);
  auto info = readDurableIndexInfo(h.getIndexWriter()->dir);
  EXPECT_EQ(incarnation, info->incarnation);
  EXPECT_GT(info->segments.front().seg_id, oldSegment);
}

TEST_F(CommitSnapshotTest, observerFailureDoesNotFailDurablePublication) {
  LuxirNode node;
  CollectionHelper h(node, "main");
  auto writer = h.getIndexWriter();
  writer->snapshots.onPublish = [](const CommitSnapshot&) { throw std::runtime_error("observer failure"); };
  auto before = writer->snapshots.snapshot()->id;
  ASSERT_TRUE(h.index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
  EXPECT_GT(writer->snapshots.snapshot()->id.index_gen, before.index_gen);
  EXPECT_FALSE(writer->isClosed());
}

TEST_F(CommitSnapshotTest, storagePressureDropsOldestReservation) {
  LuxirConfig config; config.store.ram_limit_mb = 2;
  LuxirNode node(config);
  CollectionHelper first(node, "main"), second(node, "other");
  for (auto* h : {&first, &second}) {
    ASSERT_TRUE(h->index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    h->collection().getShard()->getSnapshots().acquire();
    ASSERT_TRUE(h->index(flatdoc("id", "b"), UpdateMessage::COMMIT, false, 1).success);
    h->collection().getReaderManager().getReader();
  }
  auto& snapshots = first.collection().getShard()->getSnapshots();
  ASSERT_GT(snapshots.stats().retainedBytes, 0);
  auto current = snapshots.acquire();
  Directory::FileCreateOptions options; options.expectedSize = 2 * 1024 * 1024 - node.storageBytes() + 1;
  auto file = snapshots.dir.createFile("download", options);
  EXPECT_EQ(1, snapshots.stats().pins);
  EXPECT_NO_THROW(snapshots.openFile(current->id, current->files.front().name));
  EXPECT_EQ(1, snapshots.stats().budgetDrops);
  EXPECT_EQ(1, second.collection().getShard()->getSnapshots().stats().pins);
  EXPECT_LE(node.storageBytes(), 2 * 1024 * 1024);
}

TEST_F(CommitSnapshotTest, storagePressureSkipsOpenTransfersAndCurrent) {
  LuxirConfig config; config.store.ram_limit_mb = 2;
  LuxirNode node(config);
  CollectionHelper first(node, "main"), second(node, "other");
  std::shared_ptr<InputFile> transfer;
  for (auto* h : {&first, &second}) {
    auto& snapshots = h->collection().getShard()->getSnapshots();
    ASSERT_TRUE(h->index(flatdoc("id", "a"), UpdateMessage::COMMIT).success);
    auto old = snapshots.acquire();
    if (h == &first) transfer = snapshots.openFile(old->id, old->files.front().name);
    ASSERT_TRUE(h->index(flatdoc("id", "b"), UpdateMessage::COMMIT, false, 1).success);
    h->collection().getReaderManager().getReader();
  }
  auto& snapshots = first.collection().getShard()->getSnapshots();
  snapshots.acquire(); // The current reservation must also survive allocation failure.
  auto allocate = [&] {
    Directory::FileCreateOptions options; options.expectedSize = 2 * 1024 * 1024 - node.storageBytes() + 1;
    return snapshots.dir.createFile("download", options);
  };
  auto file = allocate();
  EXPECT_EQ(2, snapshots.stats().pins);
  EXPECT_EQ(0, second.collection().getShard()->getSnapshots().stats().pins);
  file.reset();
  EXPECT_THROW(allocate(), ApiError);
  EXPECT_EQ(2, snapshots.stats().pins);
  transfer.reset();
  EXPECT_NO_THROW(file = allocate());
  EXPECT_EQ(1, snapshots.stats().pins);
}
