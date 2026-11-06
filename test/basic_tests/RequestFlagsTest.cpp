// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <array>
#include <fmt/format.h>

#include "luxir/api/RequestFlags.h"
#include "luxir/api/padded_input.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/search/SearchOverrides.h"
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
    int64_t outputTick = inputTick;
    std::string json = fmt::format(R"json({{"ops":{{
      "grid":{{"flags":["{0}"],"range_facet":{{"field":"{1}","start":0,"end":{2},"gap":{3}}}}},
      "data":{{"flags":["{0}"],"range_facet":{{"field":"{1}","bounds":"data","gap":{4},"origin":{3}}}}}
      }}}})json", RequestFlags::DATE_NAMES[mode], field, 4 * inputTick, inputTick, 2 * inputTick);
    auto req = runFlags(helper, json);
    ASSERT_OK(req);
    for (auto name : {"grid", "data"}) {
      const auto& facet = *req->responses[0]->proto.ops.at(name)->facetResult();
      std::vector<int64_t> ids, ends;
      if (mode == 1) {
        for (const auto& pair : std::get<api::ArrArrStr>(facet.bucket_ids->kind).v) {
          ASSERT_EQ(2u, pair.v.size());
          // Compare the ISO labels in the same input unit as the numeric modes.
          auto unit = std::string_view(field) == "stamp" ? DateUnit::NANOS : DateUnit::MILLIS;
          for (size_t i = 0; i < 2; i++) {
            auto range = parseDateRange(pair.v[i], 0, TimeZone::utc());
            ASSERT_TRUE(range);
            auto value = toDateUnits(range->lo, range->loNanos, unit);
            ASSERT_TRUE(value);
            (i == 0 ? ids : ends).push_back(*value);
          }
        }
      } else {
        for (const auto& pair : std::get<api::ArrArrInt>(facet.bucket_ids->kind).v) {
          ASSERT_EQ(2u, pair.v.size());
          ids.push_back(pair.v[0]);
          ends.push_back(pair.v[1]);
        }
      }
      if (name == std::string_view("grid")) {
        EXPECT_EQ((std::vector<int64_t>{0, 0, 1, 1}), std::vector<int64_t>(facet.counts.begin(), facet.counts.end()));
        EXPECT_EQ((std::vector<int64_t>{0, outputTick, 2 * outputTick, 3 * outputTick}), ids);
        EXPECT_EQ((std::vector<int64_t>{outputTick, 2 * outputTick, 3 * outputTick, 4 * outputTick}), ends);
      } else {
        EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector<int64_t>(facet.counts.begin(), facet.counts.end()));
        EXPECT_EQ((std::vector<int64_t>{outputTick, 3 * outputTick}), ids);
        EXPECT_EQ((std::vector<int64_t>{3 * outputTick, 5 * outputTick}), ends);
      }
    }
  }
}

TEST_F(RequestFlagsTest, binaryValidationRejectsUnknownPartialAndOpOnlyOverrides) {
  CollectionHelper helper;
  for (bool opLevel : {false, true}) for (auto [flags, set] : {
      std::pair<uint64_t, uint64_t>{1, 1}, {2, 2}, {8, 0}, {0, 8},
      {uint64_t{1} << 63, 0}, {4, 0}, {0, 4}}) {
    if (!opLevel && ((flags | set) & RequestFlags::INT64_STR)) continue;
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
    auto expected = RequestFlags::validationError(flags, set,
        opLevel ? RequestFlags::Level::OP : RequestFlags::Level::REQUEST);
    EXPECT_NE(std::string::npos, req->errorMsg().find(expected)) << req->errorMsg();
  }
}

