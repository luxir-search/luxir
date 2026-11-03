// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "luxir/query/FacetTermFilter.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Buckets = std::vector<std::pair<std::string, int64_t>>;

std::string join(const Buckets& buckets) {
  std::string out;
  for (const auto& [key, count] : buckets) {
    out += key + "=" + std::to_string(count) + " ";
  }
  return out;
}

}  // namespace

// FieldFacet.term_filter picks which terms become buckets; counts stay those
// of the facet's domain.
class FacetTermFilterTest : public LuxirTest {
protected:
  CollectionHelper helper;

  // Two segments, so global ords are remapped and the whole-index top-terms
  // shortcut is a candidate for the unfiltered facet.
  void SetUp() override {
    LuxirTest::SetUp();
    struct Row { const char* cat; int count; const char* other; };
    const std::vector<Row> rows{
        {"apple", 5, "x"}, {"apricot", 4, "x"}, {"banana", 3, "y"},
        {"blueberry", 2, "x"}, {"cherry", 1, "y"}, {nullptr, 2, "x"}};
    int n = 0;
    for (int segment = 0; segment < 2; segment++) {
      std::vector<Doc> docs;
      for (const auto& row : rows) {
        for (int i = 0; i < row.count; i++) {
          if ((i % 2) != segment) continue;
          std::string id = "d" + std::to_string(n++);
          if (row.cat != nullptr) {
            docs.push_back(flatdoc("id", id, "cat_s", row.cat, "other_s", row.other,
                                   "n_i", (int64_t)n));
          } else {
            docs.push_back(flatdoc("id", id, "other_s", row.other, "n_i", (int64_t)n));
          }
        }
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
    ASSERT_EQ(2u, helper.getIndexWriter()->snapshots.readers.getReader()
                      ->segments().size());
  }

  struct Result {
    Buckets buckets;
    std::optional<int64_t> missing;
    std::string error;
    bool ok = false;
  };

  // `facet` is the field_facet body; `domain` an optional domain query.
  Result run(const std::string& facet, const std::string& domain = "") {
    auto req = localReq(helper.getSearchEngine());
    std::string json = R"json({"ops":{"f":{"field_facet":)json" + facet
        + (domain.empty() ? "" : R"json(,"domain":{"query":")json" + domain + "\"}")
        + "}}}";
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("main");
    req->execute();
    Result result;
    result.ok = req->ok();
    if (!result.ok) {
      result.error = req->errorMsg();
      return result;
    }
    const auto& facetResult = *req->responses[0]->proto.ops.at("f")->facetResult();
    if (facetResult.bucket_ids.has_value()) {
      const auto& ids = std::get<api::ColStr>(facetResult.bucket_ids->kind).v;
      for (size_t i = 0; i < ids.size(); i++) {
        result.buckets.emplace_back(std::string(ids[i]), facetResult.counts[i]);
      }
    }
    result.missing = facetResult.missing;
    return result;
  }

  Buckets ok(const std::string& facet, const std::string& domain = "") {
    auto result = run(facet, domain);
    EXPECT_TRUE(result.ok) << result.error;
    return result.buckets;
  }
};

TEST_F(FacetTermFilterTest, prefix) {
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}}),
            ok(R"json({"field":"cat_s","limit":10,"term_filter":{"prefix":"ap"}})json"));
}

TEST_F(FacetTermFilterTest, filterAppliesBeforeLimit) {
  // apple outranks banana overall, so a post-limit filter would return nothing.
  EXPECT_EQ((Buckets{{"banana", 3}}),
            ok(R"json({"field":"cat_s","limit":1,"term_filter":{"prefix":"b"}})json"));
  EXPECT_EQ((Buckets{{"apple", 5}}),
            ok(R"json({"field":"cat_s","limit":1,"term_filter":{"exclude":["nope"]}})json"));
}

TEST_F(FacetTermFilterTest, regexIsWholeTerm) {
  EXPECT_EQ((Buckets{{"apricot", 4}, {"blueberry", 2}, {"cherry", 1}}),
            ok(R"json({"field":"cat_s","limit":-1,"term_filter":{"regex":".*r.*"}})json"));
  EXPECT_TRUE(ok(R"json({"field":"cat_s","limit":-1,"term_filter":{"regex":"ap"}})json").empty());
}

