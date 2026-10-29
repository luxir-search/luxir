// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// BOOLEAN, IP and DECIMAL: logical types on the INT column and STRING terms.

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/util/IpAddress.h"

using namespace luxir;
using namespace luxir::test;
using FieldClass = api::FieldDef::FieldClass;

class ScalarFieldTypesTest : public LuxirTest {
protected:
  CollectionHelper helper{"main"};

  void SetUp() override {
    SchemaBuilder b;
    b.field("flag").type = FieldClass::BOOLEAN;
    b.field("addr").type = FieldClass::IP;
    auto& price = b.field("price");
    price.type = FieldClass::DECIMAL;
    price.scale = 2;
    price.index = api::FieldDef::IndexMode::RANGE;
    auto& prices = b.field("prices");
    prices.type = FieldClass::DECIMAL;
    prices.scale = 2;
    prices.multi = api::FieldDef::MultiMode::ARRAY;
    b.set(helper.collection());

    ASSERT_TRUE(helper.indexAll(std::array{
      flatdoc("id", "a", "flag", true, "addr", "10.0.0.1", "price", 9.99),
      flatdoc("id", "b", "flag", false, "addr", "10.0.0.200", "price", "12.50"),
      flatdoc("id", "c", "flag", "true", "addr", "192.168.1.1", "price", (int64_t)100),
      flatdoc("id", "d", "addr", "2001:DB8:0::1", "price", 0.1),
      flatdoc("id", "e"),
    }, UpdateMessage::COMMIT).success);
  }

  LocalReqHandle run(std::string_view json) {
    auto req = localReq(helper.getSearchEngine());
    auto kept = api::build::arenaStr(req->mr, json);
    parseQueryRequest(kept, req->rawRequest(), req->mr);
    req->collection("main").execute(false);
    return req;
  }

  static std::vector<std::string> ids(const LocalReq& req) {
    EXPECT_TRUE(req.ok()) << req.errorMsg();
    std::vector<std::string> out;
    for (const auto& doc : req.getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    return out;
  }

  std::vector<std::string> hits(std::string_view query) {
    auto out = ids(*run(std::format(R"({{"query":{},"fields":["id"],"limit":-1}})", query)));
    std::sort(out.begin(), out.end());
    return out;
  }

  std::string json(std::string_view request) {
    auto req = run(request);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return renderSearchResponseBody(req->responses[0]->proto);
  }

  bool rejected(const Doc& doc) { return !helper.index(doc, UpdateMessage::COMMIT).success; }
};

TEST_F(ScalarFieldTypesTest, booleanQueryFacetAndRetrieval) {
  EXPECT_EQ(vecs("a", "c"), hits(R"({"match":{"flag":true}})"));
  EXPECT_EQ(vecs("b"), hits(R"({"match":{"flag":"false"}})"));
  EXPECT_EQ(vecs("a", "c"), hits(R"("flag:true")"));
  EXPECT_EQ(vecs("a", "b", "c"), hits(R"({"any_of":{"field":"flag","values":[true,false]}})"));

  auto req = run(R"({"sort":"id","fields":["id","flag"],"limit":-1})");
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "flag", true));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "b", "flag", false));
  auto body = json(R"({"query":{"match":{"id":"b"}},"fields":["flag"],"limit":-1,
    "ops":{"flags":{"field_facet":{"field":"flag"}}}})");
  EXPECT_NE(std::string::npos, body.find(R"("flag":false)")) << body;
  EXPECT_NE(std::string::npos, body.find(R"({"val":"false","count":1})")) << body;

  EXPECT_TRUE(rejected(flatdoc("id", "x", "flag", "yes")));
  EXPECT_TRUE(rejected(flatdoc("id", "x", "flag", (int64_t)1)));
}

