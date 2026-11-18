// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/DistinctFacetCounts.h"
#include "luxir/search/ops/FacetTermFilter.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Buckets = std::vector<std::pair<std::string, int64_t>>;

std::string join(const Buckets& buckets) {
  std::string out;
  for (const auto& [key, count] : buckets) out += key + "=" + std::to_string(count) + " ";
  return out;
}

Buckets bucketsOf(const api::FacetResult& result) {
  Buckets buckets;
  if (!result.bucket_ids.has_value()) return buckets;
  const auto& ids = std::get<api::ColStr>(result.bucket_ids->kind).v;
  for (size_t i = 0; i < ids.size(); i++) buckets.emplace_back(std::string(ids[i]), result.counts[i]);
  return buckets;
}

}  // namespace

// FieldFacet.term_filter picks which terms become buckets; counts stay those
// of the facet's domain.
class FacetTermFilterTest : public LuxirTest {
protected:
  CollectionHelper helper;

  // Two segments. The first holds every term, so global ords are identity.
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
  }

  struct Result {
    Buckets buckets;
    std::optional<int64_t> missing;
    std::string error;
    bool ok = false;
  };

  // `facet` is the field_facet body; `domain` an optional domain query.
  Result run(const std::string& facet, const std::string& domain = "",
             CollectionHelper* on = nullptr) {
    auto req = localReq((on ? *on : helper).getSearchEngine());
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
    result.buckets = bucketsOf(facetResult);
    result.missing = facetResult.missing;
    return result;
  }

  Buckets ok(const std::string& facet, const std::string& domain = "",
             CollectionHelper* on = nullptr) {
    auto result = run(facet, domain, on);
    EXPECT_TRUE(result.ok) << result.error;
    return result.buckets;
  }

  // Facet "f" under top_docs "q" with the given top_docs body.
  Buckets underTopDocs(const std::string& topDocs) {
    auto req = localReq(helper.getSearchEngine());
    parseQueryRequest(R"json({"ops":{"q":{"top_docs":)json" + topDocs + "}}}",
                      req->rawRequest(), req->mr);
    req->collection("main");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    if (!req->ok()) return {};
    return bucketsOf(*req->docList("q")->ops.at("f")->facetResult());
  }
};

TEST_F(FacetTermFilterTest, prefixAcrossCounterReps) {
  SearchOverridesGuard guard(forcedFacetCounterMode);
  for (auto mode : {FacetCounterMode::AUTO, FacetCounterMode::FORCE_VECTOR,
                    FacetCounterMode::FORCE_SKINNY, FacetCounterMode::FORCE_HASH,
                    FacetCounterMode::SPAN_GLOBAL}) {
    forcedFacetCounterMode = mode;
    SCOPED_TRACE((int)mode);
    EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}}),
              ok(R"json({"field":"cat_s","limit":10,"term_filter":{"prefix":"ap"}})json"));
    EXPECT_EQ((Buckets{{"banana", 3}, {"cherry", 1}}),
              ok(R"json({"field":"cat_s","limit":10,
                     "term_filter":{"include":["cherry","banana","missing"]}})json"));
    EXPECT_EQ((Buckets{{"blueberry", 2}}),
              ok(R"json({"field":"cat_s","limit":10,"term_filter":{"regex":"b.*y"}})json"));
    // Wide filters: hash and span counters iterate their entries.
    EXPECT_EQ((Buckets{{"apricot", 4}, {"banana", 3}}),
              ok(R"json({"field":"cat_s","limit":2,"term_filter":{"exclude":["apple"]}})json"));
    // A full page with the term tier tested behind its floor.
    EXPECT_EQ((Buckets{{"apricot", 4}, {"blueberry", 2}}),
              ok(R"json({"field":"cat_s","limit":2,"term_filter":{"regex":".*r.*"}})json"));
  }
}