TEST_F(FacetTermFilterTest, includeAndExcludeLists) {
  EXPECT_EQ((Buckets{{"banana", 3}, {"cherry", 1}}),
            ok(R"json({"field":"cat_s","limit":10,
                   "term_filter":{"include":["cherry","banana","missing"]}})json"));
  EXPECT_EQ((Buckets{{"apricot", 4}, {"banana", 3}}),
            ok(R"json({"field":"cat_s","limit":2,"term_filter":{"exclude":["apple"]}})json"));
  EXPECT_EQ((Buckets{{"banana", 3}, {"blueberry", 2}, {"cherry", 1}}),
            ok(R"json({"field":"cat_s","limit":10,"term_filter":{"exclude_regex":"a.*"}})json"));
}

TEST_F(FacetTermFilterTest, conditionsCombineWithAnd) {
  EXPECT_EQ((Buckets{{"blueberry", 2}}),
            ok(R"json({"field":"cat_s","limit":10,"term_filter":{
                "prefix":"b","regex":".*e.*","exclude":["banana"]}})json"));
}

TEST_F(FacetTermFilterTest, partitionsCoverEveryTermOnce) {
  const std::vector<std::string> terms{"apple", "apricot", "banana", "blueberry", "cherry"};
  const uint32_t parts = 3;
  Buckets all;
  for (uint32_t part = 0; part < parts; part++) {
    auto got = ok(R"json({"field":"cat_s","limit":-1,"term_filter":{"partition":)json"
                  + std::to_string(part) + R"json(,"num_partitions":3}})json");
    for (const auto& [key, count] : got) {
      EXPECT_EQ(part, FacetTermFilter::partitionHash(key) % parts) << key;
      all.emplace_back(key, count);
    }
  }
  std::sort(all.begin(), all.end());
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}, {"banana", 3},
                     {"blueberry", 2}, {"cherry", 1}}), all) << join(all);
}

TEST_F(FacetTermFilterTest, countsStayExactInTheDomain) {
  auto unfiltered = ok(R"json({"field":"cat_s","limit":-1})json", "other_s:x");
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}, {"blueberry", 2}}), unfiltered);
  EXPECT_EQ((Buckets{{"apricot", 4}, {"blueberry", 2}}),
            ok(R"json({"field":"cat_s","limit":-1,"term_filter":{"exclude":["apple"]}})json",
               "other_s:x"));
}

TEST_F(FacetTermFilterTest, missingIgnoresFilter) {
  auto result = run(R"json({"field":"cat_s","limit":10,"missing":true,
                        "term_filter":{"prefix":"ap"}})json");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(2, result.missing.value_or(-1));
  EXPECT_EQ(2u, result.buckets.size());
}

TEST_F(FacetTermFilterTest, mincountZeroPadsOnlyAllowedTerms) {
  // Domain matches apple only; banana and blueberry pad with zeros, nothing
  // outside the prefix does.
  EXPECT_EQ((Buckets{{"banana", 0}, {"blueberry", 0}}),
            ok(R"json({"field":"cat_s","limit":10,"mincount":0,
                   "term_filter":{"prefix":"b"}})json", "cat_s:apple"));
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 0}}),
            ok(R"json({"field":"cat_s","limit":10,"mincount":0,
                   "term_filter":{"prefix":"ap"}})json", "cat_s:apple"));
}

TEST_F(FacetTermFilterTest, inlineSubOpsHonorFilter) {
  // limit -1 aggregates sub-ops inline, a different selection path.
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,
      "term_filter":{"prefix":"b"},
      "ops":{"total":{"expr_op":"sum(n_i)"}}}}}})json", req->rawRequest(), req->mr);
  req->collection("main");
  req->execute();
  ASSERT_OK(req);
  const auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("banana", ids[0]);
  EXPECT_EQ("blueberry", ids[1]);
  EXPECT_EQ(3, result.counts[0]);
  EXPECT_EQ(2, result.counts[1]);
}

