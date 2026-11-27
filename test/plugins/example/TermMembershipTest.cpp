// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Term pruning from a segment overlay that implements TermMembership (the
// example plugin's example_absent_prefix kind, which proves every term with
// its prefix absent, true or not).

#include <gtest/gtest.h>

#include <filesystem>
#include <format>

#include "luxir/plugin/Registries.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

void exampleSetOverlayOpenMode(int mode);

namespace {

// Searches run on a freshly started node, so that no segment starts out open.
class TermMembershipTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("luxir-membership-" + newUuid());
  LuxirConfig config;

  void SetUp() override {
    initPlugins(addConfiguredPlugins);
    config.store.backend = "fs";
    config.store.data_dir = path;
    config.index.merge_factor = 100;
    QueryPruning::disableForTests = false;
    LuxirNode node(config);
    CollectionHelper helper(node, "logs");
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string_view json = R"({"fields":{"a_s":"string","t_w":"text"},"segment_overlays":[
        {"name":"m","kind":"example_absent_prefix","fields":["a_s","t_w"],"params":{"prefix":"mm"}}]})";
    ASSERT_TRUE(api::read_json(def, json, arena));
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
    // Two segments whose term bounds [aa, zz] admit anything between.
    for (std::string_view segment : {"s1", "s2"}) {
      std::vector<Doc> batch{flatdoc("id", std::string(segment) + "a", "a_s", "aa", "t_w", "aa zz"),
                             flatdoc("id", std::string(segment) + "m", "a_s", "mmpresent", "t_w", "aa mmpresent")};
      ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
    }
    helper.commit();
  }

  void TearDown() override {
    exampleSetOverlayOpenMode(0);
    QueryPruning::disableForTests = false;
    std::filesystem::remove_all(path);
  }

  // Hits, and planner notes naming the membership proof, for `query` on a freshly started node.
  std::pair<int64_t, size_t> search(std::string_view query) {
    LuxirNode node(config);
    CollectionHelper helper(node, "logs");
    auto req = localReq(helper.getSearchEngine());
    std::string json = std::format(R"({{"ops":{{"q":{{"top_docs":{{"query":{},"limit":100,"fields":["id"]}}}}}}}})", query);
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("logs");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    size_t pruned = 0;
    for (const auto& note : helper.getSearchEngine().explain(req->rawRequest(), "")) {
      pruned += note.starts_with("op q: ") && note.contains("example absent prefix");
    }
    return {(int64_t)req->getDocs("q").size(), pruned};
  }

  static std::string term(std::string_view value) { return std::format(R"({{"match":{{"a_s":"{}"}}}})", value); }
  static std::string phrase(std::string_view text) { return std::format(R"({{"phrase":{{"field":"t_w","text":"{}"}}}})", text); }
};

}  // namespace

TEST_F(TermMembershipTest, aProvenAbsentTermPrunesEverySegment) {
  auto [hits, segments] = search(term("mm-not-indexed"));
  EXPECT_EQ(0, hits);
  EXPECT_EQ(1u, segments) << "the proof empties the weight for the collection";
}

TEST_F(TermMembershipTest, aTermInsideTheBoundsWithoutProofPrunesNothing) {
  auto [hits, segments] = search(term("nn-not-indexed"));
  EXPECT_EQ(0, hits);
  EXPECT_EQ(0u, segments);
}

TEST_F(TermMembershipTest, aPhraseWithATermAbsentEverywherePrunesEverySegment) {
  EXPECT_EQ((std::pair<int64_t, size_t>{0, 1}), search(phrase("aa mmnope")));
  EXPECT_EQ(0u, search(phrase("aa nnnope")).second);
  EXPECT_EQ(2, search(phrase("aa zz")).first);
}

TEST_F(TermMembershipTest, presentTermsAreUnaffected) {
  auto [hits, segments] = search(term("aa"));
  EXPECT_EQ(2, hits);
  EXPECT_EQ(0u, segments);
}

// The overlay here lies on purpose. Filtering on a wrong proof loses hits, which
// is why an implementation must never answer "absent" unless it is certain.
TEST_F(TermMembershipTest, aWrongProofDropsHits) {
  EXPECT_EQ(0, search(term("mmpresent")).first);
  QueryPruning::disableForTests = true;
  EXPECT_EQ(2, search(term("mmpresent")).first);
}

// An overlay that cannot open (invalid bytes, or a failed read) proves nothing:
// the lying overlay is not consulted, so the hits come back.
TEST_F(TermMembershipTest, anOverlayThatFailsToOpenPrunesNothing) {
  for (int mode : {1, 2}) {
    exampleSetOverlayOpenMode(mode);
    auto [hits, segments] = search(term("mmpresent"));
    EXPECT_EQ(2, hits) << "open mode " << mode;
    EXPECT_EQ(0u, segments);
  }
}
