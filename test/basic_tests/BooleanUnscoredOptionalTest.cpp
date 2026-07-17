#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "solux/query/BooleanQuery.h"
#include "solux/reader/SkipStats.h"

using namespace solux;
using namespace solux::test;

namespace {

struct UnscoredOptionalDropGuard {
  bool saved = BooleanQuery::Weight::disableUnscoredOptionalDropForTests;

  explicit UnscoredOptionalDropGuard(bool disabled) {
    BooleanQuery::Weight::disableUnscoredOptionalDropForTests = disabled;
  }

  ~UnscoredOptionalDropGuard() {
    BooleanQuery::Weight::disableUnscoredOptionalDropForTests = saved;
  }
};

struct SkipStatsGuard {
  bool saved = SkipStats::enabled;

  SkipStatsGuard() {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
  }
};

} // namespace

class BooleanUnscoredOptionalTest : public SoluxTest {
public:
  enum class Shape {
    REQUIRED_DISJUNCTION,
    MANDATORY_OPTIONAL,
    MANDATORY_OPTIONAL_NOT,
    FILTERED,
    PURE_DISJUNCTION,
    MIN_SHOULD_MATCH,
  };

  struct ScoredRun {
    std::vector<std::pair<std::string, uint32_t>> hits;
    int64_t count = 0;

    bool operator==(const ScoredRun&) const = default;
  };

  CollectionHelper helper;

  api::Query makeQuery(OpCursor& cursor, Shape shape) {
    auto& mr = cursor.mr();
    auto term = [&](std::string_view value) {
      return qb::match(mr, "body_w", value);
    };
    auto disjunction = [&] {
      return qb::boolean(mr, {}, {term("a"), term("b")});
    };

    switch (shape) {
      case Shape::REQUIRED_DISJUNCTION:
        return qb::boolean(mr, {disjunction()}, {term("c"), term("d")});
      case Shape::MANDATORY_OPTIONAL:
        return qb::boolean(mr, {term("a")}, {term("b")});
      case Shape::MANDATORY_OPTIONAL_NOT:
        return qb::boolean(mr, {term("a")}, {term("b")}, {term("c")});
      case Shape::FILTERED:
        return qb::boolean(mr, {disjunction()}, {term("c")}, {},
                           {qb::match(mr, "gate_s", "keep")});
      case Shape::PURE_DISJUNCTION:
        return qb::boolean(mr, {}, {term("a"), term("b"), term("c")});
      case Shape::MIN_SHOULD_MATCH:
        return qb::boolean(mr, {term("a")}, {term("b"), term("c")}, {}, {}, 1);
    }
    throw std::runtime_error("unknown test shape");
  }

  int64_t runCount(Shape shape, bool disableDrop) {
    UnscoredOptionalDropGuard guard(disableDrop);
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q");
    cursor.getNumber().limit(0);
    cursor.rawQuery() = makeQuery(cursor, shape);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount();
  }

  std::map<std::string, int64_t> runFacet(Shape shape, bool disableDrop) {
    UnscoredOptionalDropGuard guard(disableDrop);
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q");
    cursor.getNumber().limit(0);
    cursor.rawQuery() = makeQuery(cursor, shape);
    cursor.facet("buckets", "bucket_s").limit(-1);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    std::map<std::string, int64_t> out;
    const auto* list = req->docList("q");
    if (list == nullptr) return out;
    const auto* value = list->ops.find("buckets");
    if (value == nullptr) return out;
    const auto* facet = std::get_if<api::FacetResult>(&(*value)->kind);
    if (facet == nullptr || !facet->bucket_ids.has_value()) return out;
    const auto* ids = std::get_if<api::ColStr>(&facet->bucket_ids->kind);
    if (ids == nullptr || ids->v.size() != facet->counts.size()) return out;
    for (size_t i = 0; i < ids->v.size(); i++) {
      out.emplace(std::string(ids->v[i]), facet->counts[i]);
    }
    return out;
  }

