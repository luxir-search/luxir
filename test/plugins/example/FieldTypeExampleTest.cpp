// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Plugin field types through the example plugin's "pair" and "spot": schema
// validation, ingest over HTTP JSON and gRPC, queries, sorts and facets on
// derived fields, projection, merges with deletes, persistence and a missing
// type at load.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include "luxir/plugin/Registries.h"
#include "luxir/schema/FieldTypeRegistry.h"
#include "luxir/schema/Schema.h"
#include "luxir/server/HttpServer.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/GrpcClient.h"
#include "test/GrpcLuxirTest.h"
#include "test/HttpReq.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr std::string_view PAIR_SCHEMA = R"({"fields":{"pt":{"type":"pair","params":{"separator":"/"}}}})";

std::string schemaError(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  std::string error;
  if (!api::read_json(def, json, arena, &error)) return "parse: " + error;
  try {
    Schema::fromProto(def);
  } catch (const SchemaError& e) {
    return e.what();
  }
  return "";
}

std::shared_ptr<Schema> schemaOf(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  EXPECT_TRUE(api::read_json(def, json, arena));
  return Schema::fromProto(def);
}

void putSchema(Collection& collection, std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  std::string error;
  ASSERT_TRUE(api::read_json(def, json, arena, &error)) << error;
  collection.updateSchema(def, api::SchemaRequest_::Mode::SET);
}

// Each returned document's `key`, as JSON ("-" when absent), from op "q".
std::vector<std::string> values(const std::string& body, std::string_view key) {
  using Json = glz::generic_i64;
  std::vector<std::string> out;
  size_t start = 0;
  while (start < body.size()) {
    size_t nl = body.find('\n', start);
    if (nl == std::string::npos) nl = body.size();
    Json root;
    if (!glz::read_json(root, std::string_view(body).substr(start, nl - start)) && root.contains("ops") &&
        root["ops"].contains("q") && root["ops"]["q"].contains("docs")) {
      for (auto& doc : root["ops"]["q"]["docs"].get<Json::array_t>()) {
        std::string json = "-";
        if (doc.contains(key)) json = glz::write_json(doc[key]).value_or("?");
        out.push_back(json);
      }
    }
    start = nl + 1;
  }
  return out;
}

using Strings = std::vector<std::string>;

// "plugin types: ..." as the registry lists them (other configured plugins
// may add their own).
std::string pluginTypes() {
  std::string out = "plugin types: ";
  bool first = true;
  for (const auto& name : Registries::global().fieldTypes.names()) {
    if (!first) out += ", ";
    out += name;
    first = false;
  }
  return out;
}

class FieldTypeExampleTest : public LuxirTest {
protected:
  CollectionHelper helper;
  std::unique_ptr<HttpServer> server;

  void SetUp() override {
    initPlugins(addConfiguredPlugins);
    server = std::make_unique<HttpServer>(*LuxirTest::luxirNode, 1, 0);
    server->start();
  }
  void TearDown() override { server->shutdown(); }

  http::response<http::string_body> post(std::string_view target, std::string body) {
    return httpRequest(server->getPort(), http::verb::post, target, std::move(body));
  }
  void schema(std::string_view json) {
    auto res = post("/collections/main/_schema", std::string(json));
    ASSERT_EQ(200, res.result_int()) << res.body();
  }
  // The update response body.
  std::string update(std::string docs) {
    auto res = post("/collections/main/_update", R"({"docs":)" + docs + R"(,"commit":{}})");
    EXPECT_EQ(200, res.result_int()) << res.body();
    return res.body();
  }
  // The search response body: top_docs "q" with `params` (JSON members).
  std::string search(std::string params) {
    auto res = post("/collections/main/_search", R"({"ops":{"q":{"top_docs":{)" + params + "}}}}");
    EXPECT_EQ(200, res.result_int()) << res.body();
    return res.body();
  }
  Strings ids(std::string query, std::string extra = "") {
    auto all = values(search(R"("query":)" + query + R"(,"fields":["id"],"limit":100)" + extra), "id");
    for (auto& id : all) id = id.substr(1, id.size() - 2);
    return all;
  }
};

}  // namespace

