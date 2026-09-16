// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <string>

#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

class JsonResponseTest : public LuxirTest {};

namespace {

// Reference JSON string escaper: the exact semantics appendJsonString's
// scalar path implements (two-char escapes, \u00xx lowercase for other
// control chars, raw UTF-8 passthrough).  The SIMD bulk path must render
// byte-identically.
std::string refEscape(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
      case '"':  out += R"(\")"; break;
      case '\\': out += R"(\\)"; break;
      case '\n': out += R"(\n)"; break;
      case '\r': out += R"(\r)"; break;
      case '\t': out += R"(\t)"; break;
      case '\b': out += R"(\b)"; break;
      case '\f': out += R"(\f)"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), R"(\u%04x)", (unsigned)(unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

} // namespace

// String escaping renders byte-identically across the short-string scalar
// path, the SIMD bulk path, and the exotic-control-char restart, at block
// boundaries and for every byte value.
TEST_F(JsonResponseTest, stringEscapingSimdPathMatchesScalar) {
  std::vector<std::string> vals;
  vals.push_back(std::string(200, 'a'));  // clean, several AVX2 blocks
  vals.push_back("short\"q");             // < 32 bytes: scalar path
  {
    std::string v(100, 'x');  // escapes at and around the 32-byte block edge
    v[0] = '"'; v[31] = '\\'; v[32] = '\n'; v[33] = '\t'; v[98] = '"';
    vals.push_back(v);
  }
  vals.push_back(std::string(40, 'y') + "\r\b\f" + std::string(40, 'z'));
  {
    std::string v(64, 'w');  // exotic control mid-string: bulk aborts, restarts scalar
    v[40] = '\x01';
    vals.push_back(v);
  }
  {
    std::string v(47, 'v');  // exotic control in the scalar tail (< one SSE2 block)
    v[46] = '\x1f';
    vals.push_back(v);
  }
  {
    std::string v;  // every byte value, including NUL and all control chars
    for (int b = 0; b < 256; b++) v += (char)b;
    vals.push_back(v);
  }
  vals.push_back("caf\xC3\xA9 \xE2\x98\x95 utf8 passthrough padded past the block size");

  std::vector<std::string_view> views(vals.begin(), vals.end());
  luxir::api::Column col;
  auto& strCol = col.kind.emplace<luxir::api::ColStr>();
  strCol.v = std::span<const std::string_view>(views);
  std::pair<std::string_view, luxir::api::Column> colPair{"s", col};

  luxir::api::DocList dl;
  dl.columns = luxir::api::map_view<std::string_view, luxir::api::Column>(
      std::span<const std::pair<std::string_view, luxir::api::Column>>(&colPair, 1));
  dl.row_count = (int32_t)views.size();

  luxir::api::Val val;
  val.kind = dl;
  std::pair<std::string_view, ::hpp_proto::indirect_view<luxir::api::Val>> opPair{
      "q", ::hpp_proto::indirect_view<luxir::api::Val>{&val}};
  luxir::api::SearchResponse resp;
  resp.ops = luxir::api::map_view<std::string_view, ::hpp_proto::indirect_view<luxir::api::Val>>(
      std::span<const std::pair<std::string_view, ::hpp_proto::indirect_view<luxir::api::Val>>>(&opPair, 1));

  std::string expected = R"({"docs":[)";
  for (size_t i = 0; i < vals.size(); i++) {
    if (i) expected += ',';
    expected += R"({"s":)";
    expected += refEscape(vals[i]);
    expected += '}';
  }
  expected += "]}";
  EXPECT_EQ(expected, renderSearchResponseBody(resp, true));
}

TEST_F(JsonResponseTest, stringFacetRowsAndOptionalMetadata) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "x"),
    flatdoc("id", "3", "cat_s", "y"),
    flatdoc("id", "4", "other_s", "z"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& facet = req->facet("cats", "cat_s");
  facet.limit(-1);
  std::get<api::FieldFacet>(facet.rawOp().kind).missing = true;
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}],"missing":1}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, executionProfileShape) {
  luxir::api::ExecutionProfilePiece piece;
  piece.kind = "segment";
  piece.segment = 2;
  piece.max_doc = 11;
  piece.strategy = "hash";
  std::array<std::string_view, 2> details = {"all-docs domain, bulk column scan",
                                             "want=hash, found=skinny"};
  piece.details = details;
  piece.cardinality = 64;
  piece.domain_size = 1;
  piece.thread_id = 123;
  piece.elapsed_us = 7;
  luxir::api::ExecutionProfileOp op;
  op.name = "cats";
  op.pieces = {&piece, 1};
  luxir::api::SearchResponse resp;
  resp.profile.emplace().ops = {&op, 1};

  EXPECT_EQ(
      R"({"profile":{"ops":[{"name":"cats","pieces":[{"kind":"segment","segment":2,"max_doc":11,"strategy":"hash","cardinality":64,"domain_size":1,"thread_id":123,"elapsed_us":7,"details":["all-docs domain, bulk column scan","want=hash, found=skinny"]}]}]}})",
      renderSearchResponseBody(resp));
}