TEST_F(FacetTermFilterTest, skinnyOverflowRejected) {
  // 300 counts overflow the skinny byte counter. Rejecting the overflowed
  // term must not end selection before the in-range terms are read.
  CollectionHelper big;
  std::vector<Doc> docs;
  for (int i = 0; i < 300; i++) docs.push_back(flatdoc("id", "b" + std::to_string(i), "c_s", "big"));
  docs.push_back(flatdoc("id", "s1", "c_s", "small"));
  docs.push_back(flatdoc("id", "s2", "c_s", "small"));
  docs.push_back(flatdoc("id", "t1", "c_s", "tiny"));
  ASSERT_TRUE(big.indexAll(docs, UpdateMessage::COMMIT).success);
  SearchOverridesGuard guard(forcedFacetCounterMode);
  forcedFacetCounterMode = FacetCounterMode::FORCE_SKINNY;
  EXPECT_EQ((Buckets{{"small", 2}}),
            ok(R"json({"field":"c_s","limit":1,"term_filter":{"exclude":["big"]}})json", "", &big));
  EXPECT_EQ((Buckets{{"big", 300}, {"small", 2}}),
            ok(R"json({"field":"c_s","limit":2,"term_filter":{"regex":"[bs].*"}})json", "", &big));
}

TEST_F(FacetTermFilterTest, filterAppliesBeforeLimit) {
  // apple outranks banana overall, so a post-limit filter would return nothing.
  EXPECT_EQ((Buckets{{"banana", 3}}),
            ok(R"json({"field":"cat_s","limit":1,"term_filter":{"prefix":"b"}})json"));
  EXPECT_EQ((Buckets{{"apricot", 4}, {"banana", 3}}),
            ok(R"json({"field":"cat_s","limit":2,"term_filter":{"exclude":["apple"]}})json"));
}

TEST_F(FacetTermFilterTest, regexShapes) {
  auto all = [&](const std::string& filter) {
    return ok(R"json({"field":"cat_s","limit":-1,"term_filter":)json" + filter + "}");
  };
  EXPECT_EQ((Buckets{{"apricot", 4}, {"blueberry", 2}, {"cherry", 1}}), all(R"({"regex":".*r.*"})"));
  EXPECT_TRUE(all(R"({"regex":"ap"})").empty());                      // whole term
  EXPECT_EQ((Buckets{{"apple", 5}}), all(R"({"regex":"apple"})"));      // single term
  EXPECT_EQ(5u, all(R"({"regex":".*"})").size());                       // every term
  EXPECT_EQ((Buckets{{"banana", 3}, {"blueberry", 2}, {"cherry", 1}}),
            all(R"({"exclude_regex":"a.*"})"));                        // prefix hole
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}, {"banana", 3}}),
            all(R"({"exclude_regex":".*rr.*"})"));                     // general exclude
  EXPECT_TRUE(all(R"({"exclude_regex":".*"})").empty());
  EXPECT_TRUE(all(R"({"include":["nope"]})").empty());                 // nothing resolves
  EXPECT_EQ((Buckets{{"blueberry", 2}}),
            all(R"({"prefix":"b","regex":".*e.*","exclude":["banana"]})"));
  EXPECT_EQ(5u, all("{}").size());
}

TEST_F(FacetTermFilterTest, partitionsCoverEveryTermOnce) {
  // Plain and with an inline sub-op (no page floor, so the term tier is
  // resolved up front).
  for (std::string ops : {"", R"json(,"ops":{"total":{"expr_op":"sum(n_i)"}})json"}) {
    const uint32_t parts = 3;
    Buckets all;
    for (uint32_t part = 0; part < parts; part++) {
      auto got = ok(R"json({"field":"cat_s","limit":-1,"term_filter":{"partition":)json"
                    + std::to_string(part) + R"json(,"num_partitions":3})json" + ops + "}");
      for (const auto& [key, count] : got) {
        EXPECT_EQ(part, FacetOrdFilter::partitionHash(key) % parts) << key;
        all.emplace_back(key, count);
      }
    }
    std::sort(all.begin(), all.end());
    EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}, {"banana", 3},
                       {"blueberry", 2}, {"cherry", 1}}), all) << join(all) << ops;
  }
}

