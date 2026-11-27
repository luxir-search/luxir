// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Term pruning from a segment overlay that implements TermMembership (the
// example plugin's example_absent_prefix kind, which proves every term with
// its prefix absent, true or not).

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <set>

#include "luxir/plugin/Registries.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/TermFactsCache.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/Uuid.h"
#include "test/CollectionHelper.h"
#include "test/ExamplePluginHooks.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr std::string_view OVERLAY = R"("segment_overlays":[{"name":"m","kind":"example_absent_prefix","fields":["a_s","t_w","g__t"],"params":{"prefix":"mm"}}])";

// Searches run on a freshly started node, so that no segment starts out open.
class TermMembershipTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() / ("luxir-membership-" + newUuid());
  LuxirConfig config;

  static void setSchema(CollectionHelper& helper, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, json, arena)) << json;
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  static void commit(CollectionHelper& helper, std::vector<Doc> batch) {
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }

  void SetUp() override {
    initPlugins(addConfiguredPlugins);
    config.store.backend = "fs";
    config.store.data_dir = path;
    config.index.merge_factor = 100;
    QueryPruning::disableForTests = false;
    LuxirNode node(config);
    {
      // Two segments whose term bounds [aa, zz] admit anything between.
      CollectionHelper helper(node, "logs");
      setSchema(helper, std::format(R"({{"fields":{{"a_s":"string","t_w":"text"}},{}}})", OVERLAY));
      for (std::string_view segment : {"s1", "s2"}) {
        commit(helper, {flatdoc("id", std::string(segment) + "a", "a_s", "aa", "t_w", "aa zz"),
                        flatdoc("id", std::string(segment) + "m", "a_s", "mmpresent", "t_w", "aa mmpresent")});
      }
      helper.commit();
    }
    {
      // Overlays only on segments of 3 docs: a small segment has none.
      CollectionHelper helper(node, "mixed");
      setSchema(helper, std::format(R"({{"fields":{{"a_s":"string"}},"segment_overlays":[{{"name":"m","kind":"example_absent_prefix","fields":["a_s"],"params":{{"prefix":"mm"}},"min_docs":3}}]}})"));
      commit(helper, {flatdoc("id", "small1", "a_s", "aa"), flatdoc("id", "small2", "a_s", "mmpresent")});
      commit(helper, {flatdoc("id", "big1", "a_s", "aa"), flatdoc("id", "big2", "a_s", "zz"), flatdoc("id", "big3", "a_s", "mmpresent")});
      helper.commit();
    }
    {
      CollectionHelper helper(node, "dels");
      setSchema(helper, std::format(R"({{"fields":{{"a_s":"string"}},{}}})", OVERLAY));
      commit(helper, {flatdoc("id", "d1", "a_s", "aa"), flatdoc("id", "d2", "a_s", "aa"), flatdoc("id", "d3", "a_s", "zz")});
      ASSERT_TRUE(helper.deleteById("d1", UpdateMessage::COMMIT).success);
      helper.commit();
    }
    {
      // A physical variant field: probes carry its physical name.
      CollectionHelper helper(node, "variants");
      setSchema(helper, std::format(R"({{"fields":{{"g":{{"type":"string","variants":{{"t":{{"type":"text"}}}}}}}},{}}})", OVERLAY));
      commit(helper, {flatdoc("id", "v1", "g", "aa"), flatdoc("id", "v2", "g", "mmpresent zz")});
      helper.commit();
    }
  }

  void TearDown() override {
    exampleSetOverlayOpenMode(0);
    QueryPruning::disableForTests = false;
    std::filesystem::remove_all(path);
  }

  // Ids matched, and planner notes naming the membership proof, for `query`
  // on a freshly started node.
  std::pair<std::set<std::string>, size_t> search(std::string_view query, std::string_view collection = "logs") {
    LuxirNode node(config);
    CollectionHelper helper(node, collection);
    auto req = localReq(helper.getSearchEngine());
    std::string json = std::format(R"({{"ops":{{"q":{{"top_docs":{{"query":{},"limit":100,"fields":["id"]}}}}}}}})", query);
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection(collection);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    size_t proofs = 0;
    for (const auto& note : helper.getSearchEngine().explain(req->rawRequest(), "")) {
      proofs += note.starts_with("op q: ") && note.contains("example absent prefix");
    }
    std::set<std::string> ids;
    for (const auto& doc : req->getDocs("q")) {
      std::string text = docToString(doc);  // {id: "x"}
      ids.insert(text.substr(6, text.size() - 8));
    }
    return {ids, proofs};
  }

  static std::string term(std::string_view value, std::string_view field = "a_s") {
    return std::format(R"({{"match":{{"{}":"{}"}}}})", field, value);
  }
  static std::string phrase(std::string_view text) { return std::format(R"({{"phrase":{{"field":"t_w","text":"{}"}}}})", text); }
  size_t hits(std::string_view query, std::string_view collection = "logs") { return search(query, collection).first.size(); }
  size_t proofs(std::string_view query, std::string_view collection = "logs") { return search(query, collection).second; }
};

}  // namespace

