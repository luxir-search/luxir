// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Index sort over the BOOLEAN, IP and DECIMAL field types: a reordering merge
// must answer every query, sort, facet and retrieval over them the same as an
// unsorted collection, and rebuild the boolean true/false doc bitsets in the
// merged doc order.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory_resource>
#include <string>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/index/IndexWriter.h"
#include "luxir/search/IndexReader.h"
#include "luxir/server/Collection.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Json = glz::generic_i64;

constexpr const char* FIELDS = R"j("fields":{)j"
    R"j("price_dec":{"type":"decimal","scale":2,"index":"range"},)j"
    R"j("prices_decs":{"type":"decimal","scale":2,"multi":true}})j";

// Drops timings and the commit token, and rounds doubles: decimal metrics
// accumulate as doubles, so their last bits follow the doc order.
void canonicalize(Json& node) {
  if (auto* number = node.get_if<double>()) {
    *number = std::round(*number * 1e4) / 1e4;
  } else if (auto* object = node.get_if<Json::object_t>()) {
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

std::string address(Rng& rng) {
  if (rng.rint(5) == 0) return std::format("2001:db8::{:x}", rng.rint(40));
  return std::format("10.{}.0.{}", rng.rint(3), rng.rint(60));
}

std::string decimal(Rng& rng) {
  int64_t cents = (int64_t)rng.rint(200000) - 20000;
  return std::format("{}{}.{:02}", cents < 0 ? "-" : "", std::abs(cents) / 100, std::abs(cents) % 100);
}

// Document i: every warp type single and multi-valued, each sometimes
// missing, plus the sort keys.
Doc randomDoc(Rng& rng, int i) {
  auto maybe = [&]() { return rng.rint(100) < 85; };
  Doc doc = flatdoc("id", "d" + std::to_string(i), "seq_i", (int64_t)i);
  if (maybe()) doc.push_back({"ts_i", (int64_t)rng.rint(40) - 10});
  if (maybe()) {
    doc.push_back({"loc_p", std::vector<double>{(double)rng.rint(8000) / 100.0 - 40,
                                                (double)rng.rint(8000) / 100.0 - 40}});
  }
  if (maybe()) doc.push_back({"flag_b", rng.rint(3) != 0});
  if (maybe()) {
    std::vector<std::string> flags;
    for (int n = 1 + (int)rng.rint(3); n > 0; n--) flags.push_back(rng.rint(2) ? "true" : "false");
    doc.push_back({"flags_bs", flags});
  }
  if (maybe()) doc.push_back({"addr_ip", address(rng)});
  if (maybe()) {
    std::vector<std::string> addrs;
    for (int n = 1 + (int)rng.rint(3); n > 0; n--) addrs.push_back(address(rng));
    doc.push_back({"addrs_ips", addrs});
  }
  if (maybe()) doc.push_back({"price_dec", decimal(rng)});
  if (maybe()) {
    std::vector<std::string> prices;
    for (int n = 1 + (int)rng.rint(3); n > 0; n--) prices.push_back(decimal(rng));
    doc.push_back({"prices_decs", prices});
  }
  return doc;
}

struct Request {
  std::string body;
  bool unordered;  // the full match set, in no defined order
};

std::vector<Request> battery() {
  std::vector<Request> out;
  auto all = [&](const std::string& query) {
    out.push_back({R"j({"query":)j" + query + R"j(,"limit":-1,"count_limit":-1,"fields":["id"]})j", true});
  };
  for (const char* q : {
           // boolean
           R"j({"match":{"flag_b":true}})j", R"j({"match":{"flag_b":"false"}})j", R"j("flag_b:true")j",
           R"j({"any_of":{"field":"flag_b","values":[true,false]}})j",
           R"j({"range":{"field":"flag_b","gte":false}})j", R"j({"range":{"field":"flag_b","lt":true}})j",
           R"j({"match":{"flags_bs":true}})j", R"j({"match":{"flags_bs":false}})j",
           R"j({"any_of":{"field":"flags_bs","values":["false"]}})j",
           R"j({"range":{"field":"flags_bs","gt":false}})j",
           R"j("NOT flag_b:true")j", R"j("flag_b:true AND flags_bs:false")j", R"j({"exists":{"field":"flags_bs"}})j",
           // ip
           R"j({"match":{"addr_ip":"10.1.0.7"}})j", R"j({"match":{"addr_ip":"::ffff:10.1.0.7"}})j",
           R"j({"match":{"addr_ip":"10.1.0.0/24"}})j", R"j({"match":{"addr_ip":"2001:db8::/32"}})j",
           R"j({"match":{"addrs_ips":"10.2.0.0/16"}})j", R"j({"match":{"addrs_ips":"2001:db8::1f"}})j",
           R"j({"any_of":{"field":"addr_ip","values":["10.0.0.1","10.2.0.9","2001:db8::3"]}})j",
           R"j({"any_of":{"field":"addrs_ips","values":["10.1.0.5","2001:db8::a"]}})j",
           R"j({"range":{"field":"addr_ip","gt":"10.0.0.30","lte":"10.2.0.10"}})j",
           R"j({"range":{"field":"addrs_ips","gte":"2001:db8::10"}})j",
           R"j("addr_ip:\"10.0.0.0/8\" AND NOT addrs_ips:\"10.1.0.0/16\"")j",
           // decimal
           R"j({"range":{"field":"price_dec","gte":12.5,"lt":800}})j", R"j({"range":{"field":"price_dec","lt":0}})j",
           R"j({"range":{"field":"prices_decs","gt":"100.25","lte":1500}})j", R"j({"match":{"price_dec":"7.250"}})j",
           R"j("price_dec:[-50 TO 50.5]")j", R"j("prices_decs:[1000 TO *]")j"}) {
    all(q);
  }

  for (const char* sort : {R"j(["flag_b desc","seq_i"])j", R"j(["flags_bs","seq_i"])j",
                           R"j(["addr_ip","seq_i"])j", R"j(["addr_ip desc","seq_i"])j",
                           R"j(["price_dec","seq_i"])j", R"j(["price_dec desc","seq_i"])j",
                           R"j(["prices_decs desc","seq_i"])j", R"j(["prices_decs","seq_i"])j"}) {
    out.push_back({R"j({"query":{"all":true},"sort":)j" + std::string(sort)
                   + R"j(,"limit":40,"offset":7,"count_limit":-1,"fields":["id","flag_b","addr_ip","price_dec"]})j",
                   false});
  }
  // Every retrievable field of every document.
  out.push_back({R"j({"query":{"all":true},"sort":"seq_i","limit":-1})j", false});

  for (const char* query : {R"j({"all":true})j", R"j("flag_b:true OR addrs_ips:\"10.0.0.0/16\"")j"}) {
    out.push_back({R"j({"query":)j" + std::string(query) + R"j(,"limit":0,"count_limit":-1,"ops":{)j"
                   R"j("flag":{"field_facet":{"field":"flag_b","limit":-1,"missing":true}},)j"
                   R"j("flags":{"field_facet":{"field":"flags_bs","limit":-1}},)j"
                   R"j("addr":{"field_facet":{"field":"addr_ip","limit":-1,"missing":true}},)j"
                   R"j("addrs":{"field_facet":{"field":"addrs_ips","limit":-1}},)j"
                   R"j("price":{"field_facet":{"field":"price_dec","limit":20}},)j"
                   R"j("ranges":{"range_facet":{"field":"price_dec","start":-200,"end":2000,"gap":"137.5","missing":true}},)j"
                   R"j("sum_p":"sum(price_dec)","min_p":"min(price_dec)","max_p":"max(price_dec)",)j"
                   R"j("min_ps":"min(min(prices_decs))","sum_ps":"sum(max(prices_decs))"}})j", false});
  }
  return out;
}

}  // namespace