TEST_F(FacetTermFilterTest, countsStayExactInTheDomain) {
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
  // The domain matches apple only.
  EXPECT_EQ((Buckets{{"banana", 0}, {"blueberry", 0}}),
            ok(R"json({"field":"cat_s","limit":10,"mincount":0,
                   "term_filter":{"prefix":"b"}})json", "cat_s:apple"));
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 0}}),
            ok(R"json({"field":"cat_s","limit":10,"mincount":0,
                   "term_filter":{"prefix":"ap"}})json", "cat_s:apple"));
  EXPECT_EQ((Buckets{{"blueberry", 0}, {"cherry", 0}}),
            ok(R"json({"field":"cat_s","limit":10,"mincount":0,
                   "term_filter":{"regex":".*rr.*"}})json", "cat_s:apple"));
}

TEST_F(FacetTermFilterTest, inlineSubOpsHonorFilter) {
  // limit -1 aggregates sub-ops inline, through a dense or a sparse table.
  SearchOverridesGuard guard(forcedInlineFacetEntryMode);
  for (auto mode : {InlineFacetEntryMode::FORCE_DENSE, InlineFacetEntryMode::FORCE_SPARSE}) {
    forcedInlineFacetEntryMode = mode;
    for (std::string filter : {R"({"prefix":"b"})", R"({"regex":"b.*"})", R"({"exclude_regex":"[^b].*"})"}) {
      EXPECT_EQ((Buckets{{"banana", 3}, {"blueberry", 2}}),
                ok(R"json({"field":"cat_s","limit":-1,"term_filter":)json" + filter
                   + R"json(,"ops":{"total":{"expr_op":"sum(n_i)"}}})json")) << filter;
    }
  }
}

TEST_F(FacetTermFilterTest, sortBySubOp) {
  // n_i grows with the doc number, so blueberry's docs outweigh banana's.
  EXPECT_EQ((Buckets{{"blueberry", 2}, {"banana", 3}}),
            ok(R"json({"field":"cat_s","limit":2,"term_filter":{"prefix":"b"},
                   "sort":[{"expr":"top","dir":"desc"}],
                   "ops":{"top":{"expr_op":"max(n_i)"}}})json"));
}

TEST_F(FacetTermFilterTest, nestedFacet) {
  SearchOverridesGuard guard(forcedFacetFeedStrategy);
  for (auto strategy : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = strategy;
    for (std::string filter : {R"({"exclude":["y"]})", R"({"regex":"[^y]"})"}) {
      auto req = localReq(helper.getSearchEngine());
      parseQueryRequest(R"json({"ops":{"f":{"field_facet":{"field":"cat_s","limit":-1,
          "ops":{"o":{"field_facet":{"field":"other_s","limit":-1,"mincount":0,
                      "term_filter":)json" + filter + "}}}}}}}",
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
        // mincount 0 pads x where the parent has none; y never appears.
        EXPECT_EQ((Buckets{{"x", yOnly ? 0 : (cats[i] == "apple" ? 5 : cats[i] == "apricot" ? 4 : 2)}}),
                  bucketsOf(*child)) << cats[i] << " " << filter << " " << (int)strategy;
      }
    }
  }
}

TEST_F(FacetTermFilterTest, selectedBucketsBypassFilter) {
  EXPECT_EQ((Buckets{{"apple", 5}, {"apricot", 4}, {"cherry", 1}}),
            underTopDocs(R"json({"limit":0,"ops":{
                "f":{"field_facet":{"field":"cat_s","limit":10,"selected":["cherry"],
                     "term_filter":{"prefix":"ap"}}}}})json"));
  EXPECT_EQ((Buckets{{"cherry", 1}}),
            underTopDocs(R"json({"limit":0,"ops":{
                "f":{"field_facet":{"field":"cat_s","limit":0,"selected":["cherry"],
                     "term_filter":{"regex":".*p.*"}}}}})json"));
}

