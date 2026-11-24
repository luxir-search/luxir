// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Plugin sort kinds (SortKindRegistry) through the example plugin's
// abs_diff(field, center), and the _sort_ return field.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/plugin/Registries.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/Collection.h"
#include "luxir/server/HttpServer.h"
#include "plugins/example/ExampleSort.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr int SEGMENTS = 3;
constexpr int PER_SEGMENT = 3000;
constexpr int64_t CENTER = 17;

// Doc i of the corpus, indexed in order of i.
struct Row {
  int i;
  std::optional<int64_t> x;    // x_i: missing on every 13th doc
  int64_t small;               // small_i: few distinct values, so ties
  std::vector<int64_t> n;      // n_is: two values, none on every 17th doc
  std::string group;           // g_s
};

Row row(int i) {
  Row r{i, std::nullopt, (i % 11) - 5, {}, "g" + std::to_string(i % 40)};
  if (i % 13 != 0) r.x = ((int64_t)i * 7919) % 20011 - 10005;
  if (i % 17 != 0) r.n = {((int64_t)i * 31) % 2003 - 1000, ((int64_t)i * 17) % 1999 - 1000};
  return r;
}

std::optional<int64_t> absDiff(std::optional<int64_t> x) {
  if (!x) return std::nullopt;
  return *x > CENTER ? *x - CENTER : CENTER - *x;
}

std::string docId(int i) { return "d" + std::to_string(i); }

// The ids sorted by key (missing last in both directions), then index order.
template <class Key>
std::vector<std::string> bruteForce(const std::vector<Row>& rows, Key&& key, bool descending,
                                    size_t offset, size_t limit) {
  std::vector<const Row*> order;
  for (const auto& r : rows) order.push_back(&r);
  std::stable_sort(order.begin(), order.end(), [&](const Row* a, const Row* b) {
    auto ka = key(*a), kb = key(*b);
    if (ka.has_value() != kb.has_value()) return ka.has_value();
    if (!ka) return false;
    return descending ? *ka > *kb : *ka < *kb;
  });
  std::vector<std::string> ids;
  for (size_t i = offset; i < order.size() && ids.size() < limit; i++) ids.push_back(docId(order[i]->i));
  return ids;
}

struct Result {
  int status = 0;
  std::string body;
  std::vector<std::string> ids;
  std::vector<std::string> sortValues;  // each returned doc's _sort_ as JSON
  std::vector<std::string> scores;      // each returned doc's _score_ as JSON
};

class PluginSortTest : public LuxirTest {
protected:
  static inline std::vector<Row> rows;
  CollectionHelper helper;
  std::unique_ptr<HttpServer> server;

  void SetUp() override {
    initPlugins(addConfiguredPlugins);
    if (rows.empty()) {
      for (int i = 0; i < SEGMENTS * PER_SEGMENT; i++) rows.push_back(row(i));
    }
    reindex(rows, PER_SEGMENT);
    server = std::make_unique<HttpServer>(*LuxirTest::luxirNode, 1, 0);
    server->start();
  }
  void TearDown() override {
    example::AbsDiffStats::answer = example::AbsDiffStats::Answer::OWN;
    example::AbsDiffStats::seedAnswer = example::AbsDiffStats::SeedAnswer::OWN;
    disableFieldSortSeeding = savedDisableSeeding;
    server->shutdown();
  }

  bool savedDisableSeeding = disableFieldSortSeeding;