TEST_F(FieldTypeExampleTest, registeredAndFrozen) {
  auto& types = Registries::global().fieldTypes;
  EXPECT_TRUE(types.frozen());
  // Other configured plugins may register types too.
  auto names = types.names();
  EXPECT_NE(names.end(), std::ranges::find(names, std::string("pair")));
  EXPECT_NE(names.end(), std::ranges::find(names, std::string("spot")));
  auto maker = [](const ParamReader&) -> std::unique_ptr<const PluginFieldType> { return nullptr; };
  EXPECT_THROW(types.add("late", maker), std::logic_error);
  FieldTypeRegistry fresh;
  EXPECT_THROW(fresh.add("int", maker), std::invalid_argument);
  EXPECT_THROW(fresh.add("plugin", maker), std::invalid_argument);
  EXPECT_THROW(fresh.add("Bad-Name", maker), std::invalid_argument);
  fresh.add("mine", maker);
  EXPECT_THROW(fresh.add("mine", maker), std::invalid_argument);
}

TEST_F(FieldTypeExampleTest, schemaValidationTeaches) {
  EXPECT_EQ("unknown field type 'piar'; valid types: date, double, float, geo_point, id, int, string, text, "
            "vector; " + pluginTypes() + " (field: pt)",
            schemaError(R"({"fields":{"pt":"piar"}})"));
  EXPECT_EQ("field type 'pair': unknown parameter 'sep'; valid: separator, multi (field: pt)",
            schemaError(R"({"fields":{"pt":{"type":"pair","params":{"sep":"/"}}}})"));
  EXPECT_EQ("field type 'pair': 'separator' must not be empty (field: pt)",
            schemaError(R"({"fields":{"pt":{"type":"pair","params":{"separator":""}}}})"));
  EXPECT_NE(std::string::npos, schemaError(R"({"fields":{"pt":{"type":"pair","multi":true}}})")
                                   .find("'multi' is not valid for plugin field type 'pair'"));
  EXPECT_NE(std::string::npos,
            schemaError(R"({"fields":{"pt":{"type":"pair","variants":{"x":{"type":"string"}}}}})")
                .find("'variants' is not valid"));
  EXPECT_NE(std::string::npos, schemaError(R"({"fields":{"t":{"type":"text","variants":{"x":"pair"}}}})")
                                   .find("variant cannot have a plugin type"));
  // Derived names are not authorable.
  EXPECT_NE(std::string::npos, schemaError(R"({"fields":{"pt$a":"int"}})").find("'$' names a plugin field type's derived field"));
  // params only go with a plugin type name.
  EXPECT_NE(std::string::npos, schemaError(R"({"fields":{"n":{"type":"int","params":{}}}})")
                                   .find("params belongs to a plugin field type"));
  EXPECT_EQ("", schemaError(R"({"fields":{"pt":{"plugin_type":{"name":"pair"},"ignore_malformed":true}}})"));
}

namespace {

// A type for registry-level checks: one INT field with `suffix`, version() and
// canonicalParams() as given.
class ProbeType : public PluginFieldType {
public:
  uint32_t encoding;
  std::string effective;

  ProbeType(std::string suffix, uint32_t encoding, std::string effective)
      : encoding(encoding), effective(std::move(effective)) {
    fields = {{.suffix = std::move(suffix), .kind = DerivedField::Kind::INT}};
  }
  void index(const api::Val&, DerivedSink&) const override {}
  uint32_t version() const override { return encoding; }
  std::string canonicalParams() const override { return effective; }
};

std::string canonicalOf(FieldTypeRegistry& registry, std::string_view name) {
  api::AnalyzerComponent def;
  def.name = name;
  return registry.make(def)->canonical;
}

}  // namespace