TEST_F(FacetTermFilterTest, distinctCounts) {
  SearchOverridesGuard guard(forcedDistinctFacetMode);
  for (auto mode : {DistinctFacetMode::PAGED, DistinctFacetMode::HASH}) {
    forcedDistinctFacetMode = mode;
    EXPECT_EQ((Buckets{{"banana", 1}, {"blueberry", 1}}),
              ok(R"json({"field":"cat_s","count_distinct":"other_s",
                     "term_filter":{"prefix":"b"}})json"));
    EXPECT_EQ((Buckets{{"blueberry", 1}, {"cherry", 0}}),
              ok(R"json({"field":"cat_s","count_distinct":"other_s","mincount":0,
                     "term_filter":{"regex":".*rr.*"}})json", "other_s:x"));
    // Facets under a collapsed list count groups, through the same path.
    EXPECT_EQ((Buckets{{"apple", 1}, {"apricot", 1}}),
              underTopDocs(R"json({"limit":0,"collapse":"other_s","ops":{
                  "f":{"field_facet":{"field":"cat_s","term_filter":{"prefix":"ap"}}}}})json"));
  }
}

TEST_F(FacetTermFilterTest, multiValuedField) {
  CollectionHelper multi;
  multi.indexAll(std::array{
      flatdoc("id", "1", "tags_ss", std::vector<std::string>{"red", "round"}),
      flatdoc("id", "2", "tags_ss", std::vector<std::string>{"red", "square"}),
      flatdoc("id", "3", "tags_ss", std::vector<std::string>{"round"}),
  }, UpdateMessage::COMMIT);
  EXPECT_EQ((Buckets{{"red", 2}, {"round", 2}}),
            ok(R"json({"field":"tags_ss","limit":-1,"term_filter":{"regex":"r.*"}})json", "", &multi));
}