  // Main holds `corpus`, `perSegment` docs to a segment. The numeric
  // templates build no points index, which the abs_diff prune reads. c_i
  // copies small_i into a column only: a range on it has no match-window
  // product, so a query over it runs through a pull scorer.
  void reindex(const std::vector<Row>& corpus, int perSegment) {
    helper.clear();
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    ASSERT_TRUE(api::read_json(def, R"j({"fields":{"x_i":{"type":"int","index":"range"},)j"
        R"j("n_is":{"type":"int","index":"range","multi":true},"small_i":{"type":"int","index":"range"},)j"
        R"j("c_i":{"type":"int"},"g_s":{"type":"string"}}})j", arena, &error)) << error;
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
    for (size_t start = 0; start < corpus.size(); start += (size_t)perSegment) {
      std::vector<Doc> docs;
      for (size_t i = start; i < std::min(corpus.size(), start + (size_t)perSegment); i++) {
        const Row& r = corpus[i];
        Doc doc = flatdoc("id", docId(r.i), "small_i", r.small, "c_i", r.small, "g_s", r.group);
        if (r.x) doc.push_back({"x_i", *r.x});
        if (!r.n.empty()) doc.push_back({"n_is", r.n});
        docs.push_back(std::move(doc));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }

  using Json = glz::generic_i64;

  static const Json* findDocs(const Json& node) {
    if (auto* object = node.get_if<Json::object_t>()) {
      for (const auto& [key, value] : *object) {
        if (key == "docs" && value.get_if<Json::array_t>()) return &value;
        if (auto* found = findDocs(value)) return found;
      }
    }
    return nullptr;
  }

  // Runs `body` as a search of the main collection.
  Result search(const std::string& body) {
    auto res = httpRequest(server->getPort(), http::verb::post, "/collections/main/_search", body);
    Result out{(int)res.result_int(), res.body(), {}, {}, {}};
    size_t start = 0;
    while (start < out.body.size()) {
      size_t end = out.body.find('\n', start);
      if (end == std::string::npos) end = out.body.size();
      Json line;
      if (end > start && !glz::read_json(line, std::string_view(out.body).substr(start, end - start))) {
        if (const Json* docs = findDocs(line)) {
          for (const Json& doc : docs->get<Json::array_t>()) {
            if (doc.contains("id")) out.ids.push_back(doc["id"].get<std::string>());
            if (doc.contains("_sort_")) out.sortValues.push_back(glz::write_json(doc["_sort_"]).value_or(""));
            if (doc.contains("_score_")) out.scores.push_back(glz::write_json(doc["_score_"]).value_or(""));
          }
        }
      }
      start = end + 1;
    }
    return out;
  }

  // The ids of `sort` (one clause or a JSON list) over every document.
  std::vector<std::string> sorted(const std::string& sort, int64_t limit, int64_t offset = 0,
                                  const std::string& extra = "") {
    auto result = search(R"j({"query":{"all":true},"sort":)j" + sort + R"j(,"limit":)j" + std::to_string(limit)
                         + R"j(,"offset":)j" + std::to_string(offset) + R"j(,"fields":["id"])j" + extra + "}");
    EXPECT_EQ(200, result.status) << sort << "\n" << result.body.substr(0, 500);
    return result.ids;
  }

  // The filter cache off, so a filter is never a cached set the match-window
  // product can lead with.
  struct UncachedFilters {
    FilterCache& cache;
    size_t savedBytes = cache.configuration().maxBytes;
    explicit UncachedFilters(CollectionHelper& helper)
        : cache(*helper.collection().getShard()->getReaderManager().filterCache) { cache.resize(0); }
    ~UncachedFilters() { cache.resize(savedBytes); }
  };

  std::string error(const std::string& sort) {
    auto result = search(R"j({"query":{"all":true},"sort":)j" + sort + R"j(,"limit":5})j");
    EXPECT_NE(200, result.status) << sort;
    return result.body;
  }
};

}  // namespace

TEST(SortKindRegistryTest, validatesNames) {
  SortKindRegistry r;
  SortKindMaker maker = [](SortKindContext&) -> std::shared_ptr<const SortKindSource> { return nullptr; };
  r.add("my_sort", maker);
  EXPECT_THROW(r.add("my_sort", maker), std::invalid_argument);
  EXPECT_THROW(r.add("min", maker), std::invalid_argument);
  EXPECT_THROW(r.add("col", maker), std::invalid_argument);
  EXPECT_THROW(r.add("Bad-Name", maker), std::invalid_argument);
  EXPECT_THROW(r.add("1st", maker), std::invalid_argument);
  EXPECT_THROW(r.add("", maker), std::invalid_argument);
  EXPECT_THROW(r.add("no_maker", SortKindMaker{}), std::invalid_argument);
  EXPECT_NE(nullptr, r.find("my_sort"));
  EXPECT_EQ(nullptr, r.find("other"));
  r.freeze();
  EXPECT_THROW(r.add("later", maker), std::logic_error);
  EXPECT_TRUE(Registries::global().sortKinds.frozen());
  EXPECT_NE(nullptr, globalSortKindRegistry().find("abs_diff"));
}

TEST_F(PluginSortTest, ordersLikeBruteForceInBothDirections) {
  auto key = [](const Row& r) { return absDiff(r.x); };
  EXPECT_EQ(bruteForce(rows, key, false, 0, 40), sorted(R"j("abs_diff(x_i, 17)")j", 40));
  EXPECT_EQ(bruteForce(rows, key, true, 0, 40), sorted(R"j("abs_diff(x_i, 17) desc")j", 40));
  // A page further down, and the variable form.
  EXPECT_EQ(bruteForce(rows, key, false, 300, 25), sorted(R"j("abs_diff(x_i, 17)")j", 25, 300));
  EXPECT_EQ(bruteForce(rows, key, false, 0, 30),
            sorted(R"j({"expr":"abs_diff(x_i, $c)","vars":{"c":17}})j", 30));
}

TEST_F(PluginSortTest, missingValuesSortLastInBothDirections) {
  auto key = [](const Row& r) { return absDiff(r.x); };
  int64_t all = (int64_t)rows.size();
  for (bool descending : {false, true}) {
    auto ids = sorted(descending ? R"j("abs_diff(x_i, 17) desc")j" : R"j("abs_diff(x_i, 17)")j", all);
    ASSERT_EQ(rows.size(), ids.size());
    EXPECT_EQ(bruteForce(rows, key, descending, 0, rows.size()), ids);
    EXPECT_EQ(docId(rows.back().i / 13 * 13), ids.back());  // the last doc without x_i
  }
}

TEST_F(PluginSortTest, secondaryClausesBreakTies) {
  // small_i has 11 values, so abs_diff ties everywhere; x_i desc breaks them.
  std::vector<const Row*> order;
  for (const auto& r : rows) order.push_back(&r);
  std::stable_sort(order.begin(), order.end(), [](const Row* a, const Row* b) {
    int64_t da = std::abs(a->small - 2), db = std::abs(b->small - 2);
    if (da != db) return da < db;
    if (a->x.has_value() != b->x.has_value()) return a->x.has_value();
    return a->x && *a->x > *b->x;
  });
  std::vector<std::string> expected;
  for (size_t i = 0; i < 60; i++) expected.push_back(docId(order[i]->i));
  EXPECT_EQ(expected, sorted(R"j(["abs_diff(small_i, 2)", "x_i desc"])j", 60));
  // The plugin clause second: the same as the value expression it mirrors.
  EXPECT_EQ(sorted(R"j(["small_i", "abs(x_i - 17)"])j", 60), sorted(R"j(["small_i", "abs_diff(x_i, 17)"])j", 60));
}

TEST_F(PluginSortTest, multiValuedFieldsNeedAReducer) {
  auto nearest = [](const Row& r) -> std::optional<int64_t> {
    if (r.n.empty()) return std::nullopt;
    return std::min(std::abs(r.n[0] - CENTER), std::abs(r.n[1] - CENTER));
  };
  auto farthest = [](const Row& r) -> std::optional<int64_t> {
    if (r.n.empty()) return std::nullopt;
    return std::max(std::abs(r.n[0] - CENTER), std::abs(r.n[1] - CENTER));
  };
  EXPECT_EQ(bruteForce(rows, nearest, false, 0, 40), sorted(R"j("min(abs_diff(n_is, 17))")j", 40));
  EXPECT_EQ(bruteForce(rows, farthest, true, 0, 40), sorted(R"j("max(abs_diff(n_is, 17)) desc")j", 40));
  EXPECT_EQ(sorted(R"j("min(abs(n_is - 17))")j", 40), sorted(R"j("min(abs_diff(n_is, 17))")j", 40));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(n_is, 17)")j").find("needs min() or max()"));
}

