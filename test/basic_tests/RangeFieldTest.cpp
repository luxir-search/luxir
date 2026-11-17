// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/query/RangeFieldQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/schema/RangeBounds.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using FieldClass = api::FieldDef::FieldClass;
using IndexMode = api::FieldDef::IndexMode;

// 2024-01-01T00:00:00Z and 2024-02-01T00:00:00Z in epoch millis.
constexpr int64_t JAN_1 = 1704067200000;
constexpr int64_t FEB_1 = 1706745600000;

// Each doc carries the same interval in `bkd` (index=range) and `col`
// (column only), so every query checks both execution arms.
constexpr std::string_view DOCS[] = {
  R"({"id":"a","bkd":{"gte":10,"lte":20},"col":{"gte":10,"lte":20}})",
  R"({"id":"b","bkd":{"gte":15,"lte":30},"col":{"gte":15,"lte":30}})",
  R"({"id":"c","bkd":{"gte":25,"lte":40},"col":{"gte":25,"lte":40}})",
  R"({"id":"d","bkd":{"gte":50},"col":{"gte":50}})",
  R"({"id":"e","bkd":{"lte":5},"col":{"lte":5}})",
  R"({"id":"f","bkd":{"gt":20,"lt":25},"col":{"gt":20,"lt":25}})",
};

} // namespace

class RangeFieldTest : public LuxirTest {
public:
  CollectionHelper helper;
  SchemaBuilder schemaBuilder;

  void SetUp() override {
    auto& bkd = schemaBuilder.field("bkd");
    bkd.type = FieldClass::LONG_RANGE;
    bkd.index = IndexMode::RANGE;
    schemaBuilder.field("col").type = FieldClass::LONG_RANGE;
    schemaBuilder.field("price").type = FieldClass::DOUBLE_RANGE;
    auto& when = schemaBuilder.field("when");
    when.type = FieldClass::DATE_RANGE;
    when.index = IndexMode::RANGE;
    schemaBuilder.set(helper.collection());
  }

  IndexResult indexJson(std::initializer_list<std::string_view> docs, bool commit = true) {
    CollectionHelper::UpdateBuilder builder;
    for (std::string_view json : docs) {
      builder.addRaw([json](api::Map& doc, std::pmr::memory_resource& mr) {
        std::string error;
        if (!api::read_json(doc, api::build::arenaStr(mr, json), mr, &error)) {
          throw std::runtime_error("bad test doc: " + error);
        }
      });
    }
    if (commit) builder.commit();
    return helper.submit(builder);
  }

  void indexAllDocs() {
    for (std::string_view json : DOCS) ASSERT_TRUE(indexJson({json}).success);
  }

  std::vector<std::string> ids(std::string_view queryJson) {
    auto req = localReq(helper.getSearchEngine());
    auto& cursor = req->collection("main").topDocs("q");
    std::string error;
    EXPECT_TRUE(api::read_json(cursor.rawQuery(), queryJson, cursor.mr(), &error)) << error;
    cursor.fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> out;
    for (const Doc& doc : req->getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(out.begin(), out.end());
    return out;
  }

  auto request(std::string_view json) {
    auto req = localReq(helper.getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->execute();
    return req;
  }

  // The same range query over both fields; the results must agree.
  std::vector<std::string> both(std::string_view bounds, std::string_view relation = "intersects") {
    auto query = [&](std::string_view field) {
      return std::string(R"({"range":{"field":")") + std::string(field) + "\"," + std::string(bounds)
          + R"(,"relation":")" + std::string(relation) + "\"}}";
    };
    auto fromBkd = ids(query("bkd"));
    EXPECT_EQ(fromBkd, ids(query("col"))) << bounds << " " << relation;
    return fromBkd;
  }
};

using Ids = std::vector<std::string>;

TEST_F(RangeFieldTest, booleanClauses) {
  indexAllDocs();
  for (std::string_view field : {"bkd", "col"}) {
    for (std::string_view role : {"required", "filter"}) {
      SCOPED_TRACE(std::string(field) + " " + std::string(role));
      std::string query = std::string(R"({"boolean":{")") + std::string(role)
          + R"(":[{"range":{"field":")" + std::string(field)
          + R"(","gte":18,"lte":22}}],"prohibited":[{"match":{"field":"id","val":"b"}}]}})";
      EXPECT_EQ((Ids{"a", "f"}), ids(query));
    }
  }
}

