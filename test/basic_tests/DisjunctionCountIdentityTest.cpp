#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/query/BooleanQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/SearchOverrides.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct IdentityGuard {
  bool saved = BooleanQuery::disableDisjunctionCountIdentityForTests;

  explicit IdentityGuard(bool disabled) {
    BooleanQuery::disableDisjunctionCountIdentityForTests = disabled;
  }

  ~IdentityGuard() {
    BooleanQuery::disableDisjunctionCountIdentityForTests = saved;
  }
};

struct FilterFoldGuard {
  bool saved = disableTopDocsFilterFold;

  explicit FilterFoldGuard(bool disabled) {
    disableTopDocsFilterFold = disabled;
  }

  ~FilterFoldGuard() {
    disableTopDocsFilterFold = saved;
  }
};

struct WholeMembershipPlanGuard {
  bool saved = QueryPrep::disableWholeMembershipPlanForTests;

  WholeMembershipPlanGuard() {
    QueryPrep::disableWholeMembershipPlanForTests = true;
  }

  ~WholeMembershipPlanGuard() {
    QueryPrep::disableWholeMembershipPlanForTests = saved;
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
    SkipStats::reset();
  }
};

} // namespace

class DisjunctionCountIdentityTest : public LuxirTest {
public:
  using QueryFactory = std::function<api::Query(OpCursor&)>;

  struct Run {
    int64_t count = 0;
    int64_t engagements = 0;
    int64_t deleteFallbacks = 0;
    int64_t filterFallbacks = 0;
    int64_t domainOutputFallbacks = 0;
    int64_t nonTermFallbacks = 0;
    int64_t minMatchFallbacks = 0;
    int64_t requiredFallbacks = 0;
    int64_t prohibitedFallbacks = 0;
    int64_t profitabilityFallbacks = 0;
    int64_t docBlocksDecoded = 0;
    int64_t bulkFillCalls = 0;
    int64_t bulkFillDocs = 0;
    int64_t bulkFillWordBlocks = 0;
  };

  CollectionHelper helper;

  static api::Query skewedTerms(OpCursor& cursor) {
    auto& mr = cursor.mr();
    return qb::boolean(mr, {},
        {qb::match(mr, "body_w", "common"),
         qb::match(mr, "body_w", "rare"),
         qb::match(mr, "body_w", "tiny")});
  }

  Run run(const QueryFactory& makeQuery, bool disableIdentity = false,
          bool materializeDomain = false, bool externalFilter = false) {
    IdentityGuard identityGuard(disableIdentity);
    FilterFoldGuard filterGuard(externalFilter);
    WholeMembershipPlanGuard wholeGuard;
    SkipStatsGuard statsGuard;

    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q").getNumber().limit(0);
    cursor.rawQuery() = makeQuery(cursor);
    if (externalFilter) {
      cursor.matchFilter("gate_s", "keep");
    }
    if (materializeDomain) {
      cursor.facet("buckets", "bucket_s").limit(-1);
    }
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    return {
      .count = req->getMatchCount(),
      .engagements = SkipStats::disjCountIdentityEngagements,
      .deleteFallbacks = SkipStats::disjCountIdentityDeleteFallbacks,
      .filterFallbacks = SkipStats::disjCountIdentityFilterFallbacks,
      .domainOutputFallbacks =
          SkipStats::disjCountIdentityDomainOutputFallbacks,
      .nonTermFallbacks = SkipStats::disjCountIdentityNonTermFallbacks,
      .minMatchFallbacks = SkipStats::disjCountIdentityMinMatchFallbacks,
      .requiredFallbacks = SkipStats::disjCountIdentityRequiredFallbacks,
      .prohibitedFallbacks =
          SkipStats::disjCountIdentityProhibitedFallbacks,
      .profitabilityFallbacks =
          SkipStats::disjCountIdentityProfitabilityFallbacks,
      .docBlocksDecoded = SkipStats::docBlocksDecoded,
      .bulkFillCalls = SkipStats::countBulkFillCalls,
      .bulkFillDocs = SkipStats::countBulkFillDocs,
      .bulkFillWordBlocks = SkipStats::countBulkFillWordBlocks,
    };
  }