TEST_F(PluginSortTest, pruneSkipsDocsAndKeepsTheOrder) {
  example::AbsDiffStats::skippedDocs = 0;
  example::AbsDiffStats::builds = 0;
  auto pruned = sorted(R"j("abs_diff(x_i, 17)")j", 10);
  EXPECT_GT(example::AbsDiffStats::builds.load(), 0);
  EXPECT_GT(example::AbsDiffStats::skippedDocs.load(), PER_SEGMENT);
  // The value expression is the unpruned order.
  EXPECT_EQ(sorted(R"j("abs(x_i - 17)")j", 10), pruned);
  EXPECT_EQ(sorted(R"j("min(abs(n_is - 17))")j", 10), sorted(R"j("min(abs_diff(n_is, 17))")j", 10));
  // Under a filter, and with the count the prune cannot skip.
  std::string filter = R"j(,"filter":[{"range":{"field":"small_i","gte":0}}])j";
  EXPECT_EQ(sorted(R"j("abs(x_i - 17)")j", 10, 0, filter), sorted(R"j("abs_diff(x_i, 17)")j", 10, 0, filter));
  auto counted = search(R"j({"query":{"all":true},"sort":"abs_diff(x_i, 17)","limit":10,"count_limit":-1,"fields":["id"]})j");
  EXPECT_EQ(pruned, counted.ids);
  EXPECT_NE(std::string::npos, counted.body.find(R"j("found":)j" + std::to_string(rows.size()))) << counted.body.substr(0, 300);
}

// Whatever competitiveDocs answers, every doc is collected once: a
// malformed READY run is clamped, PENDING and UNSUPPORTED fall back to
// collecting.
TEST_F(PluginSortTest, competitiveDocsAnswersKeepTheResults) {
  auto expected = sorted(R"j("abs(x_i - 17)")j", 30);
  using Answer = example::AbsDiffStats::Answer;
  for (auto answer : {Answer::MALFORMED, Answer::PENDING, Answer::PENDING_AT, Answer::UNSUPPORTED}) {
    example::AbsDiffStats::answer = answer;
    auto counted = search(R"j({"query":{"all":true},"sort":"abs_diff(x_i, 17)","limit":30,"count_limit":-1,)j"
                          R"j("fields":["id"]})j");
    EXPECT_EQ(expected, counted.ids) << (int)answer;
    EXPECT_NE(std::string::npos, counted.body.find(R"j("found":)j" + std::to_string(rows.size())))
        << (int)answer << counted.body.substr(0, 300);
    EXPECT_EQ(expected, sorted(R"j("abs_diff(x_i, 17)")j", 30)) << (int)answer;
  }
}