TEST_F(RangeFieldTest, relations) {
  indexAllDocs();
  SkipStatsScope stats;
  // Stored: a [10,20] b [15,30] c [25,40] d [50,) e (,5] f [21,24].
  EXPECT_EQ((Ids{"a", "b", "f"}), both(R"("gte":18,"lte":22)"));
  EXPECT_EQ((Ids{"a", "b"}), both(R"("gte":16,"lte":19)", "contains"));
  EXPECT_EQ((Ids{"b", "f"}), both(R"("gte":12,"lte":35)", "within"));
  // Bounds touching an endpoint count as overlapping.
  EXPECT_EQ((Ids{"c"}), both(R"("gte":40,"lte":45)"));
  EXPECT_EQ((Ids{"b", "f"}), both(R"("gt":20,"lte":22)"));
  EXPECT_EQ((Ids{"a"}), both(R"("gte":10,"lte":20)", "within"));
  EXPECT_TRUE(both(R"("gte":30,"lte":20)").empty());
  EXPECT_GT(SkipStats::rangeFieldBKDArms, 0);

  // Match and the expr language are intersects queries.
  EXPECT_EQ((Ids{"a", "b"}), ids(R"({"match":{"field":"bkd","val":17}})"));
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").exprQuery("col:[18 TO 22]").fields({"id"}).limit(-1);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(3u, req->getDocs().size());
}

TEST_F(RangeFieldTest, openEnds) {
  indexAllDocs();
  EXPECT_EQ((Ids{"d"}), both(R"("gte":45)"));
  EXPECT_EQ((Ids{"e"}), both(R"("lte":0)"));
  // Open stored ends are unbounded: d contains every value above 50.
  EXPECT_EQ((Ids{"d"}), both(R"("gte":1000000,"lte":2000000)", "contains"));
  EXPECT_EQ((Ids{"a", "b", "c", "d", "f"}), both(R"("gte":0)", "within"));
  EXPECT_EQ((Ids{"a", "b", "c", "d", "e", "f"}), ids(R"({"exists":{"field":"bkd"}})"));
}

TEST_F(RangeFieldTest, retrieval) {
  ASSERT_TRUE(indexJson({
    R"({"id":"a","bkd":{"gte":10,"lt":21},"price":{"gte":1.5,"lte":2.5},"when":{"gte":"2024-01-01","lt":"2024-02-01"}})",
    R"({"id":"b","bkd":{"gte":50},"when":{"gte":"2024-01-01","lte":"2024-01"}})",
  }).success);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "bkd", "price", "when"})
      .documentFormat(api::DocFormat::ROWS).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* docs = req->docList();
  ASSERT_NE(nullptr, docs);
  ASSERT_EQ(2u, docs->docs.size());
  auto bound = [](const api::Map& row, std::string_view field, std::string_view key) -> const api::Val* {
    const auto* value = row.fields.find(field);
    if (value == nullptr) return nullptr;
    const auto* map = std::get_if<api::Map>(&(**value).kind);
    if (map == nullptr) return nullptr;
    const auto* found = map->fields.find(key);
    return found ? &**found : nullptr;
  };
  // Rows follow index order for an all query over one shard.
  const api::Map& a = docs->docs[0];
  const api::Map& b = docs->docs[1];
  EXPECT_EQ(10, bound(a, "bkd", "gte")->asInt());
  EXPECT_EQ(20, bound(a, "bkd", "lte")->asInt());
  EXPECT_DOUBLE_EQ(1.5, bound(a, "price", "gte")->asDouble());
  EXPECT_DOUBLE_EQ(2.5, bound(a, "price", "lte")->asDouble());
  EXPECT_EQ(JAN_1, bound(a, "when", "gte")->asInt());
  EXPECT_EQ(FEB_1 - 1, bound(a, "when", "lte")->asInt());
  // An open end is omitted; a partial date names its whole window.
  EXPECT_EQ(50, bound(b, "bkd", "gte")->asInt());
  EXPECT_EQ(nullptr, bound(b, "bkd", "lte"));
  EXPECT_EQ(nullptr, b.fields.find("price"));
  EXPECT_EQ(FEB_1 - 1, bound(b, "when", "lte")->asInt());

  // Date query bounds round the same way.
  EXPECT_EQ((Ids{"a", "b"}), ids(R"({"range":{"field":"when","gte":"2024-01","lte":"2024-01","relation":"within"}})"));
  EXPECT_EQ((Ids{"a"}), ids(R"({"range":{"field":"price","gte":2.0,"lte":2.0,"relation":"intersects"}})"));
}