TEST_F(FacetTermFilterTest, nestedFacet) {
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,
      "ops":{"o":{"field_facet":{"field":"other_s","limit":-1,
                  "term_filter":{"exclude":["y"]}}}}}}}})json",
      req->rawRequest(), req->mr);
  req->collection("main");
  req->execute();
  ASSERT_OK(req);
  const auto& parent = *req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& cats = std::get<api::ColStr>(parent.bucket_ids->kind).v;
  const auto* arr = std::get_if<api::ArrVal>(&(**parent.ops.find("o")).kind);
  ASSERT_NE(nullptr, arr);
  ASSERT_EQ(cats.size(), arr->v.size());
  for (size_t i = 0; i < cats.size(); i++) {
    const auto* child = arr->v[i].facetResult();
    ASSERT_NE(nullptr, child);
    bool yOnly = cats[i] == "banana" || cats[i] == "cherry";
    if (yOnly) {
      EXPECT_TRUE(!child->bucket_ids.has_value()
                  || std::get<api::ColStr>(child->bucket_ids->kind).v.empty()) << cats[i];
    } else {
      ASSERT_TRUE(child->bucket_ids.has_value()) << cats[i];
      const auto& ids = std::get<api::ColStr>(child->bucket_ids->kind).v;
      ASSERT_EQ(1u, ids.size()) << cats[i];
      EXPECT_EQ("x", ids[0]);
    }
  }
}

TEST_F(FacetTermFilterTest, selectedBucketsBypassFilter) {
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"q":{"top_docs":{"limit":0,"get_number":true,"ops":{
      "f":{"field_facet":{"field":"cat_s","limit":10,"selected":["cherry"],
           "term_filter":{"prefix":"ap"}}}}}}}})json", req->rawRequest(), req->mr);
  req->collection("main");
  req->execute();
  ASSERT_OK(req);
  const auto& result = *req->docList("q")->ops.at("f")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(3u, ids.size());
  EXPECT_EQ("apple", ids[0]);
  EXPECT_EQ("apricot", ids[1]);
  EXPECT_EQ("cherry", ids[2]);
  EXPECT_EQ(1, result.counts[2]);
}

TEST_F(FacetTermFilterTest, multiValuedField) {
  CollectionHelper multi;
  multi.indexAll(std::array{
      flatdoc("id", "1", "tags_ss", std::vector<std::string>{"red", "round"}),
      flatdoc("id", "2", "tags_ss", std::vector<std::string>{"red", "square"}),
      flatdoc("id", "3", "tags_ss", std::vector<std::string>{"round"}),
  }, UpdateMessage::COMMIT);
  auto req = localReq(multi.getSearchEngine());
  parseQueryRequest(R"json({"ops":{"f":{"field_facet":{"field":"tags_ss","limit":-1,
      "term_filter":{"regex":"r.*"}}}}})json", req->rawRequest(), req->mr);
  req->collection("main");
  req->execute();
  ASSERT_OK(req);
  const auto& result = *req->responses[0]->proto.ops.at("f")->facetResult();
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  ASSERT_EQ(2u, ids.size());
  EXPECT_EQ("red", ids[0]);
  EXPECT_EQ(2, result.counts[0]);
  EXPECT_EQ("round", ids[1]);
  EXPECT_EQ(2, result.counts[1]);
}

TEST_F(FacetTermFilterTest, emptyFilterKeepsEverything) {
  EXPECT_EQ(5u, ok(R"json({"field":"cat_s","limit":-1,"term_filter":{}})json").size());
}

TEST_F(FacetTermFilterTest, invalidFilters) {
  auto expectError = [&](const std::string& facet, const std::string& needle) {
    SCOPED_TRACE(facet);
    auto result = run(facet);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(std::string::npos, result.error.find(needle)) << result.error;
  };
  expectError(R"json({"field":"n_i","term_filter":{"prefix":"1"}})json",
              "term_filter is supported only on STRING and ID fields");
  expectError(R"json({"field":"cat_s","term_filter":{"regex":"(unclosed"}})json", "term_filter regex");
  expectError(R"json({"field":"cat_s","term_filter":{"partition":3,"num_partitions":3}})json",
              "partition 3 must be less than num_partitions 3");
  expectError(R"json({"field":"cat_s","term_filter":{"partition":1}})json",
              "partition requires num_partitions");
}
