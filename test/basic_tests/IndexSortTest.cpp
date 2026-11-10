// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Index sort: merges write their documents in the schema's index_sort order.
// One collection with the setting and one without index the same randomized
// documents, updates and deletes; every query, sort, facet, collapse, kNN and
// retrieval must answer the same by id, and a merged segment must be in key
// order.

#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <latch>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/MergeCostModel.h"
#include "luxir/index/SegmentMerger.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/Collection.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Json = glz::generic_i64;

constexpr const char* FIELDS = R"j("fields":{"r_i":{"type":"int","index":"range"},)j"
    R"j("tags_sa":{"type":"string","multi":"array"},"vec_v":{"type":"vector","dims":4,"metric":"l2"},)j"
    R"j("vecs_vs":{"type":"vector","dims":2,"metric":"l2","multi":true}})j";

// Drops timings and the commit token.
void canonicalize(Json& node) {
  if (auto* object = node.get_if<Json::object_t>()) {
    for (auto it = object->begin(); it != object->end();) {
      if (it->first.starts_with("took") || it->first == "commit") {
        it = object->erase(it);
        continue;
      }
      canonicalize(it->second);
      ++it;
    }
  } else if (auto* array = node.get_if<Json::array_t>()) {
    for (auto& element : *array) canonicalize(element);
  }
}

class IndexSortTest : public LuxirTest {
protected:
  std::unique_ptr<HttpServer> server;
  int reorders = 0;
  int concatenations = 0;

  void SetUp() override {
    server = std::make_unique<HttpServer>(*LuxirTest::luxirNode, 1, 0);
    server->start();
    Signal::listen("mergeIndexSort", [this](void* reordered, void*, void*) -> void* {
      (*(bool*)reordered ? reorders : concatenations)++;
      return nullptr;
    });
  }
  void TearDown() override {
    Signal::unlisten("mergeIndexSort");
    server->shutdown();
  }