TEST_F(RangeFieldTest, mergeAndDeletes) {
  indexAllDocs();  // one segment per doc
  ASSERT_TRUE(helper.deleteById("b", UpdateMessage::COMMIT).success);
  EXPECT_EQ((Ids{"a", "f"}), both(R"("gte":18,"lte":22)"));

  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  EXPECT_EQ(1u, helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
  EXPECT_EQ((Ids{"a", "f"}), both(R"("gte":18,"lte":22)"));
  EXPECT_EQ((Ids{"f"}), both(R"("gte":12,"lte":35)", "within"));
  EXPECT_EQ((Ids{"d"}), both(R"("gte":45)"));
  EXPECT_EQ((Ids{"e"}), both(R"("lte":0)"));
}

TEST_F(RangeFieldTest, invalidValuesAndSchemas) {
  auto result = indexJson({
    R"({"id":"empty","bkd":{"gte":5,"lte":1}})",
    R"({"id":"scalar","bkd":5})",
    R"({"id":"key","bkd":{"from":1}})",
    R"({"id":"both","bkd":{"gte":1,"gt":2}})",
    R"({"id":"ok","bkd":{"gte":1,"lte":2}})",
  });
  EXPECT_EQ(4, result.total_errors);
  EXPECT_EQ((Ids{"ok"}), ids(R"({"all":true})"));

  // relation only applies to range fields.
  auto req = localReq(helper.getSearchEngine());
  auto& cursor = req->collection("main").topDocs("q");
  ASSERT_TRUE(api::read_json(cursor.rawQuery(),
                             R"({"range":{"field":"id","gte":"a","relation":"within"}})", cursor.mr()));
  req->execute();
  EXPECT_FALSE(req->ok());

  SchemaBuilder multi;
  auto& field = multi.field("spans");
  field.type = FieldClass::LONG_RANGE;
  field.multi = api::FieldDef::MultiMode::DEFAULT;
  EXPECT_ANY_THROW(multi.set(helper.collection()));
  SchemaBuilder matched;
  auto& other = matched.field("spans");
  other.type = FieldClass::DATE_RANGE;
  other.index = IndexMode::MATCH;
  EXPECT_ANY_THROW(matched.set(helper.collection()));
}

TEST_F(RangeFieldTest, pairFlagsCountShortcutAndSparseVerification) {
  ASSERT_TRUE(indexJson({DOCS[0], DOCS[1], DOCS[2], DOCS[3], DOCS[4], DOCS[5]}).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  auto* info = segment.postingsReader().fieldInfo("bkd");
  ASSERT_NE(nullptr, info);
  EXPECT_FALSE(helper.collection().getSchema()->getFieldTypePtr("bkd")->multiValued());
  EXPECT_TRUE(info->flags & FieldType::MULTI_VALUED);
  EXPECT_TRUE(info->flags & FieldType::MULTI_ARRAY);
  IntColReader column(segment.postingsReader(), *info);
  EXPECT_TRUE(column.multiValued());
  EXPECT_FALSE(column.multiSet());
  EXPECT_TRUE(RangePointSource::onePointPerDoc(*info));
  EXPECT_TRUE(RangePointSource::onePointPerDoc(column));

  MemPool pool;
  Query::Context context(pool, *reader);
  RangeFieldQuery query("bkd", RangeFieldQuery::Relation::INTERSECTS, 18, 22);
  auto* weight = query.createWeight(context, 0);
  EXPECT_EQ(Query::CountHow::INDEX_WALK, weight->countShortcut(segment, nullptr).how);
  EXPECT_EQ(3, countForTests(weight, segment));
  auto* supplier = weight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);
  auto* plan = resolveScorerPlanForTests(pool, *supplier, 0);
  EXPECT_EQ(Query::ReportedTwoPhase::YES, plan->shape().reportedTwoPhase);
  EXPECT_FALSE(plan->shape().verificationPoints.has_value());
  SkipStatsScope stats;
  auto* scorer = plan->build(pool);
  std::vector<int32_t> docs;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) docs.push_back(doc);
  EXPECT_EQ((std::vector<int32_t>{0, 1, 5}), docs);
  EXPECT_EQ(1, SkipStats::rangeFieldSparseVerifyArms);
}

