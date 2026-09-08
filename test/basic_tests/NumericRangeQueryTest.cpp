// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "luxir/query/QueryBuilder.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/MemPool.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"

using namespace luxir;
using namespace luxir::test;

namespace {

namespace api = luxir::api;

// Numeric range (and numeric match, which is a degenerate range) executed by
// scanning the field's numeric column.  Tests assert the exact set of matching
// id_s values so bound handling, missing values, multi-valued "any", encoded
// sortable order, and two-phase conjunctions are all observable from results.
class NumericPredicateQueryTest : public LuxirTest {
protected:
  // Run a top-level query built from `build` (given the request arena) and
  // return the sorted id_s of the matching docs.
  std::vector<std::string> idsFor(auto&& build) {
    auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = lreq->collection("main").topDocs("q");
    cur.rawQuery() = build(cur.mr());
    cur.fields({"id_s"}).limit(1000);
    lreq->execute();
    std::vector<std::string> ids;
    const auto* docs = lreq->docList("q");
    if (docs && docs->columns.contains("id_s")) {
      const auto& col = std::get<luxir::api::ColStr>(docs->columns.at("id_s").kind);
      for (auto sv : col.v) ids.push_back(std::string(sv));
    }
    std::sort(ids.begin(), ids.end());
    lreq->done();
    return ids;
  }

  static std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
  }
};

// Convenience: an inclusive [gte, lte] int range with optional open sides.
auto i64Range(std::string_view field, std::optional<int64_t> gte, std::optional<int64_t> lte) {
  return [=](std::pmr::memory_resource& mr) {
    return qb::range(mr, field, gte ? qb::valI64(mr, *gte) : nullptr, nullptr,
                     lte ? qb::valI64(mr, *lte) : nullptr, nullptr);
  };
}

}  // namespace

TEST_F(NumericPredicateQueryTest, intBoundsMissingAndOpen) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "num_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "num_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "num_i", 15), UpdateMessage::COMMIT);
  // second segment
  helper.index(flatdoc("id_s", "d", "num_i", 20), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "e", "num_i", 25), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "f", "num_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "g", "num_i", 100), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "h"), UpdateMessage::COMMIT);  // no num_i

  // inclusive [10, 20]
  EXPECT_EQ(sorted({"b", "c", "d", "f"}), idsFor(i64Range("num_i", 10, 20)));
  // open upper: >= 10
  EXPECT_EQ(sorted({"b", "c", "d", "e", "f", "g"}), idsFor(i64Range("num_i", 10, std::nullopt)));
  // open lower: <= 20
  EXPECT_EQ(sorted({"a", "b", "c", "d", "f"}), idsFor(i64Range("num_i", std::nullopt, 20)));
  // no bounds: every doc that HAS a value (h has none, so excluded)
  EXPECT_EQ(sorted({"a", "b", "c", "d", "e", "f", "g"}),
            idsFor(i64Range("num_i", std::nullopt, std::nullopt)));

  // exclusive (10, 20) via gt/lt
  EXPECT_EQ(sorted({"c"}), idsFor([](std::pmr::memory_resource& mr) {
    return qb::range(mr, "num_i", nullptr, qb::valI64(mr, 10), nullptr, qb::valI64(mr, 20));
  }));
  // mixed: (10, 20]  -> gt=10, lte=20
  EXPECT_EQ(sorted({"c", "d"}), idsFor([](std::pmr::memory_resource& mr) {
    return qb::range(mr, "num_i", nullptr, qb::valI64(mr, 10), qb::valI64(mr, 20), nullptr);
  }));
  // empty range (lo > hi) matches nothing
  EXPECT_TRUE(idsFor(i64Range("num_i", 20, 10)).empty());
}

TEST_F(NumericPredicateQueryTest, numericMatchEquality) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "num_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "num_i", 10), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "num_i", 10), UpdateMessage::COMMIT);

  // Match via the range with equal bounds is also directly reachable as Match.
  EXPECT_EQ(sorted({"b", "c"}), idsFor(i64Range("num_i", 10, 10)));

  // Match with a string val -> coerced to the numeric column value.
  EXPECT_EQ(sorted({"b", "c"}), idsFor([](std::pmr::memory_resource& mr) {
    return qb::match(mr, "num_i", "10");
  }));
  // Match with a proto int arm (the common structured-client path).
  EXPECT_EQ(sorted({"b", "c"}), idsFor([](std::pmr::memory_resource& mr) {
    api::Query q;
    auto& m = q.kind.emplace<api::Match>();
    m.field = api::build::arenaStr(mr, "num_i");
    m.val = qb::valI64(mr, 10);
    return q;
  }));
  // No such value.
  EXPECT_TRUE(idsFor(i64Range("num_i", 999, 999)).empty());
}