  static void setSchema(CollectionHelper& helper, const std::string& json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    ASSERT_TRUE(api::read_json(def, json, arena, &error)) << error;
    helper.collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  // A fresh collection that merges only when forced.
  static std::unique_ptr<CollectionHelper> collection(const std::string& name, const std::string& indexSort,
                                                      const std::string& extra = "") {
    auto helper = std::make_unique<CollectionHelper>(name);
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(1000);
    setSchema(*helper, "{" + std::string(FIELDS) + (indexSort.empty() ? "" : "," + indexSort) + extra + "}");
    return helper;
  }

  static void forceMerge(CollectionHelper& helper) {
    CollectionHelper::UpdateBuilder request;
    request.commit(true, 1);
    ASSERT_TRUE(helper.submit(request).success);
    ASSERT_EQ(1u, helper.durableSegmentCount());
  }

  // A response's NDJSON lines (a limit of -1 streams its docs in batches).
  std::vector<Json> lines(const std::string& name, const std::string& body) {
    auto res = httpRequest(server->getPort(), http::verb::post, "/collections/" + name + "/_search", body);
    EXPECT_EQ(200, (int)res.result_int()) << body << "\n" << res.body().substr(0, 500);
    std::vector<Json> out;
    std::string_view rest = res.body();
    while (!rest.empty()) {
      size_t end = std::min(rest.find('\n'), rest.size());
      if (end > 0) {
        Json& line = out.emplace_back();
        EXPECT_FALSE(glz::read_json(line, rest.substr(0, end))) << rest.substr(0, end);
      }
      rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    return out;
  }

  // The canonical response; unordered pools and sorts the docs of every line.
  std::string search(const std::string& name, const std::string& body, bool unordered) {
    std::string out;
    Json::array_t docs;
    for (Json& line : lines(name, body)) {
      canonicalize(line);
      if (unordered && line.contains("docs")) {
        for (auto& doc : line["docs"].get<Json::array_t>()) docs.push_back(std::move(doc));
        line.get<Json::object_t>().erase("docs");
      }
      out += glz::write_json(line).value_or("") + "\n";
    }
    std::vector<std::string> sorted;
    for (const auto& doc : docs) sorted.push_back(glz::write_json(doc).value_or(""));
    std::sort(sorted.begin(), sorted.end());
    for (const auto& doc : sorted) out += doc + "\n";
    return out;
  }

  // Each request answers the same on both collections.
  void expectSame(const std::string& expected, const std::string& actual,
                  const std::vector<std::pair<std::string, bool>>& requests) {
    for (const auto& [body, unordered] : requests) {
      EXPECT_EQ(search(expected, body, unordered), search(actual, body, unordered)) << body;
    }
  }

  // The documents of `name` in index order.
  std::vector<Json> indexOrder(const std::string& name, const std::string& fields = R"j(["id","ts_i","ns_is"])j") {
    std::vector<Json> docs;
    for (Json& line : lines(name, R"j({"query":{"all":true},"sort":"_docid_","limit":-1,"fields":)j" + fields + "}")) {
      if (!line.contains("docs")) continue;
      for (auto& doc : line["docs"].get<Json::array_t>()) docs.push_back(std::move(doc));
    }
    return docs;
  }
};

std::string word(Rng& rng, int vocabulary) { return "w" + std::to_string(rng.rint(vocabulary)); }

std::string words(Rng& rng, int min, int max, int vocabulary) {
  std::string out;
  for (int n = min + (int)rng.rint(max - min + 1); n > 0; n--) {
    if (!out.empty()) out += ' ';
    out += word(rng, vocabulary);
  }
  return out;
}

std::vector<std::string> some(Rng& rng, const char* prefix, int distinct, int max) {
  std::vector<std::string> out;
  for (int n = (int)rng.rint(max + 1); n > 0; n--) out.push_back(prefix + std::to_string(rng.rint(distinct)));
  return out;
}

// Document i, version v: every field type the merger writes, each sometimes
// missing. ts_i takes few distinct values, so the sort has many ties.
Doc randomDoc(Rng& rng, int i) {
  auto maybe = [&]() { return rng.rint(100) < 85; };
  Doc doc = flatdoc("id", "d" + std::to_string(i), "seq_i", (int64_t)i);
  if (maybe()) doc.push_back({"ts_i", (int64_t)rng.rint(40) - 10});
  if (maybe()) {
    std::vector<int64_t> values;
    for (int n = 1 + (int)rng.rint(3); n > 0; n--) values.push_back((int64_t)rng.rint(1000) - 500);
    doc.push_back({"ns_is", values});
  }
  if (maybe()) doc.push_back({"r_i", (int64_t)rng.rint(10000)});
  if (maybe()) doc.push_back({"price_f", (float)rng.rint(1000) / 10.0f});
  if (maybe()) doc.push_back({"when_dt", (int64_t)1700000000000LL + (int64_t)rng.rint(1000000000)});
  if (maybe()) doc.push_back({"cat_s", "c" + std::to_string(rng.rint(12))});
  if (maybe()) doc.push_back({"tags_ss", some(rng, "t", 20, 3)});
  if (maybe()) doc.push_back({"tags_sa", some(rng, "a", 6, 4)});
  if (maybe()) doc.push_back({"note_sc", "n" + std::to_string(rng.rint(50))});
  if (maybe()) doc.push_back({"notes_ssc", some(rng, "m", 30, 3)});
  if (maybe()) doc.push_back({"body_t", words(rng, 2, 14, 30)});
  if (maybe()) doc.push_back({"title_w", words(rng, 1, 4, 12)});
  if (maybe()) doc.push_back({"loc_p", std::vector<double>{(double)rng.rint(8000) / 100.0 - 40,
                                                           (double)rng.rint(8000) / 100.0 - 40}});
  if (maybe()) {
    doc.push_back({"vec_v", std::vector<float>{(float)rng.rint(100) / 10, (float)rng.rint(100) / 10,
                                               (float)rng.rint(100) / 10, (float)rng.rint(100) / 10}});
  }
  if (maybe()) {
    std::vector<std::vector<float>> vectors;
    for (int n = (int)rng.rint(3); n > 0; n--) vectors.push_back({(float)rng.rint(50), (float)rng.rint(50)});
    doc.push_back({"vecs_vs", vectors});
  }
  return doc;
}

struct Request {
  std::string body;
  bool unordered;  // the full match set, in no defined order
};

// Every query type over every field kind. Relevance lists return every match
// (ties may order differently); sorted lists end in the unique seq_i, so they
// have one right order and are compared as pages.
std::vector<Request> battery() {
  std::vector<Request> out;
  auto all = [&](const std::string& query) {
    out.push_back({R"j({"query":)j" + query + R"j(,"limit":-1,"count_limit":-1,"get_scores":true,"fields":["id"]})j", true});
  };
  for (const char* q : {"body_t:w3", "body_t:(w1 OR w2) AND NOT body_t:w5", "body_t:\"w1 w2\"",
                        "body_t:\"w3 w4\"~3", "title_w:w7", "body_t:w1~1", "cat_s:c1*",
                        "wildcard(c?, field=cat_s)", "regex(t1[0-9], field=tags_ss)", "tags_sa:=(a1, a3)",
                        "tags_ss:t4 OR tags_sa:a2", "r_i:[1000 TO 4000]", "ts_i:[0 TO 9]", "price_f:>=50",
                        "ns_is:[-100 TO 100]", "ts_i:*", "*:* AND NOT ts_i:*", "note_sc:* AND NOT notes_ssc:*",
                        "when_dt:[1700100000000 TO 1700500000000]", "id:[d1 TO d3]", "body_t:w2 AND r_i:<5000"}) {
    std::string quoted;
    (void)glz::write_json(std::string(q), quoted);
    all(quoted);
  }
  all(R"j({"geo_box":{"field":"loc_p","min_lat":-10,"max_lat":15,"min_lon":-20,"max_lon":5}})j");
  all(R"j({"geo_distance":{"field":"loc_p","lat":5,"lon":-3,"radius_meters":2000000}})j");
  all(R"j({"knn":{"field":"vec_v","query":[1,2,3,4],"k":100000,"exact":true}})j");
  all(R"j({"knn":{"field":"vecs_vs","query":[10,20],"k":100000,"exact":true}})j");
  all(R"j({"boolean":{"required":[{"match":{"body_t":"w4"}}],"optional":[{"match":{"title_w":"w2"}}],)j"
      R"j("filter":["ts_i:<15"],"prohibited":[{"exists":{"field":"note_sc"}}]}})j");

  for (const char* sort : {R"j(["ts_i desc","seq_i"])j", R"j(["ns_is","seq_i"])j", R"j(["ns_is desc","seq_i"])j",
                           R"j(["price_f desc","seq_i"])j", R"j(["cat_s","r_i desc","seq_i"])j",
                           R"j(["note_sc","seq_i"])j", R"j(["when_dt","seq_i"])j", R"j(["id"])j"}) {
    out.push_back({R"j({"query":{"all":true},"sort":)j" + std::string(sort)
                   + R"j(,"limit":40,"offset":7,"count_limit":-1})j", false});
  }
  // Every retrievable field of every document.
  out.push_back({R"j({"query":{"all":true},"sort":"seq_i","limit":-1})j", false});
  // Request-version runs follow the new doc order.
  out.push_back({R"j({"query":{"all":true},"sort":"seq_i","limit":-1,"fields":["id","_version_"]})j", false});
  out.push_back({R"j({"query":"body_t:w2","sort":["score desc","seq_i"],"limit":30,"get_scores":true,"fields":["id"]})j", false});
  out.push_back({R"j({"query":"body_t:w1 OR body_t:w6","collapse":"cat_s","sort":["r_i desc","seq_i"],)j"
                 R"j("limit":-1,"count_limit":-1,"fields":["id","r_i"]})j", false});

  for (const char* query : {R"j({"all":true})j", R"j("body_t:w3 OR ts_i:<5")j"}) {
    out.push_back({R"j({"query":)j" + std::string(query) + R"j(,"limit":0,"count_limit":-1,"ops":{)j"
                   R"j("cat":{"field_facet":{"field":"cat_s","limit":-1,"missing":true}},)j"
                   R"j("tags":{"field_facet":{"field":"tags_ss","limit":-1}},)j"
                   R"j("arr":{"field_facet":{"field":"tags_sa","limit":-1}},)j"
                   R"j("ts":{"field_facet":{"field":"ts_i","limit":-1,"missing":true}},)j"
                   R"j("r":{"range_facet":{"field":"r_i","start":0,"end":10000,"gap":1000,"missing":true}},)j"
                   R"j("sum_r":"sum(r_i)","min_p":"min(price_f)","max_ts":"max(ts_i)","min_n":"min(min(ns_is))","sum_n":"sum(max(ns_is))"}})j", false});
  }
  return out;
}

void expectKeyOrder(const std::vector<Json>& docs, const char* field, bool descending) {
  std::optional<int64_t> previous;
  bool missingSeen = false;
  for (const Json& doc : docs) {
    if (!doc.contains(field)) {
      missingSeen = true;
      continue;
    }
    ASSERT_FALSE(missingSeen) << "a document with " << field << " follows one without";
    const Json& value = doc[field];
    int64_t key;
    if (auto* array = value.get_if<Json::array_t>()) {
      key = (*array)[0].get<int64_t>();
      for (const Json& v : *array) key = descending ? std::max(key, v.get<int64_t>()) : std::min(key, v.get<int64_t>());
    } else {
      key = value.get<int64_t>();
    }
    if (previous) {
      ASSERT_TRUE(descending ? key <= *previous : key >= *previous) << field << " out of order";
    }
    previous = key;
  }
}

}  // namespace

// The equivalence run: ascending and descending on a single-valued field,
// and descending on a multi-valued one (max key).
enum class Mode { SINGLE_ASC, SINGLE_DESC, MULTI_DESC };

class IndexSortEquivalenceTest : public IndexSortTest, public ::testing::WithParamInterface<Mode> {};

TEST_P(IndexSortEquivalenceTest, randomizedResultsMatchUnsortedCollection) {
  Mode mode = GetParam();
  bool multi = mode == Mode::MULTI_DESC;
  bool descending = multi || mode == Mode::SINGLE_DESC;
  const char* field = multi ? "ns_is" : "ts_i";
  std::string spec = mode == Mode::MULTI_DESC ? R"j("index_sort":{"field":"ns_is","order":"desc"})j"
      : mode == Mode::SINGLE_DESC ? R"j("index_sort":{"field":"ts_i","order":"desc"})j"
      : R"j("index_sort":{"field":"ts_i"})j";
  auto plain = collection("isort_plain", "");
  auto sorted = collection("isort_sorted", spec);
  std::vector<CollectionHelper*> both = {plain.get(), sorted.get()};
  // The multi-valued run also splits text merges into term ranges.
  for (auto* helper : both) {
    helper->getIndexWriter()->termPartitionMinBytes = multi ? 1 : INT64_MAX;
    helper->getIndexWriter()->termPartitionMinRangeBytes = multi ? 1 : MergeCostModel::MIN_TERM_RANGE_BYTES;
  }

  int docs = (int)scaleTestWork(1200);
  int batches = 6;
  int next = 0;
  auto ingest = [&](int count) {
    std::vector<Doc> batch;
    for (int n = 0; n < count; n++) {
      // A quarter of the batch updates an earlier document.
      int i = next > 0 && rng.rint(4) == 0 ? (int)rng.rint(next) : next++;
      batch.push_back(randomDoc(rng, i));
    }
    for (auto* helper : both) ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  };
  auto deleteSome = [&](int count) {
    std::vector<std::string> ids;
    for (int n = 0; n < count; n++) ids.push_back("d" + std::to_string(rng.rint(next)));
    for (auto* helper : both) ASSERT_TRUE(helper->deleteByIds(ids, UpdateMessage::COMMIT).success);
  };
  auto forceMerge = [&]() {
    for (auto* helper : both) {
      CollectionHelper::UpdateBuilder request;
      request.commit(true, 1);
      ASSERT_TRUE(helper->submit(request).success);
      ASSERT_EQ(1u, helper->durableSegmentCount());
    }
  };
  auto compare = [&](const std::string& phase) {
    for (const auto& request : battery()) {
      std::string expected = search("isort_plain", request.body, request.unordered);
      std::string actual = search("isort_sorted", request.body, request.unordered);
      if (expected == actual) continue;
      size_t at = std::mismatch(expected.begin(), expected.end(), actual.begin(), actual.end()).first
          - expected.begin();
      size_t from = at < 200 ? 0 : at - 200;
      ADD_FAILURE() << phase << ": " << request.body << "\n  unsorted: ..." << expected.substr(from, 400)
                    << "\n  sorted:   ..." << actual.substr(from, 400);
    }
  };

  for (int b = 0; b < batches; b++) {
    ingest(docs / batches);
    if (b % 2 == 1) deleteSome(docs / 40);
  }
  compare("segments");
  forceMerge();
  EXPECT_EQ(1, reorders);
  expectKeyOrder(indexOrder("isort_sorted"), field, descending);
  compare("merged");

  // Deletes and updates after the merge, then a merge of the sorted segment
  // with a new one.
  deleteSome(docs / 20);
  ingest(docs / 5);
  compare("merged plus segment");
  forceMerge();
  EXPECT_EQ(2, reorders);
  expectKeyOrder(indexOrder("isort_sorted"), field, descending);
  compare("merged again");
}

INSTANTIATE_TEST_SUITE_P(IndexSort, IndexSortEquivalenceTest,
                         ::testing::Values(Mode::SINGLE_ASC, Mode::SINGLE_DESC, Mode::MULTI_DESC),
                         [](const auto& info) {
                           return info.param == Mode::SINGLE_ASC ? "singleAsc"
                               : info.param == Mode::SINGLE_DESC ? "singleDesc" : "multiDesc";
                         });

// Sources already in key order concatenate: the permutation is the identity.
TEST_F(IndexSortTest, monotonePermutationTakesConcatenatingMerge) {
  auto helper = collection("isort_mono", R"j("index_sort":{"field":"ts_i"})j");
  for (int s = 0; s < 3; s++) {
    std::vector<Doc> batch;
    for (int i = 0; i < 5; i++) batch.push_back(flatdoc("id", "m" + std::to_string(s * 5 + i), "ts_i", (int64_t)(s * 5 + i)));
    ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  }
  // Without a value: last, also after the merge.
  ASSERT_TRUE(helper->index(flatdoc("id", "none"), UpdateMessage::COMMIT, true).success);
  CollectionHelper::UpdateBuilder request;
  request.commit(true, 1);
  ASSERT_TRUE(helper->submit(request).success);
  EXPECT_EQ(0, reorders);
  EXPECT_EQ(1, concatenations);
  auto docs = indexOrder("isort_mono");
  ASSERT_EQ(16u, docs.size());
  EXPECT_EQ("none", docs.back()["id"].get<std::string>());
}

// Docs deleted while the merge runs: the diff maps them through the
// non-monotone doc map.
TEST_F(IndexSortTest, deletesDuringReorderingMergeMapThroughPermutation) {
  auto helper = collection("isort_late", R"j("index_sort":{"field":"ts_i","order":"desc"})j");
  auto writer = helper->getIndexWriter();
  for (int s = 0; s < 2; s++) {
    std::vector<Doc> batch;
    for (int i = 0; i < 20; i++) {
      int n = s * 20 + i;
      batch.push_back(flatdoc("id", "x" + std::to_string(n), "ts_i", (int64_t)((n * 7) % 13)));
    }
    ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  }
  std::latch diff(1), releaseMerge(1), application(1), releaseApplication(1), swapped(1);
  Signal::listen("mergeLiveDocsDiff", [&](void*, void*, void*) -> void* {
    diff.count_down(); releaseMerge.wait(); return nullptr;
  });
  auto unlisten = scope_guard([] {
    Signal::unlisten("mergeLiveDocsDiff");
    Signal::unlisten("deleteAppliedToSegment");
    Signal::unlisten("mergeSwap");
  });
  auto merge = std::async(std::launch::async, [&] { writer->mergeSegments(); });
  diff.wait();
  Signal::listen("deleteAppliedToSegment", [&](void*, void*, void*) -> void* {
    if (application.try_wait()) return nullptr;
    application.count_down(); releaseApplication.wait();
    return nullptr;
  });
  Signal::listen("mergeSwap", [&](void*, void*, void*) -> void* { swapped.count_down(); return nullptr; });
  std::vector<std::string> gone = {"x0", "x5", "x13", "x22", "x39"};
  auto commit = std::async(std::launch::async, [&] { return helper->deleteByIds(gone, UpdateMessage::COMMIT); });
  application.wait();
  releaseMerge.count_down();
  swapped.wait();
  releaseApplication.count_down();
  EXPECT_TRUE(commit.get().success);
  merge.get();
  writer->updateGraph->wait_for_all();
  EXPECT_EQ(1, reorders);

  auto docs = indexOrder("isort_late");
  std::set<std::string> ids;
  for (const Json& doc : docs) ids.insert(doc["id"].get<std::string>());
  EXPECT_EQ(35u, ids.size());
  for (const auto& id : gone) EXPECT_FALSE(ids.contains(id)) << id;
  expectKeyOrder(docs, "ts_i", true);
}

// A frequent term in long documents has more positions than one window: the
// merge walks its postings once per window of merged docs and holds only that
// window's positions, writing what one buffered pass would.
TEST_F(IndexSortTest, frequentPositionalTermMergesInBoundedWindows) {
  constexpr int64_t WINDOW_POSITIONS = 1024;
  auto saved = SegmentMerger::reorderPositionsWindowBytes;
  SegmentMerger::reorderPositionsWindowBytes = WINDOW_POSITIONS * 4;
  std::array<int64_t, 3> seen{};  // windowed terms, windows, most positions held
  Signal::listen("mergeReorderPositionWindows", [&](void* stats, void*, void*) -> void* {
    auto& merge = *(std::array<int64_t, 3>*)stats;
    seen[0] += merge[0];
    seen[1] += merge[1];
    seen[2] = std::max(seen[2], merge[2]);
    return nullptr;
  });
  auto restore = scope_guard([&] {
    SegmentMerger::reorderPositionsWindowBytes = saved;
    Signal::unlisten("mergeReorderPositionWindows");
  });
  auto plain = collection("isort_wplain", "");
  auto sorted = collection("isort_wsorted", R"j("index_sort":{"field":"ts_i"})j");
  int next = 0;
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> batch;
    for (int i = 0; i < 40; i++, next++) {
      // About 200 tokens, nine in ten of them w0.
      std::string body;
      for (int t = 0; t < 200; t++) body += rng.rint(10) == 0 ? word(rng, 6) + " " : "w0 ";
      batch.push_back(flatdoc("id", "d" + std::to_string(next), "seq_i", (int64_t)next,
                              "ts_i", (int64_t)rng.rint(50), "body_t", body));
    }
    for (auto* helper : {plain.get(), sorted.get()}) ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  }
  forceMerge(*plain);
  forceMerge(*sorted);
  EXPECT_EQ(1, reorders);
  EXPECT_GE(seen[0], 1);  // w0 at least
  EXPECT_GT(seen[1], seen[0]);
  EXPECT_LE(seen[2], WINDOW_POSITIONS);
  expectKeyOrder(indexOrder("isort_wsorted"), "ts_i", false);
  expectSame("isort_wplain", "isort_wsorted", {
      {R"j({"query":"body_t:\"w0 w1\"","limit":-1,"count_limit":-1,"fields":["id"]})j", true},
      {R"j({"query":"body_t:\"w2 w0 w0 w3\"~4","limit":-1,"count_limit":-1,"fields":["id"]})j", true},
      {R"j({"query":"body_t:\"w0 w0 w0 w0 w0 w0 w0 w0 w0 w0\"","limit":-1,"count_limit":-1,"fields":["id"]})j", true},
      {R"j({"query":"body_t:w0 body_t:w4","sort":["score desc","seq_i"],"limit":-1,"get_scores":true,"fields":["id"]})j", false},
  });
}