TEST_F(RangeFieldTest, mergeIndexedAndColumnOnlyInputs) {
  ASSERT_TRUE(indexJson({DOCS[0]}).success);
  SchemaBuilder changed;
  auto& field = changed.field("bkd");
  field.type = FieldClass::LONG_RANGE;
  field.index = IndexMode::NONE;
  changed.set(helper.collection());
  ASSERT_TRUE(indexJson({DOCS[1], DOCS[2]}).success);
  auto writer = helper.getIndexWriter();
  auto before = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, before->segments().size());
  EXPECT_GT(before->field("bkd").at(0)->pointsMetaOff, 0);
  EXPECT_EQ(0, before->field("bkd").at(1)->pointsMetaOff);
  EXPECT_EQ((Ids{"a", "b"}), both(R"("gte":18,"lte":22)"));
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  auto merged = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, merged->segments().size());
  EXPECT_GT(merged->field("bkd").at(0)->pointsMetaOff, 0);
  EXPECT_EQ((Ids{"a", "b"}), both(R"("gte":18,"lte":22)"));
  EXPECT_EQ((Ids{"b"}), both(R"("gte":12,"lte":35)", "within"));
}

TEST_F(RangeFieldTest, elementTypeFlagsAndSignatures) {
  constexpr uint32_t dateKind = 2;
  uint32_t nanos = dateKind | (1u << RangeFieldType::ELEMENT_FLAGS_SHIFT);
  EXPECT_EQ(nanos, FieldType::typeParameters(FieldType::INTERVAL, nanos));
  EXPECT_EQ("type 'date_range'", FieldType::describeType(FieldType::INTERVAL, dateKind));
  EXPECT_EQ("type 'date_range' with element type 'date' unit 'ns'",
            FieldType::describeType(FieldType::INTERVAL, nanos));
  EXPECT_THROW(FieldType::typeParameters(FieldType::INTERVAL, RangeFieldType::ELEMENT_KIND_MASK), std::runtime_error);
  EXPECT_THROW(FieldType::typeParameters(FieldType::INTERVAL, 1u << RangeFieldType::ELEMENT_FLAGS_SHIFT), std::runtime_error);
  EXPECT_THROW(FieldType::typeParameters(FieldType::INTERVAL,
      dateKind | (2u << RangeFieldType::ELEMENT_FLAGS_SHIFT)), std::runtime_error);
  RangeFieldType date("when", FieldType::DATE);
  FieldType::SignatureProps props;
  date.signatureProps(props);
  EXPECT_EQ("\"date_range\"", props.at("type"));
  EXPECT_EQ("\"ms\"", props.at("unit"));
  EXPECT_EQ(dateKind, date.segmentTypeFlags());
  SchemaBuilder schema;
  auto& field = schema.field("spans");
  field.type = FieldClass::DATE_RANGE;
  field.unit = api::FieldDef::DateUnit::NS;
  EXPECT_ANY_THROW(schema.set(helper.collection()));
}

TEST_F(RangeFieldTest, sharedBoundFolding) {
  auto schema = helper.collection().getSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});
  auto zero = coerce::scalarVal((int64_t)0);
  auto max = coerce::scalarVal(INT64_MAX);
  auto bad = coerce::scalarVal(std::string_view("bad"));
  for (auto field : {"bkd", "number_i"}) {
    SCOPED_TRACE(field);
    // Empty lower bounds must not hide a malformed upper bound.
    try {
      builder.createRangeQuery(field, nullptr, &max, &bad, nullptr);
      FAIL() << "accepted a malformed upper bound";
    } catch (const RequestError& e) {
      EXPECT_EQ("invalid_value", e.code);
      EXPECT_NE(std::string::npos, std::string(e.what()).find(field));
      EXPECT_NE(std::string::npos, std::string(e.what()).find("bound 'lte'"));
    }
    for (bool lower : {true, false}) {
      try {
        builder.createRangeQuery(field, lower ? &zero : nullptr, lower ? &max : nullptr,
                                 lower ? nullptr : &zero, lower ? nullptr : &max);
        FAIL() << "accepted conflicting bounds";
      } catch (const RequestError& e) {
        EXPECT_EQ("invalid_value", e.code);
        EXPECT_EQ(fmt::format("Range on '{}' sets both {}", field,
                             lower ? "gte and gt" : "lte and lt"), e.what());
      }
    }
  }
  try {
    builder.createRangeQuery("location_p", &zero, nullptr, nullptr, nullptr);
    FAIL() << "accepted a range query on geo_point";
  } catch (const RequestError& e) {
    EXPECT_EQ("invalid_request", e.code);
    EXPECT_NE(std::string::npos, std::string(e.what()).find("not supported"));
  }
  ResolvedFieldHandle branch{};
  branch.logicalName = "logical";
  branch.physicalName = "logical__range";
  branch.fieldType = schema->getFieldTypePtr("bkd");
  try {
    builder.createRangeQuery(branch, &bad, nullptr, nullptr, nullptr);
    FAIL() << "accepted a malformed branch bound";
  } catch (const RequestError& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("'logical__range'"));
    EXPECT_EQ(std::string::npos, std::string(e.what()).find("'logical'"));
  }
}

