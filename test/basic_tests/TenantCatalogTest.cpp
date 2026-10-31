// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <future>
#include "luxir/server/TenantCatalog.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class TenantCatalogTest : public LuxirTest, public testing::WithParamInterface<bool> {
protected:
  std::filesystem::path path;
  std::unique_ptr<DirectoryFactory> factory;
  void SetUp() override {
    path = std::filesystem::temp_directory_path() / ("luxir_catalog_" + newUuid());
    if (GetParam()) factory = std::make_unique<FSDirFactory>(path);
    else factory = std::make_unique<RAMDirFactory>();
    factory = std::make_unique<CheckedDirFactory>(std::move(factory), CheckedDirMode::THROW);
    TenantCatalog::initialize(*factory, true);
  }
  void TearDown() override { factory.reset(); std::filesystem::remove_all(path); }
  void populate(TenantCatalog& catalog, std::string_view name, std::string_view id) {
    auto dir = catalog.directory(name, id);
    CommitSnapshotRegistry snapshots(*dir);
    IndexWriter writer(snapshots, {}, nullptr, 10, std::string(id));
  }
};
INSTANTIATE_TEST_SUITE_P(Backends, TenantCatalogTest, testing::Bool());

TEST_P(TenantCatalogTest, createReplaceAndRemove) {
  TenantCatalog catalog(*factory);
  auto first = catalog.reserve("main", {}, true);
  EXPECT_EQ("reserved", catalog.read().entries.front().state);
  populate(catalog, "main", first);
  catalog.activate("main", first);
  EXPECT_EQ(first, catalog.current("main"));
  EXPECT_THROW(catalog.reserve("main", {}, true), CollectionExistsError);
  auto next = catalog.reserve("main");
  populate(catalog, "main", next);
  catalog.activate("main", next);
  EXPECT_EQ("retiring", catalog.read().entries.front().state);
  TenantCatalog reopened(*factory);
  reopened.cleanup(false);
  EXPECT_EQ(std::vector<std::string>{next}, reopened.incarnations("main"));
  EXPECT_FALSE(reopened.directory("main", first)->openFile("commit_1.1"));
  auto high = reopened.read().high;
  reopened.remove("main");
  EXPECT_TRUE(reopened.read().entries.empty());
  EXPECT_EQ(high, reopened.read().high);
  EXPECT_TRUE(factory->children(StorageRoot::AUTHORITATIVE, "t/t0/i").empty());
  EXPECT_FALSE(IncarnationState::read(*reopened.directory("main", first)));
  EXPECT_FALSE(IncarnationState::read(*reopened.directory("main", next)));
  auto recreated = reopened.reserve("main");
  EXPECT_GT(recreated, next);
  populate(reopened, "main", recreated);
  reopened.activate("main", recreated);
  EXPECT_EQ(recreated, reopened.current("main"));
}

TEST_P(TenantCatalogTest, startupKeepsFollowerReservationsAndFinishesRetirement) {
  TenantCatalog catalog(*factory);
  auto old = catalog.reserve("main");
  populate(catalog, "main", old);
  catalog.activate("main", old);
  auto pending = catalog.reserve("main");
  populate(catalog, "main", pending);
  TenantCatalog follower(*factory);
  follower.cleanup(true);
  EXPECT_EQ(pending, follower.reserve("main", pending));
  EXPECT_TRUE(follower.directory("main", pending)->openFile("commit_1.1"));
  follower.activate("main", pending); // crash before deleting the old directory
  auto abandoned = follower.reserve("other");
  populate(follower, "other", abandoned);
  TenantCatalog restarted(*factory);
  restarted.cleanup(true);
  EXPECT_FALSE(restarted.directory("main", old)->openFile("commit_1.1"));
  EXPECT_TRUE(restarted.directory("other", abandoned)->openFile("commit_1.1"));
  restarted.cleanup(false);
  EXPECT_TRUE(restarted.incarnations("other").empty());
  EXPECT_FALSE(restarted.directory("other", abandoned)->openFile("commit_1.1"));
  EXPECT_EQ(pending, restarted.current("main"));
}

TEST_P(TenantCatalogTest, staleMutationIsReevaluated) {
  TenantCatalog first(*factory), second(*factory);
  first.reserve("seed");
  std::promise<void> read, resume;
  auto resumed = resume.get_future().share();
  std::atomic<unsigned> attempts{0};
  Signal::listen("catalogRead", [&](void* source, void*, void*) -> void* {
    if (source == &first) { read.set_value(); resumed.wait(); }
    return nullptr;
  });
  Signal::listen("publishSynced", [&](void*, void* name, void*) -> void* {
    if (*(std::string_view*)name == "CATALOG") attempts++;
    return nullptr;
  });
  auto unlisten = scope_guard([] { Signal::unlisten("catalogRead"); Signal::unlisten("publishSynced"); });
  auto pending = std::async(std::launch::async, [&] { return first.reserve("first"); });
  read.get_future().wait();
  auto other = second.reserve("second");
  resume.set_value();
  EXPECT_GT(pending.get(), other);
  EXPECT_EQ(3u, second.read().entries.size());
  EXPECT_EQ(3u, second.read().rev);
  EXPECT_EQ(3u, attempts.load()); // one applied mutation, one stale CAS, then its retry
}

TEST_P(TenantCatalogTest, clockBehindHighAndOrphansCannotReuseIds) {
  auto tenant = factory->directory(StorageRoot::AUTHORITATIVE, "t/t0");
  std::string bytes = R"({"rev":1,"high":8796093022207,"entries":[]})";
  tenant->publishFile("CATALOG", std::as_bytes(std::span(bytes)), Directory::PublishMode::CREATE_ONLY);
  TenantCatalog catalog(*factory);
  auto first = catalog.reserve("first");
  EXPECT_EQ("80000000000", first);
  catalog.remove("first");
  auto next = catalog.reserve("next");
  EXPECT_GT(next, first);
  auto orphan = catalog.directory("orphan", "80000000002");
  orphan->publishFile("data", {}, Directory::PublishMode::CREATE_ONLY);
  catalog.cleanup(false);
  EXPECT_FALSE(catalog.directory("orphan", "80000000002")->openFile("data"));
  EXPECT_GT(catalog.reserve("later"), "80000000002");
}

TEST_P(TenantCatalogTest, lostPublicationReplyIsReconciled) {
  TenantCatalog catalog(*factory);
  Signal::listen("catalogPublished", [](void*, void*, void*) -> void* { throw FileIOException("lost reply"); });
  auto unlisten = scope_guard([] { Signal::unlisten("catalogPublished"); });
  auto id = catalog.reserve("main");
  auto state = catalog.read();
  ASSERT_EQ(1u, state.entries.size());
  EXPECT_EQ(id, state.entries[0].id);
  EXPECT_EQ(1u, state.rev);
  EXPECT_EQ(TenantCatalog::incarnationNumber(id), state.high);
}