TEST_F(TermMembershipTest, aProvenAbsentTermEmptiesTheWeight) {
  auto [ids, proven] = search(term("mm-not-indexed"));
  EXPECT_TRUE(ids.empty());
  EXPECT_EQ(1u, proven) << "the proof empties the weight for the collection";
}

TEST_F(TermMembershipTest, aTermInsideTheBoundsWithoutProofPrunesNothing) {
  EXPECT_EQ(0u, hits(term("nn-not-indexed")));
  EXPECT_EQ(0u, proofs(term("nn-not-indexed")));
  EXPECT_EQ(2u, hits(term("aa")));
  EXPECT_EQ(0u, proofs(term("aa")));
}

TEST_F(TermMembershipTest, aPhraseWithAProvenAbsentTermMatchesNothing) {
  EXPECT_EQ(0u, hits(phrase("aa mmnope")));
  EXPECT_EQ(1u, proofs(phrase("aa mmnope")));
  EXPECT_EQ(0u, proofs(phrase("aa nnnope")));
  EXPECT_EQ(2u, hits(phrase("aa zz")));
}

// The phrase's early-out asks the term facts, with no segment pruning: it needs
// no dictionary (no TermsEnum) for any term, and no probe past what it asks.
TEST_F(TermMembershipTest, termFactsProveAPhraseTermAbsentEverywhereWithoutOpeningADictionary) {
  LuxirNode node(config);
  CollectionHelper helper(node, "logs");
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  MemPool pool;
  TermFactsCache facts(pool, *reader);
  auto* field = facts.getCachedFieldInfo("t_w");
  ASSERT_NE(nullptr, field);
  EXPECT_FALSE(facts.termAbsentEverywhere(*field, "aa"));
  EXPECT_TRUE(facts.termAbsentEverywhere(*field, "mmnope"));
  EXPECT_FALSE(facts.termAbsentEverywhere(*field, "nnnope"));
  for (const auto* terms : field->termsEnums) EXPECT_EQ(nullptr, terms);
  // The term still resolves as a miss, with the answer memoized on its entry.
  EXPECT_EQ(nullptr, facts.getCachedTerminfo(*field, "mmnope"));
  for (const auto* terms : field->termsEnums) EXPECT_EQ(nullptr, terms);
  Similarity::TermStats stats;
  EXPECT_FALSE(facts.lookupTermStats(*field, "mmnope", stats));
  for (const auto* terms : field->termsEnums) EXPECT_EQ(nullptr, terms);
}

TEST_F(TermMembershipTest, presentTermsAreUnaffected) {
  EXPECT_EQ((std::set<std::string>{"s1a", "s2a"}), search(term("aa")).first);
}

// The overlay here lies on purpose. Filtering on a wrong proof loses hits, which
// is why an implementation must never answer "absent" unless it is certain.
TEST_F(TermMembershipTest, aWrongProofDropsHits) {
  EXPECT_EQ(0u, hits(term("mmpresent")));
  QueryPruning::disableForTests = true;
  EXPECT_EQ(2u, hits(term("mmpresent")));
}