TEST_F(RangeFieldTest, strictIngestAndPermissiveQueries) {
  auto rejected = indexJson({
    R"({"id":"fraction","bkd":{"gte":1.5}})",
    R"({"id":"overflow","bkd":{"lte":"9223372036854775808"}})",
    R"({"id":"underflow","bkd":{"gt":"-9223372036854775809"}})",
    R"({"id":"date","when":{"lt":9223372036854775807}})",
    R"({"id":"empty","bkd":{"gt":9223372036854775807}})",
    R"({"id":"inverted","bkd":{"gte":3,"lte":2}})",
  });
  EXPECT_EQ(6, rejected.total_errors);
  ASSERT_GE(rejected.errors.size(), 4u);
  EXPECT_NE(std::string::npos, rejected.errors[0].error_message.find("bkd"));
  EXPECT_NE(std::string::npos, rejected.errors[0].error_message.find("gte"));
  EXPECT_NE(std::string::npos, rejected.errors[3].error_message.find("lt"));
  ASSERT_TRUE(indexJson({R"({"id":"ok","bkd":{"gte":2,"lte":4},"col":{"gte":2,"lte":4}})"}).success);
  EXPECT_EQ((Ids{"ok"}), both(R"("gte":1.5,"lte":4.9)"));
  EXPECT_EQ((Ids{"ok"}), both(R"("gte":"-9223372036854775809","lte":"9223372036854775808")"));
  EXPECT_TRUE(both(R"("gt":9223372036854775807)").empty());
  EXPECT_TRUE(ids(R"({"match":{"field":"bkd","val":2.5}})").empty());
  EXPECT_TRUE(ids(R"({"multi_match":{"text":"2.5","fields":["bkd"]}})").empty());
}

TEST_F(RangeFieldTest, doubleDomainAndSignedZero) {
  SchemaBuilder indexed;
  auto& price = indexed.field("price");
  price.type = FieldClass::DOUBLE_RANGE;
  price.index = IndexMode::RANGE;
  indexed.set(helper.collection());
  ASSERT_TRUE(indexJson({
    R"({"id":"open","price":{}})",
    R"({"id":"infinity","price":{"gte":"-Infinity","lte":"Infinity"}})",
    R"({"id":"negative","price":{"gte":-0.0,"lte":-0.0}})",
    R"({"id":"positive","price":{"gte":0.0,"lte":0.0}})",
  }).success);
  for (auto relation : {"intersects", "contains", "within"}) {
    auto open = ids(fmt::format(R"({{"range":{{"field":"price","relation":"{}"}}}})", relation));
    auto explicitEnds = ids(fmt::format(R"({{"range":{{"field":"price","gte":"-Infinity","lte":"Infinity","relation":"{}"}}}})", relation));
    EXPECT_EQ(open, explicitEnds);
    EXPECT_NE(open.end(), std::ranges::find(open, std::string("open")));
    EXPECT_NE(open.end(), std::ranges::find(open, std::string("infinity")));
  }
  EXPECT_EQ((Ids{"infinity", "negative", "open"}), ids(R"({"range":{"field":"price","gte":-0.0,"lte":-0.0}})"));
  EXPECT_EQ((Ids{"infinity", "open", "positive"}), ids(R"({"range":{"field":"price","gte":0.0,"lte":0.0}})"));
  EXPECT_EQ((Ids{"infinity", "open", "positive"}), ids(R"({"range":{"field":"price","gt":-0.0,"lte":0.0}})"));
  EXPECT_EQ((Ids{"infinity", "negative", "open"}), ids(R"({"range":{"field":"price","gte":-0.0,"lt":0.0}})"));
  for (auto bounds : {R"("gt":"Infinity")", R"("lt":"-Infinity")"}) {
    EXPECT_TRUE(ids(fmt::format(R"({{"range":{{"field":"price",{}}}}})", bounds)).empty());
    EXPECT_EQ(1, indexJson({fmt::format(R"({{"id":"bad","price":{{{}}}}})", bounds)}).total_errors);
  }
  for (auto bound : {"gte", "gt", "lte", "lt"}) {
    auto json = fmt::format(R"({{"id":"nan","price":{{"{}":"NaN"}}}})", bound);
    EXPECT_EQ(1, indexJson({json}).total_errors);
    auto query = request(fmt::format(R"({{"query":{{"range":{{"field":"price","{}":"NaN"}}}}}})", bound));
    EXPECT_FALSE(query->ok());
  }
  RangeFieldType type("price", FieldType::DOUBLE);
  auto nan = coerce::scalarVal(std::numeric_limits<double>::quiet_NaN());
  EXPECT_THROW(type.coerceIntWindow(nan, FieldType::LiteralUse::GTE, "price"), RequestError);
  EXPECT_THROW(type.coerceIngestBound(nan, FieldType::LiteralUse::LTE, "price", {}), RequestError);
}