TEST_F(PluginSortTest, collapseAndDiversify) {
  std::string collapse = R"j(,"collapse":{"field":"g_s"})j";
  auto groups = sorted(R"j("abs_diff(x_i, 17)")j", 12, 0, collapse);
  EXPECT_EQ(sorted(R"j("abs(x_i - 17)")j", 12, 0, collapse), groups);
  // Each group's head is its nearest member; groups follow their heads.
  std::vector<std::string> expected, seen;
  for (const auto& id : bruteForce(rows, [](const Row& r) { return absDiff(r.x); }, false, 0, rows.size())) {
    const Row& r = rows[(size_t)std::stoi(id.substr(1))];
    if (std::ranges::contains(seen, r.group)) continue;
    seen.push_back(r.group);
    expected.push_back(id);
    if (expected.size() == 12) break;
  }
  EXPECT_EQ(expected, groups);
  std::string diversify = R"j(,"diversify":{"field":"g_s","max_per_key":2})j";
  EXPECT_EQ(sorted(R"j("abs(x_i - 17) desc")j", 15, 0, diversify),
            sorted(R"j("abs_diff(x_i, 17) desc")j", 15, 0, diversify));
  // A plugin clause in head_sort orders the displayed members.
  std::string heads = R"j(,"collapse":{"field":"g_s","head_sort":"abs_diff(x_i, 17)"})j";
  EXPECT_EQ(sorted(R"j("small_i")j", 10, 0, R"j(,"collapse":{"field":"g_s","head_sort":"abs(x_i - 17)"})j"),
            sorted(R"j("small_i")j", 10, 0, heads));
}

TEST_F(PluginSortTest, sortValuesReturnField) {
  auto result = search(R"j({"query":{"all":true},"sort":["abs_diff(x_i, 17)","small_i desc"],"limit":3,"fields":["id","_sort_"]})j");
  ASSERT_EQ(200, result.status) << result.body;
  ASSERT_EQ(3u, result.sortValues.size()) << result.body;
  for (size_t i = 0; i < 3; i++) {
    const Row& r = rows[(size_t)std::stoi(result.ids[i].substr(1))];
    EXPECT_EQ("[" + std::to_string(*absDiff(r.x)) + "," + std::to_string(r.small) + "]", result.sortValues[i]);
  }
  // A doc without the value has null there; the last doc ranks last.
  result = search(R"j({"query":{"all":true},"sort":"abs_diff(x_i, 17) desc","limit":1,"offset":)j"
                  + std::to_string(rows.size() - 1) + R"j(,"fields":["_sort_"]})j");
  ASSERT_EQ(1u, result.sortValues.size()) << result.body;
  EXPECT_EQ("[null]", result.sortValues[0]);
  // Under collapse: the returned member's values under the list sort.
  result = search(R"j({"query":{"all":true},"sort":"abs_diff(x_i, 17)","collapse":{"field":"g_s"},"limit":2,"fields":["id","_sort_"]})j");
  ASSERT_EQ(2u, result.sortValues.size()) << result.body;
  EXPECT_EQ("[" + std::to_string(*absDiff(rows[(size_t)std::stoi(result.ids[0].substr(1))].x)) + "]",
            result.sortValues[0]);
  // The built-in clauses, and a list ranked by score.
  result = search(R"j({"query":{"all":true},"sort":["x_i desc","abs(x_i - 17)"],"limit":1,"fields":["_sort_"]})j");
  ASSERT_EQ(1u, result.sortValues.size()) << result.body;
  int64_t top = 0;
  for (const auto& r : rows) if (r.x) top = std::max(top, *r.x);
  EXPECT_EQ("[" + std::to_string(top) + "," + std::to_string(top - CENTER) + "]", result.sortValues[0]);
  result = search(R"j({"query":{"all":true},"limit":1,"fields":["_sort_"]})j");
  ASSERT_EQ(1u, result.sortValues.size()) << result.body;
  EXPECT_EQ("[1]", result.sortValues[0]);
}

// _sort_ is a column like any other: the same values in every document
// format. A fusion list ranks by its fused score, which _sort_ returns.
TEST_F(PluginSortTest, sortValuesAcrossFormatsAndFusion) {
  std::string body = R"j({"query":{"all":true},"sort":["abs_diff(x_i, 17)","small_i desc"],"limit":4,)j"
                     R"j("fields":["id","_sort_"])j";
  auto rowsFormat = search(body + R"j(,"document_format":"rows"})j");
  ASSERT_EQ(4u, rowsFormat.sortValues.size()) << rowsFormat.body;
  for (size_t i = 0; i < 4; i++) {
    const Row& r = rows[(size_t)std::stoi(rowsFormat.ids[i].substr(1))];
    EXPECT_EQ("[" + std::to_string(*absDiff(r.x)) + "," + std::to_string(r.small) + "]", rowsFormat.sortValues[i]);
  }
  for (const char* format : {"columns", "adaptive"}) {
    auto other = search(body + R"j(,"document_format":")j" + format + R"j("})j");
    ASSERT_EQ(200, other.status) << other.body;
    EXPECT_EQ(rowsFormat.ids, other.ids) << format;
    EXPECT_EQ(rowsFormat.sortValues, other.sortValues) << format;
  }
  auto fused = search(R"j({"ops":{"f":{"fusion":{"rrf":{},"sources":{)j"
                      R"j("a":{"query":{"all":true},"sort":"abs_diff(x_i, 17)","limit":10},)j"
                      R"j("b":{"query":{"all":true},"sort":"abs_diff(x_i, 17) desc","limit":10}},)j"
                      R"j("limit":5,"get_scores":true,"fields":["id","_sort_","_score_"],"document_format":"rows"}}}})j");
  ASSERT_EQ(200, fused.status) << fused.body;
  ASSERT_EQ(5u, fused.sortValues.size()) << fused.body;
  ASSERT_EQ(5u, fused.scores.size()) << fused.body;
  for (size_t i = 0; i < 5; i++) EXPECT_EQ("[" + fused.scores[i] + "]", fused.sortValues[i]);
}

