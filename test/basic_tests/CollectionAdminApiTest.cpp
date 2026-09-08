// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/AllQuery.h"
#include "luxir/reader/Postings.h"
#include "luxir/server/HttpServer.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

class CollectionAdminApiTest : public LuxirTest {};

class CollectionAdminDataDir {
  std::filesystem::path path_;

public:
  explicit CollectionAdminDataDir(std::string_view prefix) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
        (std::string(prefix) + "_" + std::to_string(stamp));
    std::filesystem::remove_all(path_);
  }

  ~CollectionAdminDataDir() {
    std::filesystem::remove_all(path_);
  }

  const std::filesystem::path& path() const { return path_; }
};

static LuxirConfig fsConfig(const CollectionAdminDataDir& data) {
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = data.path().string();
  return config;
}

TEST_F(CollectionAdminApiTest, httpLifecycleAndValidation) {
  CollectionAdminDataDir data("luxir_collection_admin_lifecycle");
  auto config = fsConfig(data);
  LuxirNode node(config);
  HttpServer server(node, 2, 0);
  server.start();
  int port = server.getPort();

  auto created = httpRequest(port, http::verb::post, "/collections/_create",
                             R"({"name":"admin_lifecycle"})");
  ASSERT_EQ(200, created.result_int()) << created.body();
  EXPECT_NE(created.body().find(R"("name":"admin_lifecycle")"), std::string::npos);

  auto duplicate = httpRequest(port, http::verb::put, "/collections/_create",
                               R"({"name":"admin_lifecycle"})");
  EXPECT_EQ(409, duplicate.result_int()) << duplicate.body();

  auto invalid = httpRequest(port, http::verb::post, "/collections/_create",
                             R"({"name":"_reserved"})");
  EXPECT_EQ(400, invalid.result_int()) << invalid.body();

  auto missing = httpRequest(port, http::verb::post, "/collections/_delete",
                             R"({"name":"admin_missing"})");
  EXPECT_EQ(404, missing.result_int()) << missing.body();
  auto empty = httpRequest(port, http::verb::post, "/collections/_delete",
                           R"({"name":""})");
  EXPECT_EQ(400, empty.result_int()) << empty.body();
  auto absent = httpRequest(port, http::verb::post, "/collections/_delete", R"({})");
  EXPECT_EQ(400, absent.result_int()) << absent.body();

  auto indexed = httpRequest(
      port, http::verb::post, "/collections/admin_lifecycle/_update",
      R"({"docs":[{"id":"old","title_w":"old generation"}],"commit":{}})");
  ASSERT_EQ(200, indexed.result_int()) << indexed.body();

  auto deleted = httpRequest(port, http::verb::post, "/collections/_delete",
                             R"({"name":"admin_lifecycle"})");
  ASSERT_EQ(200, deleted.result_int()) << deleted.body();
  EXPECT_FALSE(std::filesystem::exists(data.path() / "c" / "admin_lifecycle"));

  HttpReq missingSearch(port);
  missingSearch.collection("admin_lifecycle").matchQuery("title_w", "old").withStats().execute();
  EXPECT_NE(missingSearch.rawResponse().find("does not exist"), std::string::npos)
      << missingSearch.rawResponse();

  auto recreated = httpRequest(port, http::verb::put, "/collections/_create",
                               R"({"name":"admin_lifecycle"})");
  ASSERT_EQ(200, recreated.result_int()) << recreated.body();
  auto fresh = node.getCollection("admin_lifecycle");
  EXPECT_EQ(0, fresh->getShard()->getIndexWriter()->getIndexReader()->liveDocs());

  auto deleteMain = httpRequest(port, http::verb::post, "/collections/_delete",
                                R"({"name":"main"})");
  EXPECT_EQ(200, deleteMain.result_int()) << deleteMain.body();
  EXPECT_FALSE(std::filesystem::exists(data.path() / "c" / "main"));

  server.shutdown();
}