TEST_F(RequestFlagsTest, dateOutputInEveryDocumentFormatAndWireFormat) {
  CollectionHelper helper;
  SchemaBuilder schema;
  for (bool multi : {false, true}) {
    auto& field = schema.field(multi ? "stamps" : "stamp");
    field.type = api::FieldDef::FieldClass::DATE;
    field.unit = api::FieldDef::DateUnit::NS;
    field.multi = multi ? api::FieldDef::MultiMode::DEFAULT : api::FieldDef::MultiMode::SINGLE;
  }
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "a", "stamp", -1, "when_dt", -1,
              "stamps", std::vector<int64_t>{-1, 1000, 1000000}),
      flatdoc("id", "b")}, UpdateMessage::COMMIT).success);
  for (int mode = 0; mode < 4; mode++) for (auto format : {"rows", "columns", "adaptive"}) {
    for (bool binary : {false, true}) {
      SCOPED_TRACE(fmt::format("{} {} {}", mode, format, binary));
      auto req = runFlags(helper, fmt::format(R"json({{"flags":["{}"],"query":"*:*",
        "fields":["id","stamp","stamps","when_dt"],"sort":"id asc","batch_size":2,
        "document_format":"{}"}})json", RequestFlags::DATE_NAMES[mode], format), binary);
      ASSERT_OK(req);
      auto docs = req->getDocs();
      ASSERT_EQ(2u, docs.size());
      EXPECT_CONTAINS_DOC(docs, flatdoc("id", "b"));
      if (mode == 1) {
        EXPECT_CONTAINS_DOC(docs, flatdoc("id", "a", "stamp", "1969-12-31T23:59:59.999999999Z",
            "when_dt", "1969-12-31T23:59:59.999Z", "stamps", vecs(
                "1969-12-31T23:59:59.999999999Z", "1970-01-01T00:00:00.000001Z", "1970-01-01T00:00:00.001Z")));
      } else {
        EXPECT_CONTAINS_DOC(docs, flatdoc("id", "a", "stamp", -1,
            "when_dt", mode == 3 ? -1000000 : -1,
            "stamps", mode == 2 ? std::vector<int64_t>{-1, 0, 1} : std::vector<int64_t>{-1, 1000, 1000000}));
      }
      // Both transports receive the same concrete string/numeric columns.
      for (auto* response : req->responses) {
        std::vector<std::byte> bytes;
        ASSERT_TRUE(api::encode(response->proto, bytes));
        api::SearchResponse decoded;
        ASSERT_TRUE(api::decode(decoded, api::copyToPaddedInput(bytes, req->mr), req->mr));
        EXPECT_EQ(renderSearchResponseBody(response->proto), renderSearchResponseBody(decoded));
      }
    }
  }
  auto stream = runFlags(helper, R"json({"flags":["date_iso"],"query":"*:*",
    "fields":["id","stamp"],"sort":"id asc","batch_size":1})json");
  ASSERT_OK(stream);
  ASSERT_EQ(2u, stream->responses.size());
  EXPECT_NE(std::string::npos, renderSearchResponseBody(stream->responses[0]->proto).find(
      "1969-12-31T23:59:59.999999999Z"));
  EXPECT_NE(std::string::npos, renderSearchResponseBody(stream->responses[1]->proto).find(
      R"json("id":"b")json"));
}

TEST_F(RequestFlagsTest, dateOutputUsesEachEmittingOpsEffectiveMode) {
  CollectionHelper helper;
  indexDates(helper);
  auto req = runFlags(helper, R"json({"flags":["date_iso"],"ops":{
    "q":{"top_docs":{"query":"*:*","fields":["stamp"],"ops":{
      "ms":{"flags":["date_ms"],"top_docs":{"fields":["stamp"],"ops":{
        "native":{"flags":["date_native"],"expr_op":"min(stamp)"},
        "inherited":"min(stamp)"}}},
      "iso":"min(stamp)"}}},
    "fusion":{"flags":["date_ns"],"fusion":{"fields":["when_dt"],"rrf":{},
      "sources":{"all":{"query":"*:*","limit":2}}}}}})json");
  ASSERT_OK(req);
  const auto& list = *req->docList("q");
  EXPECT_EQ("1970-01-01T00:00:00.002Z", std::get<std::string>(*find(req->getDocs("q")[0], "stamp")));
  const auto& child = *list.ops.at("ms")->docList();
  EXPECT_EQ(2, std::get<api::ColInt>(child.columns.at("stamp").kind).v[0]);
  EXPECT_EQ(2000000, child.ops.at("native")->asInt());
  EXPECT_EQ(2, child.ops.at("inherited")->asInt());
  EXPECT_EQ("1970-01-01T00:00:00.002Z", list.ops.at("iso")->asString());
  EXPECT_EQ(2000000, std::get<api::ColInt>(req->docList("fusion")->columns.at("when_dt").kind).v[0]);
}

