// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Field facets over numeric fields with a term dictionary (index: match) count
// from the dictionary; every strategy must return exactly what the column scan
// of the same values returns.

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include "luxir/search/SearchOverrides.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using FC = api::FieldDef::FieldClass;
using IM = api::FieldDef::IndexMode;
using MM = api::FieldDef::MultiMode;

enum class Domain { ALL, WIDE, NARROW, SPARSE };

struct FacetShape {
  int64_t limit;
  int64_t mincount;
  bool missing;
  bool selectAndSum;
};

void configure(OpCursor& facet, const FacetShape& shape) {
  facet.limit(shape.limit);
  if (shape.mincount > 0) facet.mincount(shape.mincount);
  auto& raw = std::get<api::FieldFacet>(facet.rawOp().kind);
  raw.missing = shape.missing;
  if (!shape.selectAndSum) return;
  auto* sequence = api::build::allocMessage<api::Val>(facet.mr());
  auto& integers = sequence->kind.emplace<api::ArrInt>();
  auto* selected = api::build::allocArray(integers.v, 2, facet.mr());
  selected[0] = 7;
  selected[1] = 99;
  raw.selected = sequence;
  facet.sum("total", "w");
}

std::vector<std::byte> facetBytes(CollectionHelper& helper, std::string_view field, Domain domain,
                                  const FacetShape& shape, std::string* details = nullptr) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  if (details) req->profile();
  // Selections need a TopDocs parent; an unfiltered one covers every document.
  bool root = domain == Domain::ALL && !shape.selectAndSum;
  if (root) {
    configure(req->facet("f", field), shape);
  } else {
    auto& top = req->topDocs("q").limit(0).countLimit();
    if (domain != Domain::ALL) {
      top.matchQuery(domain == Domain::SPARSE ? "r" : "g",
                     domain == Domain::WIDE ? "hi" : domain == Domain::NARROW ? "lo" : "yes");
    }
    configure(top.facet("f", field), shape);
  }
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  if (!req->ok()) return {};
  const api::FacetResult* result = root ? req->responses[0]->proto.ops.at("f")->facetResult()
                                        : req->docList("q")->ops.at("f")->facetResult();
  std::vector<std::byte> encoded;
  EXPECT_TRUE(api::encode(*result, encoded));
  if (details && req->responses[0]->proto.profile) {
    for (const auto& op : req->responses[0]->proto.profile->ops) {
      for (const auto& piece : op.pieces) {
        for (const auto& detail : piece.details) *details += std::string(detail) + ";";
      }
    }
  }
  return encoded;
}

void setSchema(CollectionHelper& helper, FC type, MM multi) {
  SchemaBuilder schema;
  for (auto [name, index] : {std::pair{"n", IM::NONE}, {"m", IM::MATCH}}) {
    auto& f = schema.field(name);
    f.type = type;
    f.index = index;
    f.multi = multi;
  }
  auto& w = schema.field("w");
  w.type = FC::INT;
  w.multi = MM::SINGLE;
  for (auto name : {"g", "r"}) {
    auto& f = schema.field(name);
    f.type = FC::STRING;
    f.multi = MM::SINGLE;
  }
  schema.set(helper.collection());
}
} // namespace

class NumericTermsFacetTest : public LuxirTest {};

