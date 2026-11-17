// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <format>
#include <memory_resource>
#include <chrono>
#include <filesystem>
#include "luxir/api/luxir_types.hpp"
#include "luxir/server/HttpServer.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/StorageTestUtils.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

std::vector<std::string> idsOf(LuxirNode& node, std::string_view collection) {
  auto request = localReq(node.getSearchEngine());
  request->collection(std::string(collection)).topDocs("q").allQuery().fields({"id"}).limit(-1);
  request->execute();
  std::vector<std::string> ids;
  for (const auto& doc : request->getDocs()) {
    if (auto* value = find(doc, "id")) ids.push_back(std::get<std::string>(*value));
  }
  std::ranges::sort(ids);
  return ids;
}

class CollectionSplitTest : public LuxirTest {
protected:
  LuxirNode node;
  CollectionHelper big{node, "big"};
  std::vector<std::string> all;

  // Four committed segments of 4, 3, 2 and 1 documents.
  void SetUp() override {
    int next = 0;
    for (int size : {4, 3, 2, 1}) {
      std::vector<Doc> docs;
      for (int i = 0; i < size; i++) {
        all.push_back(std::format("d{:02}", next));
        docs.push_back(flatdoc("id", all.back(), "n_i", next++));
      }
      ASSERT_TRUE(big.indexAll(docs, UpdateMessage::COMMIT, true).success);
    }
    std::ranges::sort(all);
    ASSERT_EQ(4u, big.collection().getReaderManager().getReader()->segments().size());
  }

  std::vector<Collections::SplitPart> split(std::initializer_list<std::string_view> names) {
    std::vector<CollectionId> to;
    for (auto name : names) to.push_back(CollectionId::of(name));
    return node.splitCollection(CollectionId::of("big"), to);
  }
};

}

TEST_F(CollectionSplitTest, partsDivideTheDocumentsBySegment) {
  auto parts = split({"left", "right"});
  ASSERT_EQ(2u, parts.size());
  // Largest first, each to the lighter part: 4+1 and 3+2.
  EXPECT_EQ(5u, parts[0].docs); EXPECT_EQ(2u, parts[0].segments);
  EXPECT_EQ(5u, parts[1].docs); EXPECT_EQ(2u, parts[1].segments);

  auto left = idsOf(node, "left"), right = idsOf(node, "right");
  EXPECT_EQ(5u, left.size()); EXPECT_EQ(5u, right.size());
  std::vector<std::string> both;
  std::ranges::set_union(left, right, std::back_inserter(both));
  EXPECT_EQ(all, both);
  EXPECT_EQ(all, idsOf(node, "big"));  // the source is unchanged

  // A part is an ordinary writer that continues the source's versions: an
  // update replaces the copied document rather than losing to it.
  CollectionHelper part(node, "left");
  ASSERT_TRUE(part.index(flatdoc("id", left[0], "n_i", 100), UpdateMessage::COMMIT, true).success);
  EXPECT_EQ(left, idsOf(node, "left"));
}

TEST_F(CollectionSplitTest, partsOutliveTheSource) {
  split({"left", "right"});
  node.collections().remove(CollectionId::of("big"));
  EXPECT_EQ(5u, idsOf(node, "left").size());
  EXPECT_EQ(5u, idsOf(node, "right").size());
}

TEST_F(CollectionSplitTest, requestNamesPartsByCount) {
  std::pmr::monotonic_buffer_resource arena;
  api::SplitCollectionRequest request;
  request.from = "big";
  request.parts = 3;
  auto response = node.splitCollection(request, arena);
  ASSERT_EQ(3u, response.parts.size());
  uint64_t docs = 0;
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(std::format("big_{}", i), response.parts[i].name);
    EXPECT_EQ(response.parts[i].docs, idsOf(node, response.parts[i].name).size());
    docs += response.parts[i].docs;
  }
  EXPECT_EQ(all.size(), docs);
}