TEST_F(RequestFlagsTest, mixedDateExpressionsConvertBeforeSortingAndRendering) {
  CollectionHelper helper;
  SchemaBuilder schema;
  for (auto name : {"stamp", "absent"}) {
    auto& field = schema.field(name);
    field.type = api::FieldDef::FieldClass::DATE;
    field.unit = api::FieldDef::DateUnit::NS;
  }
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "stamp", 4000001, "when_dt", 3),
      flatdoc("id", "b", "stamp", 5000001, "when_dt", 5)}, UpdateMessage::COMMIT).success);
  for (int mode = 0; mode < 4; mode++) {
    auto req = runFlags(helper, fmt::format(R"json({{"flags":["{}"],"ops":{{
      "q":{{"top_docs":{{"fields":["id"],"sort":"stamp-when_dt asc"}}}},
      "date":"max(min(stamp),min(when_dt))",
      "fallback":"min(def(absent,when_dt))",
      "double":"max(avg(stamp),avg(when_dt))"}}}})json", RequestFlags::DATE_NAMES[mode]));
    ASSERT_OK(req);
    EXPECT_EQ("b", std::get<std::string>(*find(req->getDocs("q")[0], "id")));
    const auto& ops = req->responses.back()->proto.ops;
    if (mode == 1) {
      EXPECT_EQ("1970-01-01T00:00:00.004000001Z", ops.at("date")->asString());
      EXPECT_EQ("1970-01-01T00:00:00.003Z", ops.at("fallback")->asString());
      EXPECT_EQ("1970-01-01T00:00:00.004500001Z", ops.at("double")->asString());
    } else {
      EXPECT_EQ(mode == 2 ? 4 : 4000001, ops.at("date")->asInt());
      EXPECT_EQ(mode == 2 ? 3 : 3000000, ops.at("fallback")->asInt());
      EXPECT_DOUBLE_EQ(mode == 2 ? 4.500001 : 4500001, ops.at("double")->asDouble());
    }
  }
  ASSERT_TRUE(helper.index(flatdoc("id", "early", "stamp", 0, "when_dt", "1500-01-01"),
      UpdateMessage::COMMIT).success);
  auto overflow = runFlags(helper, R"json({"query":"id:early","fields":["id"],
      "sort":"stamp-when_dt asc"})json");
  EXPECT_FALSE(overflow->ok());
  EXPECT_NE(std::string::npos, overflow->errorMsg().find("mul()")) << overflow->errorMsg();
  EXPECT_NE(std::string::npos, overflow->errorMsg().find("int64")) << overflow->errorMsg();
}

TEST_F(RequestFlagsTest, dateFacetIdsRenderWithoutChangingBucketIdentity) {
  CollectionHelper helper;
  indexDates(helper);
  for (auto mode : {"date_native", "date_iso", "date_ms", "date_ns"}) {
    auto req = runFlags(helper, fmt::format(R"json({{"flags":["{}"],"ops":{{
      "values":{{"field_facet":{{"field":"stamp","ops":{{"earliest":"min(stamp)"}}}}}},
      "grid":{{"range_facet":{{"field":"stamp","start":"1970-01-01T00:00:00.002Z",
        "end":"1970-01-01T00:00:00.004Z","gap":{},
        "ops":{{"mid":"avg(stamp)"}}}}}}
      }}}})json", mode, std::string_view(mode) == "date_ms" ? 1 : 1000000));
    ASSERT_OK(req);
    const auto& values = *req->responses.back()->proto.ops.at("values")->facetResult();
    const auto& grid = *req->responses.back()->proto.ops.at("grid")->facetResult();
    EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector<int64_t>(values.counts.begin(), values.counts.end()));
    EXPECT_EQ((std::vector<int64_t>{1, 1}), std::vector<int64_t>(grid.counts.begin(), grid.counts.end()));
    if (std::string_view(mode) == "date_iso") {
      EXPECT_EQ("1970-01-01T00:00:00.002Z", std::get<api::ColStr>(values.bucket_ids->kind).v[0]);
      EXPECT_EQ("1970-01-01T00:00:00.002Z", std::get<api::ArrArrStr>(grid.bucket_ids->kind).v[0].v[0]);
      EXPECT_NE(std::string::npos, renderSearchResponseBody(req->responses.back()->proto).find(
          R"("val":["1970-01-01T00:00:00.002Z","1970-01-01T00:00:00.003Z"])"));
    } else {
      int64_t first = std::string_view(mode) == "date_ms" ? 2 : 2000000;
      EXPECT_EQ(first, std::get<api::ColInt>(values.bucket_ids->kind).v[0]);
      EXPECT_EQ(first, std::get<api::ArrArrInt>(grid.bucket_ids->kind).v[0].v[0]);
    }
  }
  auto page = runFlags(helper, R"json({"flags":["date_ms"],"query":"*:*","limit":0,"ops":{
    "f":{"range_facet":{"field":"stamp","start":0,"end":4,"gap":1}}}})json");
  ASSERT_OK(page);
  const auto& facet = *page->docList()->ops.at("f")->facetResult();
  int64_t id = std::get<api::ArrArrInt>(facet.bucket_ids->kind).v[2].v[0];
  auto selected = runFlags(helper, fmt::format(R"json({{"flags":["date_ms"],"query":"*:*",
    "count_limit":-1,"fields":["id"],"ops":{{"f":{{"range_facet":{{"field":"stamp",
      "start":0,"end":4,"gap":1,"selected":[{}]}}}}}}}})json", id));
  ASSERT_OK(selected);
  EXPECT_EQ(1, selected->getMatchCount());
  EXPECT_CONTAINS_DOC(selected->getDocs(), flatdoc("id", "a"));
  const auto& again = *selected->docList()->ops.at("f")->facetResult();
  EXPECT_EQ(id, std::get<api::ArrArrInt>(again.bucket_ids->kind).v[2].v[0]);
}

