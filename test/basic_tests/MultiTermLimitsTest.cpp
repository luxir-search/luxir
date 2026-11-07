// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <numeric>
#include <thread>
#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestIndex.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/query/MultiTermBudget.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/reader/DfaIntersectEnum.h"
#include "luxir/reader/FuzzySeekEnum.h"
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
  LuxirConfig defaults;
  const auto& search = defaults.search;
  MultiTermBudget budget;
  const auto& limits = budget.getLimits();
  EXPECT_EQ(0, limits.maxExpansions);
  EXPECT_EQ(0, limits.maxTermsExamined);
  EXPECT_EQ(0, limits.maxPostings);
  EXPECT_EQ(0, search.multiterm_min_prefix_length);
  EXPECT_EQ(limits.maxExpansions, search.multiterm_max_expansions);
  EXPECT_EQ(limits.maxTermsExamined, search.multiterm_max_terms_examined);
  EXPECT_EQ(limits.maxPostings, search.multiterm_max_postings);
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
  // Selecting fewer terms than the server cap fits; the scan budget still applies.
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

TEST_F(MultiTermLimitsTest, cappedPrefixCannotBypassServerCap) {
  LuxirNode node(config(10, 1'000'000));
  CollectionHelper helper(node);
  indexTerms(helper);
  for (int32_t selected : {10, 11, 1'000'000'000}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").prefixQuery("tag_s", "t", selected).limit(200);
    req->execute(false);
    if (selected == 10) {
      ASSERT_TRUE(req->ok()) << req->errorMsg();
      EXPECT_EQ(10u, req->getDocs().size());
    } else {
      EXPECT_FALSE(req->ok());
      EXPECT_EQ("too_many_expansions", req->errorCode());
    }
  }
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

TEST_F(MultiTermLimitsTest, minLiteralPrefixChecksOnlyThePattern) {
  struct Pattern { const char* json; bool allowed; };
  for (int minimum : {0, 2}) {
    LuxirNode node(config(0, 0, minimum));
    CollectionHelper helper(node);
    for (bool populated : {false, true}) {
      if (populated) {
        ASSERT_TRUE(helper.index(flatdoc("id", "one", "tag_s", "abc"), UpdateMessage::COMMIT).success);
      }
      for (auto [query, allowed] : {
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"*abc*"}})json", false},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":".*abc"}})json", false},
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"a*"}})json", false},
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"*"}})json", false},
          Pattern{R"json({"prefix":{"field":"tag_s","prefix":"a"}})json", false},
          Pattern{R"json({"prefix":{"field":"tag_s","prefix":"a","max_expansions":1}})json", false},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":"(dune|kings)"}})json", true},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":"a(b|c)"}})json", true},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":"abc"}})json", true},
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"abc"}})json", true},
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"ab*"}})json", true},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":"ab.*"}})json", true},
          Pattern{R"json({"prefix":{"field":"tag_s","prefix":"ab"}})json", true},
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"Ab*","case_insensitive":true}})json", true},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":"Ab.*","case_insensitive":true}})json", true},
          Pattern{R"json({"prefix":{"field":"tag_s","prefix":"Ab","case_insensitive":true}})json", true},
          Pattern{R"json({"wildcard":{"field":"tag_s","pattern":"a*","case_insensitive":true}})json", false},
          Pattern{R"json({"regex":{"field":"tag_s","pattern":"a.*","case_insensitive":true}})json", false},
          Pattern{R"json({"prefix":{"field":"tag_s","prefix":"a","case_insensitive":true}})json", false}}) {
        SCOPED_TRACE(query);
        auto req = localReq(helper.getSearchEngine());
        auto json = std::format(R"({{"query":{},"limit":0,"count_limit":-1}})", query);
        parseQueryRequest(api::build::arenaStr(req->mr, json), req->rawRequest(), req->mr);
        req->collection("main").execute(false);
        EXPECT_EQ(minimum == 0 || allowed, req->ok()) << req->errorMsg();
        if (minimum != 0 && !allowed) {
          EXPECT_EQ("pattern_prefix_too_short", req->errorCode());
          EXPECT_NE(std::string::npos, req->errorMsg().find("at least 2 bytes"));
          EXPECT_EQ(0, req->multiTermBudget.termsExamined());
        }
      }
    }
    for (int pass = 0; pass < 3; pass++) {
      for (auto pattern : {"a*", "ab*"}) {
        auto req = localReq(helper.getSearchEngine());
        req->topDocs("q").allQuery().filter(qb::wildcard(req->mr, "tag_s", pattern)).countLimit();
        req->execute(false);
        EXPECT_EQ(minimum == 0 || std::string_view(pattern) == "ab*", req->ok()) << req->errorMsg();
        if (!req->ok()) { EXPECT_EQ("pattern_prefix_too_short", req->errorCode()); }
      }
    }
    auto facet = localReq(helper.getSearchEngine());
    auto json = R"({"query":{"all":true},"ops":{"b":{"query_facet":{"buckets":{"short":{"prefix":{"field":"tag_s","prefix":"a"}}}}}}})";
    parseQueryRequest(api::build::arenaStr(facet->mr, json), facet->rawRequest(), facet->mr);
    facet->collection("main").execute(false);
    EXPECT_EQ(minimum == 0, facet->ok()) << facet->errorMsg();
    if (!facet->ok()) { EXPECT_EQ("pattern_prefix_too_short", facet->errorCode()); }
  }
}