TEST_F(CollectionAdminApiTest, listCollections) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();
  int port = server.getPort();

  ASSERT_EQ(200, httpRequest(port, http::verb::post, "/collections/_create",
                             R"({"name":"list_b"})").result_int());
  ASSERT_EQ(200, httpRequest(port, http::verb::post, "/collections/_create",
                             R"({"name":"list_a"})").result_int());

  // GET and POST /collections/_list and the GET /collections synonym agree.
  auto canonical = httpRequest(port, http::verb::get, "/collections/_list");
  ASSERT_EQ(200, canonical.result_int()) << canonical.body();
  auto posted = httpRequest(port, http::verb::post, "/collections/_list");
  ASSERT_EQ(200, posted.result_int()) << posted.body();
  EXPECT_EQ(canonical.body(), posted.body());
  auto synonym = httpRequest(port, http::verb::get, "/collections");
  ASSERT_EQ(200, synonym.result_int()) << synonym.body();
  EXPECT_EQ(canonical.body(), synonym.body());

  // Names are sorted; the startup default "main" is included.
  const std::string& body = canonical.body();
  auto a = body.find("\"list_a\"");
  auto b = body.find("\"list_b\"");
  auto m = body.find("\"main\"");
  ASSERT_NE(a, std::string::npos) << body;
  ASSERT_NE(b, std::string::npos) << body;
  ASSERT_NE(m, std::string::npos) << body;
  EXPECT_TRUE(a < b && b < m) << body;

  ASSERT_EQ(200, httpRequest(port, http::verb::post, "/collections/_delete",
                             R"({"name":"list_b"})").result_int());
  auto after = httpRequest(port, http::verb::get, "/collections");
  EXPECT_EQ(after.body().find("\"list_b\""), std::string::npos) << after.body();
  EXPECT_NE(after.body().find("\"list_a\""), std::string::npos) << after.body();

  // POST is only accepted on the explicit _list spelling.
  EXPECT_EQ(405, httpRequest(port, http::verb::post, "/collections").result_int());
  EXPECT_EQ(405, httpRequest(port, http::verb::delete_, "/collections/_list").result_int());

  server.shutdown();
}

TEST_F(CollectionAdminApiTest, createWithSchemaPublishesConfiguredCollection) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();

  auto created = httpRequest(
      server.getPort(), http::verb::post, "/collections/_create",
      R"({"name":"admin_schema","schema":{"fields":{"sku":{"type":"string"}}}})");
  ASSERT_EQ(200, created.result_int()) << created.body();

  auto collection = node.getCollection("admin_schema");
  EXPECT_FALSE(isDefaultSchema(collection->getSchema()));
  EXPECT_NE(collection->getSchema()->fieldTypeMap.find("sku"),
            collection->getSchema()->fieldTypeMap.end());

  auto schema = httpRequest(server.getPort(), http::verb::get,
                            "/collections/admin_schema/_schema");
  EXPECT_EQ(200, schema.result_int()) << schema.body();
  EXPECT_NE(schema.body().find(R"("sku")"), std::string::npos) << schema.body();

  auto deleted = httpRequest(server.getPort(), http::verb::post, "/collections/_delete",
                             R"({"name":"admin_schema"})");
  EXPECT_EQ(200, deleted.result_int()) << deleted.body();
  server.shutdown();
}

TEST_F(CollectionAdminApiTest, deleteWhileIndexingRejectsRacingBatchesCleanly) {
  CollectionAdminDataDir data("luxir_collection_admin_index_race");
  auto config = fsConfig(data);
  config.ingest.auto_create_collection = false;
  LuxirNode node(config);
  HttpServer server(node, 2, 0);
  server.start();
  int port = server.getPort();

  auto created = httpRequest(port, http::verb::post, "/collections/_create",
                             R"({"name":"admin_race"})");
  ASSERT_EQ(200, created.result_int()) << created.body();
  auto seed = httpRequest(port, http::verb::post, "/collections/admin_race/_update",
                          R"({"docs":[{"id":"seed"}],"commit":{}})");
  ASSERT_EQ(200, seed.result_int()) << seed.body();

  std::latch firstBatch(1);
  std::atomic<bool> sawFailure{false};
  std::atomic<int> unexpectedStatus{0};
  std::thread indexer([&] {
    for (int i = 0; i < 2000; i++) {
      auto response = httpRequest(
          port, http::verb::post, "/collections/admin_race/_update",
          R"({"docs":[{"id":"race-)" + std::to_string(i) + R"("}]})");
      if (i == 0) firstBatch.count_down();
      if (response.result_int() == 200) continue;
      // unavailable while the delete runs, not found once it is done
      if (response.result_int() == 503 || response.result_int() == 404) {
        sawFailure = true;
      } else {
        unexpectedStatus = response.result_int();
      }
      break;
    }
  });

  firstBatch.wait();
  auto deleted = httpRequest(port, http::verb::post, "/collections/_delete",
                             R"({"name":"admin_race"})");
  indexer.join();

  ASSERT_EQ(200, deleted.result_int()) << deleted.body();
  EXPECT_TRUE(sawFailure.load());
  EXPECT_EQ(0, unexpectedStatus.load());
  EXPECT_FALSE(std::filesystem::exists(data.path() / "c" / "admin_race"));
  server.shutdown();
}