// No segment holds every term, so global ords are delta-mapped and a ceiling
// is the smallest segment ceiling.
TEST_F(FacetTermFilterTest, noSegmentHoldsEveryTerm) {
  CollectionHelper split;
  ASSERT_TRUE(split.indexAll(std::array{
      flatdoc("id", "1", "c_s", "apple"), flatdoc("id", "2", "c_s", "banana"),
      flatdoc("id", "3", "c_s", "banana")}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(split.indexAll(std::array{
      flatdoc("id", "4", "c_s", "apricot"), flatdoc("id", "5", "c_s", "blueberry"),
      flatdoc("id", "6", "c_s", "cherry")}, UpdateMessage::COMMIT).success);
  EXPECT_EQ((Buckets{{"banana", 2}, {"blueberry", 1}}),
            ok(R"json({"field":"c_s","limit":-1,"term_filter":{"prefix":"b"}})json", "", &split));
  EXPECT_EQ((Buckets{{"apricot", 1}, {"cherry", 1}}),
            ok(R"json({"field":"c_s","limit":-1,"term_filter":{"include":["cherry","apricot"]}})json",
               "", &split));
  // The first segment's ceiling for "apr" is banana; the global one is apricot.
  EXPECT_EQ((Buckets{{"apricot", 1}}),
            ok(R"json({"field":"c_s","limit":-1,"term_filter":{"prefix":"apr"}})json", "", &split));
  EXPECT_EQ((Buckets{{"apricot", 0}, {"banana", 0}, {"blueberry", 0}}),
            ok(R"json({"field":"c_s","limit":3,"mincount":0,
                   "term_filter":{"exclude":["apple"]}})json", "c_s:nope", &split));
}

TEST_F(FacetTermFilterTest, literalsNormalizeAsQueries) {
  CollectionHelper folded;
  SchemaBuilder schema;
  auto& field = schema.field("lc");
  field.type = api::FieldDef_::FieldClass::STRING;
  field.column = true;
  schema.normalizer(field, {"lowercase"});
  schema.set(folded.collection());
  ASSERT_TRUE(folded.indexAll(std::array{
      flatdoc("id", "1", "lc", "Apple"), flatdoc("id", "2", "lc", "APRICOT"),
      flatdoc("id", "3", "lc", "Banana")}, UpdateMessage::COMMIT).success);
  EXPECT_EQ((Buckets{{"apple", 1}, {"apricot", 1}}),
            ok(R"json({"field":"lc","term_filter":{"prefix":"AP"}})json", "", &folded));
  EXPECT_EQ((Buckets{{"banana", 1}}),
            ok(R"json({"field":"lc","term_filter":{"include":["BANANA"],"regex":"B.*"}})json",
               "", &folded));
  // ID fields match verbatim.
  EXPECT_EQ((Buckets{{"d1", 1}}),
            ok(R"json({"field":"id","term_filter":{"include":["d1","D1"]}})json"));
}

// Values over the indexed term length are fitted to a prefix plus hash.
TEST_F(FacetTermFilterTest, fittedLongTerms) {
  CollectionHelper fitted;
  std::string longValue = std::string(300, 'x') + "tail";
  ASSERT_TRUE(fitted.indexAll(std::array{
      flatdoc("id", "1", "v_s", longValue), flatdoc("id", "2", "v_s", "xshort")},
      UpdateMessage::COMMIT).success);
  auto run = [&](const std::string& filter) {
    auto req = localReq(fitted.getSearchEngine());
    parseQueryRequest(R"json({"ops":{"f":{"field_facet":{"field":"v_s","limit":-1,"term_filter":)json"
                      + filter + "}}}}", req->rawRequest(), req->mr);
    req->collection("main");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    auto warned = std::ranges::count(req->respWarnings(), std::string_view("fitted_terms"),
                                     &api::Warning::code);
    return std::pair<size_t, long>(
        bucketsOf(*req->responses[0]->proto.ops.at("f")->facetResult()).size(), warned);
  };
  std::string literal(240, 'x');
  // The regex keeps every term with the kept prefix, as the query does.
  EXPECT_EQ((std::pair<size_t, long>(1, 0)), run(R"({"regex":")" + literal + R"(.*"})"));
  // exclude_regex never widens: it tests the fitted bytes, so the fitted term
  // is not excluded, and it warns.
  EXPECT_EQ((std::pair<size_t, long>(2, 1)), run(R"({"exclude_regex":")" + literal + R"(.*"})"));
  // A general pattern can reach fitted bytes and says so.
  EXPECT_EQ((std::pair<size_t, long>(0, 1)), run(R"({"regex":".*tail"})"));
}

TEST_F(FacetTermFilterTest, fieldWithoutTerms) {
  EXPECT_TRUE(ok(R"json({"field":"absent_s","mincount":0,"term_filter":{"regex":".*a.*"}})json")
                  .empty());
}

TEST_F(FacetTermFilterTest, invalidFilters) {
  auto expectError = [&](const std::string& facet, const std::string& needle) {
    SCOPED_TRACE(facet);
    auto result = run(facet);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(std::string::npos, result.error.find("facet 'f': term_filter")) << result.error;
    EXPECT_NE(std::string::npos, result.error.find(needle)) << result.error;
  };
  expectError(R"json({"field":"n_i","term_filter":{"prefix":"1"}})json",
              "term_filter is not supported on int field 'n_i'");
  expectError(R"json({"field":"cat_s","term_filter":{"regex":"(unclosed"}})json",
              "term_filter regex: Regex query for field 'cat_s' pattern '(unclosed'");
  expectError(R"json({"field":"cat_s","term_filter":{"partition":3,"num_partitions":3}})json",
              "partition 3 must be less than num_partitions 3");
  expectError(R"json({"field":"cat_s","term_filter":{"partition":1}})json",
              "partition requires num_partitions");
}