TEST_F(RequestFlagsTest, dateAggregateOutputAcrossExecutionBindings) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& field = schema.field("stamp");
  field.type = api::FieldDef::FieldClass::DATE;
  field.unit = api::FieldDef::DateUnit::NS;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "stamp", -2, "when_dt", -2, "cat_s", "a"),
      flatdoc("id", "b", "stamp", -1, "when_dt", -1, "cat_s", "a")}, UpdateMessage::COMMIT).success);
  auto guard = SearchOverridesGuard(forcedFacetFeedStrategy, disableAggregateSharingForTests,
                                    disableDenseFacetStateForTests);
  for (auto feed : {FacetFeedStrategy::BUCKET_DOMAINS, FacetFeedStrategy::STRING_COLUMN_REPLAY}) {
    forcedFacetFeedStrategy = feed;
    for (bool separate : {false, true}) for (int mode = 0; mode < 4; mode++) {
      disableAggregateSharingForTests = separate;
      disableDenseFacetStateForTests = separate;
      auto req = runFlags(helper, fmt::format(R"json({{"flags":["{}"],"ops":{{
        "lo":"min(stamp)","hi":"max(stamp)","mid":"avg(stamp)","pct":"percentile(stamp,50)",
        "expr":"avg(stamp)+1","duration":"max(stamp)-min(stamp)",
        "ms":"avg(when_dt)","f":{{"field_facet":{{"field":"cat_s","ops":{{
          "lo":"min(stamp)","mid":"avg(stamp)","pct":"percentile(stamp,50)"}}}}}}
        }}}})json", RequestFlags::DATE_NAMES[mode]));
      ASSERT_OK(req);
      const auto& ops = req->responses.back()->proto.ops;
      const auto& facet = *ops.at("f")->facetResult();
      const auto& lo = std::get<api::ArrVal>(facet.ops.at("lo")->kind).v[0];
      const auto& mid = std::get<api::ArrVal>(facet.ops.at("mid")->kind).v[0];
      const auto& pct = std::get<api::ArrVal>(facet.ops.at("pct")->kind).v[0];
      EXPECT_EQ(1, ops.at("duration")->asInt());
      if (mode == 1) {
        EXPECT_EQ("1969-12-31T23:59:59.999999998Z", ops.at("lo")->asString());
        EXPECT_EQ("1969-12-31T23:59:59.999999999Z", ops.at("hi")->asString());
        for (auto* value : {ops.at("mid").pointer(), ops.at("pct").pointer(), &lo, &mid, &pct}) {
          EXPECT_EQ("1969-12-31T23:59:59.999999998Z", value->asString());
        }
        EXPECT_EQ("1969-12-31T23:59:59.999999999Z", ops.at("expr")->asString());
        EXPECT_EQ("1969-12-31T23:59:59.998Z", ops.at("ms")->asString());
      } else {
        double scale = mode == 2 ? 0.000001 : 1;
        EXPECT_EQ(mode == 2 ? -1 : -2, ops.at("lo")->asInt());
        EXPECT_EQ(mode == 2 ? -1 : -2, lo.asInt());
        for (auto* value : {ops.at("mid").pointer(), ops.at("pct").pointer(), &mid, &pct}) {
          EXPECT_DOUBLE_EQ(-1.5 * scale, value->asDouble());
        }
        EXPECT_DOUBLE_EQ(-0.5 * scale, ops.at("expr")->asDouble());
        EXPECT_DOUBLE_EQ(mode == 3 ? -1500000 : -1.5, ops.at("ms")->asDouble());
      }
    }
  }
}