// version() and canonicalParams() join the canonical definition; suffixes
// are capped so <field>$<suffix> fits a name.
TEST_F(FieldTypeExampleTest, canonicalDefinitionAndSuffixLimit) {
  FieldTypeRegistry registry;
  auto add = [&](std::string name, std::string suffix, uint32_t version, std::string params) {
    registry.add(name, [=](const ParamReader&) -> std::unique_ptr<const PluginFieldType> {
      return std::make_unique<ProbeType>(suffix, version, params);
    });
  };
  add("plain", "v", 0, "");
  add("versioned", "v", 3, R"({"z":1,"a":true})");
  add("bad_params", "v", 0, "[1]");
  add("long_suffix", std::string(DerivedField::MAX_SUFFIX_LEN + 1, 's'), 0, "");
  add("max_suffix", std::string(DerivedField::MAX_SUFFIX_LEN, 's'), 0, "");
  EXPECT_EQ(R"({"name":"plain"})", canonicalOf(registry, "plain"));
  EXPECT_EQ(R"({"name":"versioned","params":{"a":true,"z":1},"version":3})", canonicalOf(registry, "versioned"));
  EXPECT_THROW(canonicalOf(registry, "bad_params"), std::invalid_argument);
  try {
    canonicalOf(registry, "long_suffix");
    FAIL() << "a 33-byte suffix was accepted";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("suffix is longer than 32 bytes")) << e.what();
  }
  EXPECT_NO_THROW(canonicalOf(registry, "max_suffix"));
  // A field whose derived names would pass 127 bytes is refused.
  std::string longName(121, 'p');
  EXPECT_NE(std::string::npos, schemaError(R"({"fields":{")" + longName + R"(":"pair"}})")
                                   .find("generated physical field name exceeds 127 bytes"));
  EXPECT_EQ("", schemaError(R"({"fields":{")" + longName.substr(0, 120) + R"(":"pair"}})"));
}

// Built-in settings reach a plugin-typed field neither directly nor through
// its parent; a built-in type below clears the plugin type, a child without
// a type inherits it.
TEST_F(FieldTypeExampleTest, inheritance) {
  EXPECT_NE(std::string::npos,
            schemaError(R"({"fields":{"p":{"type":"text","analyzer":"english"},"c":{"parent":"p","type":"pair"}}})")
                .find("field has plugin type 'pair' but inherits 'analyzer' from its parent chain"));
  EXPECT_NE(std::string::npos,
            schemaError(R"({"fields":{"p":{"type":"int","multi":true},"c":{"parent":"p","type":"pair"}}})")
                .find("inherits 'multi'"));
  EXPECT_NE(std::string::npos,
            schemaError(R"({"fields":{"p":{"type":"string","column":false},"c":{"parent":"p","type":"pair"}}})")
                .find("inherits 'column'"));

  auto schema = schemaOf(R"({"fields":{"p":{"type":"pair","params":{"separator":"/"}},"i":{"parent":"p","type":"int"},)"
                         R"("c":{"parent":"p"},"g":{"parent":"c","ignore_malformed":true}}})");
  EXPECT_EQ(FieldType::INT, schema->getFieldTypePtr("i")->type());
  EXPECT_EQ(nullptr, schema->getFieldTypePtr("i$a"));
  auto signatures = schema->signatures();
  EXPECT_EQ(signatures.at("p$key").properties.at("plugin_type"), signatures.at("g$key").properties.at("plugin_type"));
  EXPECT_EQ(signatures.at("p$key").properties.at("plugin_type"), signatures.at("c$key").properties.at("plugin_type"));
  EXPECT_TRUE(static_cast<CompositeFieldType*>(schema->getFieldTypePtr("g"))->ignoreMalformed);
}

// The type's definition is the derived fields' identity, and the authored
// schema keeps it.
TEST_F(FieldTypeExampleTest, signaturesAndEcho) {
  auto slash = schemaOf(PAIR_SCHEMA);
  auto signatures = slash->signatures();
  EXPECT_EQ(R"({"name":"pair","params":{"multi":false,"separator":"/"}})",
            signatures.at("pt$key").properties.at("plugin_type"));
  EXPECT_EQ("$key", signatures.at("pt$key").label);
  EXPECT_EQ(R"("plugin")", signatures.at("pt").properties.at("type"));
  auto first = slash->withHistory(1, nullptr);
  EXPECT_EQ(1u, schemaOf(PAIR_SCHEMA)->withHistory(2, first.get())->introduction("pt$a"));
  EXPECT_EQ(2u, schemaOf(R"({"fields":{"pt":"pair"}})")->withHistory(2, first.get())->introduction("pt$a"));
  // canonicalParams(): the defaults spelled out are the same definition, and
  // "params" may come before "type".
  auto plain = schemaOf(R"({"fields":{"pt":"pair"}})")->signatures().at("pt$a").properties.at("plugin_type");
  EXPECT_EQ(plain, schemaOf(R"({"fields":{"pt":{"type":"pair","params":{"separator":":","multi":false}}}})")
                       ->signatures().at("pt$a").properties.at("plugin_type"));
  EXPECT_EQ(signatures.at("pt$a").properties.at("plugin_type"),
            schemaOf(R"({"fields":{"pt":{"params":{"separator":"/"},"type":"pair"}}})")
                ->signatures().at("pt$a").properties.at("plugin_type"));

  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  slash->toProto(&def, arena);
  std::string json;
  ASSERT_TRUE(api::write_json(def, json));
  EXPECT_NE(std::string::npos, json.find(R"("plugin_type":{"name":"pair","params":{"separator":"/"}})")) << json;
}