TEST_F(RangeFieldTest, dateGranulesTimeZonesAndNow) {
  ASSERT_TRUE(indexJson({
    R"({"id":"june","when":{"gte":"2024-06","lte":"2024-06"}})",
    R"({"id":"apia","when":{"gte":"2011-12-30T10:00:00Z","lte":"2011-12-31T09:59:59.999Z"}})",
    R"({"id":"now","when":{"gte":"now","lte":"now"}})",
  }).success);
  EXPECT_EQ((Ids{"june"}), ids(R"({"range":{"field":"when","gte":"2024-06-01T00:00:00Z","lte":"2024-06-30T23:59:59.999Z","relation":"within"}})"));
  auto skipped = localReq(helper.getSearchEngine());
  auto& cursor = skipped->collection("main").timeZone("Pacific/Apia").topDocs("q");
  ASSERT_TRUE(api::read_json(cursor.rawQuery(), R"({"range":{"field":"when","gte":"2011-12-30","lte":"2011-12-30"}})", cursor.mr()));
  cursor.withStats();
  skipped->execute();
  ASSERT_TRUE(skipped->ok()) << skipped->errorMsg();
  EXPECT_EQ(1, skipped->getMatchCount());
  EXPECT_TRUE(skipped->hasWarning("date_granule_skipped"));

  MemPool pool;
  auto schema = helper.collection().getSchema();
  QueryBuilder builder(pool, *schema, CoerceContext{JAN_1, TimeZone::utc()});
  for (auto text : {"now", "now/DAY"}) {
    auto now = coerce::scalarVal(std::string_view(text));
    auto* query = dynamic_cast<RangeFieldQuery*>(builder.createRangeQuery("when", &now, nullptr, &now, nullptr));
    ASSERT_NE(nullptr, query);
    FilterKeyBuilder key;
    EXPECT_EQ(std::string_view(text) == "now" ? FilterKeyScope::REQUEST_STABLE : FilterKeyScope::SEGMENT_STABLE,
              query->appendFilterKey(key, FilterKeyContext{}));
  }
  RangeFieldType date("when", FieldType::DATE);
  auto beyond = coerce::scalarVal(datetime_detail::kMaxEpochMs + 1);
  EXPECT_THROW(date.coerceIngestBound(beyond, FieldType::LiteralUse::LT, "when", {}), RequestError);
  auto minimum = coerce::scalarVal(datetime_detail::kMinEpochMs - 1);
  EXPECT_THROW(date.coerceIngestBound(minimum, FieldType::LiteralUse::GT, "when", {}), RequestError);
  auto last = coerce::scalarVal(datetime_detail::kMaxEpochMs);
  auto first = coerce::scalarVal(datetime_detail::kMinEpochMs);
  EXPECT_TRUE(date.coerceIngestBound(last, FieldType::LiteralUse::GT, "when", {}).empty());
  EXPECT_TRUE(date.coerceIngestBound(first, FieldType::LiteralUse::LT, "when", {}).empty());
}