// Stored fields under a reorder: a source whose docs stay one increasing run
// is emitted in place (its compressed chunks copied when it has no deletes,
// re-added in order when it does), and out-of-order sources decode once into
// scratch. Here with the default group and a named ZSTD dictionary group.
TEST_F(IndexSortTest, storedFieldsCopyRunsAndScratchTheRest) {
  auto saved = SegmentMerger::reorderStoredRamBytes;
  SegmentMerger::reorderStoredRamBytes = 256;  // spills where the directory has a scratch path
  int copies = 0, inOrder = 0;
  Signal::listen("storedFieldsMergeSource", [&](void* resource, void*, void* copy) -> void* {
    if (*(std::string_view*)resource == "_stored_") (*(bool*)copy ? copies : inOrder)++;
    return nullptr;
  });
  auto restore = scope_guard([&] {
    SegmentMerger::reorderStoredRamBytes = saved;
    Signal::unlisten("storedFieldsMergeSource");
  });
  // One doc per default chunk: every chunk is full, so a source without
  // deletes qualifies for a chunk copy.
  std::string group = R"j(,"stored_resources":{"_stored_":{"max_docs_per_chunk":1},)j"
                      R"j("_stored_notes_":{"codec":"zstd","zstd_dictionary":true}})j";
  auto plain = collection("isort_splain", "", group);
  auto sorted = collection("isort_ssorted", R"j("index_sort":{"field":"ts_i"})j", group);
  for (auto* helper : {plain.get(), sorted.get()}) {
    setSchema(*helper, R"j({"fields":{"note_t":{"type":"text","stored_resource":"_stored_notes_"}}})j");
  }
  int next = 0;
  auto ingest = [&](std::vector<int64_t> keys) {
    std::vector<Doc> batch;
    for (int64_t key : keys) {
      batch.push_back(flatdoc("id", "d" + std::to_string(next), "seq_i", (int64_t)next, "ts_i", key,
                              "body_t", words(rng, 3, 30, 40), "tags_ss", some(rng, "t", 20, 3),
                              "note_t", "note " + words(rng, 10, 40, 25)));
      next++;
    }
    for (auto* helper : {plain.get(), sorted.get()}) ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  };
  auto range = [](int64_t from, int64_t to, bool shuffled, Rng& rng) {
    std::vector<int64_t> keys;
    for (int64_t k = from; k < to; k++) keys.push_back(k);
    if (shuffled) {
      for (size_t i = keys.size(); i > 1; i--) std::swap(keys[i - 1], keys[(size_t)rng.rint((int64_t)i)]);
    }
    return keys;
  };
  // Two in-order segments concatenate into one sorted segment.
  ingest(range(0, 60, false, rng));
  ingest(range(60, 120, false, rng));
  forceMerge(*plain);
  forceMerge(*sorted);
  EXPECT_EQ(0, reorders);
  // Out-of-order key ranges, and an in-order one with a delete.
  ingest(range(200, 260, true, rng));
  ingest(range(130, 190, true, rng));
  ingest(range(300, 330, false, rng));
  for (auto* helper : {plain.get(), sorted.get()}) ASSERT_TRUE(helper->deleteByIds({"d245"}, UpdateMessage::COMMIT).success);
  forceMerge(*plain);
  copies = inOrder = 0;
  forceMerge(*sorted);
  EXPECT_EQ(1, reorders);
  EXPECT_EQ(1, copies);   // the merged 0..119 segment
  EXPECT_EQ(1, inOrder);  // 300..329, with a delete
  expectKeyOrder(indexOrder("isort_ssorted"), "ts_i", false);
  expectSame("isort_splain", "isort_ssorted", {
      {R"j({"query":{"all":true},"sort":"seq_i","limit":-1})j", false},
      {R"j({"query":"note_t:note","sort":"seq_i","limit":-1,"fields":["id","note_t"]})j", false},
  });
}