// The true/false doc bitsets behind boolean predicates: both sentinels (no
// doc, every doc), multi-valued docs in both sets, range forms, conjunction
// and exclusion over them, and survival across commits, deletes and a merge.
TEST_F(ScalarFieldTypesTest, booleanBitsets) {
  helper.clear();
  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "1", "on_b", true, "tags_bs", vecs("true", "false")),
    flatdoc("id", "2", "on_b", "true", "tags_bs", vecs("false")),
    flatdoc("id", "3", "on_b", true),
  }, UpdateMessage::COMMIT).success);
  // Segment 1: every doc is true (dense sentinel), no doc is false (no bitset).
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto* info = reader->segments()[0].postingsReader().fieldInfo("on_b");
  ASSERT_NE(nullptr, info);
  EXPECT_NE(0, info->flags & FieldType::BOOL_BITSETS);
  EXPECT_EQ(3, info->trueDocs);
  EXPECT_EQ(0, info->falseDocs);
  EXPECT_EQ(vecs("1", "2", "3"), hits(R"({"match":{"on_b":true}})"));
  EXPECT_EQ(vecs(), hits(R"({"match":{"on_b":false}})"));
  EXPECT_EQ(vecs("1"), hits(R"({"match":{"tags_bs":true}})"));
  EXPECT_EQ(vecs("1", "2"), hits(R"({"match":{"tags_bs":false}})"));
  EXPECT_EQ(vecs("1", "2"), hits(R"({"any_of":{"field":"tags_bs","values":["false",true]}})"));
  EXPECT_EQ(vecs("1", "2", "3"), hits(R"({"range":{"field":"on_b","gte":false}})"));
  EXPECT_EQ(vecs("1", "2", "3"), hits(R"({"range":{"field":"on_b","gt":false}})"));
  EXPECT_EQ(vecs(), hits(R"({"range":{"field":"on_b","lt":true}})"));
  EXPECT_EQ(vecs("1", "2"), hits(R"("on_b:true AND tags_bs:false")"));

  ASSERT_TRUE(helper.indexAll(std::array{
    flatdoc("id", "4", "on_b", false),
    flatdoc("id", "5", "on_b", true),
    flatdoc("id", "6"),
  }, UpdateMessage::COMMIT).success);
  EXPECT_EQ(vecs("4"), hits(R"({"match":{"on_b":false}})"));
  EXPECT_EQ(vecs("1", "2", "3", "5"), hits(R"({"match":{"on_b":true}})"));
  EXPECT_EQ(vecs("4", "6"), hits(R"("NOT on_b:true")"));
  EXPECT_EQ(vecs("1", "2", "3", "4", "5"), hits(R"({"exists":{"field":"on_b"}})"));

  ASSERT_TRUE(helper.deleteByIds({"2", "5"}, UpdateMessage::COMMIT).success);
  helper.getIndexWriter()->mergeSegments();
  reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  info = reader->segments()[0].postingsReader().fieldInfo("on_b");
  ASSERT_NE(nullptr, info);
  EXPECT_EQ(2, info->trueDocs);
  EXPECT_EQ(1, info->falseDocs);
  EXPECT_EQ(vecs("1", "3"), hits(R"({"match":{"on_b":true}})"));
  EXPECT_EQ(vecs("4"), hits(R"({"match":{"on_b":false}})"));
  EXPECT_EQ(vecs("1"), hits(R"({"match":{"tags_bs":true}})"));
  EXPECT_EQ(vecs("1"), hits(R"({"match":{"tags_bs":false}})"));
}

