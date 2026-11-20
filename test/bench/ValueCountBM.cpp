// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// Value counts over multi-valued ARRAY columns: a keyword, an ip and an int
// array of 1-5 values, about a third of the multi-valued documents holding a
// repeat. Each case runs at the request root and under a 20-bucket terms facet.
struct ValueCountFixture {
  static constexpr std::string_view collectionName = "value_count_bm";
  static constexpr int32_t numSegments = 8;
  CollectionHelper helper{collectionName};
  int64_t numDocs = unit_tests ? LuxirTest::scaleTestWork(800) : 2000000;

  ValueCountFixture() {
    std::array<int32_t, numSegments> shape;
    for (int32_t segment = 0; segment < numSegments; segment++) {
      shape[segment] = (int32_t)(numDocs * (segment + 1) / numSegments - numDocs * segment / numSegments);
    }
    if (helper.indexMatchesShape(shape)) return;
    auto& node = helper.getNode();
    node.deleteCollection(collectionName);
    helper = CollectionHelper(node, collectionName);
    helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
    SchemaBuilder schema;
    using Class = api::FieldDef::FieldClass;
    for (auto [name, type] : {std::pair{"tags", Class::STRING}, {"ips", Class::IP}, {"nums", Class::INT}}) {
      auto& field = schema.field(name);
      field.type = type;
      field.multi = api::FieldDef::MultiMode::ARRAY;
    }
    schema.set(helper.collection());
    int64_t doc = 0;
    for (int32_t segment = 0; segment < numSegments; segment++) {
      std::vector<Doc> docs;
      docs.reserve((size_t)shape[segment]);
      for (int64_t end = doc + shape[segment]; doc < end; doc++) {
        int64_t size = 1 + doc % 5;
        bool repeat = size > 1 && doc % 3 == 0;
        std::vector<std::string> tags, ips;
        std::vector<int64_t> nums;
        for (int64_t i = 0; i < size; i++) {
          int64_t value = repeat && i == size - 1 ? (doc * 7) % 1000 : (doc * 7 + i * 13) % 1000;
          tags.push_back("t" + std::to_string(value));
          ips.push_back("10.0." + std::to_string(value / 256) + "." + std::to_string(value % 256));
          nums.push_back(value);
        }
        docs.push_back(flatdoc("id", std::to_string(doc), "cat_s", "c" + std::to_string(doc % 20),
                               "tags", tags, "ips", ips, "nums", nums));
      }
      EXPECT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }
};

void BM_ValueCount(benchmark::State& state, std::string_view metric, bool terms) {
  // A function-local static would outlive the node and hang process exit;
  // each case reopens the corpus instead, which indexMatchesShape keeps.
  ValueCountFixture fixture;
  std::string json = terms
      ? R"({"limit":0,"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,"ops":{"m":")"
          + std::string(metric) + R"("}}}}})"
      : R"({"limit":0,"ops":{"m":")" + std::string(metric) + R"("}})";
  auto run = [&]() {
    auto req = localReq(fixture.helper.getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection(ValueCountFixture::collectionName);
    req->execute(false);
    return req;
  };
  {
    auto req = run();
    if (!req->ok()) {
      state.SkipWithError(req->errorMsg());
      return;
    }
  }
  for (auto _ : state) {
    auto req = run();
    benchmark::DoNotOptimize(req->responses[0]->proto);
  }
  state.counters["docs"] = (double)fixture.numDocs;
}

} // namespace

LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, root_sum_count_tags, "sum(count(tags))", false);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, root_sum_count_ips, "sum(count(ips))", false);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, root_sum_count_nums, "sum(count(nums))", false);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, root_value_count_tags, "value_count(tags)", false);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, terms_sum_count_tags, "sum(count(tags))", true);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, terms_sum_count_ips, "sum(count(ips))", true);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, terms_sum_count_nums, "sum(count(nums))", true);
LUXIR_BENCHMARK_CAPTURE(BM_ValueCount, terms_value_count_tags, "value_count(tags)", true);