TEST_F(NumericPredicateQueryTest, multiValuedAnyAndEmptyArray) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "prices_is", vec_i(20, 35, 45)), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "prices_is", 35), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "prices_is", vec_i()), UpdateMessage::NO_COMMIT);  // empty array
  helper.index(flatdoc("id_s", "d", "prices_is", 5), UpdateMessage::COMMIT);

  // A doc matches if ANY value is in range.
  EXPECT_EQ(sorted({"a", "b"}), idsFor(i64Range("prices_is", 30, 40)));
  EXPECT_EQ(sorted({"d"}), idsFor(i64Range("prices_is", 1, 10)));
  // Numeric match against a multi-valued field: any value equals.
  EXPECT_EQ(sorted({"a", "b"}), idsFor(i64Range("prices_is", 35, 35)));
  // No bounds: docs WITH a value, but an empty array is not a value (c excluded).
  EXPECT_EQ(sorted({"a", "b", "d"}), idsFor(i64Range("prices_is", std::nullopt, std::nullopt)));
}

TEST_F(NumericPredicateQueryTest, floatSortableOrderAndSignedZero) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "val_f", -3.5f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "val_f", -0.5f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "n", "val_f", -0.0f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "val_f", 0.0f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "val_f", 0.5f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "e", "val_f", 3.5f), UpdateMessage::COMMIT);

  auto fRange = [](std::string_view field, std::optional<double> gte, std::optional<double> lte) {
    return [=](std::pmr::memory_resource& mr) {
      return qb::range(mr, field, gte ? qb::valF64(mr, *gte) : nullptr, nullptr,
                       lte ? qb::valF64(mr, *lte) : nullptr, nullptr);
    };
  };

  // [-1.0, 1.0] includes b(-0.5), n(-0.0), c(0.0), d(0.5).
  EXPECT_EQ(sorted({"b", "c", "d", "n"}), idsFor(fRange("val_f", -1.0, 1.0)));
  // Encoded sortable order: -0.0 sorts below +0.0, so gte=+0.0 excludes n(-0.0).
  EXPECT_EQ(sorted({"c", "d", "e"}), idsFor(fRange("val_f", 0.0, std::nullopt)));
  // gte=-0.0 includes both -0.0 and +0.0.
  EXPECT_EQ(sorted({"c", "d", "e", "n"}), idsFor(fRange("val_f", -0.0, std::nullopt)));
  // exclusive lower
  EXPECT_EQ(sorted({"c", "d", "e", "n"}), idsFor([](std::pmr::memory_resource& mr) {
    return qb::range(mr, "val_f", nullptr, qb::valF64(mr, -0.5), nullptr, nullptr);
  }));
}

TEST_F(NumericPredicateQueryTest, doubleRange) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "val_d", -2.5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "val_d", 3.14), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "val_d", 1e10), UpdateMessage::COMMIT);

  auto dRange = [](std::optional<double> gte, std::optional<double> lte) {
    return [=](std::pmr::memory_resource& mr) {
      return qb::range(mr, "val_d", gte ? qb::valF64(mr, *gte) : nullptr, nullptr,
                       lte ? qb::valF64(mr, *lte) : nullptr, nullptr);
    };
  };
  EXPECT_EQ(sorted({"b", "c"}), idsFor(dRange(0.0, std::nullopt)));
  EXPECT_EQ(sorted({"a", "b"}), idsFor(dRange(-3.0, 100.0)));
  EXPECT_EQ(sorted({"c"}), idsFor(dRange(1e9, std::nullopt)));
}

TEST_F(NumericPredicateQueryTest, dateRangeIsoStrings) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "when_dt", "2000-01-01"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "when_dt", "2010-06-15T12:00:00Z"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "when_dt", "2020-12-31"), UpdateMessage::COMMIT);

  auto dtRange = [](const char* gte, const char* lte, const char* gt, const char* lt) {
    return [=](std::pmr::memory_resource& mr) {
      return qb::range(mr, "when_dt", gte ? qb::valStr(mr, gte) : nullptr,
                       gt ? qb::valStr(mr, gt) : nullptr, lte ? qb::valStr(mr, lte) : nullptr,
                       lt ? qb::valStr(mr, lt) : nullptr);
    };
  };
  EXPECT_EQ(sorted({"b"}), idsFor(dtRange("2005-01-01", "2015-01-01", nullptr, nullptr)));
  EXPECT_EQ(sorted({"b", "c"}), idsFor(dtRange("2010-06-15T12:00:00Z", nullptr, nullptr, nullptr)));
  // exclusive upper at exactly b's instant excludes b
  EXPECT_EQ(sorted({"a"}), idsFor(dtRange(nullptr, nullptr, nullptr, "2010-06-15T12:00:00Z")));
}