TEST_F(CollectionSplitTest, refusalsCreateNothing) {
  EXPECT_THROW(split({"only"}), RequestError);
  EXPECT_THROW(split({"a", "a"}), RequestError);
  EXPECT_THROW(split({"a", "big"}), RequestError);
  EXPECT_THROW(split({"a", "b", "c", "d", "e"}), RequestError);  // four segments
  EXPECT_THROW(node.splitCollection(CollectionId::of("absent"), std::vector{CollectionId::of("a"), CollectionId::of("b")}),
               CollectionNotFoundError);

  // A taken name fails the split after earlier parts exist; they are removed.
  CollectionHelper taken(node, "taken");
  EXPECT_THROW(split({"first", "taken"}), CollectionExistsError);
  EXPECT_THROW(node.getCollection("first"), CollectionNotFoundError);
  EXPECT_FALSE(node.collections().catalog().find("first"));

  std::pmr::monotonic_buffer_resource arena;
  api::SplitCollectionRequest request;
  request.from = "big";
  request.parts = 2;
  std::string_view names[] = {"x", "y"};
  request.to = names;
  EXPECT_THROW(node.splitCollection(request, arena), RequestError);
}

TEST(CollectionSplitStorageTest, httpSplitLinksFilesAndSurvivesRestart) {
  auto path = std::filesystem::temp_directory_path() /
      std::format("luxir_split_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::remove_all(path);
  auto cleanup = scope_guard([&] { std::filesystem::remove_all(path); });
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  {
    LuxirNode node(config);
    CollectionHelper big(node, "big");
    for (int i = 0; i < 2; i++) {
      ASSERT_TRUE(big.indexAll({flatdoc("id", std::format("a{}", i)), flatdoc("id", std::format("b{}", i))},
                               UpdateMessage::COMMIT, true).success);
    }
    HttpServer server(node, 1, 0); server.start();
    auto response = httpRequest(server.getPort(), http::verb::post, "/collections/_split", R"({"from":"big","parts":2})");
    ASSERT_EQ(200, response.result_int()) << response.body();
    EXPECT_EQ(R"({"from":"big","parts":[{"name":"big_0","docs":2,"segments":1},{"name":"big_1","docs":2,"segments":1}]})" "\n",
              response.body());
    EXPECT_EQ(409, httpRequest(server.getPort(), http::verb::post, "/collections/_split", R"({"from":"big","parts":2})").result_int());
    EXPECT_EQ(400, httpRequest(server.getPort(), http::verb::post, "/collections/_split", R"({"from":"big","parts":3})").result_int());
    EXPECT_EQ(404, httpRequest(server.getPort(), http::verb::post, "/collections/_split", R"({"from":"none","parts":2})").result_int());
  }
  // Segment files are shared with the source, not copied.
  size_t linked = 0;
  for (const auto& entry : std::filesystem::directory_iterator(incarnationPath(path, "big_0"))) {
    if (entry.is_regular_file() && std::filesystem::hard_link_count(entry.path()) > 1) linked++;
  }
  EXPECT_GT(linked, 0u);
  LuxirNode node(config);
  EXPECT_EQ(2u, idsOf(node, "big_0").size());
  EXPECT_EQ(2u, idsOf(node, "big_1").size());
  EXPECT_EQ(4u, idsOf(node, "big").size());
}

TEST_F(CollectionSplitTest, catalogActivatesPartsTogether) {
  auto& storage = node.collections().catalog();
  auto a = storage.reserve("ra", {}, true), b = storage.reserve("rb", {}, true);
  // One taken name refuses the whole update.
  std::vector<std::pair<std::string, std::string>> names{{"ra", a}, {"big", b}};
  EXPECT_THROW(storage.activate(names), CollectionExistsError);
  EXPECT_EQ("reserved", storage.owned(a).state);
  names = {{"ra", a}, {"rb", b}};
  storage.activate(names);
  EXPECT_EQ("active", storage.owned(a).state);
  EXPECT_EQ("active", storage.owned(b).state);
}