TEST_F(RangeFieldTest, dateOutputModesAndReingest) {
  ASSERT_TRUE(indexJson({
    R"({"id":"canonical","when":{"gte":"2024-01-01T00:00:00Z","lt":"2024-02-01"},"bkd":{"gte":2,"lte":4},"price":{"gte":-0.0,"lte":1.5}})",
    R"({"id":"open","when":{"gte":"2024-01-01"},"bkd":{},"price":{"gte":"-Infinity","lte":"Infinity"}})",
  }).success);
  for (auto format : {"rows", "columns"}) for (auto mode : {"date_native", "date_iso", "date_ms", "date_ns"}) {
    SCOPED_TRACE(fmt::format("{} {}", format, mode));
    auto json = fmt::format(R"({{"flags":["{}"],"query":"*:*","fields":["id","when","bkd","price"],"sort":"id asc","document_format":"{}","limit":-1}})", mode, format);
    auto req = request(json);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto* list = req->docList();
    ASSERT_NE(nullptr, list);
    auto bound = [&](size_t row, std::string_view field, std::string_view key) -> const api::Val* {
      const api::Val* value;
      if (std::string_view(format) == "rows") value = &**list->docs[row].fields.find(field);
      else value = &std::get<api::ColVal>(list->columns.at(field).kind).v[row];
      auto* found = std::get<api::Map>(value->kind).fields.find(key);
      return found ? &**found : nullptr;
    };
    ASSERT_NE(nullptr, bound(0, "when", "gte"));
    if (std::string_view(mode) == "date_iso") {
      EXPECT_EQ("2024-01-01T00:00:00.000Z", std::get<std::string_view>(bound(0, "when", "gte")->kind));
    } else {
      int64_t scale = std::string_view(mode) == "date_ns" ? 1000000 : 1;
      EXPECT_EQ(JAN_1 * scale, bound(0, "when", "gte")->asInt());
      EXPECT_EQ((FEB_1 - 1) * scale, bound(0, "when", "lte")->asInt());
    }
    EXPECT_EQ(nullptr, bound(1, "when", "lte"));
    EXPECT_EQ(nullptr, bound(1, "price", "gte"));
    EXPECT_EQ(nullptr, bound(1, "price", "lte"));
    EXPECT_EQ(nullptr, bound(1, "bkd", "gte"));
    // Native and ISO endpoint values can be sent back without an input-unit override.
    if (std::string_view(format) == "rows" && std::string_view(mode) != "date_ns") {
      CollectionHelper::UpdateBuilder copies;
      for (const auto& row : list->docs) copies.addRaw([&row](api::Map& doc, auto&) { doc = row; });
      copies.commit();
      ASSERT_TRUE(helper.submit(copies).success);
      auto returned = request(json);
      ASSERT_TRUE(returned->ok()) << returned->errorMsg();
      std::string before, after;
      ASSERT_TRUE(api::write_json(*list, before));
      ASSERT_TRUE(api::write_json(*returned->docList(), after));
      EXPECT_EQ(before, after);
      EXPECT_EQ((Ids{"canonical"}), ids(R"({"range":{"field":"when","gte":"2024-01","lte":"2024-01","relation":"within"}})"));
      EXPECT_EQ((Ids{"canonical"}), ids(R"({"range":{"field":"bkd","gte":2,"lte":4,"relation":"within"}})"));
    }
  }
}

TEST_F(RangeFieldTest, matchBoostMultiMatchAndParsers) {
  indexAllDocs();
  for (auto field : {"bkd", "col"}) {
    for (auto query : {
      fmt::format(R"({{"match":{{"field":"{}","val":17}}}})", field),
      fmt::format(R"({{"expr":"{}:17"}})", field),
      fmt::format(R"({{"simple_query":{{"q":"{}:17","fields":["id"]}}}})", field),
      fmt::format(R"({{"simple_query":{{"q":"{}:\"17\"","fields":["id"]}}}})", field)}) {
      EXPECT_EQ((Ids{"a", "b"}), ids(query));
    }
  }
  MemPool pool;
  auto schema = helper.collection().getSchema();
  QueryBuilder builder(pool, *schema, CoerceContext{});
  auto* boosted = dynamic_cast<BoostQuery*>(builder.createMatchQuery("bkd", coerce::scalarVal((int64_t)17),
      QueryBuilder::Operator::OR, 0, 3.0f));
  ASSERT_NE(nullptr, boosted);
  EXPECT_FLOAT_EQ(3, boosted->getBoost());
  EXPECT_EQ(QueryKind::RANGE_FIELD, boosted->getChild()->getKind());
  for (auto mode : {"combined", "per_field"}) {
    EXPECT_TRUE(ids(fmt::format(R"({{"multi_match":{{"text":"17","fields":["*"],"mode":"{}"}}}})", mode)).empty());
    for (auto fields : {R"([{"field":"bkd","weight":3}])", R"([{"field":"col","weight":3}])",
                        R"([{"field":"*"},{"field":"bkd","weight":3}])"}) {
      auto req = request(fmt::format(R"({{"query":{{"multi_match":{{"text":"17","fields":{},"mode":"{}"}}}},"get_scores":true,"fields":["id"],"limit":-1}})", fields, mode));
      ASSERT_TRUE(req->ok()) << req->errorMsg();
      EXPECT_EQ(2u, req->getDocs().size());
      for (float score : std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v) EXPECT_FLOAT_EQ(3, score);
    }
    EXPECT_EQ((Ids{"a"}), ids(fmt::format(R"({{"multi_match":{{"text":"a","fields":["bkd","id"],"mode":"{}"}}}})", mode)));
  }
  auto invalid = request(R"({"query":{"match":{"field":"bkd","val":"bad"}}})");
  EXPECT_FALSE(invalid->ok());
}