TEST_F(PluginSortTest, unclaimedFormsKeepTheValuePath) {
  auto key = [](const Row& r) { return absDiff(r.x); };
  EXPECT_EQ(bruteForce(rows, key, false, 0, 20), sorted(R"j("abs(x_i - 17)")j", 20));
  // A claimed name inside a larger expression is the value language's.
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(x_i, 17) + 1")j").find("unknown value function"));
  EXPECT_NE(std::string::npos, error(R"j("min(abs_diff(x_i, 17), 3)")j").find("abs_diff"));
  // Whitespace before '(' still names the call, which the value language
  // would not read at all.
  EXPECT_EQ(bruteForce(rows, key, false, 0, 20), sorted(R"j("abs_diff (x_i, 17)")j", 20));
  EXPECT_EQ(sorted(R"j("min(abs_diff(n_is, 17))")j", 20), sorted(R"j(" min ( abs_diff (n_is, 17) ) ")j", 20));
}

TEST_F(PluginSortTest, badArgumentsAreRequestErrors) {
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(x_i)")j").find("abs_diff() takes 2 arguments, got 1"));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(x_i, 1.5)")j").find("argument 2 must be an integer"));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(g_s, 1)")j").find("must be an INT field"));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(x_i, x_i + 1)")j").find("argument 2 must be a field name, number"));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(x_i, $nope)")j").find("undefined variable $nope"));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(17, x_i)")j").find("argument 1 must be a field name"));
  EXPECT_NE(std::string::npos, error(R"j({"expr":"abs_diff(x_i, $c)","vars":{"c":"far"}})j").find("must be an integer"));
  EXPECT_NE(std::string::npos, error(R"j("abs_diff(nope_xyz, 17)")j").find("nope_xyz"));
}

// Seed windows change only the visit order: every form of query sorts as
// brute force and as the unseeded path, the windows were run, and no doc is
// collected twice (a limit holding every doc returns each once).
TEST_F(PluginSortTest, seedWindowsKeepTheResults) {
  auto key = [](const Row& r) { return absDiff(r.x); };
  std::string filter = R"j(,"filter":[{"range":{"field":"small_i","gte":0}}])j";
  struct Case {
    std::string sort;
    int64_t limit;
    int64_t offset = 0;
    std::string extra = "";
    bool seeds = true;  // seeding is for the ascending prune
  };
  std::vector<Case> cases = {
      {R"j("abs_diff(x_i, 17)")j", 10},
      {R"j("abs_diff(x_i, 17)")j", 25, 300},
      {R"j("abs_diff(x_i, 17) desc")j", 20, 0, "", false},
      {R"j("abs_diff(x_i, 17)")j", 15, 0, filter},
      {R"j("min(abs_diff(n_is, 17))")j", 30},
      {R"j(["abs_diff(x_i, 17)", "g_s desc"])j", 40},
      // small_i has 11 values: ties fall to the later clause, or doc order.
      {R"j(["abs_diff(small_i, 2)", "x_i desc"])j", 50},
      {R"j("abs_diff(small_i, 2)")j", 700, 100},
      {R"j("abs_diff(x_i, 17)")j", (int64_t)rows.size()},
  };
  for (const auto& c : cases) {
    disableFieldSortSeeding = true;
    auto unseeded = sorted(c.sort, c.limit, c.offset, c.extra);
    disableFieldSortSeeding = false;
    example::AbsDiffStats::seededSegments = 0;
    auto seeded = sorted(c.sort, c.limit, c.offset, c.extra);
    EXPECT_EQ(unseeded, seeded) << c.sort << c.extra;
    EXPECT_EQ(c.seeds ? SEGMENTS : 0, example::AbsDiffStats::seededSegments.load()) << c.sort;
    auto unique = seeded;
    std::ranges::sort(unique);
    EXPECT_EQ(unique.end(), std::ranges::adjacent_find(unique)) << c.sort;
  }
  EXPECT_EQ(bruteForce(rows, key, false, 300, 25), sorted(R"j("abs_diff(x_i, 17)")j", 25, 300));
  EXPECT_EQ(bruteForce(rows, key, false, 0, rows.size()), sorted(R"j("abs_diff(x_i, 17)")j", (int64_t)rows.size()));
  EXPECT_EQ(sorted(R"j(["abs(x_i - 17)", "g_s desc"])j", 40), sorted(R"j(["abs_diff(x_i, 17)", "g_s desc"])j", 40));
  auto small = [](const Row& r) -> std::optional<int64_t> { return std::abs(r.small - 2); };
  EXPECT_EQ(bruteForce(rows, small, false, 100, 700), sorted(R"j("abs_diff(small_i, 2)")j", 700, 100));
}