TEST_F(NumericPredicateQueryTest, conjunctionAndConstantScore) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "text_w", "red apple", "num_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "text_w", "red apple", "num_i", 15), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "c", "text_w", "green apple", "num_i", 15), UpdateMessage::COMMIT);

  // term leads, range verifies (two-phase): only b is red AND in [10,20].
  EXPECT_EQ(sorted({"b"}), idsFor([](std::pmr::memory_resource& mr) {
    return qb::boolean(mr, {qb::match(mr, "text_w", "red"),
                            qb::range(mr, "num_i", qb::valI64(mr, 10), nullptr,
                                      qb::valI64(mr, 20), nullptr)});
  }));

  // constant_score(range) preserves two-phase in the same conjunction.
  EXPECT_EQ(sorted({"b"}), idsFor([](std::pmr::memory_resource& mr) {
    auto r = qb::range(mr, "num_i", qb::valI64(mr, 10), nullptr, qb::valI64(mr, 20), nullptr);
    return qb::boolean(mr, {qb::match(mr, "text_w", "red"), qb::constantScore(mr, r)});
  }));

  // constant_score(range) standalone still matches the range's docs.
  EXPECT_EQ(sorted({"b", "c"}), idsFor([](std::pmr::memory_resource& mr) {
    auto r = qb::range(mr, "num_i", qb::valI64(mr, 10), nullptr, qb::valI64(mr, 20), nullptr);
    return qb::constantScore(mr, r);
  }));
}

// Builder-level validation (no index needed): pair rules, field-type gating,
// and deterministic coercion of every supplied bound.
TEST(NumericRangeBuilder, validation) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  api::Val a; a.kind = (int64_t)5;
  api::Val b; b.kind = (int64_t)10;
  // At most one of gte/gt and one of lte/lt.
  EXPECT_THROW(builder.createRangeQuery("num_i", &a, &b, nullptr, nullptr), std::runtime_error);
  EXPECT_THROW(builder.createRangeQuery("num_i", nullptr, nullptr, &a, &b), std::runtime_error);
  // Term-backed fields build a term range (numeric Vals coerce to term text).
  EXPECT_NE(builder.createRangeQuery("text_w", &a, nullptr, &b, nullptr), nullptr);
  // Ranges do not apply to vector fields.
  EXPECT_THROW(builder.createRangeQuery("emb_v", &a, nullptr, &b, nullptr), std::runtime_error);
  // A malformed bound errors deterministically even when the other side would
  // already collapse the range to empty (gt=INT64_MAX).
  api::Val maxV; maxV.kind = std::numeric_limits<int64_t>::max();
  api::Val bad; bad.kind = std::string_view("notanumber");
  EXPECT_THROW(builder.createRangeQuery("num_i", nullptr, &maxV, nullptr, &bad), std::runtime_error);
  // A valid range builds.
  EXPECT_NE(builder.createRangeQuery("num_i", &a, nullptr, &b, nullptr), nullptr);
}

TEST_F(NumericPredicateQueryTest, int64Extremes) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "lo", "num_i", std::numeric_limits<int64_t>::min()),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "mid", "num_i", 0), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "hi", "num_i", std::numeric_limits<int64_t>::max()),
               UpdateMessage::COMMIT);

  const int64_t IMAX = std::numeric_limits<int64_t>::max();
  const int64_t IMIN = std::numeric_limits<int64_t>::min();

  // gt INT64_MAX cannot overflow into a bound: it is an empty range.
  EXPECT_TRUE(idsFor([=](std::pmr::memory_resource& mr) {
    return qb::range(mr, "num_i", nullptr, qb::valI64(mr, IMAX), nullptr, nullptr);
  }).empty());
  // gte INT64_MAX matches the max doc.
  EXPECT_EQ(sorted({"hi"}), idsFor(i64Range("num_i", IMAX, std::nullopt)));
  // lt INT64_MIN is an empty range.
  EXPECT_TRUE(idsFor([=](std::pmr::memory_resource& mr) {
    return qb::range(mr, "num_i", nullptr, nullptr, nullptr, qb::valI64(mr, IMIN));
  }).empty());
  // lte INT64_MIN matches the min doc.
  EXPECT_EQ(sorted({"lo"}), idsFor(i64Range("num_i", std::nullopt, IMIN)));
  // full span matches all with a value.
  EXPECT_EQ(sorted({"hi", "lo", "mid"}), idsFor(i64Range("num_i", IMIN, IMAX)));
}
