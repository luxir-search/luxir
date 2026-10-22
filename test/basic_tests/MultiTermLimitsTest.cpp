// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestIndex.h"
#include "luxir/query/MultiTermBudget.h"
#include "luxir/reader/DfaIntersectEnum.h"
#include "luxir/util/automaton/WildcardCompiler.h"

using namespace luxir;
using namespace luxir::test;

// Request-level guards on term-dictionary expansion (MultiTermBudget).
class MultiTermLimitsTest : public LuxirTest {
protected:
  static constexpr int32_t TERMS = 100;

  // One segment whose tag_s dictionary is t000..t099, one term per doc.
  static void indexTerms(CollectionHelper& helper) {
    std::vector<Doc> docs;
    for (int32_t i = 0; i < TERMS; i++) {
      char tag[8];
      snprintf(tag, sizeof(tag), "t%03d", i);
      docs.push_back(flatdoc("id", "d" + std::to_string(i), "tag_s", tag));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }

  static LuxirConfig config(int32_t maxExpansions, int64_t maxExamined,
                            int32_t minPrefix = 1) {
    LuxirConfig config;
    config.search.multiterm_max_expansions = maxExpansions;
    config.search.multiterm_max_terms_examined = maxExamined;
    config.search.multiterm_min_prefix_length = minPrefix;
    return config;
  }
};

TEST_F(MultiTermLimitsTest, serverDefaultsMatchBudgetDefaults) {
  SearchConfig search;
  MultiTermLimits limits;
  EXPECT_EQ(limits.maxExpansions, search.multiterm_max_expansions);
  EXPECT_EQ(limits.maxTermsExamined, search.multiterm_max_terms_examined);
  EXPECT_EQ(limits.maxPostings, search.multiterm_max_postings);
  EXPECT_EQ(limits.minPrefixLength, search.multiterm_min_prefix_length);
}

TEST_F(MultiTermLimitsTest, completeExpansionRejectedAtServerCap) {
  LuxirNode node(config(10, 1'000'000));
  CollectionHelper helper(node);
  indexTerms(helper);

  auto narrow = localReq(helper.getSearchEngine());
  narrow->collection("main").topDocs("q").prefixQuery("tag_s", "t00").limit(100);
  narrow->execute();
  ASSERT_TRUE(narrow->ok()) << narrow->errorMsg();
  EXPECT_EQ(10u, narrow->getDocs().size());

  auto wide = localReq(helper.getSearchEngine());
  wide->collection("main").topDocs("q").prefixQuery("tag_s", "t0").limit(200);
  wide->execute(false);
  EXPECT_FALSE(wide->ok());
  EXPECT_EQ("too_many_expansions", wide->errorCode());
  EXPECT_NE(wide->errorMsg().find("matches more than 10 terms"),
            std::string::npos) << wide->errorMsg();

  // Patterns are held to the same cap.
  auto pattern = localReq(helper.getSearchEngine());
  pattern->collection("main").topDocs("q").wildcardQuery("tag_s", "t*1").limit(200);
  pattern->execute();
  ASSERT_TRUE(pattern->ok()) << pattern->errorMsg();
  EXPECT_EQ(10u, pattern->getDocs().size());
  auto widePattern = localReq(helper.getSearchEngine());
  widePattern->collection("main").topDocs("q").regexQuery("tag_s", "t0.*").limit(200);
  widePattern->execute(false);
  EXPECT_EQ("too_many_expansions", widePattern->errorCode());
}

TEST_F(MultiTermLimitsTest, truncatingPrefixStaysUnderTheCap) {
  // A prefix that keeps its top terms expands to at most max_expansions, so
  // the server cap on complete expansions does not apply; the scan budget does.
  LuxirNode node(config(10, 1'000'000));
  CollectionHelper helper(node);
  indexTerms(helper);

  auto capped = localReq(helper.getSearchEngine());
  capped->collection("main").topDocs("q").prefixQuery("tag_s", "t", 5).limit(200);
  capped->execute();
  ASSERT_TRUE(capped->ok()) << capped->errorMsg();
  EXPECT_EQ(5u, capped->getDocs().size());

  LuxirNode tight(config(10, 40));
  CollectionHelper tightHelper(tight);
  indexTerms(tightHelper);
  auto scanned = localReq(tightHelper.getSearchEngine());
  scanned->collection("main").topDocs("q").prefixQuery("tag_s", "t", 5).limit(200);
  scanned->execute(false);
  EXPECT_FALSE(scanned->ok());
  EXPECT_EQ("too_many_terms_examined", scanned->errorCode());
}

TEST_F(MultiTermLimitsTest, zeroHitScanStopsAtTermsExaminedBudget) {
  // minPrefix 0 leaves the up-front check out, so the scan itself must stop.
  LuxirNode node(config(1000, 40, 0));
  CollectionHelper helper(node);
  indexTerms(helper);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").wildcardQuery("tag_s", "*zz*");
  req->execute(false);
  EXPECT_FALSE(req->ok());
  EXPECT_EQ("too_many_terms_examined", req->errorCode());
  EXPECT_NE(req->errorMsg().find("wildcard query '*zz*' on field 'tag_s'"),
            std::string::npos) << req->errorMsg();

  auto bounded = localReq(helper.getSearchEngine());
  bounded->collection("main").topDocs("q").wildcardQuery("tag_s", "t00*zz*");
  bounded->execute();
  ASSERT_TRUE(bounded->ok()) << bounded->errorMsg();
  EXPECT_TRUE(bounded->getDocs().empty());
}

TEST_F(MultiTermLimitsTest, termsExaminedBudgetIsRequestWide) {
  // Each 100-term scan fits a 150-term budget alone, but not two in one request.
  LuxirNode node(config(1000, 150, 0));
  CollectionHelper helper(node);
  indexTerms(helper);

  auto single = localReq(helper.getSearchEngine());
  single->collection("main").topDocs("q").wildcardQuery("tag_s", "t0*9").limit(100);
  single->execute();
  ASSERT_TRUE(single->ok()) << single->errorMsg();
  EXPECT_EQ(10u, single->getDocs().size());

  auto both = localReq(helper.getSearchEngine());
  both->collection("main").topDocs("q").wildcardQuery("tag_s", "t0*9")
      .filter(qb::wildcard(both->mr, "tag_s", "t0*1")).limit(100);
  both->execute(false);
  EXPECT_FALSE(both->ok());
  EXPECT_EQ("too_many_terms_examined", both->errorCode());
}

TEST_F(MultiTermLimitsTest, shortPrefixScanRejectedBeforeScanning) {
  LuxirNode node(config(1000, 40));
  CollectionHelper helper(node);
  indexTerms(helper);

  // No literal prefix and an unbounded language: the whole 100-term
  // dictionary is the cost, more than the 40-term budget.
  auto leading = localReq(helper.getSearchEngine());
  leading->collection("main").topDocs("q").regexQuery("tag_s", ".*5");
  leading->execute(false);
  EXPECT_FALSE(leading->ok());
  EXPECT_EQ("pattern_prefix_too_short", leading->errorCode());
  EXPECT_NE(leading->errorMsg().find("must scan 100 dictionary terms"),
            std::string::npos) << leading->errorMsg();

  // A finite language seeks term by term, so it needs no literal prefix.
  auto finite = localReq(helper.getSearchEngine());
  finite->collection("main").topDocs("q").regexQuery("tag_s", "(t001|t042)").limit(10);
  finite->execute();
  ASSERT_TRUE(finite->ok()) << finite->errorMsg();
  EXPECT_EQ(2u, finite->getDocs().size());

  // With a literal prefix the scan runs, bounded by the budget as it goes.
  auto prefixed = localReq(helper.getSearchEngine());
  prefixed->collection("main").topDocs("q").wildcardQuery("tag_s", "t09*5").limit(10);
  prefixed->execute();
  ASSERT_TRUE(prefixed->ok()) << prefixed->errorMsg();
  EXPECT_EQ(1u, prefixed->getDocs().size());
}

TEST_F(MultiTermLimitsTest, fuzzyExpansionSharesTheBudget) {
  LuxirNode node(config(1000, 5, 0));
  CollectionHelper helper(node);
  indexTerms(helper);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").fuzzyQuery("tag_s", "t050", 2, 0);
  req->execute(false);
  EXPECT_FALSE(req->ok());
  EXPECT_EQ("too_many_terms_examined", req->errorCode());
  EXPECT_NE(req->errorMsg().find("fuzzy query 't050'"), std::string::npos) << req->errorMsg();
}

TEST_F(MultiTermLimitsTest, postingsBudgetBoundsTheUnion) {
  LuxirConfig limited = config(1000, 1'000'000);
  limited.search.multiterm_max_postings = 20;
  LuxirNode node(limited);
  CollectionHelper helper(node);
  indexTerms(helper);

  auto fits = localReq(helper.getSearchEngine());
  fits->collection("main").topDocs("q").prefixQuery("tag_s", "t01").limit(200);
  fits->execute();
  ASSERT_TRUE(fits->ok()) << fits->errorMsg();

  auto over = localReq(helper.getSearchEngine());
  over->collection("main").topDocs("q").prefixQuery("tag_s", "t0").limit(200);
  over->execute(false);
  EXPECT_FALSE(over->ok());
  EXPECT_EQ("too_many_expansion_postings", over->errorCode());
}

// The visit limit stops every scan shape at exactly the limit.
TEST_F(MultiTermLimitsTest, enumsStopAtVisitLimit) {
  TestIndex index;
  TestField field{index, "value_s"};
  field.startIndexing();
  for (int32_t i = 0; i < TERMS; i++) field.add(i, "v" + std::to_string(1000 + i));
  index.flush();
  field.startReading();
  auto* segment = field.currentSegment();

  auto drain = [](FilteredTermsEnum& terms) {
    int32_t accepted = 0;
    while (terms.next()) accepted++;
    return accepted;
  };

  {
    TermsEnum source(index.pool, segment->postingsReader(), field.fieldInfo);
    PrefixTermsEnum terms(source, "v");
    terms.setVisitLimit(30);
    EXPECT_EQ(30, drain(terms));
    EXPECT_TRUE(terms.stoppedAtVisitLimit());
    EXPECT_EQ(30, terms.termsVisited());
  }
  {
    automaton::Budget budget;
    automaton::ByteDfa dfa = automaton::compileWildcard("*7", budget);
    auto view = dfa.view();
    auto [prefix, state] = view.commonPrefixAndState();
    std::string suffix = dfa.commonSuffix();
    TermsEnum source(index.pool, segment->postingsReader(), field.fieldInfo);
    DfaIntersectEnum terms(index.pool, source, view, DfaScanPlan{prefix, suffix, state});
    terms.setVisitLimit(30);
    EXPECT_EQ(3, drain(terms));  // v1007, v1017, v1027
    EXPECT_TRUE(terms.stoppedAtVisitLimit());
    EXPECT_EQ(30, terms.termsVisited());
  }
  {
    TermsEnum source(index.pool, segment->postingsReader(), field.fieldInfo);
    PrefixTermsEnum terms(source, "v");
    EXPECT_EQ(TERMS, drain(terms));
    EXPECT_FALSE(terms.stoppedAtVisitLimit());
  }
}