// The best keys at the end of the segment: unseeded, the bottom only tightens
// as the scan reaches them, so the prune never pays off; seeded, the heap
// holds them before the sweep and the first consult skips nearly everything.
TEST_F(PluginSortTest, seedWindowsPruneFromTheFirstConsult) {
  constexpr int N = 8000;
  std::vector<Row> corpus;
  for (int i = 0; i < N; i++) {
    // Three docs share each distance, nearest last.
    corpus.push_back(Row{i, CENTER + (N - 1 - i) / 3, i % 7, {}, "g" + std::to_string(i % 5)});
  }
  reindex(corpus, N);
  auto key = [](const Row& r) { return absDiff(r.x); };
  std::string filter = R"j(,"filter":[{"range":{"field":"small_i","gte":2}}])j";
  for (const std::string& extra : {std::string(), filter}) {
    std::vector<Row> domain;
    for (const auto& r : corpus) if (extra.empty() || r.small >= 2) domain.push_back(r);
    disableFieldSortSeeding = true;
    example::AbsDiffStats::skippedDocs = 0;
    auto unseeded = sorted(R"j("abs_diff(x_i, 17)")j", 10, 0, extra);
    int64_t unseededSkips = example::AbsDiffStats::skippedDocs.load();
    disableFieldSortSeeding = false;
    example::AbsDiffStats::skippedDocs = 0;
    example::AbsDiffStats::seedWindows = 0;
    auto seeded = sorted(R"j("abs_diff(x_i, 17)")j", 10, 0, extra);
    int64_t seededSkips = example::AbsDiffStats::skippedDocs.load();
    EXPECT_EQ(bruteForce(domain, key, false, 0, 10), seeded) << extra;
    EXPECT_EQ(unseeded, seeded) << extra;
    EXPECT_GT(example::AbsDiffStats::seedWindows.load(), 0) << extra;
    EXPECT_GT(seededSkips, N * 9 / 10) << extra;
    EXPECT_LT(unseededSkips, N / 10) << extra;
    // A page further down, still under the prune.
    EXPECT_EQ(bruteForce(domain, key, false, 40, 20), sorted(R"j("abs_diff(x_i, 17)")j", 20, 40, extra)) << extra;
  }
}

// Whatever seedWindows answers - none, or windows unsorted, overlapping,
// empty, reversed and past the segment - each doc is collected once.
TEST_F(PluginSortTest, malformedSeedWindowsAreClampedSafely) {
  using SeedAnswer = example::AbsDiffStats::SeedAnswer;
  auto all = (int64_t)rows.size();
  auto expectedTop = sorted(R"j("abs(x_i - 17)")j", 30);
  auto expectedAll = sorted(R"j("abs(x_i - 17)")j", all);
  std::string filter = R"j(,"filter":[{"range":{"field":"small_i","gte":0}}])j";
  auto expectedFiltered = sorted(R"j("abs(x_i - 17)")j", 30, 0, filter);
  for (auto answer : {SeedAnswer::NONE, SeedAnswer::MALFORMED}) {
    example::AbsDiffStats::seedAnswer = answer;
    example::AbsDiffStats::seedWindows = 0;
    EXPECT_EQ(expectedTop, sorted(R"j("abs_diff(x_i, 17)")j", 30)) << (int)answer;
    EXPECT_EQ(expectedAll, sorted(R"j("abs_diff(x_i, 17)")j", all)) << (int)answer;
    EXPECT_EQ(expectedFiltered, sorted(R"j("abs_diff(x_i, 17)")j", 30, 0, filter)) << (int)answer;
    EXPECT_EQ(answer == SeedAnswer::MALFORMED, example::AbsDiffStats::seedWindows.load() > 0);
  }
}

// Under collapse and diversify the windows fill the groups with real
// matches; the groups are the unseeded and the value expression's.
TEST_F(PluginSortTest, seedWindowsUnderCollapseAndDiversify) {
  for (const std::string& extra : {std::string(R"j(,"collapse":{"field":"g_s"})j"),
                                   std::string(R"j(,"diversify":{"field":"g_s","max_per_key":2})j")}) {
    auto expected = sorted(R"j("abs(x_i - 17)")j", 12, 0, extra);
    disableFieldSortSeeding = true;
    EXPECT_EQ(expected, sorted(R"j("abs_diff(x_i, 17)")j", 12, 0, extra)) << extra;
    disableFieldSortSeeding = false;
    example::AbsDiffStats::seededSegments = 0;
    EXPECT_EQ(expected, sorted(R"j("abs_diff(x_i, 17)")j", 12, 0, extra)) << extra;
    EXPECT_EQ(SEGMENTS, example::AbsDiffStats::seededSegments.load()) << extra;
    EXPECT_EQ(sorted(R"j(["abs(x_i - 17)", "g_s"])j", 12, 4, extra),
              sorted(R"j(["abs_diff(x_i, 17)", "g_s"])j", 12, 4, extra)) << extra;
  }
}