// An overlay that cannot open (invalid bytes, or a failed read) proves nothing:
// the lying overlay is not consulted, so the hits come back.
TEST_F(TermMembershipTest, anOverlayThatFailsToOpenPrunesNothing) {
  for (int mode : {1, 2}) {
    exampleSetOverlayOpenMode(mode);
    auto [ids, proven] = search(term("mmpresent"));
    EXPECT_EQ(2u, ids.size()) << "open mode " << mode;
    EXPECT_EQ(0u, proven);
  }
}

// A file that no longer holds what the overlay was built with fails the
// overlay's integrity check: it proves nothing.
TEST_F(TermMembershipTest, aTamperedOverlayFilePrunesNothing) {
  size_t tampered = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
    if (!entry.is_regular_file() || entry.file_size() != 1) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (in.get() != '-') continue;
    in.close();
    std::ofstream(entry.path(), std::ios::binary | std::ios::trunc).put('x');
    ++tampered;
  }
  ASSERT_GT(tampered, 0u);
  auto [ids, proven] = search(term("mmpresent"));
  EXPECT_EQ(2u, ids.size());
  EXPECT_EQ(0u, proven);
}

// Only the segments with an overlay are pruned: the small segment, which has
// none, still answers (the proof here is a lie, so the big segment's hit is lost).
TEST_F(TermMembershipTest, aSegmentWithoutAnOverlayIsNotPruned) {
  EXPECT_EQ((std::set<std::string>{"small2"}), search(term("mmpresent"), "mixed").first);
  EXPECT_EQ((std::set<std::string>{"small1", "big1"}), search(term("aa"), "mixed").first);
}

TEST_F(TermMembershipTest, deletedDocumentsDoNotChangeTheProof) {
  EXPECT_EQ((std::set<std::string>{"d2"}), search(term("aa"), "dels").first);
  auto [ids, proven] = search(term("mm-not-indexed"), "dels");
  EXPECT_TRUE(ids.empty());
  EXPECT_EQ(1u, proven);
}

TEST_F(TermMembershipTest, aVariantFieldIsProbedByItsPhysicalName) {
  EXPECT_EQ(0u, hits(term("mmpresent", "g__t"), "variants"));
  EXPECT_EQ(1u, proofs(term("mmpresent", "g__t"), "variants"));
  EXPECT_EQ(1u, hits(term("zz", "g__t"), "variants"));
}

// With a term that really is absent, pruned and unpruned execution agree for
// every boolean shape; a clause that is not required supplies no proof.
TEST_F(TermMembershipTest, booleanShapesAgreeWithUnprunedExecution) {
  std::string absent = term("mm-not-indexed"), present = term("aa");
  for (auto query : {
           std::format(R"({{"boolean":{{"required":[{},{}]}}}})", present, absent),
           std::format(R"({{"boolean":{{"optional":[{},{}]}}}})", present, absent),
           std::format(R"({{"boolean":{{"filter":[{}],"prohibited":[{}]}}}})", present, absent),
           std::format(R"({{"boolean":{{"required":[{}],"prohibited":[{}]}}}})", absent, present),
           std::format(R"({{"boolean":{{"optional":[{},{}],"min_match":2}}}})", present, absent),
           std::format(R"({{"constant_score":{{"query":{}}}}})", absent),
       }) {
    QueryPruning::disableForTests = false;
    auto pruned = search(query);
    QueryPruning::disableForTests = true;
    EXPECT_EQ(search(query).first, pruned.first) << query;
  }
  QueryPruning::disableForTests = false;
  EXPECT_EQ(2u, hits(std::format(R"({{"boolean":{{"optional":[{},{}]}}}})", term("aa"), term("mm-not-indexed"))));
  EXPECT_EQ(0u, hits(std::format(R"({{"boolean":{{"required":[{},{}]}}}})", term("aa"), term("mm-not-indexed"))));
}