TEST_F(JsonResponseTest, integerFacetPreservesZeroBucketId) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "price_i", (int64_t)0),
    flatdoc("id", "2", "price_i", (int64_t)0),
    flatdoc("id", "3", "price_i", (int64_t)7),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->facet("prices", "price_i").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"prices":{"buckets":[{"val":0,"count":2},{"val":7,"count":1}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, rangeFacetRowsUseIntegerBounds) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "price_i", (int64_t)5),
    flatdoc("id", "2", "price_i", (int64_t)15),
    flatdoc("id", "3", "price_i", (int64_t)15),
    flatdoc("id", "4", "price_i", (int64_t)25),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->rangeFacet("prices", "price_i").range(0, 30, 10);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"prices":{"buckets":[{"val":[0,10],"count":1},{"val":[10,20],"count":2},{"val":[20,30],"count":1}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, facetMetricsRenderPerBucketAndEmptyAsNull) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "a", "price_i", (int64_t)10),
    flatdoc("id", "2", "cat_s", "a", "price_i", (int64_t)30),
    flatdoc("id", "3", "cat_s", "b"),
    flatdoc("id", "4", "cat_s", "b"),
    flatdoc("id", "5", "cat_s", "c", "price_i", (int64_t)5),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& facet = req->facet("cats", "cat_s");
  facet.limit(-1);
  facet.min("minimum", "price_i");
  facet.sum("total", "price_i");
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"cats":{"buckets":[{"val":"a","count":2,"total":40,"minimum":10},{"val":"b","count":2,"total":null,"minimum":null},{"val":"c","count":1,"total":5,"minimum":5}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, nestedFacetRowsRecurse) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x", "sub_s", "p"),
    flatdoc("id", "2", "cat_s", "x", "sub_s", "q"),
    flatdoc("id", "3", "cat_s", "y", "sub_s", "p"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& outer = req->facet("outer", "cat_s");
  outer.limit(-1);
  outer.facet("inner", "sub_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"outer":{"buckets":[{"val":"x","count":2,"inner":{"buckets":[{"val":"p","count":1},{"val":"q","count":1}]}},{"val":"y","count":1,"inner":{"buckets":[{"val":"p","count":1}]}}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, wholeDomainStatsStayUnderOpsWithoutDocs) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "price_i", (int64_t)10),
    flatdoc("id", "2", "price_i", (int64_t)30),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->avg("average", "price_i");
  req->min("empty", "other_i");
  req->sum("empty_sum", "other_i");
  req->sum("total", "price_i");
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(R"({"ops":{"average":20,"empty":null,"empty_sum":null,"total":40}})",
            renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, rowsFormatRendersDocObjects) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x", "n_i", (int64_t)5),
    flatdoc("id", "2"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().fields({"id", "cat_s", "n_i"})
      .documentFormat(api::DocFormat::ROWS).limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  // Row maps signal missing structurally: doc 2 has no cat_s/n_i keys.
  EXPECT_EQ(
      R"({"ops":{"q":{"docs":[{"id":"1","cat_s":"x","n_i":5},{"id":"2"}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, namedDocListAndFacetRemainUnderOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "x"),
    flatdoc("id", "3", "cat_s", "y"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("hits").allQuery().fields({"id"}).limit(2).getNumber();
  req->facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"hits":{"found":3,"docs":[{"id":"1"},{"id":"2"}]},"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, shorthandUnwrapsImplicitQ) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "x"),
    flatdoc("id", "3", "cat_s", "y"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& td = req->topDocs("q");
  td.allQuery().fields({"id"}).limit(1).getNumber();
  td.facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"found":3,"docs":[{"id":"1"}],"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}]}}})",
      renderSearchResponseBody(req->responses[0]->proto, true));
  EXPECT_EQ(
      R"({"ops":{"q":{"found":3,"docs":[{"id":"1"}],"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}]}}}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, secondDocListRendersItsNestedOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "y"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("first").allQuery().fields({"id"}).limit(1).getNumber();
  auto& second = req->topDocs("second");
  second.allQuery().fields({"id"}).limit(1);
  second.facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"first":{"found":2,"docs":[{"id":"1"}]},"second":{"docs":[{"id":"1"}],"ops":{"cats":{"buckets":[{"val":"x","count":1},{"val":"y","count":1}]}}}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, everyDocListRendersUnderOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1"),
    flatdoc("id", "2"),
    flatdoc("id", "3"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("first").allQuery().fields({"id"}).limit(1).getNumber();
  req->topDocs("second").allQuery().fields({"id"}).limit(2);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"first":{"found":3,"docs":[{"id":"1"}]},"second":{"docs":[{"id":"1"},{"id":"2"}]}}})",
      renderSearchResponseBody(req->responses[0]->proto));
}

} // namespace luxir::test