  void SetUp() override {
    std::vector<Doc> docs;
    docs.reserve(256);
    for (int32_t doc = 0; doc < 256; doc++) {
      std::string body = "phrase lead ";
      if (doc < 250) body += "common ";
      if (doc == 0 || doc == 250 || doc == 251) body += "rare ";
      if (doc == 251 || doc == 252) body += "tiny ";
      if (doc < 128) body += "left ";
      if (doc >= 64 && doc < 192) body += "right ";
      docs.push_back(flatdoc(
          "id", "d" + std::to_string(doc),
          "body_w", body,
          "gate_s", (doc & 1) == 0 ? "keep" : "drop",
          "bucket_s", (doc & 1) == 0 ? "even" : "odd"));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
};

TEST_F(DisjunctionCountIdentityTest, skewedTermsMatchEnumeratedOracle) {
  Run enumerated = run(skewedTerms, true);
  Run identity = run(skewedTerms);

  EXPECT_EQ(253, identity.count);
  EXPECT_EQ(enumerated.count, identity.count);
  EXPECT_EQ(0, enumerated.engagements);
  EXPECT_GT(identity.engagements, 0);
  EXPECT_LT(identity.docBlocksDecoded, enumerated.docBlocksDecoded);
}

TEST_F(DisjunctionCountIdentityTest, balancedTermsStayOnWindowEnumeration) {
  auto balanced = [](OpCursor& cursor) {
    auto& mr = cursor.mr();
    return qb::boolean(mr, {},
        {qb::match(mr, "body_w", "left"),
         qb::match(mr, "body_w", "right")});
  };
  Run enumerated = run(balanced, true);
  Run normal = run(balanced);

  EXPECT_EQ(192, normal.count);
  EXPECT_EQ(enumerated.count, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.profitabilityFallbacks, 0);
  EXPECT_EQ(enumerated.docBlocksDecoded, normal.docBlocksDecoded);
  EXPECT_EQ(enumerated.bulkFillCalls, normal.bulkFillCalls);
  EXPECT_EQ(enumerated.bulkFillDocs, normal.bulkFillDocs);
  EXPECT_EQ(enumerated.bulkFillWordBlocks, normal.bulkFillWordBlocks);
}

TEST_F(DisjunctionCountIdentityTest, deletesFallBack) {
  helper.deleteById("d17", UpdateMessage::COMMIT);
  Run enumerated = run(skewedTerms, true);
  Run normal = run(skewedTerms);

  EXPECT_EQ(enumerated.count, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.deleteFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, externalFilterFallsBack) {
  Run enumerated = run(skewedTerms, true, false, true);
  Run normal = run(skewedTerms, false, false, true);

  EXPECT_EQ(enumerated.count, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.filterFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, domainOutputFallsBack) {
  Run enumerated = run(skewedTerms, true, true);
  Run normal = run(skewedTerms, false, true);

  EXPECT_EQ(enumerated.count, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.domainOutputFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, nonTermClauseFallsBack) {
  auto phraseAndTerm = [](OpCursor& cursor) {
    auto& mr = cursor.mr();
    return qb::boolean(mr, {},
        {qb::phraseWords(mr, "body_w", {"phrase", "lead"}),
         qb::match(mr, "body_w", "rare")});
  };
  Run normal = run(phraseAndTerm);

  EXPECT_EQ(256, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.nonTermFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, minShouldMatchFallsBack) {
  auto minMatch = [](OpCursor& cursor) {
    auto& mr = cursor.mr();
    return qb::boolean(mr, {},
        {qb::match(mr, "body_w", "common"),
         qb::match(mr, "body_w", "rare")}, {}, {}, 2);
  };
  Run normal = run(minMatch);

  EXPECT_EQ(1, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.minMatchFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, requiredClauseFallsBack) {
  auto required = [](OpCursor& cursor) {
    auto& mr = cursor.mr();
    return qb::boolean(mr,
        {qb::match(mr, "body_w", "common")},
        {qb::match(mr, "body_w", "rare"),
         qb::match(mr, "body_w", "right")}, {}, {}, 1);
  };
  Run normal = run(required);

  EXPECT_EQ(129, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.requiredFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, prohibitedClauseFallsBack) {
  auto prohibited = [](OpCursor& cursor) {
    auto& mr = cursor.mr();
    return qb::boolean(mr, {},
        {qb::match(mr, "body_w", "common"),
         qb::match(mr, "body_w", "left")},
        {qb::match(mr, "body_w", "rare")});
  };
  Run normal = run(prohibited);

  EXPECT_EQ(249, normal.count);
  EXPECT_EQ(0, normal.engagements);
  EXPECT_GT(normal.prohibitedFallbacks, 0);
}

TEST_F(DisjunctionCountIdentityTest, multiWindowIdentityStaysDocsOnly) {
  // The identity loop interleaves docs-only window fills with cursor
  // advances on the same term scorers. A corpus wider than one count window
  // forces an advance AFTER a fill, which must stay on the docs-only
  // protocol; the single-window corpora above never reach that interleave.
  CollectionHelper wide;
  const int32_t total = 12800;
  std::vector<Doc> docs;
  docs.reserve((size_t) total);
  for (int32_t doc = 0; doc < total; doc++) {
    std::string body = "filler ";
    if ((doc & 1) == 0) body += "wcommon ";
    if (doc == 1 || doc == 5000 || doc == 9000 || doc == 12799) {
      body += "wrare ";
    }
    docs.push_back(flatdoc(
        "id", "w" + std::to_string(doc),
        "body_w", body));
  }
  wide.indexAll(docs, UpdateMessage::COMMIT);

  auto countUnion = [&](bool disableIdentity) {
    IdentityGuard identityGuard(disableIdentity);
    WholeMembershipPlanGuard wholeGuard;
    SkipStatsGuard statsGuard;
    auto req = localReq(wide.getSearchEngine());
    req->collection("main");
    auto& cursor = req->topDocs("q").getNumber().limit(0);
    auto& mr = cursor.mr();
    cursor.rawQuery() = qb::boolean(mr, {},
        {qb::match(mr, "body_w", "wcommon"),
         qb::match(mr, "body_w", "wrare")});
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return std::pair<int64_t, int64_t>(
        req->getMatchCount(), SkipStats::disjCountIdentityEngagements);
  };

  auto [enumeratedCount, enumeratedEngagements] = countUnion(true);
  auto [identityCount, identityEngagements] = countUnion(false);

  // wcommon on 6400 evens; wrare adds odd docs 1 and 12799 (5000 and 9000
  // already match wcommon).
  EXPECT_EQ(6402, identityCount);
  EXPECT_EQ(enumeratedCount, identityCount);
  EXPECT_EQ(0, enumeratedEngagements);
  EXPECT_GT(identityEngagements, 0);
}