// An ARRAY field whose source column is single-valued and indexed by doc id
// (every doc has exactly one value) merges with multi-valued sources.
TEST_F(IndexSortTest, arrayOrdsFromSingleValuedDocIdIndexedSource) {
  auto plain = collection("isort_aplain", "");
  auto sorted = collection("isort_asorted", R"j("index_sort":{"field":"ts_i"})j");
  int next = 0;
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> batch;
    for (int i = 0; i < 30; i++, next++) {
      // Segment 0: exactly one value per doc; the others: zero to four.
      auto tags = segment == 0 ? std::vector<std::string>{"a" + std::to_string(rng.rint(6))} : some(rng, "a", 6, 4);
      Doc doc = flatdoc("id", "d" + std::to_string(next), "seq_i", (int64_t)next, "ts_i", (int64_t)rng.rint(100));
      if (!tags.empty()) doc.push_back({"tags_sa", tags});
      batch.push_back(doc);
    }
    for (auto* helper : {plain.get(), sorted.get()}) ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  }
  {
    auto reader = sorted->getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ(3u, reader->segments().size());
    auto& postings = reader->segments()[0].postingsReader();
    const auto* info = postings.fieldInfo("tags_sa");
    ASSERT_NE(nullptr, info);
    OrdColReader column(postings, *info);
    ASSERT_FALSE(column.multiValued());
    ASSERT_TRUE(column.docIdIndexed());
  }
  forceMerge(*plain);
  forceMerge(*sorted);
  EXPECT_EQ(1, reorders);
  expectSame("isort_aplain", "isort_asorted", {
      {R"j({"query":{"all":true},"sort":"seq_i","limit":-1,"fields":["id","tags_sa"]})j", false},
      {R"j({"query":"tags_sa:=(a1, a3)","limit":-1,"count_limit":-1,"fields":["id"]})j", true},
      {R"j({"query":{"all":true},"limit":0,"ops":{"arr":{"field_facet":{"field":"tags_sa","limit":-1,"missing":true}}}})j", false},
  });
}