// An uncached filter with no match-window product (a column-only range)
// sends the segment down the pull-scorer path, which seeds the same way: the
// scorer runs the windows, resets and sweeps, jumping them. The best keys at
// the end of the segment, so unseeded the prune never pays off.
TEST_F(PluginSortTest, seedWindowsOnThePullPath) {
  constexpr int N = 8000;
  std::vector<Row> corpus;
  for (int i = 0; i < N; i++) {
    corpus.push_back(Row{i, CENTER + (N - 1 - i) / 3, i % 7, {}, "g" + std::to_string(i % 5)});
  }
  reindex(corpus, N);
  UncachedFilters uncached(helper);
  SkipStatsScope stats;
  auto key = [](const Row& r) { return absDiff(r.x); };
  std::string column = R"j({"range":{"field":"c_i","gte":2}})j";
  std::string points = R"j({"range":{"field":"small_i","lte":5}})j";
  struct Case {
    std::string query;
    std::string filters;
    bool (*keep)(const Row&);
  };
  std::vector<Case> cases = {
      {R"j({"all":true})j", column, [](const Row& r) { return r.small >= 2; }},
      {R"j({"all":true})j", column + "," + points, [](const Row& r) { return r.small >= 2 && r.small <= 5; }},
      {R"j({"range":{"field":"c_i","gte":2}})j", "", [](const Row& r) { return r.small >= 2; }},
  };
  for (const auto& c : cases) {
    std::string extra = R"j(,"query":)j" + c.query
        + (c.filters.empty() ? "" : R"j(,"filter":[)j" + c.filters + "]");
    auto run = [&](int64_t limit, int64_t offset) {
      auto result = search(R"j({"sort":"abs_diff(x_i, 17)","limit":)j" + std::to_string(limit) + R"j(,"offset":)j"
                           + std::to_string(offset) + R"j(,"fields":["id"])j" + extra + "}");
      EXPECT_EQ(200, result.status) << result.body.substr(0, 500);
      return result.ids;
    };
    std::vector<Row> domain;
    for (const auto& r : corpus) if (c.keep(r)) domain.push_back(r);
    disableFieldSortSeeding = true;
    example::AbsDiffStats::skippedDocs = 0;
    auto unseeded = run(10, 0);
    int64_t unseededSkips = example::AbsDiffStats::skippedDocs.load();
    disableFieldSortSeeding = false;
    SkipStats::reset();
    example::AbsDiffStats::skippedDocs = 0;
    auto seeded = run(10, 0);
    int64_t seededSkips = example::AbsDiffStats::skippedDocs.load();
    EXPECT_EQ(bruteForce(domain, key, false, 0, 10), seeded) << extra;
    EXPECT_EQ(unseeded, seeded) << extra;
    EXPECT_EQ(1, SkipStats::fieldSortSeedWindowPullActivations) << extra;
    EXPECT_EQ(0, SkipStats::fieldSortBulkCollections) << extra;
    EXPECT_GT(seededSkips, N * 9 / 10) << extra;
    EXPECT_LT(unseededSkips, N / 10) << extra;
    EXPECT_EQ(bruteForce(domain, key, false, 40, 20), run(20, 40)) << extra;
    EXPECT_EQ(bruteForce(domain, key, false, 0, domain.size()), run((int64_t)domain.size(), 0)) << extra;
  }
}

