// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <array>
#include <fmt/format.h>

#include "luxir/api/RequestFlags.h"
#include "luxir/api/padded_input.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
void indexDates(CollectionHelper& helper) {
  SchemaBuilder schema;
  for (auto name : {"stamp", "when_dt"}) {
    auto& field = schema.field(name);
    field.type = api::FieldDef::FieldClass::DATE;
    field.unit = std::string_view(name) == "stamp" ? api::FieldDef::DateUnit::NS : api::FieldDef::DateUnit::MS;
    field.index = api::FieldDef::IndexMode::RANGE;
  }
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "stamp", 2000000, "when_dt", 2),
      flatdoc("id", "b", "stamp", 3000000, "when_dt", 3)}, UpdateMessage::COMMIT).success);
}

void wireRoundTrip(LocalReq& req) {
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(req.rawRequest(), bytes));
  api::SearchRequest decoded;
  ASSERT_TRUE(api::decode(decoded, api::copyToPaddedInput(bytes, req.mr), req.mr));
  req.rawRequest() = decoded;
}

auto runFlags(CollectionHelper& helper, std::string_view json, bool binary = false) {
  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(json, req->rawRequest(), req->mr);
  if (binary) wireRoundTrip(*req);
  req->execute(false);
  return req;
}
}

class RequestFlagsTest : public LuxirTest {};

TEST_F(RequestFlagsTest, numericLiteralUnitsInEveryDialectAndWireFormat) {
  CollectionHelper helper;
  indexDates(helper);
  for (int mode = 0; mode < 4; mode++) for (auto field : {"stamp", "when_dt"}) {
    bool nanos = mode == 3 || (mode < 2 && std::string_view(field) == "stamp");
    int64_t tick = nanos ? 1000000 : 1;
    int64_t value = 2 * tick;
    std::array queries{
        fmt::format(R"({{"match":{{"field":"{}","val":{}}}}})", field, value),
        fmt::format(R"({{"match":{{"field":"{}","val":"{}"}}}})", field, value),
        fmt::format(R"({{"range":{{"field":"{}","gte":{},"lt":{}}}}})", field, value, value + tick),
        fmt::format(R"({{"any_of":{{"field":"{}","values":[{}]}}}})", field, value),
        fmt::format(R"({{"expr":"{}:{}"}})", field, value),
        fmt::format(R"({{"simple_query":{{"q":"{}:{}","fields":["id"]}}}})", field, value),
        fmt::format(R"({{"match":{{"field":"{}","val":"1970-01-01T00:00:00.002Z"}}}})", field)};
    for (const auto& query : queries) for (bool binary : {false, true}) {
      SCOPED_TRACE(query);
      std::string json = fmt::format(R"({{"flags":["{}"],"query":{},"limit":0,"count_limit":-1}})",
          RequestFlags::DATE_NAMES[mode], query);
      auto req = runFlags(helper, json, binary);
      ASSERT_OK(req);
      EXPECT_EQ(1, req->getMatchCount());
      EXPECT_EQ((uint64_t)mode, req->flags);
    }
  }
}

TEST_F(RequestFlagsTest, overridesApplyBeforeChildDomainsAndFacetSelections) {
  CollectionHelper helper;
  indexDates(helper);
  for (bool binary : {false, true}) {
    auto req = runFlags(helper, R"json({"flags":["date_ns"],"ops":{"parent":{
      "flags":["date_ms"],"top_docs":{"query":"stamp:>=2","limit":0,"count_limit":-1,"ops":{
        "inherited":{"top_docs":{"query":"stamp:2","limit":0,"count_limit":-1}},
        "native":{"flags":["date_native"],"top_docs":{"query":"stamp:2000000","limit":0,"count_limit":-1,
          "ops":{"iso":{"flags":["date_iso"],"top_docs":{"query":"stamp:2000000","limit":0,"count_limit":-1}}}}},
        "reset":{"flags":["date_ns"],"domain":{"query":"stamp:2000000","filter":["stamp:>=2000000"]},
          "top_docs":{"limit":0,"count_limit":-1}},
        "pins":{"flags":["date_ns"],"field_facet":{"field":"when_dt","selected":[2000000]}},
        "grid":{"flags":["date_ns"],"range_facet":{"field":"stamp","start":0,"end":4000000,
          "gap":1000000,"selected":[2000000]}},
        "queries":{"flags":["date_native"],"query_facet":{"buckets":{"a":"stamp:2000000"},"selected":["a"]}},
        "offset":"min((stamp+1)-stamp)"}}}}})json", binary);
    ASSERT_OK(req);
    EXPECT_EQ(1, req->getMatchCount("parent"));
    const auto& children = req->docList("parent")->ops;
    for (auto name : {"inherited", "native", "reset"}) {
      EXPECT_EQ(1, children.at(name)->docList()->found.value_or(-1)) << name;
    }
    EXPECT_EQ(1, children.at("native")->docList()->ops.at("iso")->docList()->found.value_or(-1));
    EXPECT_EQ(1, children.at("offset")->asInt());
  }
}