TEST_F(NumericTermsFacetTest, dictionaryStrategiesMatchColumnScan) {
  const double doubles[] = {-2.5, -0.0, 0.0, 1.5, 7, std::numeric_limits<double>::quiet_NaN(),
                            -std::numeric_limits<double>::infinity(), 1e300, 7, 7};
  for (auto type : {FC::INT, FC::DOUBLE}) {
    for (auto multi : {MM::SINGLE, MM::SET, MM::ARRAY}) {
      SCOPED_TRACE((int)type);
      SCOPED_TRACE((int)multi);
      CollectionHelper helper;
      helper.clear();
      setSchema(helper, type, multi);
      helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
      bool integral = type == FC::INT;
      for (int i = 0; i < 360; i++) {
        Doc doc = flatdoc("id", std::to_string(i), "w", i % 11,
                          "g", i % 10 < 3 ? "lo" : "hi", "r", i % 40 == 0 ? "yes" : "no");
        if (i % 9 != 0) {
          int k = (i * 7) % 10, k2 = (i * 3 + 1) % 10;
          for (auto name : {"n", "m"}) {
            if (multi == MM::SINGLE) {
              if (integral) doc.push_back({name, (int64_t)(k - 3)});
              else doc.push_back({name, doubles[k]});
            } else if (integral) {
              doc.push_back({name, vec_i((int64_t)(k - 3), (int64_t)(k2 - 3), (int64_t)(k - 3))});
            } else {
              doc.push_back({name, vec(doubles[k], doubles[k2], doubles[k])});
            }
          }
        }
        ASSERT_TRUE(helper.index(doc, i % 120 == 119 ? UpdateMessage::COMMIT
                                                     : UpdateMessage::NO_COMMIT).success);
      }
      for (int stage = 0; stage < 3; stage++) {
        SCOPED_TRACE(stage);
        for (auto domain : {Domain::ALL, Domain::WIDE, Domain::NARROW, Domain::SPARSE}) {
          SCOPED_TRACE((int)domain);
          for (auto shape : {FacetShape{4, 0, true, true}, FacetShape{-1, 2, false, false}}) {
            SCOPED_TRACE(shape.limit);
            auto expected = facetBytes(helper, "n", domain, shape);
            for (auto strategy : {StrFacetStrategy::AUTO, StrFacetStrategy::COLUMN_DOMAIN,
                                  StrFacetStrategy::COLUMN_COMPLEMENT, StrFacetStrategy::TERM_DRIVEN}) {
              for (auto source : {FacetBucketDomainSource::AUTO, FacetBucketDomainSource::POSTINGS}) {
                SCOPED_TRACE((int)strategy);
                SCOPED_TRACE((int)source);
                SearchOverridesGuard guard(forcedStrFacetStrategy, forcedFacetBucketDomainSource);
                forcedStrFacetStrategy = strategy;
                forcedFacetBucketDomainSource = source;
                EXPECT_EQ(expected, facetBytes(helper, "m", domain, shape));
              }
            }
          }
        }
        if (stage == 0) {
          ASSERT_TRUE(helper.deleteByIds({"3", "40", "121", "200", "359"}, UpdateMessage::COMMIT).success);
        }
        if (stage == 1) {
          CollectionHelper::UpdateBuilder merge;
          merge.commit(true, 1);
          ASSERT_TRUE(helper.submit(merge).success);
          ASSERT_EQ(1u, helper.durableSegmentCount());
        }
      }
    }
  }
}

// The planner reads the dictionary only where it is cheaper than the column:
// a low-cardinality field walks its terms for an all-docs domain, and a field
// with a distinct value per document keeps the column.
TEST_F(NumericTermsFacetTest, plannerWalksDictionaryForLowCardinality) {
  CollectionHelper helper;
  SchemaBuilder schema;
  for (auto name : {"low", "unique"}) {
    auto& f = schema.field(name);
    f.type = FC::INT;
    f.index = IM::MATCH;
    f.multi = MM::SINGLE;
  }
  schema.set(helper.collection());
  std::vector<Doc> docs;
  for (int i = 0; i < 4096; i++) docs.push_back(flatdoc("id", std::to_string(i), "low", i % 50, "unique", i));
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  FacetShape shape{10, 0, true, false};
  std::string low, unique;
  facetBytes(helper, "low", Domain::ALL, shape, &low);
  facetBytes(helper, "unique", Domain::ALL, shape, &unique);
  EXPECT_NE(std::string::npos, low.find("docFreq-only dictionary walk")) << low;
  EXPECT_NE(std::string::npos, unique.find("parent-count=column-domain")) << unique;
}