// Objects in over HTTP JSON; derived fields answer queries, sorts and facets
// by name; the field itself projects back to the object and is only retrievable.
TEST_F(FieldTypeExampleTest, ingestQuerySortFacetAndProjectOverHttp) {
  schema(PAIR_SCHEMA);
  auto body = update(R"([{"id":"d1","pt":{"a":1,"b":5}},{"id":"d2","pt":{"b":4,"a":2}},{"id":"d3","pt":{"a":3,"b":3}},)"
                     R"({"id":"d4"}])");
  EXPECT_EQ(std::string::npos, body.find("errors")) << body;

  EXPECT_EQ((Strings{"d2", "d3"}), ids(R"("pt$a:>=2")", R"(,"sort":"id asc")"));
  EXPECT_EQ((Strings{"d2"}), ids(R"({"match":{"field":"pt$key","val":"2/4"}})"));
  EXPECT_EQ((Strings{"d2"}), ids(R"("pt$key:\"2/4\"")"));
  EXPECT_EQ((Strings{"d3", "d2", "d1", "d4"}), ids(R"({"all":true})", R"(,"sort":"pt$b asc")"));

  auto facet = post("/collections/main/_search",
                    R"({"ops":{"keys":{"field_facet":{"field":"pt$key","limit":-1}}}})");
  EXPECT_NE(std::string::npos, facet.body().find(R"({"val":"1/5","count":1},{"val":"2/4","count":1})"))
      << facet.body();

  auto projected = search(R"("query":{"all":true},"fields":["id","pt","pt$b"],"sort":"id asc")");
  EXPECT_EQ((Strings{R"({"a":1,"b":5})", R"({"a":2,"b":4})", R"({"a":3,"b":3})", "-"}), values(projected, "pt"));
  EXPECT_EQ((Strings{"5", "4", "3", "-"}), values(projected, "pt$b"));
  // The default field set has the field and hides its derived fields.
  auto all = search(R"("query":{"all":true},"sort":"id asc")");
  EXPECT_EQ(R"({"a":1,"b":5})", values(all, "pt").at(0));
  EXPECT_EQ("-", values(all, "pt$a").at(0));

  // The packed bytes are the type's encoding, not a value.
  auto opaque = post("/collections/main/_search", R"({"query":{"all":true},"fields":["pt$packed"]})");
  EXPECT_NE(std::string::npos, opaque.body().find("Field 'pt$packed' holds plugin type 'pair''s internal encoding "
                                                  "and is not retrievable; retrieve 'pt' for its value"))
      << opaque.body();
  auto direct = post("/collections/main/_search", R"({"query":"pt:x"})");
  EXPECT_NE(std::string::npos, direct.body().find("has plugin type 'pair' and is only retrievable")) << direct.body();
  auto unknown = post("/collections/main/_search", R"({"query":{"match":{"field":"pt$z","val":"1"}}})");
  EXPECT_NE(std::string::npos, unknown.body().find("field 'pt' has pt$a, pt$b, pt$key, pt$packed")) << unknown.body();
  auto resolved = httpRequest(server->getPort(), http::verb::get, "/collections/main/_schema?view=resolved");
  EXPECT_NE(std::string::npos, resolved.body().find(R"("$key": {)")) << resolved.body();
  EXPECT_NE(std::string::npos, resolved.body().find(R"("name": "pt$key")")) << resolved.body();
  // A derived field is not a document key.
  body = update(R"([{"id":"d5","pt$a":1}])");
  EXPECT_NE(std::string::npos, body.find("Invalid logical document key")) << body;
}

// A value the plugin rejects fails its document, naming the field; with
// ignore_malformed only the field is skipped.
TEST_F(FieldTypeExampleTest, malformedValues) {
  schema(PAIR_SCHEMA);
  auto body = update(R"([{"id":"ok","pt":{"a":1,"b":2}},{"id":"bad","pt":{"a":1}},{"id":"str","pt":"1/2"}])");
  EXPECT_NE(std::string::npos, body.find("Field 'pt': cannot read pair value: 'b' is required")) << body;
  EXPECT_NE(std::string::npos, body.find("expected an object")) << body;
  EXPECT_EQ((Strings{"ok"}), ids(R"({"all":true})"));

  schema(R"({"fields":{"pt":{"type":"pair","params":{"separator":"/"},"ignore_malformed":true}}})");
  body = update(R"([{"id":"bad","pt":{"a":1}},{"id":"good","pt":{"a":7,"b":8}}])");
  EXPECT_EQ(std::string::npos, body.find("errors")) << body;
  EXPECT_EQ((Strings{"bad", "good", "ok"}), ids(R"({"all":true})", R"(,"sort":"id asc")"));
  EXPECT_EQ((Strings{"good"}), ids(R"("pt$a:7")"));
}

// A plugin's DocumentError keeps its code; an exception that is not a
// malformed-value error fails the document even with ignore_malformed.
TEST_F(FieldTypeExampleTest, pluginErrors) {
  for (const char* type : {R"("spot")", R"({"type":"spot","ignore_malformed":true})"}) {
    schema(std::string(R"({"fields":{"s":)") + type + "}}");
    auto body = update(R"([{"id":"t1","s":{"lat":1,"lon":2,"tokens":[["b",4],["a",1]]}}])");
    EXPECT_NE(std::string::npos, body.find("Field 's': field type 'spot' failed to index the value: "
                                           "field type 'spot': token positions must be >= 0 and non-decreasing"))
        << body;
    EXPECT_NE(std::string::npos, body.find("field_type_error")) << body;
  }
  schema(R"({"fields":{"s":"spot"}})");
  auto body = update(R"([{"id":"l1","s":{"lat":1,"lon":2,"label":7}},{"id":"t2","s":{"lat":1,"lon":2,"tokens":[["x",2],["y",2]]}}])");
  EXPECT_NE(std::string::npos, body.find("Field 's': cannot read spot value: 'label' must be a string")) << body;
  EXPECT_NE(std::string::npos, body.find("invalid_label")) << body;
  EXPECT_EQ((Strings{"t2"}), ids(R"({"match":{"field":"s$letters","val":"y"}})"));
}

// A derived term over 255 bytes is fitted like a built-in string's, not a
// malformed value: the document indexes and an exact query finds it.
TEST_F(FieldTypeExampleTest, longDerivedTermsAreFitted) {
  std::string separator(300, '-');
  for (const char* ignore : {"false", "true"}) {
    schema(R"({"fields":{"pt":{"type":"pair","params":{"separator":")" + separator + R"("},"ignore_malformed":)" +
           ignore + "}}}");
    auto body = update(R"([{"id":"long","pt":{"a":1,"b":2}}])");
    EXPECT_EQ(std::string::npos, body.find("errors")) << body;
    EXPECT_EQ((Strings{"long"}), ids(R"({"match":{"field":"pt$key","val":"1)" + separator + R"(2"}})"));
    EXPECT_EQ((Strings{R"({"a":1,"b":2})"}), values(search(R"("query":{"all":true},"fields":["pt"])"), "pt"));
  }
}

TEST(DerivedValuesTest, exactFlagsFittableIndexedStrings) {
  std::vector<DerivedField> fields = {{.suffix = "k", .kind = DerivedField::Kind::STRING},
                                      {.suffix = "b", .kind = DerivedField::Kind::STRING, .index = false},
                                      {.suffix = "n", .kind = DerivedField::Kind::INT}};
  std::string longValue(PackedTerm::MIN_FITTED_LEN, 'x');
  std::vector<api::Val> row(3);
  row[0].kind = std::string_view(longValue);
  row[1].kind = std::string_view(longValue);
  row[2].kind = int64_t{1};
  DerivedValues values{row, fields};
  EXPECT_FALSE(values.exact(0));
  EXPECT_TRUE(values.exact(1));
  EXPECT_TRUE(values.exact(2));
  row[0].kind = std::string_view(longValue).substr(1);
  EXPECT_TRUE(values.exact(0));
  std::array<std::string_view, 2> many{"short", longValue};
  row[0].kind = api::ArrStr{many};
  EXPECT_FALSE(values.exact(0));
}

TEST_F(FieldTypeExampleTest, multiValuedPairsAndTemplates) {
  schema(R"({"fields":{"ps":{"type":"pair","params":{"multi":true}}},"templates":{"_pair":"pair"}})");
  update(R"([{"id":"m1","ps":[{"a":1,"b":2},{"a":3,"b":4}],"x_pair":{"a":9,"b":9}},{"id":"m2","ps":[{"a":5,"b":6}]}])");
  EXPECT_EQ((Strings{"m1"}), ids(R"("ps$key:\"3:4\"")"));
  EXPECT_EQ((Strings{"m1"}), ids(R"("x_pair$b:9")"));
  auto body = search(R"("query":{"all":true},"fields":["ps","x_pair"],"sort":"id asc")");
  auto ps = values(body, "ps");
  ASSERT_EQ(2u, ps.size());
  EXPECT_NE(std::string::npos, ps[0].find(R"({"a":3,"b":4})")) << ps[0];
  EXPECT_EQ(R"([{"a":5,"b":6}])", ps[1]);
  EXPECT_EQ((Strings{R"({"a":9,"b":9})", "-"}), values(body, "x_pair"));
}

// Analyzed and pre-analyzed TEXT and a GEO_POINT with its points index.
TEST_F(FieldTypeExampleTest, textTokensAndPoints) {
  schema(R"({"fields":{"s":"spot"}})");
  update(R"([{"id":"s1","s":{"label":"running dogs","lat":50.5,"lon":10.5}},{"id":"s2","s":{"lat":-20,"lon":30}}])");
  EXPECT_EQ((Strings{"s1"}), ids(R"({"match":{"field":"s$label","val":"DOGS"}})"));
  EXPECT_EQ((Strings{"s1"}), ids(R"({"phrase":{"field":"s$letters","text":"d o g s"}})"));
  EXPECT_EQ((Strings{"s1"}),
            ids(R"({"geo_box":{"field":"s$at","min_lat":50,"max_lat":51,"min_lon":10,"max_lon":11}})"));
  auto res = post("/collections/main/_search", R"({"query":{"all":true},"fields":["s"]})");
  EXPECT_NE(std::string::npos, res.body().find("has plugin type 'spot', which returns no value")) << res.body();
}

// Derived fields merge like any other field: after deletes and a forced
// merge the survivors keep their values in every structure.
TEST_F(FieldTypeExampleTest, mergesWithDeletesKeepDerivedFields) {
  putSchema(helper.collection(), PAIR_SCHEMA);
  for (int seg = 0; seg < 3; seg++) {
    std::string docs = "[";
    for (int i = 0; i < 4; i++) {
      int n = seg * 4 + i;
      if (i) docs += ",";
      docs += R"({"id":"d)" + std::to_string(n) + R"(","pt":{"a":)" + std::to_string(n) + R"(,"b":)" +
              std::to_string(100 - n) + "}}";
    }
    update(docs + "]");
  }
  helper.deleteByIds({"d1", "d5", "d10"}, UpdateMessage::COMMIT);
  auto merged = post("/collections/main/_update", R"({"docs":[],"commit":{"max_segments":1,"wait_for_merges":true}})");
  ASSERT_EQ(200, merged.result_int()) << merged.body();
  EXPECT_EQ(1u, helper.durableSegmentCount());

  EXPECT_EQ((Strings{"d11", "d9", "d8", "d7"}), ids(R"("pt$a:>=7")", R"(,"sort":"pt$b asc")"));
  EXPECT_EQ((Strings{"d6"}), ids(R"("pt$key:\"6/94\"")"));
  EXPECT_EQ((Strings{}), ids(R"("pt$key:\"5/95\"")"));
  auto body = search(R"("query":"pt$a:<=3","fields":["pt"],"sort":"pt$a asc")");
  EXPECT_EQ((Strings{R"({"a":0,"b":100})", R"({"a":2,"b":98})", R"({"a":3,"b":97})"}), values(body, "pt"));
}

class FieldTypeGrpcTest : public GrpcLuxirTest {};

TEST_F(FieldTypeGrpcTest, objectsOverProtobuf) {
  initPlugins(addConfiguredPlugins);
  CollectionHelper helper;
  putSchema(helper.collection(), PAIR_SCHEMA);
  std::pmr::monotonic_buffer_resource mr;
  api::UpdateRequest request;
  std::string error;
  ASSERT_TRUE(api::read_json(request, R"({"docs":[{"id":"g1","pt":{"a":4,"b":2}},{"id":"g2","pt":{"a":1}}],"commit":{}})",
                             mr, &error)) << error;
  Reply<api::UpdateResponse> response;
  grpc::ClientContext context;
  ASSERT_TRUE(hppUnaryCall(getChannel().get(), rpc::Update, &context, request, &response).ok());
  EXPECT_EQ(api::UpdateResponse_::Status::PARTIAL, response.msg.status);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").exprQuery("pt$key:\"4/2\"").fields({"id", "pt$a"}).limit(10);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  EXPECT_EQ("g1", std::get<std::string>(*find(docs[0], "id")));
  EXPECT_EQ(4, std::get<int64_t>(*find(docs[0], "pt$a")));
}

class FieldTypePersistenceTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("luxir-fieldtype-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  void TearDown() override { std::filesystem::remove_all(path); }

  static Strings sortedIds(LuxirNode& node, std::string_view collection) {
    auto req = localReq(node.getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").exprQuery("pt$a:>=0").fields({"id"}).limit(10);
    qb::sort(top, "pt$b", qb::ASC);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    Strings out;
    for (const auto& doc : req->getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    return out;
  }
};

// The type is part of the persisted schema: a reopened node serves the
// derived fields, and a binary without the type refuses that collection with
// the teaching message while its other collections keep serving.
TEST_F(FieldTypePersistenceTest, reopensAndAMissingTypeFailsTheCollectionOnly) {
  initPlugins(addConfiguredPlugins);
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  {
    LuxirNode node(config);
    for (const char* name : {"good", "bad"}) {
      CollectionHelper helper(node, name);
      putSchema(helper.collection(), PAIR_SCHEMA);
      ASSERT_TRUE(helper.index(flatdoc("id", "a"), UpdateMessage::NO_COMMIT).success);
      CollectionHelper::UpdateBuilder b;
      b.addRaw([](api::Map& doc, std::pmr::memory_resource& mr) {
        std::string error;
        EXPECT_TRUE(api::read_json(doc, R"({"id":"p","pt":{"a":1,"b":2}})", mr, &error)) << error;
      });
      b.addRaw([](api::Map& doc, std::pmr::memory_resource& mr) {
        EXPECT_TRUE(api::read_json(doc, R"({"id":"q","pt":{"a":3,"b":1}})", mr));
      });
      b.commit();
      ASSERT_TRUE(helper.submit(b).success);
    }
  }
  {
    LuxirNode node(config);
    EXPECT_EQ((Strings{"q", "p"}), sortedIds(node, "good"));
  }
  FSDirectory container(path / "t" / "t0" / "bad");
  FSDirectory dir(path / "t" / "t0" / "bad" / *CollectionStorage::current(container));
  auto loaded = readDurableIndexInfo(dir);
  api::SchemaDef missing;
  std::string missingJson = R"({"fields":{"pt":"from_a_missing_plugin"}})";  // the views point into it
  ASSERT_TRUE(api::read_json(missing, missingJson, *loaded.arena));
  std::vector<std::byte> schemaBytes, manifest;
  ASSERT_TRUE(api::encode(missing, schemaBytes));
  loaded.info.schema->source_def = std::span<const std::byte>(schemaBytes);
  ASSERT_TRUE(api::encode(loaded.info, manifest));
  Manifest::write(dir, loaded.info.index_gen, manifest);

  LuxirNode node(config);
  EXPECT_EQ((Strings{"q", "p"}), sortedIds(node, "good"));
  try {
    node.getCollection("bad");
    FAIL() << "the collection with a missing field type loaded";
  } catch (const CollectionResolutionError& e) {
    std::string message = e.what();
    EXPECT_NE(std::string::npos, message.find("failed to load")) << message;
    EXPECT_NE(std::string::npos, message.find("unknown field type 'from_a_missing_plugin'; valid types: "
                                              "date, double, float, geo_point, id, int, string, text, vector; "
                                              + pluginTypes())) << message;
  }
}