TEST_F(ScalarFieldTypesTest, ipExactCidrRangeSortAndFacets) {
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"addr":"10.0.0.1"}})"));
  EXPECT_EQ(vecs("a"), hits(R"({"match":{"addr":"::ffff:10.0.0.1"}})"));
  EXPECT_EQ(vecs("d"), hits(R"({"match":{"addr":"2001:db8::1"}})"));
  EXPECT_EQ(vecs("a", "b"), hits(R"({"match":{"addr":"10.0.0.0/24"}})"));
  EXPECT_EQ(vecs("a", "b", "c"), hits(R"({"match":{"addr":"0.0.0.0/0"}})"));
  EXPECT_EQ(vecs("d"), hits(R"({"match":{"addr":"2001:db8::/32"}})"));
  EXPECT_EQ(vecs("b", "c"), hits(R"({"range":{"field":"addr","gt":"10.0.0.1","lte":"192.168.1.1"}})"));
  EXPECT_EQ(vecs("c", "d"), hits(R"({"any_of":{"field":"addr","values":["192.168.1.1","2001:db8::1"]}})"));
  EXPECT_EQ(vecs("a", "b"), hits(R"("addr:\"10.0.0.0/8\"")"));
  EXPECT_EQ(vecs("a"), hits(R"({"simple_query":{"q":"addr:10.0.0.1","fields":["id"]}})"));

  // Address order: IPv4 (mapped into ::ffff:0:0/96) before 2001:db8::/32.
  auto req = run(R"({"sort":"addr","fields":["id","addr"],"limit":-1})");
  EXPECT_EQ(vecs("a", "b", "c", "d", "e"), ids(*req));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "b", "addr", "10.0.0.200"));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "d", "addr", "2001:db8::1"));
  auto body = json(R"({"limit":0,"ops":{"addrs":{"field_facet":{"field":"addr","limit":1,
    "selected":["2001:db8::1"]}}}})");
  EXPECT_NE(std::string::npos, body.find(R"({"val":"2001:db8::1","count":1})")) << body;

  EXPECT_FALSE(run(R"({"query":{"prefix":{"field":"addr","prefix":"10."}}})")->ok());
  EXPECT_FALSE(run(R"({"query":{"any_of":{"field":"addr","values":["10.0.0.0/8"]}}})")->ok());
  EXPECT_TRUE(rejected(flatdoc("id", "x", "addr", "10.0.0.256")));
  EXPECT_TRUE(rejected(flatdoc("id", "x", "addr", (int64_t)7)));
}

TEST_F(ScalarFieldTypesTest, ipCidrBounds) {
  auto block = ip::parseCidr("10.1.2.3/15");
  ASSERT_TRUE(block.has_value());
  char text[ip::TEXT_BUF_SIZE];
  EXPECT_EQ("10.0.0.0", ip::format(ip::bytes(block->lo), text));
  EXPECT_EQ("10.1.255.255", ip::format(ip::bytes(block->hi), text));
  block = ip::parseCidr("fe80::1/10");
  ASSERT_TRUE(block.has_value());
  EXPECT_EQ("fe80::", ip::format(ip::bytes(block->lo), text));
  EXPECT_EQ("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff", ip::format(ip::bytes(block->hi), text));
  EXPECT_FALSE(ip::parseCidr("10.0.0.0/33").has_value());
  EXPECT_FALSE(ip::parseCidr("10.0.0.0").has_value());
  EXPECT_FALSE(ip::parse("fe80::1%eth0").has_value());
}

TEST_F(ScalarFieldTypesTest, decimalRangeSortFacetsAndStats) {
  EXPECT_EQ(vecs("a", "b"), hits(R"({"range":{"field":"price","gte":9.99,"lt":100}})"));
  EXPECT_EQ(vecs("b"), hits(R"({"match":{"price":12.5}})"));
  EXPECT_EQ(vecs("d"), hits(R"({"match":{"price":"0.10"}})"));
  EXPECT_EQ(vecs("c"), hits(R"("price:100")"));

  auto req = run(R"JSON({"sort":"price","fields":["id","price"],"limit":-1,
    "ops":{"total":"sum(price)","top":"max(price)"}})JSON");
  EXPECT_EQ(vecs("d", "a", "b", "c", "e"), ids(*req));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "price", 9.99));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "d", "price", 0.1));
  EXPECT_EQ(122.59, req->docList()->ops.at("total")->asDouble());
  EXPECT_EQ(100.0, req->docList()->ops.at("top")->asDouble());

  auto body = json(R"({"limit":0,"ops":{
    "ranges":{"range_facet":{"field":"price","start":0,"end":100,"gap":"12.5"}},
    "prices":{"field_facet":{"field":"price","limit":1}}}})");
  EXPECT_NE(std::string::npos, body.find(R"({"val":[0,12.5],"count":2})")) << body;
  EXPECT_NE(std::string::npos, body.find(R"({"val":[12.5,25],"count":1})")) << body;
  EXPECT_NE(std::string::npos, body.find(R"({"val":0.1,"count":1})")) << body;

  EXPECT_TRUE(rejected(flatdoc("id", "x", "price", 1.234)));
  EXPECT_TRUE(rejected(flatdoc("id", "x", "price", "1e-3")));
  EXPECT_TRUE(rejected(flatdoc("id", "x", "price", "abc")));
  EXPECT_TRUE(rejected(flatdoc("id", "x", "price", (int64_t)std::numeric_limits<int64_t>::max())));
}