TEST_F(CollectionAdminApiTest, heldReaderSearchSurvivesDelete) {
  CollectionAdminDataDir data("luxir_collection_admin_held_reader");
  auto config = fsConfig(data);
  config.ingest.auto_create_collection = false;
  LuxirNode node(config);
  node.createCollection(nullptr, "admin_held_reader");
  CollectionHelper helper(node, "admin_held_reader");
  auto indexed = helper.index(flatdoc("id", "held"), UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success) << indexed.error_message;

  auto writer = helper.getIndexWriter();
  auto reader = writer->getIndexReader();
  node.deleteCollection("admin_held_reader");

  EXPECT_THROW(writer->getIndexReader(), IndexWriterClosedError);
  EXPECT_FALSE(std::filesystem::exists(data.path() / "c" / "admin_held_reader"));

  MemPool pool;
  Query::Context context(pool, *reader);
  AllQuery query;
  auto* weight = query.createWeight(context, 0);
  int32_t matches = 0;
  for (auto& segment : reader->segments()) {
    auto* scorer = weight->createScorer(pool, segment);
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      matches++;
    }
  }
  EXPECT_EQ(1, matches);
}

TEST_F(CollectionAdminApiTest, restartKeepsDeletedCollectionAbsentAndPurgesTrash) {
  CollectionAdminDataDir data("luxir_collection_admin_restart");
  auto config = fsConfig(data);
  config.ingest.auto_create_collection = false;

  {
    LuxirNode node(config);
    node.createCollection(nullptr, "admin_restart");
    CollectionHelper helper(node, "admin_restart");
    ASSERT_TRUE(helper.index(flatdoc("id", "restart"), UpdateMessage::COMMIT).success);
    node.deleteCollection("admin_restart");
    EXPECT_FALSE(std::filesystem::exists(data.path() / "c" / "admin_restart"));
  }

  std::filesystem::create_directories(data.path() / "trash" / "interrupted");
  std::ofstream(data.path() / "trash" / "interrupted" / "file") << "stale";

  {
    LuxirNode restarted(config);
    EXPECT_THROW(restarted.getCollection("admin_restart"), CollectionNotFoundError);
    EXPECT_TRUE(std::filesystem::is_empty(data.path() / "trash"));
  }
}

TEST_F(CollectionAdminApiTest, deleteRecoversLoadFailureTombstone) {
  CollectionAdminDataDir data("luxir_collection_admin_corrupt");
  auto config = fsConfig(data);
  config.ingest.auto_create_collection = false;

  {
    LuxirNode node(config);
    node.createCollection(nullptr, "admin_corrupt");
    CollectionHelper helper(node, "admin_corrupt");
    ASSERT_TRUE(helper.index(flatdoc("id", "corrupt"), UpdateMessage::COMMIT).success);
  }
  std::ofstream(data.path() / "c" / "admin_corrupt" /
                std::string(Postings::INDEX_INFO_FILE),
                std::ios::binary | std::ios::trunc)
      << "\xff\xff\xff\xff\xff\xff\xff\xff";

  LuxirNode node(config);
  EXPECT_THROW(node.getCollection("admin_corrupt"), CollectionUnavailableError);
  EXPECT_NO_THROW(node.deleteCollection("admin_corrupt"));
  EXPECT_FALSE(std::filesystem::exists(data.path() / "c" / "admin_corrupt"));
}

} // namespace luxir::test