enum class Mode { SINGLE_ASC, SINGLE_DESC, HILBERT };

class IndexSortWarpTypesTest : public LuxirTest, public ::testing::WithParamInterface<Mode> {
protected:
  std::unique_ptr<HttpServer> server;
  int reorders = 0;

  void SetUp() override {
    server = std::make_unique<HttpServer>(*LuxirTest::luxirNode, 1, 0);
    server->start();
    Signal::listen("mergeIndexSort", [this](void* reordered, void*, void*) -> void* {
      if (*(bool*)reordered) reorders++;
      return nullptr;
    });
  }
  void TearDown() override {
    Signal::unlisten("mergeIndexSort");
    server->shutdown();
  }

  static std::unique_ptr<CollectionHelper> collection(const std::string& name, const std::string& indexSort) {
    auto helper = std::make_unique<CollectionHelper>(name);
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(1000);
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    std::string json = "{" + std::string(FIELDS) + (indexSort.empty() ? "" : "," + indexSort) + "}";
    EXPECT_TRUE(api::read_json(def, json, arena, &error)) << error;
    helper->collection().updateSchema(def, api::SchemaRequest_::Mode::SET);
    return helper;
  }

  // The canonical response; unordered pools and sorts the docs of every line.
  std::string search(const std::string& name, const std::string& body, bool unordered) {
    auto res = httpRequest(server->getPort(), http::verb::post, "/collections/" + name + "/_search", body);
    EXPECT_EQ(200, (int)res.result_int()) << body << "\n" << res.body().substr(0, 500);
    std::string out;
    std::vector<std::string> docs;
    std::string_view rest = res.body();
    while (!rest.empty()) {
      size_t end = std::min(rest.find('\n'), rest.size());
      if (end > 0) {
        Json line;
        EXPECT_FALSE(glz::read_json(line, rest.substr(0, end))) << rest.substr(0, end);
        canonicalize(line);
        if (unordered && line.contains("docs")) {
          for (auto& doc : line["docs"].get<Json::array_t>()) docs.push_back(glz::write_json(doc).value_or(""));
          line.get<Json::object_t>().erase("docs");
        }
        out += glz::write_json(line).value_or("") + "\n";
      }
      rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    std::sort(docs.begin(), docs.end());
    for (const auto& doc : docs) out += doc + "\n";
    return out;
  }

  // The one segment's true/false doc counts of a boolean field.
  static std::pair<int32_t, int32_t> bitsetCounts(CollectionHelper& helper, const char* field) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    EXPECT_EQ(1u, reader->segments().size());
    const auto* info = reader->segments()[0].postingsReader().fieldInfo(field);
    EXPECT_NE(nullptr, info);
    if (info == nullptr) return {-1, -1};
    EXPECT_NE(0, info->flags & FieldType::BOOL_BITSETS) << field;
    return {info->trueDocs, info->falseDocs};
  }