// Summing 0.1 ten times in doubles gives 0.9999999999999999; decimal
// aggregates add the scaled integers and divide once.
TEST_F(ScalarFieldTypesTest, decimalAggregatesAreExact) {
  std::vector<Doc> docs;
  for (int i = 0; i < 9; i++) {
    docs.push_back(flatdoc("id", std::format("t{}", i), "price", "0.10",
                           "prices", vecs("0.10", "0.20")));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto req = run(R"JSON({"query":{"match":{"price":0.1}},"limit":0,"ops":{
    "total":"sum(price)","mean":"avg(price)","low":"min(price)",
    "tops":"sum(max(prices))","pair":"sum(max(price, price))"}})JSON");
  ASSERT_OK(req);
  const auto& ops = req->docList()->ops;
  EXPECT_EQ(1.0, ops.at("total")->asDouble());
  EXPECT_EQ(0.1, ops.at("mean")->asDouble());
  EXPECT_EQ(0.1, ops.at("low")->asDouble());
  EXPECT_EQ(1.8, ops.at("tops")->asDouble());
  EXPECT_EQ(1.0, ops.at("pair")->asDouble());
}

TEST_F(ScalarFieldTypesTest, scaledDecimalParse) {
  const char* error = nullptr;
  EXPECT_EQ(125, coerce::parseScaledDecimal("1.25", 2, error));
  EXPECT_EQ(-1200, coerce::parseScaledDecimal("-12", 2, error));
  EXPECT_EQ(1500, coerce::parseScaledDecimal("1.5e1", 2, error));
  EXPECT_EQ(1, coerce::parseScaledDecimal("1e-2", 2, error));
  EXPECT_EQ(10, coerce::parseScaledDecimal("0.1000", 2, error));
  EXPECT_EQ(std::numeric_limits<int64_t>::min(),
            coerce::parseScaledDecimal("-9223372036854775808", 0, error));
  EXPECT_FALSE(coerce::parseScaledDecimal("9223372036854775808", 0, error).has_value());
  EXPECT_FALSE(coerce::parseScaledDecimal("0.125", 2, error).has_value());
  EXPECT_EQ(100, coerce::parseScaledDecimal("1.", 2, error));
  EXPECT_FALSE(coerce::parseScaledDecimal(".", 2, error).has_value());
  EXPECT_FALSE(coerce::parseScaledDecimal("1e", 2, error).has_value());
}

TEST_F(ScalarFieldTypesTest, schemaValidationAndTemplates) {
  auto invalid = [](auto configure) {
    SchemaBuilder b;
    configure(b);
    EXPECT_THROW(b.build(), SchemaError);
  };
  invalid([](SchemaBuilder& b) { b.field("p").type = FieldClass::DECIMAL; });
  invalid([](SchemaBuilder& b) {
    auto& f = b.field("p");
    f.type = FieldClass::DECIMAL;
    f.scale = 19;
  });
  invalid([](SchemaBuilder& b) {
    auto& f = b.field("p");
    f.type = FieldClass::INT;
    f.scale = 2;
  });
  invalid([](SchemaBuilder& b) {
    auto& f = b.field("p");
    f.type = FieldClass::IP;
    f.stored = true;
  });

  helper.clear();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "ok_b", true, "src_ip", "192.0.2.7",
                                   "hops_ips", vecs("192.0.2.1", "::1")),
                           UpdateMessage::COMMIT).success);
  // Multi-valued terms come back in address order.
  EXPECT_EQ(vecs("a"), hits(R"("ok_b:true AND src_ip:\"192.0.2.0/24\" AND hops_ips:\"::1\"")"));
  auto req = run(R"({"fields":["id","src_ip","hops_ips"]})");
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "src_ip", "192.0.2.7",
                                              "hops_ips", vecs("::1", "192.0.2.1")));
}