TEST_F(RequestFlagsTest, dateCollapseKeysAndRanksKeepNativeOrdering) {
  CollectionHelper helper;
  indexDates(helper);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a2", "stamp", 2000000),
      flatdoc("id", "b2", "stamp", 3000000)}, UpdateMessage::COMMIT).success);
  for (bool metrics : {false, true}) for (auto sort : {"stamp asc", "stamp+1 asc"}) {
    auto req = runFlags(helper, fmt::format(R"json({{"flags":["date_iso"],"query":"*:*",
      "fields":["id"],"document_format":"rows","get_scores":true,"sort":"{}",
      "collapse":{{"field":["stamp"],"head_sort":"id desc"{}}}}})json", sort,
      metrics ? R"json(,"sort":"mid asc","ops":{"mid":{"flags":["date_ms"],"expr_op":"avg(stamp)"}})json" : ""));
    ASSERT_OK(req);
    const auto& groups = *req->docList()->groups;
    ASSERT_EQ(2u, groups.keys.size());
    EXPECT_EQ("a2", std::get<std::string>(*find(req->getDocs()[0], "id")));
    EXPECT_EQ(api::GroupComponent_::Type::DATE, groups.keys[0].component[0].type);
    EXPECT_EQ("1970-01-01T00:00:00.002Z", std::get<std::string_view>(groups.keys[0].component[0].value));
    EXPECT_EQ(metrics || std::string_view(sort) == "stamp asc" ? "1970-01-01T00:00:00.002Z"
        : "1970-01-01T00:00:00.002000001Z", groups.ranks[0].v[0].asString());
    if (metrics) {
      EXPECT_DOUBLE_EQ(2, std::get<api::ArrVal>(groups.ops.at("mid")->kind).v[0].asDouble());
    }
  }
}

TEST_F(RequestFlagsTest, dateOutputRangeErrorsNameTheSource) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "when_dt", "1500-01-01"), UpdateMessage::COMMIT).success);
  for (auto op : {R"json({"top_docs":{"fields":["when_dt"]}})json",
                 R"json({"field_facet":{"field":"when_dt"}})json",
                 R"json({"range_facet":{"field":"when_dt","start":"1500-01-01","end":"1500-01-02","calendar_gap":{"n":1,"unit":"day"}}})json",
                 R"json({"expr_op":"min(when_dt)"})json", R"json({"expr_op":"avg(when_dt)"})json",
                 R"json({"top_docs":{"fields":["id"],"collapse":{"field":["when_dt"]}}})json"}) {
    auto req = runFlags(helper, fmt::format(R"json({{"flags":["date_ns"],"ops":{{"q":{}}}}})json", op));
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("when_dt")) << req->errorMsg();
    EXPECT_NE(std::string::npos, req->errorMsg().find("ns date range")) << req->errorMsg();
  }
  for (auto expression : {"avg(when_dt)+1e100", "avg(when_dt)+1e308+1e308"}) {
    auto req = runFlags(helper, fmt::format(R"json({{"flags":["date_iso"],"ops":{{"bad":"{}"}}}})json", expression));
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("when_dt")) << req->errorMsg();
    EXPECT_NE(std::string::npos, req->errorMsg().find("date_iso")) << req->errorMsg();
  }
}