TEST_F(MultiTermLimitsTest, weightsOfOneQueryShareItsExpansion) {
  // A count, a ranking pass and a sub-op domain each build their own weight
  // of one query, but they reuse one dictionary scan and one budget charge.
  LuxirNode node(config(1000, 1'000'000, 0));
  CollectionHelper helper(node);
  indexTerms(helper);

  SkipStatsScope stats;
  auto wildcard = localReq(helper.getSearchEngine());
  auto& list = wildcard->collection("main").topDocs("q");
  list.wildcardQuery("tag_s", "t0*9").countLimit(-1).limit(5).getScores();
  list.facet("tags", "tag_s");
  wildcard->execute();
  ASSERT_TRUE(wildcard->ok()) << wildcard->errorMsg();
  EXPECT_EQ(10, wildcard->getMatchCount());
  EXPECT_EQ(1, SkipStats::multitermExpansions);
  EXPECT_EQ(100, wildcard->multiTermBudget.termsExamined());

  auto fuzzy = [&](int64_t countLimit, bool facet) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.fuzzyQuery("tag_s", "t050", 2, 0).countLimit(countLimit).limit(5);
    if (facet) cur.facet("tags", "tag_s");
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->multiTermBudget.termsExamined();
  };
  int64_t uncounted = fuzzy(0, false);
  EXPECT_GT(uncounted, 0);
  EXPECT_EQ(uncounted, fuzzy(-1, false));
  EXPECT_EQ(uncounted, fuzzy(-1, true));
}