// Sources where the sort field has no int column (never set) contribute
// docs without a key, which go last in concatenation order; so do docs of a
// keyed source that lack a value.
TEST_F(IndexSortTest, sourcesWithoutTheKeyColumnGoLast) {
  auto plain = collection("isort_kplain", "");
  auto sorted = collection("isort_ksorted", R"j("index_sort":{"field":"k_i"})j");
  auto ingest = [&](std::vector<Doc> batch) {
    for (auto* helper : {plain.get(), sorted.get()}) ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  };
  ingest({flatdoc("id", "s0", "seq_i", (int64_t)0), flatdoc("id", "s1", "seq_i", (int64_t)1)});
  ingest({flatdoc("id", "n0", "seq_i", (int64_t)2), flatdoc("id", "n1", "seq_i", (int64_t)3)});
  ingest({flatdoc("id", "i0", "seq_i", (int64_t)4, "k_i", (int64_t)30), flatdoc("id", "i1", "seq_i", (int64_t)5, "k_i", (int64_t)10),
          flatdoc("id", "i3", "seq_i", (int64_t)6), flatdoc("id", "i2", "seq_i", (int64_t)7, "k_i", (int64_t)20)});
  forceMerge(*plain);
  forceMerge(*sorted);
  EXPECT_EQ(1, reorders);
  std::vector<std::string> order;
  for (const Json& doc : indexOrder("isort_ksorted", R"j(["id"])j")) order.push_back(doc["id"].get<std::string>());
  // Keyed docs by k_i, then the rest in source (keyed source first) order.
  EXPECT_EQ((std::vector<std::string>{"i1", "i2", "i0", "i3", "s0", "s1", "n0", "n1"}), order);
  expectSame("isort_kplain", "isort_ksorted", {
      {R"j({"query":{"all":true},"sort":"seq_i","limit":-1})j", false},
      {R"j({"query":"k_i:[15 TO 40]","limit":-1,"count_limit":-1,"fields":["id"]})j", true},
  });
}