  int32_t count(const std::string& name, const std::string& query) {
    std::string docs = search(name, R"j({"query":)j" + query + R"j(,"limit":-1,"fields":["id"]})j", true);
    int32_t out = 0;
    for (size_t at = docs.find(R"("id":)"); at != std::string::npos; at = docs.find(R"("id":)", at + 1)) out++;
    return out;
  }
};

// Ascending and descending on an INT, and Hilbert order on a geo_point; the
// sort key is unrelated to the warp-typed values, so the merge permutes them.
TEST_P(IndexSortWarpTypesTest, reorderingMergeMatchesUnsortedCollection) {
  Mode mode = GetParam();
  std::string spec = mode == Mode::SINGLE_DESC ? R"j("index_sort":{"field":"ts_i","order":"desc"})j"
      : mode == Mode::HILBERT ? R"j("index_sort":{"field":"loc_p","order":"hilbert"})j"
      : R"j("index_sort":{"field":"ts_i"})j";
  auto plain = collection("wsort_plain", "");
  auto sorted = collection("wsort_sorted", spec);
  std::vector<CollectionHelper*> both = {plain.get(), sorted.get()};

  int docs = (int)scaleTestWork(900);
  int batches = 6;
  int next = 0;
  auto ingest = [&](int count) {
    std::vector<Doc> batch;
    for (int n = 0; n < count; n++) {
      // A quarter of the batch updates an earlier document.
      int i = next > 0 && rng.rint(4) == 0 ? (int)rng.rint(next) : next++;
      batch.push_back(randomDoc(rng, i));
    }
    // d0, never deleted, holds the values the exact matches look for.
    batch.push_back(flatdoc("id", "d0", "seq_i", (int64_t)0, "ts_i", (int64_t)rng.rint(40), "flag_b", false,
                            "addr_ip", "10.1.0.7", "addrs_ips", vecs("2001:db8::1f", "10.1.0.5"),
                            "price_dec", "7.25", "prices_decs", vecs("0.10", "1200")));
    for (auto* helper : both) ASSERT_TRUE(helper->indexAll(batch, UpdateMessage::COMMIT, true).success);
  };
  auto deleteSome = [&](int count) {
    std::vector<std::string> ids;
    for (int n = 0; n < count; n++) ids.push_back("d" + std::to_string(1 + rng.rint(next - 1)));
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
      std::string expected = search("wsort_plain", request.body, request.unordered);
      std::string actual = search("wsort_sorted", request.body, request.unordered);
      // Every query matches something.
      if (request.unordered) {
        EXPECT_NE(std::string::npos, expected.find(R"("id":)")) << request.body;
      }
      if (expected == actual) continue;
      size_t at = std::mismatch(expected.begin(), expected.end(), actual.begin(), actual.end()).first
          - expected.begin();
      size_t from = at < 200 ? 0 : at - 200;
      ADD_FAILURE() << phase << ": " << request.body << "\n  unsorted: ..." << expected.substr(from, 400)
                    << "\n  sorted:   ..." << actual.substr(from, 400);
    }
  };
  // The merged bitsets hold as many docs as the unsorted ones, and as many
  // as the column values say.
  auto compareBitsets = [&]() {
    for (const char* field : {"flag_b", "flags_bs"}) {
      auto counts = bitsetCounts(*sorted, field);
      EXPECT_EQ(bitsetCounts(*plain, field), counts) << field;
      EXPECT_GT(counts.first, 0) << field;
      EXPECT_GT(counts.second, 0) << field;
      std::string name = field;
      for (auto [value, docs] : {std::pair{"true", counts.first}, std::pair{"false", counts.second}}) {
        EXPECT_EQ(count("wsort_plain", std::format(R"j({{"range":{{"field":"{}","gte":{},"lte":{}}}}})j",
                                                   name, value, value)), docs) << field << " " << value;
      }
    }
  };

  for (int b = 0; b < batches; b++) {
    ingest(docs / batches);
    if (b % 2 == 1) deleteSome(docs / 40);
  }
  compare("segments");
  forceMerge();
  EXPECT_EQ(1, reorders);
  compare("merged");
  compareBitsets();

  // Deletes and updates after the merge, then a merge of the sorted segment
  // with a new one.
  deleteSome(docs / 20);
  ingest(docs / 5);
  compare("merged plus segment");
  forceMerge();
  EXPECT_EQ(2, reorders);
  compare("merged again");
  compareBitsets();
}

INSTANTIATE_TEST_SUITE_P(IndexSort, IndexSortWarpTypesTest,
                         ::testing::Values(Mode::SINGLE_ASC, Mode::SINGLE_DESC, Mode::HILBERT),
                         [](const auto& info) {
                           return info.param == Mode::SINGLE_ASC ? "singleAsc"
                               : info.param == Mode::SINGLE_DESC ? "singleDesc" : "hilbert";
                         });