TEST_F(MultiTermLimitsTest, facetTermFilterIsExemptFromMinLiteralPrefix) {
  LuxirNode node(config(0, 0, 5));
  CollectionHelper helper(node);
  ASSERT_TRUE(helper.index(flatdoc("id", "one", "tag_s", "abc"), UpdateMessage::COMMIT).success);
  auto req = localReq(helper.getSearchEngine());
  auto json = R"({"query":{"all":true},"ops":{"f":{"field_facet":{"field":"tag_s","term_filter":{"regex":"a.*"}}}}})";
  parseQueryRequest(api::build::arenaStr(req->mr, json), req->rawRequest(), req->mr);
  req->collection("main").execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
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

TEST_F(MultiTermLimitsTest, scoredCaseInsensitiveMatchHonorsLimits) {
  auto errorCode = [](const LuxirConfig& limited) {
    LuxirNode node(limited);
    CollectionHelper helper(node);
    std::vector<Doc> docs;
    for (auto term : {"aaa", "Aaa", "aAa", "aaA"}) {
      docs.push_back(flatdoc("id", term, "word_w", term));
    }
    EXPECT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q")
        .exprQuery("match(aaa, field=word_w, case_insensitive=true)").limit(10).getScores();
    req->execute(false);
    return req->ok() ? std::string() : std::string(req->errorCode());
  };
  auto postings = [](int64_t limit) {
    LuxirConfig limited = config(0, 0, 0);
    limited.search.multiterm_max_postings = limit;
    return limited;
  };

  EXPECT_EQ("", errorCode(config(0, 0, 0)));
  EXPECT_EQ("", errorCode(config(4, 0, 0)));
  EXPECT_EQ("too_many_expansions", errorCode(config(3, 0, 0)));
  EXPECT_EQ("too_many_terms_examined", errorCode(config(0, 1, 0)));
  EXPECT_EQ("", errorCode(postings(4)));
  EXPECT_EQ("too_many_expansion_postings", errorCode(postings(3)));
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

// A real allowance with small slices exercises refills inside rejected-term loops.
class SmallScanAllowance final : public ScanAllowance {
  int64_t limit;
  bool throwAtEnd;
  bool fixedSlice;
public:
  int64_t charged = 0;
  int reports = 0;
  SmallScanAllowance(int64_t limit, bool throwAtEnd = false, bool fixedSlice = false)
      : limit(limit), throwAtEnd(throwAtEnd), fixedSlice(fixedSlice) {}
  int64_t charge(int64_t visited) override {
    ++reports;
    charged += visited;
    if (charged > limit) {
      if (throwAtEnd) throw std::runtime_error("scan cancelled");
      return 0;
    }
    return fixedSlice ? 7 : std::min((int64_t) 7, limit - charged + 1);
  }
};

TEST_F(MultiTermLimitsTest, enumsContinueAndStopAcrossSlices) {
  TestIndex index;
  TestField field{index, "value_s"};
  field.startIndexing();
  std::vector<std::string> stored;
  for (int32_t i = 0; i < TERMS; i++) {
    stored.push_back("v" + std::to_string(1000 + i));
    field.add(i, stored.back());
  }
  std::vector<std::string_view> explicitTerms(stored.begin(), stored.end());
  index.flush();
  field.startReading();
  auto* segment = field.currentSegment();
  enum class Run { UNLIMITED, GENEROUS, EXACT, STOP, THROW, FINAL_STOP };
  auto noStats = [](const auto&) {};
  auto verify = [&](auto make, size_t acceptedAtStop, auto checkBaseline) {
    std::vector<std::string> expected;
    int64_t total = 0;
    for (Run mode : {Run::UNLIMITED, Run::GENEROUS, Run::EXACT, Run::STOP, Run::THROW, Run::FINAL_STOP}) {
      SCOPED_TRACE((int) mode);
      TermsEnum source(index.pool, segment->postingsReader(), field.fieldInfo);
      auto* terms = make(source);
      SmallScanAllowance allowance(mode == Run::GENEROUS ? total + 10 : mode == Run::EXACT ? total : mode == Run::FINAL_STOP ? total - 1 : 30,
                                   mode == Run::THROW, mode == Run::FINAL_STOP);
      if (mode != Run::UNLIMITED) terms->setAllowance(&allowance);
      std::vector<std::string> actual;
      auto drain = [&] { while (terms->next()) actual.emplace_back(terms->termView()); };
      if (mode == Run::THROW) {
        EXPECT_THROW(drain(), std::runtime_error);
        EXPECT_EQ(31, terms->termsVisited());
        EXPECT_EQ(terms->termsVisited(), allowance.charged);
        continue;
      }
      drain();
      int reports = allowance.reports;
      EXPECT_FALSE(terms->next());
      EXPECT_EQ(reports, allowance.reports);
      if (mode != Run::UNLIMITED) { EXPECT_EQ(terms->termsVisited(), allowance.charged); }
      if (mode == Run::UNLIMITED) {
        expected = actual;
        total = terms->termsVisited();
        ASSERT_GT(total, 30);
        checkBaseline(*terms);
      } else if (mode == Run::GENEROUS || mode == Run::EXACT) {
        EXPECT_EQ(expected, actual);
        EXPECT_EQ(total, terms->termsVisited());
        EXPECT_FALSE(terms->stoppedAtVisitLimit());
        EXPECT_GT(allowance.reports, 4);
      } else if (mode == Run::FINAL_STOP) {
        EXPECT_EQ(expected, actual);
        EXPECT_EQ(total, terms->termsVisited());
        EXPECT_TRUE(terms->stoppedAtVisitLimit());
      } else {
        EXPECT_EQ(31, terms->termsVisited());
        EXPECT_TRUE(terms->stoppedAtVisitLimit());
        ASSERT_LE(actual.size(), expected.size());
        EXPECT_TRUE(std::equal(actual.begin(), actual.end(), expected.begin()));
        EXPECT_EQ(acceptedAtStop, actual.size());
      }
    }
  };
  verify([&](TermsEnum& te) { return index.pool.make<PrefixTermsEnum>(te, "v"); }, 31, noStats);
  verify([&](TermsEnum& te) {
    return index.pool.make<RangeTermsEnum>(te, "v1000", true, "v1099", true);
  }, 31, noStats);
  verify([&](TermsEnum& te) { return index.pool.make<ExplicitTermsEnum>(te, explicitTerms); }, 31, noStats);
  verify([&](TermsEnum& te) { return index.pool.make<FuzzySeekEnum>(index.pool, te, "v", "1050", 2); }, 31, noStats);
  // No suffix uses the smart path; a cyclic pattern with a suffix uses linear
  // ranges; all-live transitions have their own fused sequential path.
  struct DfaCase { const char* pattern; size_t acceptedAtStop; bool linear; };
  for (auto [pattern, acceptedAtStop, linear] : {
      DfaCase{"v?0??", 31, false}, DfaCase{"*7", 3, true}, DfaCase{"*", 31, false}}) {
    SCOPED_TRACE(pattern);
    automaton::Budget budget;
    auto dfa = automaton::compileWildcard(pattern, budget);
    auto view = dfa.view();
    auto [prefix, state] = view.commonPrefixAndState();
    std::string suffix = dfa.commonSuffix();
    verify([&](TermsEnum& te) {
      return index.pool.make<DfaIntersectEnum>(index.pool, te, view, DfaScanPlan{prefix, suffix, state});
    }, acceptedAtStop, [&](const DfaIntersectEnum& terms) {
      if (linear) {
        EXPECT_GT(terms.linearTerms(), 0);
      } else {
        EXPECT_EQ(0, terms.linearTerms());
      }
    });
  }
}

// Rebuild the same six-segment layout for each operator limit. Each request
// enables parallel segment execution; no writes happen while it runs.
static void checkSegmentTotals(bool postings) {
  int64_t total = 0;
  std::vector<int32_t> layout;
  for (int run = 0; run < 7; run++) {
    int64_t limit = run == 0 ? 1'000'000 : total - (run % 2 == 0 ? 1 : 0);
    LuxirConfig cfg;
    cfg.search.multiterm_max_terms_examined = postings ? 0 : limit;
    cfg.search.multiterm_max_postings = postings ? limit : 0;
    LuxirNode node(cfg);
    CollectionHelper helper(node);
    for (int segment = 0; segment < 6; segment++) {
      std::vector<Doc> docs;
      for (int i = 0; i < 100; i++) {
        docs.push_back(flatdoc("id", std::to_string(segment * 100 + i),
                              "tag_s", "t" + std::to_string(1000 + i)));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
    auto req = localReq(helper.getSearchEngine());
    req->maxParallel = -1;
    if (postings) req->topDocs("q").prefixQuery("tag_s", "t").countLimit();
    else req->topDocs("q").wildcardQuery("tag_s", "*zz*").countLimit();
    req->execute(false);
    std::vector<int32_t> currentLayout;
    for (auto& segment : req->reader->segments()) currentLayout.push_back(segment.maxDoc());
    ASSERT_EQ(6u, currentLayout.size());
    int64_t charged = postings ? req->multiTermBudget.postingsCharged()
                               : req->multiTermBudget.termsExamined();
    if (run == 0) {
      total = charged;
      layout = currentLayout;
      ASSERT_GT(total, 0);
    }
    EXPECT_EQ(layout, currentLayout);
    EXPECT_EQ(total, charged);
    if (limit >= total) {
      ASSERT_TRUE(req->ok()) << req->errorMsg();
      if (postings) { EXPECT_EQ(600, req->getMatchCount()); }
    } else {
      EXPECT_FALSE(req->ok());
      EXPECT_EQ(postings ? "too_many_expansion_postings" : "too_many_terms_examined", req->errorCode());
    }
  }
}

TEST_F(MultiTermLimitsTest, segmentTermsOutcomeDependsOnTotal) { checkSegmentTotals(false); }

TEST_F(MultiTermLimitsTest, concurrentChargesDependOnlyOnTotal) {
  constexpr int64_t limit = 1'000'003;
  for (int run = 0; run < 6; run++) {
    int64_t work = limit + run % 2;
    MultiTermLimits limits;
    limits.maxTermsExamined = limit;
    MultiTermBudget budget(limits);
    std::array<int, 8> stops{};
    std::barrier start(8);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < stops.size(); i++) {
      threads.emplace_back([&, i] {
        int64_t remaining = work / 8 + ((int64_t) i < work % 8);
        start.arrive_and_wait();
        while (remaining != 0) {
          int64_t count = std::min(remaining, MultiTermBudget::SCAN_SLICE);
          stops[i] += budget.charge(count) == 0;
          remaining -= count;
          std::this_thread::yield();
        }
      });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(work, budget.termsExamined());
    int stopped = std::accumulate(stops.begin(), stops.end(), 0);
    if (work == limit) { EXPECT_EQ(0, stopped); }
    else { EXPECT_GT(stopped, 0); }
  }
}

TEST_F(MultiTermLimitsTest, defaultsRunWidePrefix) {
  CollectionHelper helper;
  std::string body;
  for (int i = 0; i < 65'537; i++) body += "t" + std::to_string(i) + " ";
  ASSERT_TRUE(helper.index(flatdoc("id", "wide", "body_w", body), UpdateMessage::COMMIT).success);
  auto req = localReq(helper.getSearchEngine());
  req->topDocs("q").prefixQuery("body_w", "t").countLimit();
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(1, req->getMatchCount());
}

TEST_F(MultiTermLimitsTest, refillsConsumeTermsAndPostings) {
  TestIndex index;
  TestField field(index, "value_s");
  field.startIndexing();
  for (int i = 0; i < 10; i++) field.add(i, "t" + std::to_string(i));
  index.flush();
  field.startReading();
  for (bool terms : {true, false}) {
    MultiTermLimits limits;
    limits.maxTermsExamined = terms ? 15 : 0;
    limits.maxPostings = terms ? 0 : 15;
    MultiTermBudget budget(limits);
    MemPool pool;
    Query::Context context(pool, *index.reader, {&budget});
    PrefixQuery query("value_s", "t");
    auto* weight = query.createWeight(context, 0);
    auto& segment = index.reader->segments()[0];
    ASSERT_NE(nullptr, weight->createScorer(pool, segment));
    EXPECT_EQ(10, terms ? budget.termsExamined() : budget.postingsCharged());
    weight->releaseSegmentState(segment);
    EXPECT_THROW(weight->createScorer(pool, segment), ResourceExhaustedError);
    EXPECT_EQ(terms ? 16 : 20, terms ? budget.termsExamined() : budget.postingsCharged());
  }
}

TEST_F(MultiTermLimitsTest, segmentsSharePostingsEstimate) { checkSegmentTotals(true); }

TEST_F(MultiTermLimitsTest, queryFacetSharesRootBudget) {
  LuxirNode node(config(0, 150, 0));
  CollectionHelper helper(node);
  indexTerms(helper);
  for (bool withFacet : {true, false}) {
    auto req = localReq(helper.getSearchEngine());
    std::string json = R"({"query":{"prefix":{"field":"tag_s","prefix":"t"}},"limit":0,"count_limit":-1)";
    if (withFacet) json += R"(,"ops":{"buckets":{"query_facet":{"buckets":{"selected":{"prefix":{"field":"tag_s","prefix":"t0"}}}}}})";
    json += "}";
    parseQueryRequest(api::build::arenaStr(req->mr, json), req->rawRequest(), req->mr);
    req->collection("main").execute(false);
    if (withFacet) {
      EXPECT_FALSE(req->ok());
      EXPECT_EQ("too_many_terms_examined", req->errorCode());
    } else {
      ASSERT_TRUE(req->ok()) << req->errorMsg();
    }
  }
}