TEST_F(RequestFlagsTest, lossyDateMsOutputDoesNotMergeSubMillisecondBucketsOrGroups) {
  CollectionHelper helper;
  indexDates(helper);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "c", "stamp", 2000001),
      flatdoc("id", "d", "stamp", 2000002)}, UpdateMessage::COMMIT).success);
  auto req = runFlags(helper, R"json({"flags":["date_ms"],"ops":{
    "f":{"range_facet":{"field":"stamp","start":"1970-01-01T00:00:00.002000001Z",
      "end":"1970-01-01T00:00:00.002000003Z","gap":1}},
    "v":{"field_facet":{"field":"stamp"}},
    "q":{"top_docs":{"fields":["id"],"sort":"stamp asc","count_limit":-1,
      "collapse":{"field":["stamp"]}}}}})json");
  ASSERT_OK(req);
  const auto& ops = req->responses.back()->proto.ops;
  const auto& facet = *ops.at("f")->facetResult();
  ASSERT_EQ(1u, facet.counts.size());
  EXPECT_EQ(2, facet.counts[0]);
  for (const auto& bounds : std::get<api::ArrArrInt>(facet.bucket_ids->kind).v) {
    EXPECT_EQ(2, bounds.v[0]);
    EXPECT_EQ(2, bounds.v[1]);
  }
  EXPECT_EQ(4u, ops.at("v")->facetResult()->counts.size());
  EXPECT_EQ(4, req->getMatchCount("q"));
}

TEST_F(RequestFlagsTest, dateIsoFormatsReservedFencesAndRejectsDoubleRoundingPastDomain) {
  CollectionHelper helper;
  SchemaBuilder schema;
  auto& field = schema.field("stamp");
  field.type = api::FieldDef::FieldClass::DATE;
  field.unit = api::FieldDef::DateUnit::NS;
  schema.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "last", "stamp", INT64_MAX - 1), UpdateMessage::COMMIT).success);
  auto req = runFlags(helper, R"json({"flags":["date_iso"],"ops":{
    "q":{"top_docs":{"fields":["stamp"]}},
    "f":{"range_facet":{"field":"stamp","start":9223372036854775806,
      "end":9223372036854775807,"gap":1}}}})json");
  ASSERT_OK(req);
  const auto& facet = *req->responses.back()->proto.ops.at("f")->facetResult();
  const auto& bounds = std::get<api::ArrArrStr>(facet.bucket_ids->kind).v[0].v;
  EXPECT_EQ("2262-04-11T23:47:16.854775806Z", bounds[0]);
  EXPECT_EQ("2262-04-11T23:47:16.854775807Z", bounds[1]);
  auto rounded = runFlags(helper, R"json({"flags":["date_iso"],"ops":{"mid":"avg(stamp)"}})json");
  EXPECT_FALSE(rounded->ok());
  EXPECT_NE(std::string::npos, rounded->errorMsg().find("stamp"));
  EXPECT_NE(std::string::npos, rounded->errorMsg().find("date_iso"));
}

TEST_F(RequestFlagsTest, numericDateGroupKeysNormalizeEachTupleComponent) {
  CollectionHelper helper;
  indexDates(helper);
  for (int mode : {0, 2, 3}) {
    auto req = runFlags(helper, fmt::format(R"json({{"flags":["{}"],"query":"*:*",
      "fields":["id"],"sort":"stamp asc","collapse":{{"field":["stamp","when_dt"]}}}})json",
      RequestFlags::DATE_NAMES[mode]));
    ASSERT_OK(req);
    const auto& keys = req->docList()->groups->keys;
    ASSERT_EQ(2u, keys.size());
    EXPECT_EQ(mode == 2 ? 2 : 2000000, std::get<int64_t>(keys[0].component[0].value));
    EXPECT_EQ(mode == 3 ? 2000000 : 2, std::get<int64_t>(keys[0].component[1].value));
  }
}

TEST_F(RequestFlagsTest, int64StringsLeaveBinaryResponseValuesNative) {
  CollectionHelper helper;
  indexDates(helper);
  for (bool binary : {false, true}) {
    auto req = runFlags(helper, R"json({"flags":["int64_str"],"query":"*:*",
      "fields":["stamp"],"ops":{"child":{"flags":["date_ms"],"expr_op":"min(stamp)"}}})json", binary);
    ASSERT_OK(req);
    EXPECT_EQ(RequestFlags::INT64_STR, req->flags);
    EXPECT_EQ(2000000, std::get<api::ColInt>(req->docList()->columns.at("stamp").kind).v[0]);
    EXPECT_EQ(2, req->docList()->ops.at("child")->asInt());
    std::vector<std::byte> bytes;
    ASSERT_TRUE(api::encode(req->responses[0]->proto, bytes));
    api::SearchResponse response;
    ASSERT_TRUE(api::decode(response, api::copyToPaddedInput(bytes, req->mr), req->mr));
    EXPECT_EQ(2000000, std::get<api::ColInt>(response.ops.at("q")->docList()->columns.at("stamp").kind).v[0]);
  }
}