TEST_F(RangeFieldTest, boundDiagnostics) {
  auto rejected = indexJson({R"({"id":"fraction","bkd":{"gte":1.5}})"});
  ASSERT_EQ(1u, rejected.errors.size());
  EXPECT_EQ("Field 'bkd' branch 'self': field 'bkd': cannot coerce 1.5 to an integer "
            "(value is not integral) (bound 'gte')", rejected.errors[0].error_message);
  auto schema = helper.collection().getSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});
  auto bad = coerce::scalarVal(std::string_view("bad"));
  for (auto field : {"number_i", "bkd"}) {
    auto* element = field == std::string_view("bkd")
        ? ((RangeFieldType*)schema->getFieldTypePtr(field))->element.get() : schema->getFieldTypePtr(field);
    std::string before = fmt::format("field '{}': cannot coerce 'bad' to an integer range bound", field);
    try {
      element->coerceIntWindow(bad, FieldType::LiteralUse::LTE, field);
      FAIL() << "accepted an invalid element bound";
    } catch (const RequestError& e) {
      EXPECT_EQ(before, e.what());
    }
    try {
      builder.createRangeQuery(field, nullptr, nullptr, &bad, nullptr);
      FAIL() << "accepted an invalid query bound";
    } catch (const RequestError& e) {
      EXPECT_EQ(before + " (bound 'lte')", e.what());
      EXPECT_EQ("invalid_value", e.code);
    }
  }
}

TEST_F(RangeFieldTest, tryCoercionMatchesElements) {
  using Use = FieldType::LiteralUse;
  RangeFieldType range("range", FieldType::DOUBLE);
  DoubleFieldType scalar("scalar");
  auto domain = range.domainBounds();
  const api::Val literals[] = {
    coerce::scalarVal((int64_t)17), coerce::scalarVal(1.5), coerce::scalarVal(-0.0),
    coerce::scalarVal(0.0f), coerce::scalarVal(std::numeric_limits<double>::infinity()),
    coerce::scalarVal(-std::numeric_limits<double>::infinity()),
    coerce::scalarVal(std::numeric_limits<double>::quiet_NaN()),
    coerce::scalarVal(std::string_view("Infinity")), coerce::scalarVal(std::string_view("-Infinity")),
    coerce::scalarVal(std::string_view("NaN")), coerce::scalarVal(std::string_view("17")),
    coerce::scalarVal(std::string_view("1.5")), coerce::scalarVal(std::string_view("-0")),
    coerce::scalarVal(std::string_view("1e3")), coerce::scalarVal(std::string_view("bad")),
    coerce::scalarVal(true), api::Val{}, api::Val{.kind = api::Map{}}
  };
  for (const auto& value : literals) {
    SCOPED_TRACE(coerce::describe(value));
    auto encoded = scalar.tryCoerceColInt64(value, "scalar");
    bool accepted = encoded && domain.lo <= *encoded && *encoded <= domain.hi;
    for (auto use : {Use::GTE, Use::GT, Use::LTE, Use::LT}) {
      auto window = range.tryCoerceIntWindow(value, use, "range");
      ASSERT_EQ(accepted, window.has_value());
      if (!accepted) {
        EXPECT_THROW(range.coerceIntWindow(value, use, "range"), RequestError);
        continue;
      }
      auto expected = scalar.coerceIntWindow(value, use, "scalar");
      expected.lo = std::max(expected.lo, domain.lo);
      expected.hi = std::min(expected.hi, domain.hi);
      EXPECT_EQ(expected.empty(), window->empty());
      if (!expected.empty()) {
        EXPECT_EQ(expected.lo, window->lo);
        EXPECT_EQ(expected.hi, window->hi);
      }
    }
  }
  for (auto type : {FieldType::INT, FieldType::DATE}) {
    RangeFieldType other("other", type);
    auto bad = coerce::scalarVal(std::string_view("bad"));
    for (auto use : {Use::MATCH, Use::GTE, Use::GT, Use::LTE, Use::LT}) {
      EXPECT_FALSE(other.tryCoerceIntWindow(bad, use, "other"));
      EXPECT_THROW(other.coerceIntWindow(bad, use, "other"), RequestError);
    }
  }
}