// Across segments, with ties falling to a later clause or doc order, under
// collapse, and with malformed windows, the pull path seeds and sorts as the
// unseeded path; a scored request stays unseeded.
TEST_F(PluginSortTest, pullPathSeedingKeepsTheResults) {
  UncachedFilters uncached(helper);
  SkipStatsScope stats;
  std::string filter = R"j(,"filter":[{"range":{"field":"c_i","gte":-3}}])j";
  struct Case {
    std::string sort;
    int64_t limit;
    std::string extra = "";
    int64_t seeds = SEGMENTS;
  };
  std::vector<Case> cases = {
      {R"j("abs_diff(x_i, 17)")j", 25},
      {R"j(["abs_diff(small_i, 2)", "x_i desc"])j", 50},
      {R"j("abs_diff(small_i, 2)")j", 700},
      {R"j(["abs_diff(x_i, 17)", "g_s desc"])j", 40},
      {R"j("abs_diff(x_i, 17)")j", (int64_t)rows.size()},
      {R"j("abs_diff(x_i, 17)")j", 12, R"j(,"collapse":{"field":"g_s"})j"},
      {R"j("abs_diff(x_i, 17)")j", 20, R"j(,"get_scores":true)j", 0},
  };
  for (const auto& c : cases) {
    auto expected = sorted(c.sort, c.limit, 0, filter + c.extra);
    disableFieldSortSeeding = true;
    EXPECT_EQ(expected, sorted(c.sort, c.limit, 0, filter + c.extra)) << c.sort << c.extra;
    disableFieldSortSeeding = false;
    for (auto answer : {example::AbsDiffStats::SeedAnswer::OWN, example::AbsDiffStats::SeedAnswer::MALFORMED}) {
      example::AbsDiffStats::seedAnswer = answer;
      SkipStats::reset();
      auto seeded = sorted(c.sort, c.limit, 0, filter + c.extra);
      EXPECT_EQ(expected, seeded) << c.sort << c.extra << (int)answer;
      EXPECT_EQ(c.seeds, SkipStats::fieldSortSeedWindowPullActivations) << c.sort << c.extra << (int)answer;
      auto unique = seeded;
      std::ranges::sort(unique);
      EXPECT_EQ(unique.end(), std::ranges::adjacent_find(unique)) << c.sort;
    }
  }
  auto key = [](const Row& r) { return r.small >= -3 ? absDiff(r.x) : std::nullopt; };
  std::vector<Row> domain;
  for (const auto& r : rows) if (r.small >= -3) domain.push_back(r);
  EXPECT_EQ(bruteForce(domain, key, false, 0, 25), sorted(R"j("abs_diff(x_i, 17)")j", 25, 0, filter));
}

// A filter much sparser near the center than overall: round 0, sized by the
// overall density, leaves the heap short, so later rounds widen the windows
// until it fills and the sweep prunes from its first consult. On the windowed
// and the pull path, and with later rounds overlapping the earlier ones or
// malformed, the results are brute force's and each doc is collected once.
TEST_F(PluginSortTest, seedRoundsFillTheHeapUnderASparseFilter) {
  using SeedAnswer = example::AbsDiffStats::SeedAnswer;
  constexpr int N = 20000;
  std::vector<Row> corpus;
  for (int i = 0; i < N; i++) {
    // Nearest last; of the nearest quarter, only every 100th doc passes.
    int64_t small = i >= N * 3 / 4 ? (i % 100 == 0 ? 3 : 0) : i % 7;
    corpus.push_back(Row{i, CENTER + (N - 1 - i) / 3, small, {}, "g" + std::to_string(i % 5)});
  }
  reindex(corpus, N);
  UncachedFilters uncached(helper);
  SkipStatsScope stats;
  auto key = [](const Row& r) { return absDiff(r.x); };
  std::vector<Row> domain;
  for (const auto& r : corpus) if (r.small >= 2) domain.push_back(r);
  // A filter over a points field runs windowed; a query over a column-only
  // field has no match-window product, so it runs through a pull scorer.
  for (std::string field : {"small_i", "c_i"}) {
    std::string range = R"j({"range":{"field":")j" + field + R"j(","gte":2}})j";
    bool pull = field == "c_i";
    auto run = [&](int64_t limit, int64_t offset) {
      auto result = search(R"j({"sort":"abs_diff(x_i, 17)","limit":)j" + std::to_string(limit) + R"j(,"offset":)j"
                           + std::to_string(offset) + R"j(,"fields":["id"],)j"
                           + (pull ? R"j("query":)j" + range : R"j("query":{"all":true},"filter":[)j" + range + "]")
                           + "}");
      EXPECT_EQ(200, result.status) << result.body.substr(0, 500);
      return result.ids;
    };
    disableFieldSortSeeding = true;
    auto unseeded = run(10, 0);
    disableFieldSortSeeding = false;
    for (auto answer : {SeedAnswer::OWN, SeedAnswer::OVERLAP, SeedAnswer::MALFORMED}) {
      example::AbsDiffStats::seedAnswer = answer;
      SkipStats::reset();
      example::AbsDiffStats::skippedDocs = 0;
      example::AbsDiffStats::seedRounds = 0;
      auto seeded = run(10, 0);
      std::string what = field + " " + std::to_string((int)answer);
      EXPECT_EQ(bruteForce(domain, key, false, 0, 10), seeded) << what;
      EXPECT_EQ(unseeded, seeded) << what;
      EXPECT_EQ(1, SkipStats::fieldSortSeedWindowActivations) << what;
      EXPECT_EQ(pull ? 1 : 0, SkipStats::fieldSortSeedWindowPullActivations) << what;
      EXPECT_GT(SkipStats::fieldSortSeedWindowRounds, 0) << what;
      EXPECT_GT(example::AbsDiffStats::seedRounds.load(), 0) << what;
      EXPECT_GT(example::AbsDiffStats::skippedDocs.load(), N * 9 / 10) << what;
      EXPECT_EQ(bruteForce(domain, key, false, 30, 20), run(20, 30)) << what;
      EXPECT_EQ(bruteForce(domain, key, false, 0, domain.size()), run((int64_t)domain.size(), 0)) << what;
    }
  }
}
