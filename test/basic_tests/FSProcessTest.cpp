// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <glaze/glaze.hpp>
#include "test/StorageTestUtils.h"
#include "test/TestProcess.h"
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/server/TenantCatalog.h"
#include "luxir/util/Uuid.h"

using namespace luxir;
using namespace luxir::test;

TEST(FSProcessTest, matchAcrossProcesses) {
  constexpr int rounds = 24;
  constexpr auto root = StorageRoot::AUTHORITATIVE;
  if (TestProcess::role()) {
    FSDirFactory factory(TestProcess::path(), true);
    auto dir = factory.directory(root, "t/t0");
    for (int round = 0; round < rounds; round++) {
      EXPECT_EQ('r', TestProcess::command());
      auto before = IncarnationState::read(*dir).value();
      auto next = before.value;
      next.rev++;
      auto bytes = glz::write_json(next).value();
      Signal::listen("publishSynced", [](void*, void*, void*) -> void* {
        TestProcess::reply('s');
        EXPECT_EQ('g', TestProcess::command());
        return nullptr;
      });
      auto applied = dir->publishFile("STATE", std::as_bytes(std::span(bytes)), Directory::PublishCondition::match(before.version));
      Signal::unlisten("publishSynced");
      TestProcess::reply(applied ? '1' : '0');
      if (!applied) {
        before = IncarnationState::read(*dir).value();
        next = before.value;
        next.rev++;
        bytes = glz::write_json(next).value();
        EXPECT_TRUE(dir->publishFile("STATE", std::as_bytes(std::span(bytes)), Directory::PublishCondition::match(before.version)));
      }
      TestProcess::reply('d');
    }
    return;
  }
  auto path = std::filesystem::temp_directory_path() / ("luxir-process-cas-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirFactory factory(path);
  auto dir = factory.directory(root, "t/t0");
  reserveIncarnation(*dir);
  TestProcess first(path, "first"), second(path, "second");
  for (int round = 0; round < rounds; round++) {
    first.send('r'); second.send('r');
    ASSERT_EQ('s', first.receive()); ASSERT_EQ('s', second.receive());
    // Opening an owned view must not remove the other live processes' scratch files.
    factory.directory(root, "t/t0");
    first.send('g'); second.send('g');
    EXPECT_EQ(1, (first.receive() == '1') + (second.receive() == '1'));
    EXPECT_EQ('d', first.receive()); EXPECT_EQ('d', second.receive());
    EXPECT_EQ(1u + 2 * (round + 1), IncarnationState::read(*dir)->value.rev);
  }
  EXPECT_EQ(0, first.join()); EXPECT_EQ(0, second.join());
}

TEST(FSProcessTest, writerTakeover) {
  constexpr auto root = StorageRoot::AUTHORITATIVE;
  if (auto role = TestProcess::role()) {
    bool first = std::string_view(role) == "first";
    FSDirFactory factory(TestProcess::path(), true);
    auto dir = factory.directory(root, "t/t0/i/10000000000");
    CommitSnapshotRegistry snapshots(*dir);
    IndexWriter writer(snapshots, {}, nullptr, 10, "10000000000");
    EXPECT_EQ(first ? 1u : 2u, IncarnationState::read(*dir)->value.epoch);
    TestProcess::reply('o');
    EXPECT_EQ('g', TestProcess::command());
    auto& inverter = writer.obtainInverter();
    inverter.startDoc();
    std::string id = first ? "superseded" : "winner";
    inverter.getIndexHandler("id").index(inverter, id);
    inverter.finishDoc();
    writer.releaseInverter(inverter, true);
    if (first) {
      try { writer.commit(); FAIL() << "superseded writer committed"; }
      catch (const std::exception& e) { EXPECT_NE(std::string::npos, std::string(e.what()).find("superseded")); }
      EXPECT_TRUE(writer.isClosed());
    } else writer.commit();
    return;
  }
  auto path = std::filesystem::temp_directory_path() / ("luxir-process-writer-" + newUuid());
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  FSDirFactory factory(path);
  TenantCatalog::initialize(factory, true);
  TenantCatalog catalog(factory, "t0", "test");
  auto id = catalog.reserve("main", "10000000000");
  TestProcess first(path, "first");
  ASSERT_EQ('o', first.receive());
  catalog.activate("main", id);
  TestProcess second(path, "second");
  ASSERT_EQ('o', second.receive());
  second.send('g');
  ASSERT_EQ(0, second.join());
  auto dir = catalog.directory(id);
  auto winner = Manifest::load(*dir);
  first.send('g');
  ASSERT_EQ(0, first.join());
  auto after = Manifest::load(*dir);
  EXPECT_EQ(*winner.bytes, *after.bytes);
  EXPECT_EQ(1, IndexReader(*dir).liveDocs());
  auto info = readDurableIndexInfo(*dir);
  EXPECT_EQ(2u, info->segments[0].creator_epoch);
  EXPECT_TRUE(dir->openFile("s01.1_00"));
  EXPECT_TRUE(dir->openFile("s01.2_00"));
  expectValidInventory(*dir, *info);
}