  ScoredRun runScored(Shape shape, bool getNumber, bool disableDrop) {
    UnscoredOptionalDropGuard guard(disableDrop);
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q");
    cursor.getScores().limit(3).batchSize(4).fields({"id"});
    if (getNumber) cursor.getNumber();
    cursor.rawQuery() = makeQuery(cursor, shape);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    ScoredRun out;
    out.count = getNumber ? req->getMatchCount() : 0;
    const auto* list = req->docList("q");
    if (list == nullptr) return out;
    const auto* idColumn = list->columns.find("id");
    const auto* scoreColumn = list->columns.find("_score_");
    if (idColumn == nullptr || scoreColumn == nullptr) return out;
    const auto* ids = std::get_if<api::ColStr>(&idColumn->kind);
    const auto* scores = std::get_if<api::ColFloat>(&scoreColumn->kind);
    if (ids == nullptr || scores == nullptr || ids->v.size() != scores->v.size()) return out;
    for (size_t i = 0; i < ids->v.size(); i++) {
      out.hits.emplace_back(std::string(ids->v[i]),
                            std::bit_cast<uint32_t>(scores->v[i]));
    }
    return out;
  }

  void SetUp() override {
    std::vector<Doc> docs = {
      flatdoc("id", "d0", "body_w", "a c", "gate_s", "keep", "bucket_s", "x"),
      flatdoc("id", "d1", "body_w", "b d", "gate_s", "keep", "bucket_s", "y"),
      flatdoc("id", "d2", "body_w", "a b c d", "gate_s", "drop", "bucket_s", "x"),
      flatdoc("id", "d3", "body_w", "a", "gate_s", "drop", "bucket_s", "y"),
      flatdoc("id", "d4", "body_w", "b", "gate_s", "drop", "bucket_s", "x"),
      flatdoc("id", "d5", "body_w", "c d", "gate_s", "keep", "bucket_s", "y"),
      flatdoc("id", "d6", "body_w", "a c e", "gate_s", "keep", "bucket_s", "x"),
      flatdoc("id", "d7", "body_w", "b e", "gate_s", "drop", "bucket_s", "y"),
      flatdoc("id", "d8", "body_w", "c", "gate_s", "keep", "bucket_s", "x"),
      flatdoc("id", "d9", "body_w", "e", "gate_s", "keep", "bucket_s", "y"),
    };
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
};

TEST_F(BooleanUnscoredOptionalTest, exactCountsAndFacetDomainsMatchWithoutOptionals) {
  struct Case {
    Shape shape;
    int64_t expected;
  };
  const std::array cases = {
    Case{Shape::REQUIRED_DISJUNCTION, 7},
    Case{Shape::MANDATORY_OPTIONAL, 4},
    Case{Shape::MANDATORY_OPTIONAL_NOT, 1},
    Case{Shape::FILTERED, 3},
    Case{Shape::PURE_DISJUNCTION, 9},
    Case{Shape::MIN_SHOULD_MATCH, 3},
  };

  for (const auto& testCase : cases) {
    EXPECT_EQ(testCase.expected, runCount(testCase.shape, false));
    EXPECT_EQ(testCase.expected, runCount(testCase.shape, true));
  }

  auto reducedFacet = runFacet(Shape::REQUIRED_DISJUNCTION, false);
  auto retainedFacet = runFacet(Shape::REQUIRED_DISJUNCTION, true);
  EXPECT_EQ(retainedFacet, reducedFacet);
  EXPECT_EQ((std::map<std::string, int64_t>{{"x", 4}, {"y", 3}}), reducedFacet);
}

TEST_F(BooleanUnscoredOptionalTest, requiredDisjunctionCountUsesBulkUnion) {
  SkipStatsGuard stats;
  EXPECT_EQ(7, runCount(Shape::REQUIRED_DISJUNCTION, false));
  EXPECT_GT(SkipStats::countBulkFillCalls, 0);
}

TEST_F(BooleanUnscoredOptionalTest, scoredTopKAndTopKCountAreUnchanged) {
  struct Case {
    Shape shape;
    int64_t expected;
  };
  const std::array cases = {
    Case{Shape::REQUIRED_DISJUNCTION, 7},
    Case{Shape::MANDATORY_OPTIONAL, 4},
    Case{Shape::MANDATORY_OPTIONAL_NOT, 1},
    Case{Shape::FILTERED, 3},
    Case{Shape::PURE_DISJUNCTION, 9},
    Case{Shape::MIN_SHOULD_MATCH, 3},
  };

  for (const auto& testCase : cases) {
    auto topK = runScored(testCase.shape, false, false);
    EXPECT_EQ(topK, runScored(testCase.shape, false, true));

    auto topKCount = runScored(testCase.shape, true, false);
    EXPECT_EQ(topKCount, runScored(testCase.shape, true, true));
    EXPECT_EQ(topK.hits, topKCount.hits);
    EXPECT_EQ(testCase.expected, topKCount.count);
  }
}
