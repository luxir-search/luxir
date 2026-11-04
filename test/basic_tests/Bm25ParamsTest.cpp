// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Per-field BM25 k1 and b: schema validation and their effect on scores.

#include <cmath>
#include <limits>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

using FieldClass = api::FieldDef::FieldClass;

class Bm25ParamsTest : public LuxirTest {
public:
  CollectionHelper helper{"main"};
  google::protobuf::Arena protoArena;
  ArenaResource arena{&protoArena};

  // Three text fields over the same content: default, saturating at once
  // (tiny k1), and ignoring length (b = 0).
  void SetUp() override {
    SchemaBuilder b;
    b.field("body").type = FieldClass::TEXT;
    auto& sat = b.field("sat");
    sat.type = FieldClass::TEXT;
    sat.k1 = 0.001f;
    auto& nolen = b.field("nolen");
    nolen.type = FieldClass::TEXT;
    nolen.b = 0.0f;
    b.set(helper.collection());
    const std::string longText = "dune x x x x x x x x x x x x";
    const std::pair<const char*, const char*> docs[] = {
        {"short", "dune"}, {"repeated", "dune dune dune dune"}, {"long", longText.c_str()}};
    for (auto [id, text] : docs) {
      helper.index(flatdoc("id", id, "body", text, "sat", text, "nolen", text), UpdateMessage::NO_COMMIT);
    }
    helper.commit();
  }

  std::map<std::string, float> scores(std::string_view field) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").rawQuery() = qb::match(arena, field, "dune");
    req->collection("main").topDocs("q").fields({"id"}).limit(-1).getScores();
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::map<std::string, float> out;
    const auto* docs = req->docList();
    if (docs == nullptr) return out;
    const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    const auto& scored = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) out[std::string(ids[i])] = scored[i];
    return out;
  }
};

TEST_F(Bm25ParamsTest, k1ControlsTermFrequencySaturation) {
  auto def = scores("body");
  EXPECT_GT(def["repeated"], def["short"] * 1.1f);
  auto sat = scores("sat");
  EXPECT_NEAR(sat["repeated"], sat["short"], sat["short"] * 0.01f);
}

TEST_F(Bm25ParamsTest, bControlsLengthNormalization) {
  auto def = scores("body");
  EXPECT_GT(def["short"], def["long"] * 1.1f);
  auto nolen = scores("nolen");
  EXPECT_FLOAT_EQ(nolen["short"], nolen["long"]);
}

TEST_F(Bm25ParamsTest, validation) {
  auto rejects = [](FieldClass type, auto set) {
    SchemaBuilder b;
    auto& f = b.field("f");
    f.type = type;
    set(f);
    EXPECT_THROW(b.build(), SchemaError);
  };
  rejects(FieldClass::INT, [](api::FieldDef& f) { f.k1 = 1.0f; });
  rejects(FieldClass::STRING, [](api::FieldDef& f) { f.b = 0.5f; });
  rejects(FieldClass::TEXT, [](api::FieldDef& f) { f.k1 = -1.0f; });
  rejects(FieldClass::TEXT, [](api::FieldDef& f) { f.k1 = std::numeric_limits<float>::infinity(); });
  rejects(FieldClass::TEXT, [](api::FieldDef& f) { f.b = 1.5f; });
  rejects(FieldClass::TEXT, [](api::FieldDef& f) { f.b = std::nanf(""); });

  SchemaBuilder ok;
  auto& str = ok.field("s");
  str.type = FieldClass::STRING;
  str.k1 = 2.0f;
  EXPECT_NO_THROW(ok.build());
}