// A new index_sort applies from the next merge on, which reorders the
// segment the old setting sorted.
TEST_F(IndexSortTest, changedIndexSortReordersTheNextMerge) {
  auto helper = collection("isort_change", R"j("index_sort":{"field":"ts_i"})j");
  int next = 0;
  auto ingest = [&]() {
    std::vector<Doc> batch;
    for (int i = 0; i < 20; i++, next++) {
      batch.push_back(flatdoc("id", "d" + std::to_string(next), "ts_i", (int64_t)rng.rint(1000),
                              "r_i", (int64_t)rng.rint(1000)));
    }
    ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  };
  ingest();
  ingest();
  forceMerge(*helper);
  expectKeyOrder(indexOrder("isort_change", R"j(["id","ts_i","r_i"])j"), "ts_i", false);
  setSchema(*helper, R"j({"index_sort":{"field":"r_i","order":"desc"}})j");
  ingest();
  forceMerge(*helper);
  EXPECT_EQ(2, reorders);
  expectKeyOrder(indexOrder("isort_change", R"j(["id","ts_i","r_i"])j"), "r_i", true);
}

// The same documents ingested at random and reordered by the merge, or
// ingested in key order and concatenated, merge to the same order and the
// same bytes, except for request-version runs: under the reorder each doc's
// request is no longer implied by its position, so they hold about log2
// (requests) bits per doc, written one run per doc without start columns.
TEST_F(IndexSortTest, reorderedSegmentSizeMatchesPresortedIngest) {
  constexpr int DOCS = 40000, BATCH = 200, PER_SEGMENT = 10000;
  auto shuffled = collection("isort_zrand", "");
  auto presorted = collection("isort_zpre", "");
  for (auto* helper : {shuffled.get(), presorted.get()}) {
    setSchema(*helper, R"j({"index_sort":{"field":"k_i"}})j");
  }
  std::map<std::string, Doc> docs;
  std::vector<std::string> order;
  for (int i = 0; i < DOCS; i++) {
    std::string id = std::format("{:016x}", (uint64_t)rng.rint(INT64_MAX));
    docs[id] = flatdoc("id", id, "k_i", (int64_t)rng.rint(INT32_MAX), "price_i", (int64_t)rng.rint(100000),
                       "body_t", words(rng, 5, 40, 2000));
    order.push_back(id);
  }
  auto ingest = [&](CollectionHelper& helper) {
    for (int b = 0; b < DOCS; b += BATCH) {
      std::vector<Doc> batch;
      for (int i = b; i < b + BATCH; i++) batch.push_back(docs[order[i]]);
      bool last = (b + BATCH) % PER_SEGMENT == 0;
      ASSERT_TRUE(helper.indexAll(batch, last ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT, true).success);
    }
    forceMerge(helper);
  };
  ingest(*shuffled);
  order.clear();
  for (const Json& doc : indexOrder("isort_zrand", R"j(["id"])j")) order.push_back(doc["id"].get<std::string>());
  ingest(*presorted);
  EXPECT_EQ(1, reorders);
  EXPECT_EQ(1, concatenations);
  std::vector<std::string> presortedOrder;
  for (const Json& doc : indexOrder("isort_zpre", R"j(["id"])j")) presortedOrder.push_back(doc["id"].get<std::string>());
  ASSERT_EQ(order, presortedOrder);

  struct Sizes {
    int64_t total, versions, runs, denseBlocks, blocks;
  };
  auto sizes = [](CollectionHelper& helper) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    EXPECT_EQ(1u, reader->segments().size());
    auto& postings = reader->segments()[0].postingsReader();
    const auto& runs = postings.versionRuns();
    Sizes out{(int64_t)postings.sizeInBytes(), (int64_t)runs.byteSize(), (int64_t)runs.size(), 0, runs.numBlocks()};
    for (uint32_t i = 0; i < runs.numBlocks(); i++) out.denseBlocks += runs.block(i).header().step == 1;
    return out;
  };
  Sizes reordered = sizes(*shuffled), concatenated = sizes(*presorted);
  // Everything but the version runs: within 0.2%.
  int64_t rest = reordered.total - reordered.versions, presortedRest = concatenated.total - concatenated.versions;
  EXPECT_LE(std::abs(rest - presortedRest), presortedRest / 500) << rest << " vs " << presortedRest;
  // One run per request in key order; one per doc, 8 bits for 200 requests
  // plus the block descriptors, after the reorder.
  EXPECT_EQ(DOCS / BATCH, concatenated.runs);
  EXPECT_EQ(DOCS, reordered.runs);
  EXPECT_EQ(reordered.blocks, reordered.denseBlocks);
  EXPECT_LE(reordered.versions, DOCS + reordered.blocks * 80);
}