TEST_F(RequestFlagsTest, explicitAndDataFacetGridsConvertGapAndOriginAtTheEdge) {
  CollectionHelper helper;
  indexDates(helper);
  for (int mode = 0; mode < 4; mode++) for (auto field : {"stamp", "when_dt"}) {
    bool nanos = mode == 3 || (mode < 2 && std::string_view(field) == "stamp");
    int64_t inputTick = nanos ? 1000000 : 1;
    int64_t nativeTick = std::string_view(field) == "stamp" ? 1000000 : 1;
    std::string json = fmt::format(R"json({{"ops":{{
      "grid":{{"flags":["{0}"],"range_facet":{{"field":"{1}","start":0,"end":{2},"gap":{3}}}}},
      "data":{{"flags":["{0}"],"range_facet":{{"field":"{1}","bounds":"data","gap":{4},"origin":{3}}}}}
      }}}})json", RequestFlags::DATE_NAMES[mode], field, 4 * inputTick, inputTick, 2 * inputTick);
    auto req = runFlags(helper, json);
    ASSERT_OK(req);
    for (auto name : {"grid", "data"}) {
      const auto& facet = *req->responses[0]->proto.ops.at(name)->facetResult();
      const auto& bounds = std::get<api::ArrArrInt>(facet.bucket_ids->kind).v;
      std::vector<int64_t> ids, ends;
      for (const auto& pair : bounds) {
        ASSERT_EQ(2u, pair.v.size());
        ids.push_back(pair.v[0]);
        ends.push_back(pair.v[1]);
      }
      if (name == std::string_view("grid")) {
        EXPECT_EQ((std::vector<int64_t>{0, 0, 1, 1}), std::vector<int64_t>(facet.counts.begin(), facet.counts.end()));
        EXPECT_EQ((std::vector<int64_t>{0, nativeTick, 2 * nativeTick, 3 * nativeTick}), ids);
        EXPECT_EQ((std::vector<int64_t>{nativeTick, 2 * nativeTick, 3 * nativeTick, 4 * nativeTick}), ends);
      } else {
        EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector<int64_t>(facet.counts.begin(), facet.counts.end()));
        EXPECT_EQ((std::vector<int64_t>{nativeTick, 3 * nativeTick}), ids);
        EXPECT_EQ((std::vector<int64_t>{3 * nativeTick, 5 * nativeTick}), ends);
      }
    }
  }
}

TEST_F(RequestFlagsTest, binaryValidationRejectsReservedUnknownAndPartialOverrides) {
  CollectionHelper helper;
  for (bool opLevel : {false, true}) for (auto [flags, set] : {
      std::pair<uint64_t, uint64_t>{1, 1}, {2, 2}, {8, 0}, {0, 8},
      {uint64_t{1} << 63, 0}, {4, 0}, {0, 4}}) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs("q").allQuery().limit(0);
    if (opLevel) {
      auto* op = const_cast<api::SearchOp*>(req->rawRequest().ops.at("q").pointer());
      op->flags = flags;
      op->flags_set = set;
    } else {
      req->rawRequest().flags = flags;
      req->rawRequest().flags_set = set;
    }
    wireRoundTrip(*req);
    req->execute(false);
    EXPECT_FALSE(req->ok());
    auto expected = RequestFlags::validationError(flags, set);
    EXPECT_NE(std::string::npos, req->errorMsg().find(expected)) << req->errorMsg();
  }
}
