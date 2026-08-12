#include <solux/query/AllQuery.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TopKAssert.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "solux/query/TermQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/ForcePrepareQuery.h"
#include "solux/search/Collector.h"
#include "solux/reader/PosEnum.h"


using namespace solux;
using namespace solux::test;

template<typename T>
concept HasTermFreq = requires(T& docs) { docs.termFreq(); };
template<typename T>
concept HasNextDocOnly = requires(T& docs) { docs.nextDocOnly(); };
template<typename T>
concept HasAdvanceDocOnly = requires(T& docs) { docs.advanceDocOnly(1); };
template<typename T>
concept HasIntoBitSet = requires(T& docs, std::span<uint64_t> bits) {
  docs.intoBitSet(bits, 0, 1);
};
template<typename T>
concept HasPeekDocBlock = requires(T& docs) { docs.peekDocBlock(); };

static_assert(!HasTermFreq<DocsOnlyEnum>);
static_assert(!HasNextDocOnly<DocsOnlyEnum>);
static_assert(!HasAdvanceDocOnly<DocsOnlyEnum>);
static_assert(!HasNextDocOnly<DocsPosEnum>);
static_assert(!HasAdvanceDocOnly<DocsPosEnum>);
static_assert(!HasIntoBitSet<DocsPosEnum>);
static_assert(!HasPeekDocBlock<DocsPosEnum>);
static_assert(HasTermFreq<DocsFreqEnum>);
static_assert(HasNextDocOnly<DocsFreqEnum>);
static_assert(HasAdvanceDocOnly<DocsFreqEnum>);
static_assert(HasIntoBitSet<DocsFreqEnum>);

constexpr int32_t kMaxScoreDisjunctionSegDocs = 3 * Postings::DOCS_BLOCK_SIZE + 40;

class TermScorerTest : public SoluxTest {
protected:

  std::vector<const char*> text = {
          "now is the time",
          "for all good men",
          "to come to the aid of their country",
          "to the moon!"
  };

  // Field stats for the above field values:
  // docCount == 4
  // maxDoc == 8 (0 through 7)
  // sumTotalTermFreq = 19
  // sumDocFreq = 18 (just one overlap... "to" appears twice in doc 5)
  // numTerms = 15  (repeated terms are "to":3, "the":"3", hence 15+2extra+2extra = 19 sumTotalTermFreq

  // Term stats for the term "to" in the above field.
  // docFreq == 2
  // totalTermFreq = 3
  int testScores(Query::Scorer* scorer, std::vector<int> expectedDocs, std::vector<float> expectedScores) {
    if (scorer == nullptr) {
      EXPECT_EQ(expectedDocs.size(), 0);
      return 0;
    }
    for (size_t i = 0; i < expectedDocs.size(); i++) {
      EXPECT_EQ(expectedDocs[i], scorer->next());
      EXPECT_FLOAT_EQ(expectedScores[i], scorer->score());
    }
    int32_t doc = scorer->next();
    EXPECT_EQ(doc, PostingsReader::END);
    return 0;
  }


};

struct DisjunctionTopKRun {
  int64_t visited = 0;
  int64_t nonEssentialLookups = 0;
  int64_t bs1Windows = 0;
  int64_t domainDriveWindows = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

struct MsmTopKRun {
  int64_t visited = 0;
  int64_t wandVisited = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

struct TermImpactTopKRun {
  int64_t visited = 0;
  int64_t skippedBlocks = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

struct QueryTopKRun {
  int64_t visited = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

struct BulkDomainDriveGuard {
  bool saved;

  explicit BulkDomainDriveGuard(bool disabled)
    : saved(BooleanQuery::disableBulkDomainDriveForTests) {
    BooleanQuery::disableBulkDomainDriveForTests = disabled;
  }

  ~BulkDomainDriveGuard() {
    BooleanQuery::disableBulkDomainDriveForTests = saved;
  }
};

struct WindowDispatchGuard {
  bool saved;

  explicit WindowDispatchGuard(bool disabled)
    : saved(BooleanQuery::disableWindowDispatchForTests) {
    BooleanQuery::disableWindowDispatchForTests = disabled;
  }

  ~WindowDispatchGuard() {
    BooleanQuery::disableWindowDispatchForTests = saved;
  }
};

struct PartitionLatchGuard {
  bool saved;

  explicit PartitionLatchGuard(bool disabled)
    : saved(BooleanQuery::disablePartitionLatchForTests) {
    BooleanQuery::disablePartitionLatchForTests = disabled;
  }

  ~PartitionLatchGuard() {
    BooleanQuery::disablePartitionLatchForTests = saved;
  }
};

struct MandOptBulkGuard {
  bool saved;

  explicit MandOptBulkGuard(bool disabled)
    : saved(BooleanQuery::disableMandOptBulkForTests) {
    BooleanQuery::disableMandOptBulkForTests = disabled;
  }

  ~MandOptBulkGuard() {
    BooleanQuery::disableMandOptBulkForTests = saved;
  }
};

struct DenseScoredGuard {
  bool saved;

  explicit DenseScoredGuard(bool disabled)
    : saved(BooleanQuery::ConjunctionBulkScorer::disableDenseScoredForTests) {
    BooleanQuery::ConjunctionBulkScorer::disableDenseScoredForTests = disabled;
  }

  ~DenseScoredGuard() {
    BooleanQuery::ConjunctionBulkScorer::disableDenseScoredForTests = saved;
  }
};

struct ScoredProbeGuard {
  bool saved;

  explicit ScoredProbeGuard(bool disabled)
    : saved(BooleanQuery::ConjunctionBulkScorer::disableScoredProbeForTests) {
    BooleanQuery::ConjunctionBulkScorer::disableScoredProbeForTests = disabled;
  }

  ~ScoredProbeGuard() {
    BooleanQuery::ConjunctionBulkScorer::disableScoredProbeForTests = saved;
  }
};

struct ExactFreqOnSurvivalGuard {
  bool saved;

  explicit ExactFreqOnSurvivalGuard(bool disabled)
    : saved(BooleanQuery::ConjunctionScorer::
                disableExactFreqOnSurvivalForTests) {
    BooleanQuery::ConjunctionScorer::
        disableExactFreqOnSurvivalForTests = disabled;
  }

  ~ExactFreqOnSurvivalGuard() {
    BooleanQuery::ConjunctionScorer::
        disableExactFreqOnSurvivalForTests = saved;
  }
};

struct FilterMaskProbeGuard {
  bool saved;

  explicit FilterMaskProbeGuard(bool disabled)
    : saved(BooleanQuery::disableFilterMaskProbeForTests) {
    BooleanQuery::disableFilterMaskProbeForTests = disabled;
  }

  ~FilterMaskProbeGuard() {
    BooleanQuery::disableFilterMaskProbeForTests = saved;
  }
};

struct FilteredConjunctionBatchGuard {
  bool saved;

  explicit FilteredConjunctionBatchGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredConjunctionBatchForTests) {
    BooleanQuery::disableFilteredConjunctionBatchForTests = disabled;
  }

  ~FilteredConjunctionBatchGuard() {
    BooleanQuery::disableFilteredConjunctionBatchForTests = saved;
  }
};

struct ExactTermCountGuard {
  bool saved;

  explicit ExactTermCountGuard(bool disabled)
    : saved(BooleanQuery::disableExactTermCountForTests) {
    BooleanQuery::disableExactTermCountForTests = disabled;
  }

  ~ExactTermCountGuard() {
    BooleanQuery::disableExactTermCountForTests = saved;
  }
};

struct FilteredScoredBulkGuard {
  bool saved;

  explicit FilteredScoredBulkGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredScoredBulkForTests) {
    BooleanQuery::disableFilteredScoredBulkForTests = disabled;
  }

  ~FilteredScoredBulkGuard() {
    BooleanQuery::disableFilteredScoredBulkForTests = saved;
  }
};

struct FilteredMandOptFillGateGuard {
  bool saved;

  explicit FilteredMandOptFillGateGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredMandOptFillGateForTests) {
    BooleanQuery::disableFilteredMandOptFillGateForTests = disabled;
  }

  ~FilteredMandOptFillGateGuard() {
    BooleanQuery::disableFilteredMandOptFillGateForTests = saved;
  }
};

struct ExactFilteredMandOptCompositionGuard {
  bool saved;

  explicit ExactFilteredMandOptCompositionGuard(bool disabled)
    : saved(BooleanQuery::disableExactFilteredMandOptCompositionForTests) {
    BooleanQuery::disableExactFilteredMandOptCompositionForTests = disabled;
  }

  ~ExactFilteredMandOptCompositionGuard() {
    BooleanQuery::disableExactFilteredMandOptCompositionForTests = saved;
  }
};

struct FilteredUnionWandGuard {
  bool saved;

  explicit FilteredUnionWandGuard(bool disabled)
    : saved(BooleanQuery::disableFilteredUnionWandForTests) {
    BooleanQuery::disableFilteredUnionWandForTests = disabled;
  }

  ~FilteredUnionWandGuard() {
    BooleanQuery::disableFilteredUnionWandForTests = saved;
  }
};

struct PhraseMatchCountGuard {
  bool savedEnabled;
  int64_t savedCalls;

  explicit PhraseMatchCountGuard(bool enabled)
    : savedEnabled(PhraseQuery::Scorer::countMatchesForTests),
      savedCalls(PhraseQuery::Scorer::matchCallsForTests) {
    PhraseQuery::Scorer::matchCallsForTests = 0;
    PhraseQuery::Scorer::countMatchesForTests = enabled;
  }

  ~PhraseMatchCountGuard() {
    PhraseQuery::Scorer::countMatchesForTests = savedEnabled;
    PhraseQuery::Scorer::matchCallsForTests = savedCalls;
  }

  int64_t calls() const {
    return PhraseQuery::Scorer::matchCallsForTests;
  }
};

struct NumericRangeShapeGuard {
  bool saved;

  explicit NumericRangeShapeGuard(bool disabled)
    : saved(NumericRangeQuery::disableShapesForTests) {
    NumericRangeQuery::disableShapesForTests = disabled;
  }

  ~NumericRangeShapeGuard() {
    NumericRangeQuery::disableShapesForTests = saved;
  }
};

struct PhraseFilterTopKRun {
  int64_t visited = 0;
  int64_t matchCalls = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

struct SkipStatsGuard {
  bool saved;

  explicit SkipStatsGuard(bool enabled = true) : saved(SkipStats::enabled) {
    SkipStats::enabled = enabled;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
  }
};

struct ForceEagerImpactsGuard {
  bool saved;

  explicit ForceEagerImpactsGuard(bool enabled)
    : saved(ImpactsIndex::forceEagerForTests) {
    ImpactsIndex::forceEagerForTests = enabled;
  }

  ~ForceEagerImpactsGuard() {
    ImpactsIndex::forceEagerForTests = saved;
  }
};

struct CandidateLeapfrogGuard {
  bool saved;

  explicit CandidateLeapfrogGuard(bool disabled)
    : saved(TermQuery::disableCandidateLeapfrogForTests) {
    TermQuery::disableCandidateLeapfrogForTests = disabled;
  }

  ~CandidateLeapfrogGuard() {
    TermQuery::disableCandidateLeapfrogForTests = saved;
  }
};

std::vector<TopDocsCollector::ScoreDoc> sortedCollectorDocs(TopDocsCollector& collector) {
  auto docs = collector.sort();
  std::vector<TopDocsCollector::ScoreDoc> out(docs.begin(), docs.end());
  std::sort(out.begin(), out.end(), TopDocsCollector::scoreAndDocComp);
  return out;
}

void addPhraseDeferralDocs(CollectionHelper& helper, int32_t nDocs, int32_t filterStep) {
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "alpha beta filler";
    if ((doc % filterStep) == 0) {
      body += " needle";
    }
    docs.push_back(flatdoc("id", "p" + std::to_string(doc), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
}

int64_t countStandalonePhraseMatchChecks(IndexReader& reader) {
  PhraseMatchCountGuard guard(true);
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  auto* weight = phrase.createWeight(qContext, 0);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    while (scorer->next() != PostingsReader::END) {}
  }
  return guard.calls();
}

PhraseFilterTopKRun runPhraseFilterConjunctionTopK(IndexReader& reader, int32_t topK,
                                                   bool countMatches) {
  PhraseMatchCountGuard guard(countMatches);
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  TermQuery filterTerm("body_w", "needle");
  std::vector<Query*> mandatory = {&phrase};
  std::vector<Query*> filter = {&filterTerm};
  BooleanQuery query(mandatory, {}, {}, filter);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
    collectTopK(segnum, scorer, nullptr, nullptr, collector);
  }

  PhraseFilterTopKRun result;
  result.visited = collector.totalHits();
  result.matchCalls = guard.calls();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

void assertSameTopKExact(const PhraseFilterTopKRun& expected,
                         const PhraseFilterTopKRun& actual) {
  ASSERT_EQ(expected.topDocs.size(), actual.topDocs.size());
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(expected.topDocs[i].doc, actual.topDocs[i].doc);
    EXPECT_EQ(std::bit_cast<uint32_t>(expected.topDocs[i].score),
              std::bit_cast<uint32_t>(actual.topDocs[i].score));
  }
}

void appendRepeatedTerm(std::string& body, std::string_view term, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(term);
  }
}

void addNegatedThetaDocs(TestField& field, int32_t nDocs) {
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    int32_t tf = block == 0 ? 20 - (doc % 7) : (block == 9 ? 9 - (doc % 3) : 1);
    appendRepeatedTerm(body, "mand", tf);
    body += " join conjoin quick fox";
    if ((doc % 3) == 0) body += " bonus";
    if ((doc % 13) == 0) body += " rarebonus rarebonus";
    if ((doc % 7) == 0) body += " excludeone";
    if ((doc % 11) == 0) body += " excludetwo";
    appendRepeatedTerm(body, "filler", 4 + (doc % 3));
    field.add(doc, body);
  }
}

QueryTopKRun runQueryTopK(IndexReader& reader, Query& query, int32_t topK,
                          bool allowPruning) {
  MemPool pool;
  Query::Context context(pool, reader);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  auto segments = context.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer != nullptr) {
      collectTopK(segnum, scorer, nullptr, nullptr, collector, allowPruning);
    }
  }
  return {collector.totalHits(), sortedCollectorDocs(collector)};
}

void assertQueryTopKExact(const QueryTopKRun& expected, const QueryTopKRun& actual) {
  ASSERT_EQ(expected.topDocs.size(), actual.topDocs.size());
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(expected.topDocs[i].doc, actual.topDocs[i].doc) << "i=" << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(expected.topDocs[i].score),
              std::bit_cast<uint32_t>(actual.topDocs[i].score)) << "i=" << i;
  }
}

QueryTopKRun runComposedMaxScoreTopK(IndexReader& reader, std::span<Query*> queries,
                                     int32_t topK, int32_t windowSize,
                                     bool allowPruning, int32_t prepositionFirst = -1) {
  MemPool pool;
  Query::Context context(pool, reader);
  auto segments = context.topReader.segments();
  TopDocsCollector collector(topK);
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto scorers = pool.make_span<Query::Scorer*>(queries.size());
    size_t count = 0;
    for (auto* query : queries) {
      auto* weight = query->createWeight(context, Query::NEED_SCORES);
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) scorers[count++] = scorer;
    }
    scorers = scorers.first(count);
    if (scorers.empty()) {
      ADD_FAILURE() << "no scorers for segment " << segnum;
      continue;
    }
    if (prepositionFirst >= 0) {
      EXPECT_NE(dynamic_cast<BooleanQuery::MandNotScorer*>(scorers[0]), nullptr);
      scorers[0]->advance(prepositionFirst);
    }
    auto* scorer = pool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
        pool, scorers, segments[segnum].maxDoc(), windowSize);
    collectTopK(segnum, scorer, nullptr, nullptr, collector, allowPruning);
  }
  return {collector.totalHits(), sortedCollectorDocs(collector)};
}

void antiCorrelatedTfLen(int32_t postingOrd, int32_t& tf, int32_t& len) {
  int32_t block = postingOrd / Postings::DOCS_BLOCK_SIZE;
  int32_t local = postingOrd % Postings::DOCS_BLOCK_SIZE;
  if (block == 0) {
    tf = 12 - (local % 3);
    len = tf + 1 + (local % 2);
  } else if (local == 0) {
    tf = 10 - std::min(block, 5);
    len = 24 + std::min(block, 5) * 2;
  } else if (local == 1) {
    tf = 1;
    len = 2;
  } else {
    tf = 1 + (local % 3 == 0 ? 1 : 0);
    len = 8 + (local % 5);
  }
  if (len < tf) {
    len = tf;
  }
}

void addAntiCorrelatedFrontierDocs(TestField& f, int32_t postingCount) {
  std::string body;
  for (int32_t doc = 0; doc < postingCount * 2; doc++) {
    body.clear();
    if ((doc % 2) == 0) {
      int32_t tf = 0;
      int32_t len = 0;
      antiCorrelatedTfLen(doc / 2, tf, len);
      appendRepeatedTerm(body, "frontier", tf);
      appendRepeatedTerm(body, "filler", len - tf);
    } else {
      appendRepeatedTerm(body, "filler", 3 + (doc % 3));
    }
    f.add(doc, body);
  }
}

TermImpactTopKRun runSingleTermFrontierTopK(IndexReader& reader, int32_t topK,
                                            bool useFrontierBound,
                                            bool allowPruning) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", "frontier", 1.0f, useFrontierBound);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  TermImpactTopKRun result;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = dynamic_cast<TermQuery::Scorer*>(
        weight->createScorer(pool, segments[segnum]));
    if (scorer == nullptr) {
      continue;
    }
    if (allowPruning) {
      collectTopK(segnum, scorer, nullptr, nullptr, collector);
    } else {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        collector.collect(segnum, doc, scorer->score());
      }
    }
    result.skippedBlocks += scorer->skippedBlocks();
  }

  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

void addMaxScoreDisjunctionDocs(CollectionHelper& helper) {
  const int32_t segDocs = kMaxScoreDisjunctionSegDocs;
  const int32_t segCount = 3;

  for (int32_t seg = 0; seg < segCount; seg++) {
    std::vector<Doc> docs;
    docs.reserve(segDocs);
    for (int32_t local = 0; local < segDocs; local++) {
      int32_t doc = seg * segDocs + local;
      std::string body = "common";
      if (doc < 6) {
        // Strictly decreasing rare tf (doc 0 has the most) with constant length:
        // doc 0 alone maximizes the rare clause, so the top-k threshold stays
        // strictly below the sum of clause maxima.  That keeps the rare clause
        // essential while the near-zero-idf common clause is demoted, so the
        // non-essential lookup path is actually exercised (partial demotion).
        int32_t rareTf = 8 - doc;
        for (int32_t i = 0; i < rareTf; i++) body += " rare";
        for (int32_t i = rareTf; i < 8; i++) body += " pad";
        body += " medium";
      } else {
        if ((doc % 9) == 0) body += " medium";
        for (int32_t i = 0; i < 60; i++) body += " filler";
      }
      docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w", body));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
}

DisjunctionTopKRun runMaxScoreDisjunctionTopK(IndexReader& reader, int32_t topK,
                                              int32_t windowSize = DocsEnumMeta::L1_DOCS) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery common("body_w", "common");
  TermQuery medium("body_w", "medium");
  TermQuery rare("body_w", "rare");
  std::array<Query::Weight*, 3> weights = {
    common.createWeight(qContext, Query::NEED_SCORES),
    medium.createWeight(qContext, Query::NEED_SCORES),
    rare.createWeight(qContext, Query::NEED_SCORES)
  };
  TopDocsCollector collector(topK);
  DisjunctionTopKRun result;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count == 0) continue;
    Query::Scorer* scorer = count == 1
      ? arr[0]
      : pool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
          pool, std::span<Query::Scorer*>(arr, (size_t) count),
          segments[segnum].maxDoc(), windowSize);
    scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
    collectTopK(segnum, scorer, nullptr, nullptr, collector);
    if (auto* maxScore = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(scorer)) {
      result.nonEssentialLookups += maxScore->nonEssentialLookupCount();
    }
  }

  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runExhaustiveDisjunctionTopK(IndexReader& reader, int32_t topK) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery common("body_w", "common");
  TermQuery medium("body_w", "medium");
  TermQuery rare("body_w", "rare");
  std::array<Query::Weight*, 3> weights = {
    common.createWeight(qContext, Query::NEED_SCORES),
    medium.createWeight(qContext, Query::NEED_SCORES),
    rare.createWeight(qContext, Query::NEED_SCORES)
  };
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count == 0) continue;
    Query::Scorer* scorer = count == 1
      ? arr[0]
      : pool.make<BooleanQuery::DisjunctionScorer>(
          pool, std::span<Query::Scorer*>(arr, (size_t) count),
          singlePhaseScorersForTests(pool, (size_t)count), true);
    collectTopK(segnum, scorer, nullptr, nullptr, collector, false);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

// Execution paths are not required to produce bit-identical sums (accepted
// policy): compare tie-group-aware, per TopKAssert.h.
void assertSameTopKDocs(const DisjunctionTopKRun& expected, const DisjunctionTopKRun& actual, int32_t topK) {
  SCOPED_TRACE(::testing::Message() << "k=" << topK);
  assertTopKEquivalent(expected.topDocs, actual.topDocs);
}

DisjunctionTopKRun runBooleanTopK(IndexReader& reader, std::span<Query*> mandatory,
                                  std::span<Query*> optional, int32_t topK,
                                  bool allowPruning) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    collectTopK(segnum, scorer, nullptr, nullptr, collector, allowPruning);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

std::vector<int32_t> collectDocIds(Query::Scorer* scorer) {
  std::vector<int32_t> docs;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    docs.push_back(doc);
  }
  return docs;
}

std::vector<TermQuery> makeTermQueries(std::span<const std::string_view> terms) {
  std::vector<TermQuery> queries;
  queries.reserve(terms.size());
  for (auto term : terms) {
    queries.emplace_back("body_w", term);
  }
  return queries;
}

std::vector<Query*> queryPointers(std::span<TermQuery> queries) {
  std::vector<Query*> pointers;
  pointers.reserve(queries.size());
  for (auto& query : queries) {
    pointers.push_back(&query);
  }
  return pointers;
}

std::vector<std::string> makeMtTermStrings(int32_t numTerms) {
  std::vector<std::string> terms;
  terms.reserve((size_t) numTerms);
  for (int32_t term = 0; term < numTerms; term++) {
    terms.push_back("mt" + std::to_string(term));
  }
  return terms;
}

void addDenseManyClauseDisjunctionDocs(TestField& field, int32_t nDocs, int32_t numTerms) {
  std::vector<std::string> terms = makeMtTermStrings(numTerms);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    for (int32_t term = 0; term < numTerms; term++) {
      if (((doc + term) & 1) != 0) {
        continue;
      }
      int32_t tf = 1 + (int32_t) ((doc * 31 + term * 17) % 4);
      appendRepeatedTerm(body, terms[(size_t) term], tf);
      used += tf;
    }
    int32_t len = used + 4 + (doc % 3);
    appendRepeatedTerm(body, "filler", len - used);
    field.add(doc, body);
  }
}

std::unique_ptr<DocSet> makeEveryNthDocSet(int32_t maxDoc, int32_t step, bool arrayDocSet) {
  if (arrayDocSet) {
    std::vector<int32_t> docs;
    docs.reserve((size_t)((maxDoc + step - 1) / step));
    for (int32_t doc = 0; doc < maxDoc; doc += step) {
      docs.push_back(doc);
    }
    return std::make_unique<ArrDocSet>(std::move(docs));
  }

  auto filter = std::make_unique<RAMBitDocSet>(maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    if ((doc % step) == 0) {
      filter->mutableBits().set(doc);
    }
  }
  return filter;
}

Query::Weight* createDenseDisjunctionWeight(Query::Context& qContext, int32_t numTerms,
                                            std::vector<std::string>& terms,
                                            std::vector<TermQuery>& queries,
                                            std::vector<Query*>& optional) {
  terms = makeMtTermStrings(numTerms);
  queries.clear();
  queries.reserve((size_t) numTerms);
  for (const auto& term : terms) {
    queries.emplace_back("body_w", term);
  }
  optional.clear();
  optional.reserve((size_t) numTerms);
  for (auto& query : queries) {
    optional.push_back(&query);
  }
  BooleanQuery query({}, optional, {}, {});
  return query.createWeight(qContext, Query::NEED_SCORES);
}

DisjunctionTopKRun runExhaustiveTermDisjunctionTopK(IndexReader& reader,
                                                    std::span<const std::string_view> terms,
                                                    int32_t topK) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto queries = makeTermQueries(terms);
  std::vector<Query::Weight*> weights;
  weights.reserve(queries.size());
  for (auto& query : queries) {
    weights.push_back(query.createWeight(qContext, Query::NEED_SCORES));
  }

  TopDocsCollector collector(topK);
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count == 0) continue;
    Query::Scorer* scorer = count == 1
      ? arr[0]
      : pool.make<BooleanQuery::DisjunctionScorer>(
          pool, std::span<Query::Scorer*>(arr, (size_t) count),
          singlePhaseScorersForTests(pool, (size_t)count), true);
    collectTopK(segnum, scorer, nullptr, nullptr, collector, false);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runDenseFilteredPullTopK(IndexReader& reader, int32_t numTerms, int32_t topK,
                                            int32_t filterStep = 2, bool arrayDocSet = false) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::vector<std::string> terms;
  std::vector<TermQuery> queries;
  std::vector<Query*> optional;
  auto* weight = createDenseDisjunctionWeight(qContext, numTerms, terms, queries, optional);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto filter = makeEveryNthDocSet(segments[segnum].maxDoc(), filterStep, arrayDocSet);
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) continue;
    collectTopK(segnum, scorer, filter.get(), nullptr, collector);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runDenseFilteredBulkTopK(IndexReader& reader, int32_t numTerms, int32_t topK,
                                            int32_t filterStep = 2, bool arrayDocSet = false) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::vector<std::string> terms;
  std::vector<TermQuery> queries;
  std::vector<Query*> optional;
  auto* weight = createDenseDisjunctionWeight(qContext, numTerms, terms, queries, optional);
  TopDocsCollector collector(topK);
  DisjunctionTopKRun result;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto filter = makeEveryNthDocSet(segments[segnum].maxDoc(), filterStep, arrayDocSet);
    auto* supplier = weight->scorerSupplier(pool, segments[segnum]);
    if (supplier == nullptr) continue;
    auto* bulk = supplier->bulkScorer(pool);
    if (bulk == nullptr) {
      ADD_FAILURE() << "bulkScorer returned null for dense segment " << segnum;
      continue;
    }
    auto* maxScoreBulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(bulk);
    if (maxScoreBulk == nullptr) {
      ADD_FAILURE() << "bulkScorer returned unexpected type for dense segment " << segnum;
      continue;
    }
    int64_t beforeBs1Windows = maxScoreBulk->bs1WindowCount();
    int64_t beforeDomainDriveWindows = maxScoreBulk->domainDriveWindowCount();
    collectTopKWindowed(segnum, bulk, filter.get(), collector, nullptr,
                        segments[segnum].maxDoc());
    result.bs1Windows += maxScoreBulk->bs1WindowCount() - beforeBs1Windows;
    result.domainDriveWindows += maxScoreBulk->domainDriveWindowCount() - beforeDomainDriveWindows;
  }

  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runBulkTermDisjunctionTopK(IndexReader& reader,
                                              std::span<const std::string_view> terms,
                                              int32_t topK,
                                              bool segmentCollectors,
                                              MaxScoreAccumulator* accumulator) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);

  TopDocsCollector merged(topK);
  TopDocsCollector single(topK);
  int64_t visited = 0;
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* supplier = weight->scorerSupplier(pool, segments[segnum]);
    if (supplier == nullptr) continue;
    auto* bulk = supplier->bulkScorer(pool);
    if (bulk == nullptr) {
      ADD_FAILURE() << "bulkScorer returned null for segment " << segnum;
      continue;
    }
    if (segmentCollectors) {
      TopDocsCollector segmentCollector(topK);
      collectTopKWindowed(segnum, bulk, nullptr, segmentCollector, accumulator,
                          segments[segnum].maxDoc());
      visited += segmentCollector.totalHits();
      merged.merge(segmentCollector);
    } else {
      collectTopKWindowed(segnum, bulk, nullptr, single, accumulator,
                          segments[segnum].maxDoc());
    }
  }

  DisjunctionTopKRun result;
  if (segmentCollectors) {
    result.visited = visited;
    result.topDocs = sortedCollectorDocs(merged);
  } else {
    result.visited = single.totalHits();
    result.topDocs = sortedCollectorDocs(single);
  }
  return result;
}

int64_t countBulkTermDisjunctionSegment(MemPool& pool, Query::Context& qContext,
                                        IndexReader::Segment& segment,
                                        std::span<const std::string_view> terms,
                                        DocSet* filter) {
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, segment);
  if (supplier == nullptr) {
    return 0;
  }
  auto* bulk = supplier->bulkScorer(pool);
  if (bulk == nullptr) {
    ADD_FAILURE() << "bulkScorer returned null";
    return -1;
  }

  int64_t count = 0;
  for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
    int32_t next = bulk->countNextWindow(count, nullptr, filter, cursor, segment.maxDoc());
    if (next == PostingsReader::END) {
      break;
    }
    if (next <= cursor) {
      ADD_FAILURE() << "countNextWindow made no progress";
      break;
    }
    cursor = next;
  }
  return count;
}

int64_t countPullTermDisjunctionSegment(MemPool& pool, Query::Context& qContext,
                                        IndexReader::Segment& segment,
                                        std::span<const std::string_view> terms,
                                        DocSet* filter) {
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = weight->createScorer(pool, segment);
  if (scorer == nullptr) {
    return 0;
  }

  int64_t count = 0;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    if (filter == nullptr || filter->get(doc)) {
      count++;
    }
  }
  return count;
}

std::unique_ptr<DocSet> makeEveryNthSegmentDocSet(IndexReader::Segment& segment,
                                                  int32_t step, bool arrayDocSet,
                                                  bool liveOnly);

int64_t countBulkTermDisjunctionAll(IndexReader& reader,
                                    std::span<const std::string_view> terms,
                                    int32_t filterStep, bool arrayDocSet, bool liveOnly) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  int64_t count = 0;
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto filter = makeEveryNthSegmentDocSet(segments[segnum], filterStep,
                                            arrayDocSet, liveOnly);
    count += countBulkTermDisjunctionSegment(pool, qContext, segments[segnum],
                                             terms, filter.get());
  }
  return count;
}

int64_t countPullTermDisjunctionAll(IndexReader& reader,
                                    std::span<const std::string_view> terms,
                                    int32_t filterStep, bool arrayDocSet, bool liveOnly) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  int64_t count = 0;
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto filter = makeEveryNthSegmentDocSet(segments[segnum], filterStep,
                                            arrayDocSet, liveOnly);
    count += countPullTermDisjunctionSegment(pool, qContext, segments[segnum],
                                             terms, filter.get());
  }
  return count;
}

int64_t countBulkTermConjunctionSegment(MemPool& pool, Query::Context& qContext,
                                        IndexReader::Segment& segment,
                                        std::span<const std::string_view> terms,
                                        DocSet* filter) {
  auto queries = makeTermQueries(terms);
  auto mandatory = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(std::span<Query*>(mandatory.data(), mandatory.size()), empty, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, segment);
  if (supplier == nullptr) {
    return 0;
  }
  auto* bulk = supplier->bulkScorer(pool);
  if (bulk == nullptr) {
    ADD_FAILURE() << "bulkScorer returned null";
    return -1;
  }

  int64_t count = 0;
  for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
    int32_t next = bulk->countNextWindow(count, nullptr, filter, cursor, segment.maxDoc());
    if (next == PostingsReader::END) {
      break;
    }
    if (next <= cursor) {
      ADD_FAILURE() << "countNextWindow made no progress";
      break;
    }
    cursor = next;
  }
  return count;
}

int64_t countPullTermConjunctionSegment(MemPool& pool, Query::Context& qContext,
                                        IndexReader::Segment& segment,
                                        std::span<const std::string_view> terms,
                                        DocSet* filter) {
  auto queries = makeTermQueries(terms);
  auto mandatory = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(std::span<Query*>(mandatory.data(), mandatory.size()), empty, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = weight->createScorer(pool, segment);
  if (scorer == nullptr) {
    return 0;
  }

  int64_t count = 0;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    if (filter == nullptr || filter->get(doc)) {
      count++;
    }
  }
  return count;
}

std::vector<std::string_view> termViews(std::span<const std::string> terms) {
  std::vector<std::string_view> views;
  views.reserve(terms.size());
  for (const auto& term : terms) {
    views.push_back(term);
  }
  return views;
}

std::vector<std::string> makeSweepTermStrings(int32_t numTerms) {
  std::vector<std::string> terms;
  terms.reserve((size_t) numTerms);
  for (int32_t i = 0; i < numTerms; i++) {
    terms.push_back("sweep" + std::to_string(i));
  }
  return terms;
}

void addSweepDisjunctionDocs(TestField& field, int32_t numTerms) {
  const int32_t nDocs = 3 * DocsEnumMeta::L1_DOCS + 211;
  auto terms = makeSweepTermStrings(numTerms);

  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    auto add = [&](std::string_view term, int32_t count) {
      appendRepeatedTerm(body, term, count);
      used += count;
    };

    if (doc < 32) {
      for (int32_t term = 0; term < numTerms; term++) {
        add(terms[(size_t) term], 8 + ((doc + term) % 4));
      }
    } else {
      bool any = false;
      for (int32_t term = 0; term < numTerms; term++) {
        bool match = ((doc + term * 7) % (term + 2)) == 0;
        if (doc % 97 == 0) {
          match = true;
        }
        if (match) {
          any = true;
          add(terms[(size_t) term], 1 + ((doc + term) % 3));
        }
      }
      if (!any) {
        add("sweep_filler", 1);
      }
    }

    int32_t len = used + 4 + (doc % 3);
    add("sweep_filler", len - used);
    field.add(doc, body);
  }
}

std::vector<std::string> makeWindowDispatchTermStrings(int32_t numTerms) {
  std::vector<std::string> terms;
  terms.reserve((size_t) numTerms);
  for (int32_t i = 0; i < numTerms; i++) {
    terms.push_back("wd_t" + std::to_string(i));
  }
  return terms;
}

void addWindowDispatchRandomDocs(TestField& field, int32_t nDocs, int32_t numTerms) {
  auto terms = makeWindowDispatchTermStrings(numTerms);
  uint32_t state = 0x51ed1234U;
  auto nextRand = [&]() {
    state = state * 1664525U + 1013904223U;
    return state;
  };

  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    for (int32_t term = 0; term < numTerms; term++) {
      int32_t period = term == 0 ? 2
        : term == 1 ? 3
        : term == 2 ? 5
        : term == 3 ? 11
        : term == 4 ? 31
        : term == 5 ? 97
        : term == 6 ? 257
        : 521;
      bool match = ((doc + term * 17) % period) == 0;
      if ((nextRand() & ((1U << (term + 1)) - 1U)) == 0U) {
        match = true;
      }
      if (doc == DocsEnumMeta::L1_DOCS && term == 0) {
        match = true;
      }
      if (doc == 2 * DocsEnumMeta::L1_DOCS && term == 1) {
        match = true;
      }
      if (match) {
        int32_t repeats = 1 + (int32_t) ((doc + term) % 3);
        appendRepeatedTerm(body, terms[(size_t) term], repeats);
        used += repeats;
      }
    }
    int32_t len = used + 4 + (doc % 3);
    appendRepeatedTerm(body, "wd_pad", len - used);
    field.add(doc, body);
  }
}

void addWindowDispatchBoundaryDocs(TestField& field) {
  const int32_t nDocs = 3 * DocsEnumMeta::L1_DOCS + 100;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    auto add = [&](std::string_view term) {
      appendRepeatedTerm(body, term, 1);
      used++;
    };

    if (doc == DocsEnumMeta::L1_DOCS) add("wd_at_end_a");
    if (doc == 2 * DocsEnumMeta::L1_DOCS) add("wd_at_end_b");
    if (doc == 0) add("wd_top2_end_a");
    if (doc == DocsEnumMeta::L1_DOCS) add("wd_top2_end_b");
    if (doc == 17) {
      add("wd_same_a");
      add("wd_same_b");
    }
    if (doc == 1000) add("wd_exhaust_a");
    if (doc == 7000 || doc == 11000) add("wd_exhaust_b");
    if (doc == 1000) add("wd_rare_driver");
    if (doc >= 7000 && (doc % 3) == 0) add("wd_common_late");
    if (doc < DocsEnumMeta::L1_DOCS) add("wd_half_exact_a");
    if (doc >= DocsEnumMeta::L1_DOCS / 2 && doc < DocsEnumMeta::L1_DOCS) add("wd_half_exact_b");
    if (doc < DocsEnumMeta::L1_DOCS) add("wd_half_inside_a");
    if (doc >= DocsEnumMeta::L1_DOCS / 2 - 1 && doc < DocsEnumMeta::L1_DOCS) {
      add("wd_half_inside_b");
    }
    if (doc == 0 || doc == 3000) add("wd_half_gap_a");
    if (doc >= DocsEnumMeta::L1_DOCS / 2 && doc < DocsEnumMeta::L1_DOCS) add("wd_half_gap_b");
    if ((doc % 2) == 0) add("wd_latch_low");
    if (doc >= 1000 && doc < 12000 && (doc % 2) == 0) add("wd_latch_a");
    if (doc >= 7000 && doc < 12000 && (doc % 2) == 0) add("wd_latch_b");
    if ((doc % 2) == 0) add("wd_dead_dense_a");
    if ((doc % 3) == 0) add("wd_dead_dense_b");

    int32_t len = used + 4 + (doc % 3);
    appendRepeatedTerm(body, "wd_boundary_pad", len - used);
    field.add(doc, body);
  }
}

std::vector<int32_t> makeContiguousProbeBlock(int32_t docBase) {
  std::vector<int32_t> docs;
  docs.reserve(Postings::DOCS_BLOCK_SIZE);
  for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
    docs.push_back(docBase + i);
  }
  return docs;
}

std::vector<int32_t> makePackedProbeBlock(int32_t docBase) {
  std::vector<int32_t> docs;
  docs.reserve(Postings::DOCS_BLOCK_SIZE);
  for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
    docs.push_back(docBase + i * 8);
  }
  return docs;
}

std::vector<int32_t> makeWordProbeBlock(int32_t docBase) {
  std::vector<int32_t> docs;
  docs.reserve(Postings::DOCS_BLOCK_SIZE);
  int32_t doc = docBase;
  for (int32_t i = 0; i < Postings::DOCS_BLOCK_SIZE; i++) {
    if (i == 0) {
      doc = docBase;
    } else {
      doc += (i % 37) == 0 ? 31 : ((i % 3) == 0 ? 2 : 3);
    }
    docs.push_back(doc);
  }
  return docs;
}

std::vector<int32_t> makeMixedProbePostings() {
  auto docs = makeContiguousProbeBlock(0);
  auto packed = makePackedProbeBlock(docs.back() + 1);
  docs.insert(docs.end(), packed.begin(), packed.end());
  auto word = makeWordProbeBlock(docs.back() + 1);
  docs.insert(docs.end(), word.begin(), word.end());
  return docs;
}

void indexProbeTermDocs(CollectionHelper& helper, std::string_view term,
                        const std::vector<int32_t>& postings,
                        std::string_view idPrefix, int32_t extraDocs = 8) {
  helper.clear();
  std::vector<Doc> docs;
  int32_t maxDoc = postings.empty() ? extraDocs : postings.back() + extraDocs;
  docs.reserve((size_t) maxDoc);
  size_t posting = 0;
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    std::string body;
    int32_t used = 0;
    if (posting < postings.size() && postings[posting] == doc) {
      int32_t tf = 1 + (int32_t) (posting % 5);
      appendRepeatedTerm(body, term, tf);
      used += tf;
      posting++;
    }
    int32_t len = used + 17 + (doc % 11);
    appendRepeatedTerm(body, "probe_filler", len - used);
    docs.push_back(flatdoc("id", std::string(idPrefix) + "_" + std::to_string(doc),
                           "body_w", body));
  }
  ASSERT_EQ(posting, postings.size());
  helper.indexAll(docs, UpdateMessage::COMMIT);
}

std::vector<float> initialCandidateScores(int32_t size) {
  std::vector<float> scores((size_t) size);
  for (int32_t i = 0; i < size; i++) {
    scores[(size_t) i] = (float) (i % 9) * 0.125f;
  }
  return scores;
}

struct CandidateSweepRun {
  std::vector<int32_t> docs;
  std::vector<float> scores;
  int64_t scoredWordProbes = 0;
};

CandidateSweepRun runApplyTermCandidateSweep(IndexReader& reader, std::string_view term,
                                             const std::vector<int32_t>& candidates,
                                             bool required, bool collectStats = false) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", term);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = weight->createScorer(pool, segment);
  EXPECT_NE(scorer, nullptr);

  CandidateSweepRun run;
  run.docs = candidates;
  run.scores = initialCandidateScores((int32_t) candidates.size());
  bool savedStats = SkipStats::enabled;
  if (collectStats) {
    SkipStats::enabled = true;
    SkipStats::reset();
  }
  int32_t size = scorer == nullptr ? 0 : scorer->applyToCandidates(
      run.docs.data(), run.scores.data(), (int32_t) run.docs.size(), required);
  if (required) {
    run.docs.resize((size_t) size);
    run.scores.resize((size_t) size);
  }
  if (collectStats) {
    run.scoredWordProbes = SkipStats::scoredWordProbeAdvances;
    SkipStats::enabled = savedStats;
  }
  return run;
}

CandidateSweepRun runPerDocTermCandidateSweep(IndexReader& reader, std::string_view term,
                                              const std::vector<int32_t>& candidates,
                                              bool required) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", term);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = weight->createScorer(pool, segment);
  EXPECT_NE(scorer, nullptr);

  CandidateSweepRun run;
  run.docs = candidates;
  run.scores = initialCandidateScores((int32_t) candidates.size());
  if (scorer == nullptr) {
    if (required) {
      run.docs.clear();
      run.scores.clear();
    }
    return run;
  }

  int32_t write = 0;
  int32_t current = scorer->docId();
  for (int32_t i = 0; i < (int32_t) candidates.size(); i++) {
    int32_t target = run.docs[(size_t) i];
    if (current < target) {
      current = scorer->advance(target);
    }
    bool matched = current == target;
    if (matched) {
      run.scores[(size_t) i] += scorer->score();
    }
    if (required && matched) {
      if (write != i) {
        run.docs[(size_t) write] = run.docs[(size_t) i];
        run.scores[(size_t) write] = run.scores[(size_t) i];
      }
      write++;
    }
  }
  if (required) {
    run.docs.resize((size_t) write);
    run.scores.resize((size_t) write);
  }
  return run;
}

void expectCandidateSweepEqual(const CandidateSweepRun& expected,
                               const CandidateSweepRun& actual) {
  ASSERT_EQ(expected.docs, actual.docs);
  ASSERT_EQ(expected.scores.size(), actual.scores.size());
  for (size_t i = 0; i < expected.scores.size(); i++) {
    EXPECT_EQ(std::bit_cast<uint32_t>(expected.scores[i]),
              std::bit_cast<uint32_t>(actual.scores[i])) << "i=" << i;
  }
}

struct FillRun {
  std::vector<int32_t> docs;
  std::vector<float> scores;
};

FillRun collectTermPerDoc(IndexReader& reader, std::string_view term,
                          int32_t start, int32_t upTo) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", term);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = weight->createScorer(pool, segment);
  FillRun run;
  if (scorer == nullptr) {
    return run;
  }
  int32_t doc = scorer->docId() < start ? scorer->advance(start) : scorer->docId();
  while (doc < upTo) {
    run.docs.push_back(doc);
    run.scores.push_back(scorer->score());
    doc = scorer->next();
  }
  return run;
}

void expectFillRunNear(const FillRun& expected, const FillRun& actual) {
  ASSERT_EQ(expected.docs, actual.docs);
  ASSERT_EQ(expected.scores.size(), actual.scores.size());
  for (size_t i = 0; i < expected.scores.size(); i++) {
    EXPECT_NEAR(expected.scores[i], actual.scores[i], 1.0e-5f) << "doc="
                                                              << expected.docs[i];
  }
}

FillRun fillTermScoreBlockCalls(IndexReader& reader, std::string_view term,
                                int32_t start, int32_t upTo,
                                std::span<const int32_t> counts,
                                float minCompetitiveScore) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", term);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = weight->createScorer(pool, segment);
  EXPECT_NE(scorer, nullptr);

  FillRun run;
  if (scorer == nullptr) {
    return run;
  }
  scorer->setMinCompetitiveScore(minCompetitiveScore);
  if (scorer->docId() < start) {
    scorer->advance(start);
  }

  std::vector<int32_t> docs;
  std::vector<float> scores;
  for (int32_t count : counts) {
    docs.resize((size_t) count);
    scores.resize((size_t) count);
    int32_t n = scorer->fillScoreBlock(docs.data(), scores.data(), count, upTo);
    for (int32_t i = 0; i < n; i++) {
      run.docs.push_back(docs[(size_t) i]);
      run.scores.push_back(scores[(size_t) i]);
    }
  }
  return run;
}

std::unique_ptr<DocSet> makeEveryNthSegmentDocSet(IndexReader::Segment& segment,
                                                  int32_t step, bool arrayDocSet,
                                                  bool liveOnly) {
  if (step <= 0) {
    return nullptr;
  }
  auto isLive = [&](int32_t doc) {
    return !liveOnly || segment.liveDocs() == nullptr || segment.liveDocs()->bitset().get(doc);
  };
  if (arrayDocSet) {
    std::vector<int32_t> docs;
    docs.reserve((size_t) ((segment.maxDoc() + step - 1) / step));
    for (int32_t doc = 0; doc < segment.maxDoc(); doc += step) {
      if (isLive(doc)) {
        docs.push_back(doc);
      }
    }
    return std::make_unique<ArrDocSet>(std::move(docs));
  }

  auto filter = std::make_unique<RAMBitDocSet>(segment.maxDoc());
  for (int32_t doc = 0; doc < segment.maxDoc(); doc += step) {
    if (isLive(doc)) {
      filter->mutableBits().set(doc);
    }
  }
  return filter;
}

DisjunctionTopKRun runFilteredExhaustiveTermDisjunctionTopK(
    IndexReader& reader, std::span<const std::string_view> terms, int32_t topK,
    int32_t filterStep = 0, bool arrayDocSet = false, bool liveOnly = false) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto queries = makeTermQueries(terms);
  std::vector<Query::Weight*> weights;
  weights.reserve(queries.size());
  for (auto& query : queries) {
    weights.push_back(query.createWeight(qContext, Query::NEED_SCORES));
  }

  TopDocsCollector collector(topK);
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count == 0) continue;
    Query::Scorer* scorer = count == 1
      ? arr[0]
      : pool.make<BooleanQuery::DisjunctionScorer>(
          pool, std::span<Query::Scorer*>(arr, (size_t) count),
          singlePhaseScorersForTests(pool, (size_t)count), true);
    auto filter = makeEveryNthSegmentDocSet(segments[segnum], filterStep,
                                            arrayDocSet, liveOnly);
    collectTopK(segnum, scorer, filter.get(), nullptr, collector, false);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runFilteredBulkTermDisjunctionTopK(
    IndexReader& reader, std::span<const std::string_view> terms, int32_t topK,
    int32_t filterStep = 0, bool arrayDocSet = false, bool liveOnly = false,
    bool allowPruning = true, bool disableWindowDispatch = false) {
  WindowDispatchGuard dispatchGuard(disableWindowDispatch);
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* supplier = weight->scorerSupplier(pool, segments[segnum]);
    if (supplier == nullptr) continue;
    auto* bulk = supplier->bulkScorer(pool);
    if (bulk == nullptr) {
      ADD_FAILURE() << "bulkScorer returned null for segment " << segnum;
      continue;
    }
    auto filter = makeEveryNthSegmentDocSet(segments[segnum], filterStep,
                                            arrayDocSet, liveOnly);
    collectTopKWindowed(segnum, bulk, filter.get(), collector, nullptr,
                        segments[segnum].maxDoc(), allowPruning);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runBulkBooleanTopK(IndexReader& reader,
                                      std::span<Query*> optional,
                                      int32_t topK,
                                      bool* usedCostAwareOrder) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::span<Query*> empty;
  BooleanQuery query(empty, optional, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  bool usedCostOrder = false;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* supplier = weight->scorerSupplier(pool, segments[segnum]);
    if (supplier == nullptr) continue;
    auto* bulk = supplier->bulkScorer(pool);
    auto* maxScoreBulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(bulk);
    if (maxScoreBulk == nullptr) {
      ADD_FAILURE() << "bulkScorer returned unexpected type for segment " << segnum;
      continue;
    }
    usedCostOrder |= maxScoreBulk->usesCostAwareWindowOrderForTests();
    collectTopKWindowed(segnum, bulk, nullptr, collector, nullptr,
                        segments[segnum].maxDoc());
  }

  if (usedCostAwareOrder != nullptr) {
    *usedCostAwareOrder = usedCostOrder;
  }
  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runMandOptSupplierTopK(IndexReader& reader,
                                          std::span<Query*> mandatory,
                                          std::span<Query*> optional,
                                          int32_t topK,
                                          bool disableBulk,
                                          int32_t filterStep = 0,
                                          bool arrayDocSet = false,
                                          bool liveOnly = false,
                                          bool allowPruning = true,
                                          bool* sawBulkOut = nullptr) {
  MandOptBulkGuard guard(disableBulk);
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::span<Query*> empty;
  BooleanQuery query(mandatory, optional, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  MaxScoreAccumulator accumulator;
  bool sawBulk = false;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* supplier = weight->scorerSupplier(pool, segments[segnum]);
    if (supplier == nullptr) {
      continue;
    }
    auto filter = makeEveryNthSegmentDocSet(segments[segnum], filterStep,
                                            arrayDocSet, liveOnly);
    auto* bulk = supplier->bulkScorer(pool);
    if (bulk != nullptr) {
      sawBulk = true;
      collectTopKWindowed(segnum, bulk, filter.get(), collector,
                          allowPruning ? &accumulator : nullptr,
                          segments[segnum].maxDoc(), allowPruning);
      continue;
    }
    auto* scorer = buildScorerForTests(
        pool, *supplier, std::numeric_limits<int64_t>::max());
    if (scorer != nullptr) {
      collectTopK(segnum, scorer, filter.get(), nullptr, collector, allowPruning,
                  allowPruning ? &accumulator : nullptr);
    }
  }

  if (sawBulkOut != nullptr) {
    *sawBulkOut = sawBulk;
  }
  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

BulkScorer* createMandOptBulkScorer(MemPool& pool, Query::Context& qContext,
                                    IndexReader::Segment& segment,
                                    std::span<Query*> mandatory,
                                    std::span<Query*> optional) {
  std::span<Query*> empty;
  BooleanQuery query(mandatory, optional, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, segment);
  if (supplier == nullptr) {
    return nullptr;
  }
  return supplier->bulkScorer(pool);
}

struct WindowScore {
  int32_t doc = 0;
  float score = 0.0f;
};

std::vector<WindowScore> exhaustiveWindowScores(IndexReader& reader,
                                                std::span<const std::string_view> terms,
                                                int32_t minDoc, int32_t maxDoc) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto queries = makeTermQueries(terms);
  std::vector<Query::Weight*> weights;
  weights.reserve(queries.size());
  for (auto& query : queries) {
    weights.push_back(query.createWeight(qContext, Query::NEED_SCORES));
  }

  std::vector<WindowScore> scores;
  auto& segment = qContext.topReader.segments()[0];
  auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
  int32_t count = 0;
  for (auto* weight : weights) {
    auto* scorer = weight->createScorer(pool, segment);
    if (scorer != nullptr) arr[count++] = scorer;
  }
  if (count == 0) {
    return scores;
  }
  Query::Scorer* scorer = count == 1
    ? arr[0]
    : pool.make<BooleanQuery::DisjunctionScorer>(
        pool, std::span<Query::Scorer*>(arr, (size_t) count),
        singlePhaseScorersForTests(pool, (size_t)count), true);
  int32_t doc = scorer->docId() < minDoc ? scorer->advance(minDoc) : scorer->docId();
  while (doc < maxDoc) {
    scores.push_back({doc, scorer->score()});
    doc = scorer->next();
  }
  return scores;
}

BulkScorer* createBulkTermDisjunctionScorer(MemPool& pool, Query::Context& qContext,
                                            IndexReader::Segment& segment,
                                            std::span<const std::string_view> terms) {
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, segment);
  if (supplier == nullptr) {
    return nullptr;
  }
  return supplier->bulkScorer(pool);
}

struct SingleEssentialWindowRun {
  std::vector<int32_t> docs;
  std::vector<float> scores;
  int64_t directFills = 0;
  int64_t sweepWindows = 0;
};

SingleEssentialWindowRun runSingleEssentialBulkWindow(IndexReader& reader,
                                                      std::span<const std::string_view> terms,
                                                      int32_t windowStart, int32_t windowEnd,
                                                      float theta, bool withFilter) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto& segment = qContext.topReader.segments()[0];
  auto* bulk = createBulkTermDisjunctionScorer(pool, qContext, segment, terms);
  EXPECT_NE(bulk, nullptr);

  std::unique_ptr<DocSet> filter;
  if (withFilter) {
    filter = makeEveryNthDocSet(segment.maxDoc(), 1, false);
  }

  ScoreWindow window;
  SingleEssentialWindowRun run;
  SkipStatsGuard stats;
  if (bulk != nullptr) {
    int32_t next = bulk->scoreNextWindow(window, filter.get(), windowStart, windowEnd, theta);
    EXPECT_EQ(next, PostingsReader::END);
    run.docs.reserve((size_t) window.size);
    run.scores.reserve((size_t) window.size);
    for (int32_t i = 0; i < window.size; i++) {
      run.docs.push_back(window.docs[(size_t) i]);
      run.scores.push_back(window.scores[(size_t) i]);
    }
  }
  run.directFills = SkipStats::maxScoreDirectFills;
  run.sweepWindows = SkipStats::maxScoreSweepWindows;
  return run;
}

struct BulkWindowRun {
  std::vector<int32_t> docs;
  std::vector<float> scores;
  int64_t deadOuterJumps = 0;
  int64_t anchorJumps = 0;
  int64_t top2Conversions = 0;
  int64_t halfWindowClips = 0;
  int64_t partitionLatchReuses = 0;
  int64_t partitionLatchBreaks = 0;
  int64_t thresholdRefreshes = 0;
};

BulkWindowRun collectBulkWindows(IndexReader& reader, std::span<const std::string_view> terms,
                                 int32_t minDoc, int32_t maxDoc, float theta,
                                 bool disableWindowDispatch) {
  WindowDispatchGuard dispatchGuard(disableWindowDispatch);
  MemPool pool;
  Query::Context qContext(pool, reader);
  auto& segment = qContext.topReader.segments()[0];
  auto* bulk = createBulkTermDisjunctionScorer(pool, qContext, segment, terms);
  EXPECT_NE(bulk, nullptr);

  BulkWindowRun run;
  if (bulk == nullptr) {
    return run;
  }

  ScoreWindow window;
  SkipStatsGuard stats;
  for (int32_t cursor = minDoc; cursor != PostingsReader::END && cursor < maxDoc; ) {
    int32_t next = bulk->scoreNextWindow(window, nullptr, cursor, maxDoc, theta);
    for (int32_t i = 0; i < window.size; i++) {
      run.docs.push_back(window.docs[(size_t) i]);
      run.scores.push_back(window.scores[(size_t) i]);
    }
    if (next == PostingsReader::END) {
      break;
    }
    EXPECT_GT(next, cursor);
    cursor = next;
  }
  run.deadOuterJumps = SkipStats::maxScoreDeadOuterJumps;
  run.anchorJumps = SkipStats::maxScoreAnchorJumps;
  run.top2Conversions = SkipStats::maxScoreTop2Conversions;
  run.halfWindowClips = SkipStats::maxScoreHalfWindowClips;
  run.partitionLatchReuses = SkipStats::maxScorePartitionLatchReuses;
  run.partitionLatchBreaks = SkipStats::maxScorePartitionLatchBreaks;
  run.thresholdRefreshes = SkipStats::maxScoreThresholdRefreshes;
  return run;
}

void expectBulkWindowRunsEqual(const BulkWindowRun& expected, const BulkWindowRun& actual) {
  ASSERT_EQ(expected.docs, actual.docs);
  ASSERT_EQ(expected.scores.size(), actual.scores.size());
  for (size_t i = 0; i < expected.scores.size(); i++) {
    EXPECT_EQ(std::bit_cast<uint32_t>(expected.scores[i]),
              std::bit_cast<uint32_t>(actual.scores[i])) << "doc=" << expected.docs[i];
  }
}

struct DomainCountRun {
  int64_t count = 0;
  std::unique_ptr<DocSet> domain;
};

std::vector<int32_t> docSetDocs(DocSet* docSet, int32_t maxDoc) {
  std::vector<int32_t> docs;
  if (docSet == nullptr) {
    return docs;
  }
  docs.reserve((size_t) docSet->card());
  if (docSet->type == DocSet::ARRAY) {
    auto arr = ((ArrDocSet*) docSet)->docs();
    docs.assign(arr.begin(), arr.end());
    return docs;
  }
  const auto& bits = ((BitDocSet*) docSet)->bits();
  int32_t doc = maxDoc == 0 ? FixedBitSet::MAX_INDEX : bits.nextSetBit(0);
  while (doc < maxDoc) {
    docs.push_back(doc);
    if (doc + 1 >= maxDoc) {
      break;
    }
    doc = bits.nextSetBit(doc + 1);
  }
  return docs;
}

void expectDocSetEqual(DocSet* actual, DocSet* expected, int32_t maxDoc) {
  ASSERT_NE(actual, nullptr);
  ASSERT_NE(expected, nullptr);
  EXPECT_EQ(actual->card(), expected->card());
  EXPECT_EQ(docSetDocs(actual, maxDoc), docSetDocs(expected, maxDoc));
  for (int32_t doc = 0; doc < maxDoc; doc++) {
    ASSERT_EQ(actual->get(doc), expected->get(doc)) << "doc=" << doc;
  }
}

DomainCountRun pullDomain(Query::Weight* weight, MemPool& pool,
                          IndexReader::Segment& segment, DocSet* filter) {
  DocSetBuilder builder(segment.maxDoc());
  int64_t count = 0;
  auto* scorer = weight->createScorer(pool, segment);
  if (scorer != nullptr) {
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      if (filter != nullptr && !filter->get(doc)) {
        continue;
      }
      builder.add(doc);
      count++;
    }
  }
  return {count, builder.build()};
}

DomainCountRun bulkCountDomain(Query::Weight* weight, MemPool& pool,
                               IndexReader::Segment& segment, DocSet* filter) {
  DocSetBuilder builder(segment.maxDoc());
  int64_t count = 0;
  auto* supplier = weight->scorerSupplier(pool, segment);
  if (supplier != nullptr) {
    auto* bulk = supplier->bulkScorer(pool);
    if (bulk == nullptr) {
      ADD_FAILURE() << "bulkScorer returned null";
    } else {
      for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
        int32_t next = bulk->countNextWindow(count, &builder, filter, cursor, segment.maxDoc());
        if (next == PostingsReader::END) {
          break;
        }
        if (next <= cursor) {
          ADD_FAILURE() << "countNextWindow made no progress";
          break;
        }
        cursor = next;
      }
    }
  }
  EXPECT_EQ(count, builder.card());
  return {count, builder.build()};
}

void expectBulkDomainMatchesPull(Query::Weight* bulkWeight, Query::Weight* pullWeight,
                                 MemPool& pool, IndexReader::Segment& segment,
                                 DocSet* filter) {
  auto expected = pullDomain(pullWeight, pool, segment, filter);
  auto actual = bulkCountDomain(bulkWeight, pool, segment, filter);
  EXPECT_EQ(actual.count, expected.count);
  expectDocSetEqual(actual.domain.get(), expected.domain.get(), segment.maxDoc());
}

void addBulkFillTermDoc(TestField& f, int32_t doc, bool a, bool b, bool c) {
  std::string body;
  if (a) appendRepeatedTerm(body, "bf_a", 1 + (doc % 5));
  if (b) appendRepeatedTerm(body, "bf_b", 1 + ((doc / 3) % 4));
  if (c) appendRepeatedTerm(body, "bf_c", 1 + ((doc / 7) % 3));
  appendRepeatedTerm(body, "filler", 1 + (doc % 11));
  f.add(doc, body);
}

void addBulkTieDisjunctionDocs(CollectionHelper& helper) {
  for (int32_t seg = 0; seg < 2; seg++) {
    std::vector<Doc> docs;
    docs.reserve(32);
    for (int32_t local = 0; local < 32; local++) {
      std::string term = (local % 2) == 0 ? "tie_a" : "tie_b";
      std::string body = term + " filler filler filler";
      docs.push_back(flatdoc("id", "t" + std::to_string(seg) + "_" + std::to_string(local),
                             "body_w", body));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
}


std::string makeCrossSegmentBody(int32_t tf, int32_t len) {
  std::string body;
  appendRepeatedTerm(body, "needle", tf);
  appendRepeatedTerm(body, "filler", len - tf);
  return body;
}

void addCrossSegmentAccumulatorDocs(CollectionHelper& helper, std::vector<std::vector<std::string>>& idsBySeg,
                                    int32_t segCount = 3) {
  const std::array<int32_t, 3> segDocs = {
    2 * Postings::DOCS_BLOCK_SIZE + 17,
    6 * Postings::DOCS_BLOCK_SIZE + 31,
    4 * Postings::DOCS_BLOCK_SIZE + 19
  };
  idsBySeg.clear();
  idsBySeg.resize((size_t) segCount);

  for (int32_t seg = 0; seg < segCount; seg++) {
    std::vector<Doc> docs;
    docs.reserve((size_t) segDocs[(size_t) seg]);
    idsBySeg[(size_t) seg].reserve((size_t) segDocs[(size_t) seg]);
    for (int32_t local = 0; local < segDocs[(size_t) seg]; local++) {
      int32_t tf;
      int32_t len;
      if (seg == 0 && local < 16) {
        tf = 96 - local * 3;
        len = 128;
      } else if (seg == 0) {
        tf = 2;
        len = 220 + (local % 37);
      } else {
        tf = 1;
        int32_t docsInSeg = segDocs[(size_t) seg];
        int32_t lenRange = 140;
        len = 260 - (local * lenRange / std::max(1, docsInSeg - 1));
        if (seg == 2) len += 20;
      }
      std::string id = "s" + std::to_string(seg) + "_" + std::to_string(local);
      idsBySeg[(size_t) seg].push_back(id);
      docs.push_back(flatdoc("id", id, "body_w", makeCrossSegmentBody(tf, len)));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
}

DisjunctionTopKRun runCrossSegmentTermTopK(IndexReader& reader, int32_t topK,
                                           bool allowPruning,
                                           int32_t segmentCount = std::numeric_limits<int32_t>::max(),
                                           MaxScoreAccumulator* accumulator = nullptr) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", "needle");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0;
       segnum < (int32_t) segments.size() && segnum < segmentCount;
       segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) continue;
    collectTopK(segnum, scorer, nullptr, nullptr, collector, allowPruning, accumulator);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

void collectCrossSegmentTermSegment(IndexReader& reader, int32_t segnum, int32_t topK,
                                    bool allowPruning, MaxScoreAccumulator* accumulator,
                                    TopDocsCollector& collector) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", "needle");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto segments = qContext.topReader.segments();
  ASSERT_LT(segnum, (int32_t) segments.size());
  auto* scorer = weight->createScorer(pool, segments[segnum]);
  ASSERT_NE(scorer, nullptr);
  collectTopK(segnum, scorer, nullptr, nullptr, collector, allowPruning, accumulator);
}

std::vector<std::string> localResultIds(LocalReq& req, std::string_view opName = "q") {
  std::vector<std::string> ids;
  const auto* docs = req.docList(opName);
  if (docs == nullptr) return ids;
  const auto* idColumn = docs->columns.find("id");
  if (idColumn == nullptr) return ids;
  const auto* idCol = std::get_if<solux::api::ColStr>(&idColumn->kind);
  if (idCol == nullptr) return ids;
  for (const auto& id : idCol->v) ids.emplace_back(id);
  return ids;
}

std::vector<float> localResultScores(LocalReq& req, std::string_view opName = "q") {
  std::vector<float> scores;
  const auto* docs = req.docList(opName);
  if (docs == nullptr) return scores;
  const auto* scoreColumn = docs->columns.find("_score_");
  if (scoreColumn == nullptr) return scores;
  const auto* scoreCol = std::get_if<solux::api::ColFloat>(&scoreColumn->kind);
  if (scoreCol == nullptr) return scores;
  for (float score : scoreCol->v) scores.push_back(score);
  return scores;
}

void addWandMsmDocs(CollectionHelper& helper) {
  const int32_t nDocs = 8 * Postings::DOCS_BLOCK_SIZE + 73;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);

  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    auto add = [&](std::string_view term, int32_t count) {
      appendRepeatedTerm(body, term, count);
      used += count;
    };

    bool hot = doc < 48;
    add("msm_a", 1);
    if (hot || (doc % 2) == 0) add("msm_b", 1);
    if (hot || (doc % 3) == 0) add("msm_c", 1);
    if (hot) {
      add("msm_d", 120 - doc);
      add("msm_e", 90 - doc / 2);
    } else {
      if ((doc % 29) == 0) add("msm_d", 1);
      if ((doc % 43) == 0) add("msm_e", 1);
    }

    int32_t len = hot ? 240 : 90 + (doc % 53);
    if (len < used) len = used;
    add("filler", len - used);
    docs.push_back(flatdoc("id", "w" + std::to_string(doc), "body_w", body));
  }

  helper.indexAll(docs, UpdateMessage::COMMIT);
}

std::array<Query::Weight*, 5> createWandMsmWeights(Query::Context& qContext,
                                                   TermQuery& a, TermQuery& b,
                                                   TermQuery& c, TermQuery& d,
                                                   TermQuery& e) {
  return {
    a.createWeight(qContext, Query::NEED_SCORES),
    b.createWeight(qContext, Query::NEED_SCORES),
    c.createWeight(qContext, Query::NEED_SCORES),
    d.createWeight(qContext, Query::NEED_SCORES),
    e.createWeight(qContext, Query::NEED_SCORES)
  };
}

Query::Scorer* createMsmScorer(MemPool& pool, std::span<Query::Weight*> weights,
                               IndexReader::Segment& segment, int32_t minMatch,
                               bool wand) {
  auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
  int32_t count = 0;
  for (auto* weight : weights) {
    auto* scorer = weight->createScorer(pool, segment);
    if (scorer != nullptr) arr[count++] = scorer;
  }
  if (count < minMatch) return nullptr;
  std::span<Query::Scorer*> span(arr, (size_t) count);
  if (count == minMatch) {
    auto costs = pool.make_span<int64_t>((size_t) count);
    return pool.make<BooleanQuery::ConjunctionScorer>(
        pool, span, costs,
        singlePhaseScorersForTests(pool, (size_t)count), span, true, true);
  }
  if (wand) {
    return pool.make<BooleanQuery::MinShouldMatchWandScorer>(pool, span, minMatch);
  }
  return pool.make<BooleanQuery::MinShouldMatchScorer>(pool, span, minMatch);
}

MsmTopKRun runMsmTopK(IndexReader& reader, int32_t minMatch, int32_t topK, bool wand) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery a("body_w", "msm_a");
  TermQuery b("body_w", "msm_b");
  TermQuery c("body_w", "msm_c");
  TermQuery d("body_w", "msm_d");
  TermQuery e("body_w", "msm_e");
  auto weights = createWandMsmWeights(qContext, a, b, c, d, e);
  TopDocsCollector collector(topK);
  MsmTopKRun result;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = createMsmScorer(pool, weights, segments[segnum], minMatch, wand);
    if (scorer == nullptr) continue;
    collectTopK(segnum, scorer, nullptr, nullptr, collector, wand);
    if (auto* wandScorer = dynamic_cast<BooleanQuery::MinShouldMatchWandScorer*>(scorer)) {
      result.wandVisited += wandScorer->visited();
    }
  }

  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

std::vector<TopDocsCollector::ScoreDoc> runMsmMatches(IndexReader& reader, int32_t minMatch, bool wand) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery a("body_w", "msm_a");
  TermQuery b("body_w", "msm_b");
  TermQuery c("body_w", "msm_c");
  TermQuery d("body_w", "msm_d");
  TermQuery e("body_w", "msm_e");
  auto weights = createWandMsmWeights(qContext, a, b, c, d, e);
  std::vector<TopDocsCollector::ScoreDoc> docs;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = createMsmScorer(pool, weights, segments[segnum], minMatch, wand);
    if (scorer == nullptr) continue;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      docs.push_back({scorer->score(), segdoc(segnum, doc)});
    }
  }
  return docs;
}

void assertSameMsmTopK(const MsmTopKRun& expected, const MsmTopKRun& actual, int32_t minMatch, int32_t topK) {
  ASSERT_EQ(actual.topDocs.size(), expected.topDocs.size()) << "minMatch=" << minMatch << " k=" << topK;
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(actual.topDocs[i].doc, expected.topDocs[i].doc)
      << "minMatch=" << minMatch << " k=" << topK << " i=" << i;
    EXPECT_FLOAT_EQ(actual.topDocs[i].score, expected.topDocs[i].score)
      << "minMatch=" << minMatch << " k=" << topK << " i=" << i;
  }
}

void assertSameMsmMatches(std::span<const TopDocsCollector::ScoreDoc> expected,
                          std::span<const TopDocsCollector::ScoreDoc> actual,
                          int32_t minMatch) {
  ASSERT_EQ(actual.size(), expected.size()) << "minMatch=" << minMatch;
  for (size_t i = 0; i < expected.size(); i++) {
    EXPECT_EQ(actual[i].doc, expected[i].doc) << "minMatch=" << minMatch << " i=" << i;
    EXPECT_FLOAT_EQ(actual[i].score, expected[i].score) << "minMatch=" << minMatch << " i=" << i;
  }
}

// Many-clause corpus: hot docs (early) match every term with a distinct decreasing
// `mt0` tf so the top-k scores are distinct (tie-free); later docs match sparse
// low-score subsets.  Exercises the WAND pivot's float-summation bound across many
// clauses (the failure mode the 5-clause tests do not reach).
void addManyTermMsmDocs(CollectionHelper& helper, int32_t numTerms) {
  const int32_t nDocs = 6 * Postings::DOCS_BLOCK_SIZE + 51;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    auto add = [&](const std::string& term, int32_t count) {
      appendRepeatedTerm(body, term, count);
      used += count;
    };
    bool hot = doc < 40;
    if (hot) add("mt0", 200 - doc);
    else if ((doc % 2) == 0) add("mt0", 1);
    for (int32_t t = 1; t < numTerms; t++) {
      if (hot || (doc % (t + 2)) == 0) add("mt" + std::to_string(t), 1);
    }
    int32_t len = hot ? 260 : 100 + (doc % 41);
    if (len < used) len = used;
    add("filler", len - used);
    docs.push_back(flatdoc("id", "m" + std::to_string(doc), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
}

MsmTopKRun runManyTermMsmTopK(IndexReader& reader, int32_t numTerms, int32_t minMatch,
                              int32_t topK, bool wand) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  // TermQuery holds a string_view of the term, so the backing strings must outlive
  // the queries/scorers (literals work elsewhere; these are built names).
  std::vector<std::string> termStrs;
  termStrs.reserve((size_t) numTerms);
  for (int32_t t = 0; t < numTerms; t++) termStrs.push_back("mt" + std::to_string(t));
  std::vector<TermQuery> queries;
  queries.reserve((size_t) numTerms);
  for (int32_t t = 0; t < numTerms; t++) queries.emplace_back("body_w", termStrs[(size_t) t]);
  auto* warr = pool.make_arr<Query::Weight*>((size_t) numTerms);
  for (int32_t t = 0; t < numTerms; t++) warr[(size_t) t] = queries[(size_t) t].createWeight(qContext, Query::NEED_SCORES);
  std::span<Query::Weight*> weights(warr, (size_t) numTerms);
  TopDocsCollector collector(topK);
  MsmTopKRun result;
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = createMsmScorer(pool, weights, segments[segnum], minMatch, wand);
    if (scorer == nullptr) continue;
    collectTopK(segnum, scorer, nullptr, nullptr, collector, wand);
    if (auto* w = dynamic_cast<BooleanQuery::MinShouldMatchWandScorer*>(scorer)) result.wandVisited += w->visited();
  }
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}


TEST_F(TermScorerTest, singleSeg) {

  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[1]);
    f.add(5, text[2]);
    f.add(7, text[3]);
    testIndex.flush();
    f.startReading();

    // Field stats for foo_w:
    // docCount == 4
    // maxDoc == 8 (0 through 7)
    // sumTotalTermFreq = 19
    // sumDocFreq = 18 (just one overlap... "to" appears twice in doc 5)
    // numTerms = 15  (repeated terms are "to":3, "the":"3", hence 15+2extra+2extra = 19 sumTotalTermFreq

    // Term stats for foo_w:to
    // docFreq == 2
    // totalTermFreq = 3

    TermsEnum tenum = f.createTermsEnum();
    ASSERT_EQ(f.fieldInfo.docsWithField, 4);
    ASSERT_EQ(tenum.docsWithField(), 4);
    ASSERT_EQ(tenum.numTerms(), 15);
    ASSERT_EQ(tenum.sumTotalTermFreq(), 19);
    ASSERT_EQ(tenum.sumDocFreq(), 18);

    ASSERT_EQ(tenum.seek("to"), true);

    DocsFreqEnum denum(tenum);
    ASSERT_EQ(denum.numDocs(), 2);
    ASSERT_EQ(denum.totalTermFreq(), 3);

    Similarity::FieldStats fieldStats;
    fieldStats.sumTotalTermFreq = tenum.sumTotalTermFreq();
    fieldStats.sumDocFreq = tenum.sumDocFreq();
    fieldStats.docsWithField = tenum.docsWithField();
    fieldStats.maxDoc = f.currentSegment()->postingsReader().maxDoc();

    Similarity::TermStats termStats;
    termStats.docFreq = denum.numDocs();
    termStats.totalTermFreq = denum.totalTermFreq();

    Similarity sim;
    auto simScorer = sim.getScorer(1.0, fieldStats, termStats);

    NormsReader& normsCol = *f.normsReader;

    // lucene scores the docs as follows:
    // doc=5 score=0.36330473
    // doc=7 score=0.37098017
    TermQuery::Scorer termScorer(denum, &normsCol, &simScorer);
    ASSERT_EQ(termScorer.next(), 5);
    ASSERT_EQ(termScorer.docId(), 5);
    ASSERT_EQ(termScorer.termFreq(), 2);
    ASSERT_EQ(termScorer.score(), 0.36330473f);
    ASSERT_EQ(termScorer.next(), 7);
    ASSERT_EQ(termScorer.termFreq(), 1);
    ASSERT_EQ(termScorer.score(), 0.37098017f);
    ASSERT_EQ(termScorer.next(), PostingsReader::END);


    // Now try from the beginning:
    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      TermQuery tq("foo_w", "to");
      Query::Context qContext(testIndex.pool, *testIndex.reader);

      auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
      TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(testIndex.pool,
                                                                                         qContext.topReader.segments()[0]));
      testScores(scorer, {5, 7}, {0.36330473f, 0.37098017f});
    }
  }
}


TEST_F(TermScorerTest, multiSeg) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[1]);
    testIndex.flush();
    f.startIndexing();
    f.add(2, text[2]);
    f.add(4, text[3]);
    testIndex.flush();
    f.startReading();

    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      TermQuery tq("foo_w", "to");
      Query::Context qContext(testIndex.pool, *testIndex.reader);

      auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g1 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]));
        testScores(scorer, {}, {}); // first segment doesn't have "to"
      }
      {
        auto g2 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]));
        testScores(scorer, {2, 4}, {0.36330473f, 0.37098017f});
      }
    }
  }

  // Now try the test again with "to" in both segments this time.
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[2]);
    testIndex.flush();
    f.startIndexing();
    f.add(2, text[1]);
    f.add(4, text[3]);
    testIndex.flush();
    f.startReading();

    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      TermQuery tq("foo_w", "to");
      Query::Context qContext(testIndex.pool, *testIndex.reader);

      auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g1 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]));
        testScores(scorer, {3}, {0.36330473f});
      }
      {
        auto g2 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]));
        testScores(scorer, {4}, {0.37098017f});
      }
    }


    // try an all-scorer
    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      float score = 1.0f;
      AllQuery allQuery;
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = allQuery.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g1 = testIndex.pool.rewindScopeGuard();
        AllQuery::Scorer* scorer = dynamic_cast<AllQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]));
        testScores(scorer, {0,1,2,3}, {score, score, score, score});
      }
      {
        auto g2 = testIndex.pool.rewindScopeGuard();
        AllQuery::Scorer* scorer = dynamic_cast<AllQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]));
        testScores(scorer, {0,1,2,3,4}, {score, score, score, score, score});
      }
    }
  }
}


TEST_F(TermScorerTest, boolScore) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[1]);
    testIndex.flush();
    f.startIndexing();
    f.add(2, text[2]);
    f.add(4, text[3]);
    testIndex.flush();
    f.startReading();

    TermQuery to("foo_w", "to");   // appears in text[2,3] (docs 2,4)
    TermQuery the("foo_w", "the");  // appears in text[0,2,3] (docs 1,2,4)
    TermQuery moon("foo_w", "moon!");  // appears in text[3] (docs 4)

    // The "to" scorer should be null for seg 0 in this test
    {
      std::vector<Query*> queries = {&to, &the};
      BooleanQuery q({}, queries, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }


    // conjunction scorer
    {
      std::vector<Query*> queries = {&to, &the};
      BooleanQuery q(queries, {}, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }

    // mix of conjunction, disjunction
    {
      std::vector<Query*> mand = {&to};
      std::vector<Query*> opt = {&the};
      BooleanQuery q(mand, opt, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }

    // mix of conjunction, disjunction opposite order
    {
      std::vector<Query*> mand = {&the};
      std::vector<Query*> opt = {&to};
      BooleanQuery q(mand, opt, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }


    // add single prohibited clause
    {
      std::vector<Query*> mand = {&the};
      std::vector<Query*> opt = {&to};
      std::vector<Query*> neg = {&moon};

      BooleanQuery q(mand, opt, neg, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2}, {0.48997432f});
      }
    }

    // add more prohibited clauses
    {
      TermQuery does_not_exist("foo_w", "does_not_exist");
      TermQuery time("foo_w", "time");  // matches doc 1
      TermQuery men("foo_w", "men");  // matches doc 3

      std::vector<Query*> mand = {&the};
      std::vector<Query*> opt = {&to};
      std::vector<Query*> neg = {&does_not_exist, &moon, &time, &men};

      BooleanQuery q(mand, opt, neg, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2}, {0.48997432f});
      }
    }

    // mandatory clause with filter
    {
      std::vector<Query*> mand = {&the};
      std::vector<Query*> filter = {&to};

      BooleanQuery q(mand, {}, {},
                     filter);  // this should match the same as a "to" and "the" conjunction, but score differently.


      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.1266696f, 0.19089644f});
      }
    }

    // optional clause with filter
    {
      std::vector<Query*> opt = {&the};
      std::vector<Query*> filter = {&to};

      BooleanQuery q({}, opt, {},
                     filter);  // this should match the same as a "to" and "the" conjunction, but score differently.


      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.1266696f, 0.19089644f});
      }
    }


    // phrase scoring (add idfs of terms, and termfreq is number of occurances of phrase)
    // optional clause with filter
    {
      std::vector<std::string_view> terms = {"to", "the"};
      std::vector<std::int32_t> positions = {0, 1};

      PhraseQuery phrase("foo_w", terms, positions);

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.37283403f, 0.56187654f});
      }
    }

    // reversed phrase shouldn't match anything
    {
      std::vector<std::string_view> terms = {"the", "to"};
      std::vector<std::int32_t> positions = {0,1};

      PhraseQuery q("foo_w", terms, positions);

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {}, {});
      }
    }

    // test that reused terms are handled correctly (i.e. cached docsenum are cloned when needed)
    {
      std::vector<std::string_view> terms = {"to", "the"};
      std::vector<std::int32_t> positions = {0, 1};
      PhraseQuery phrase("foo_w", terms, positions);

      std::vector<Query*> queries = {&to, &the, &phrase};
      BooleanQuery q({}, queries, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.86280835f, 1.1237532f});
      }
    }

  }
}

TEST_F(TermScorerTest, phraseConjunctionDefersPositionChecks) {
  constexpr int32_t nDocs = 512;
  constexpr int32_t filterStep = 64;
  constexpr int32_t topK = 100;
  CollectionHelper helper("main");
  addPhraseDeferralDocs(helper, nDocs, filterStep);
  auto reader = helper.getIndexWriter()->getIndexReader();

  int64_t rawPhraseChecks = countStandalonePhraseMatchChecks(*reader);
  EXPECT_EQ(nDocs, rawPhraseChecks);

  PhraseFilterTopKRun baseline = runPhraseFilterConjunctionTopK(*reader, topK, false);
  PhraseFilterTopKRun measured = runPhraseFilterConjunctionTopK(*reader, topK, true);

  int64_t expectedMatches = (nDocs + filterStep - 1) / filterStep;
  EXPECT_EQ(expectedMatches, baseline.visited);
  EXPECT_EQ(expectedMatches, measured.visited);
  EXPECT_EQ(expectedMatches, measured.matchCalls);
  EXPECT_LT(measured.matchCalls * 4, rawPhraseChecks);
  assertSameTopKExact(baseline, measured);

}


// Regression for the norm-encoding fix (impact-scoring.md "Step 0"): the field-length
// column stores the SmallFloat-encoded norm byte, not the raw token count. Before the
// fix a doc over 40 tokens was mis-scored, and a doc over 255 tokens wrapped mod 256
// (e.g. 256 -> byte 0 -> "length 0", the shortest-doc bonus), so a very long doc could
// outscore a short one. With the same tf, the BM25 score must be monotone non-increasing
// in length, with no wrap across the 40- and 255-token boundaries.
TEST_F(TermScorerTest, normEncodingMonotone) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_w");
  f.startIndexing();

  // Each doc has "needle" exactly once (tf=1) plus filler to hit a target token
  // count straddling the encoding boundaries. docids ascend with length, so the
  // scorer (docid order) yields scores that must descend.
  std::vector<int> lengths = {5, 40, 41, 100, 255, 256, 300, 600};
  std::vector<std::string> docs;
  for (int len : lengths) {
    std::string s = "needle";
    for (int i = 1; i < len; i++) s += " fill";
    docs.push_back(std::move(s));
  }
  for (size_t i = 0; i < docs.size(); i++) {
    f.add((int32_t)i, docs[i]);
  }
  testIndex.flush();
  f.startReading();

  TermQuery tq("foo_w", "needle");
  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      weight->createScorer(testIndex.pool, qContext.topReader.segments()[0]));
  ASSERT_NE(scorer, nullptr);

  std::vector<float> scores;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    ASSERT_EQ(scorer->termFreq(), 1);
    scores.push_back(scorer->score());
  }
  ASSERT_EQ(scores.size(), lengths.size());

  // Monotone non-increasing across every boundary, including 40/41 and 255/256.
  // (Adjacent lengths in the same quantization bucket score equal, hence <=.)
  for (size_t i = 1; i < scores.size(); i++) {
    EXPECT_LE(scores[i], scores[i - 1])
        << "length " << lengths[i] << " outscored length " << lengths[i - 1];
  }
  // The length effect is real (quantization didn't collapse it): the longest doc
  // scores strictly below the shortest. The pre-fix 256-token wrap inverted this.
  EXPECT_LT(scores.back(), scores.front());
}

TEST_F(TermScorerTest, termImpactMaxScoreBounds) {
  const int32_t N = 5 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    int32_t tf = 1 + ((doc / Postings::DOCS_BLOCK_SIZE) * 3 + (doc % 5)) % 17;
    int32_t len = 4 + ((doc * 11) % 90);
    if (len < tf) {
      len = tf;
    }
    std::string text;
    for (int32_t i = 0; i < tf; i++) {
      text += "impact ";
    }
    for (int32_t i = tf; i < len; i++) {
      text += "filler ";
    }
    if (doc == 7) {
      text += "rare ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  TermQuery impactForBounds("body_w", "impact");
  TermQuery impactForActuals("body_w", "impact");
  auto* boundWeight = impactForBounds.createWeight(qContext, Query::NEED_SCORES);
  auto* actualWeight = impactForActuals.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];

  auto* actualScorer = dynamic_cast<TermQuery::Scorer*>(
      actualWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(actualScorer, nullptr);
  std::vector<std::pair<int32_t, float>> actualScores;
  for (int32_t doc = actualScorer->next(); doc != PostingsReader::END; doc = actualScorer->next()) {
    actualScores.push_back({doc, actualScorer->score()});
  }
  ASSERT_EQ((int32_t) actualScores.size(), N);

  auto actualMax = [&](int32_t current, int32_t upTo) {
    float maxScore = 0.0f;
    for (auto [doc, score] : actualScores) {
      if (doc >= current && doc <= upTo) {
        maxScore = std::max(maxScore, score);
      }
    }
    return maxScore;
  };

  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      boundWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  auto assertBound = [&](int32_t upTo) {
    float bound = scorer->getMaxScore(upTo);
    ASSERT_TRUE(std::isfinite(bound)) << "upTo=" << upTo << " doc=" << scorer->docId();
    EXPECT_GE(bound + 1e-6f, actualMax(scorer->docId(), upTo))
        << "upTo=" << upTo << " doc=" << scorer->docId();
  };

  for (int32_t upTo : {0, 17, 127, 128, 255, 400, N - 1, PostingsReader::END}) {
    assertBound(upTo);
  }

  ASSERT_EQ(scorer->advance(200), 200);
  for (int32_t upTo : {200, 255, 511, N - 1, PostingsReader::END}) {
    assertBound(upTo);
  }

  ASSERT_EQ(scorer->advance(600), 600);
  for (int32_t upTo : {600, N - 1, PostingsReader::END}) {
    assertBound(upTo);
  }

  auto* shallowScorer = dynamic_cast<TermQuery::Scorer*>(
      boundWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(shallowScorer, nullptr);
  ASSERT_EQ(shallowScorer->docId(), -1);
  auto expectedGroupLastDoc = [&](int32_t target) {
    if (target >= N) {
      return PostingsReader::END;
    }
    int32_t group = target / DocsEnumMeta::L1_DOCS;
    return std::min(N - 1, (group + 1) * DocsEnumMeta::L1_DOCS - 1);
  };
  for (int32_t target : {0, 1, 127, 128, 129, 3 * Postings::DOCS_BLOCK_SIZE + 5, N - 1}) {
    EXPECT_EQ(shallowScorer->advanceShallow(target), expectedGroupLastDoc(target)) << target;
    EXPECT_EQ(shallowScorer->docId(), -1);
  }
  EXPECT_EQ(shallowScorer->advanceShallow(N + 10), PostingsReader::END);
  EXPECT_EQ(shallowScorer->docId(), -1);

  TermQuery rareQuery("body_w", "rare");
  auto* rareWeight = rareQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* rareScorer = dynamic_cast<TermQuery::Scorer*>(
      rareWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(rareScorer, nullptr);
  EXPECT_TRUE(std::isinf(rareScorer->getMaxScore(PostingsReader::END)));
  EXPECT_EQ(rareScorer->advanceShallow(0), PostingsReader::END);
}

TEST_F(TermScorerTest, termImpactRefineMaxScoreUsesWindowBlocks) {
  const int32_t N = 5 * Postings::DOCS_BLOCK_SIZE;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    int32_t tf = block == 0 ? 40 : block == 2 ? 3 : 2;
    int32_t len = block == 0 ? tf : 120;
    std::string text;
    for (int32_t i = 0; i < tf; i++) text += "shallowimpact ";
    for (int32_t i = tf; i < len; i++) text += "filler ";
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  TermQuery globalQuery("body_w", "shallowimpact");
  auto* globalWeight = globalQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* globalScorer = dynamic_cast<TermQuery::Scorer*>(
      globalWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(globalScorer, nullptr);
  float globalMax = globalScorer->getMaxScore(PostingsReader::END);
  ASSERT_TRUE(std::isfinite(globalMax));

  TermQuery windowQuery("body_w", "shallowimpact");
  auto* windowWeight = windowQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* windowScorer = dynamic_cast<TermQuery::Scorer*>(
      windowWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(windowScorer, nullptr);

  int32_t ws = 2 * Postings::DOCS_BLOCK_SIZE;
  int32_t we = 3 * Postings::DOCS_BLOCK_SIZE - 1;
  ASSERT_EQ(windowScorer->advance(ws), ws);
  ASSERT_EQ(windowScorer->advanceShallow(ws), N - 1);
  float cheapWindowMax = windowScorer->getMaxScore(we);
  EXPECT_FLOAT_EQ(cheapWindowMax, globalMax);

  float refinedMax = windowScorer->refineMaxScore(we);
  ASSERT_EQ(windowScorer->advanceShallow(ws), we);
  int32_t startBlock = windowScorer->blockContaining(ws);
  int32_t endBlock = windowScorer->blockContaining(we);
  ASSERT_LT(endBlock, windowScorer->impacts.blockCount());
  float bruteMax = 0.0f;
  for (int32_t block = startBlock; block <= endBlock; block++) {
    bruteMax = std::max(bruteMax, windowScorer->impacts.impact(block));
  }

  EXPECT_FLOAT_EQ(refinedMax, bruteMax);
  EXPECT_LE(refinedMax, cheapWindowMax);
  EXPECT_LT(refinedMax, cheapWindowMax);
}

TEST_F(TermScorerTest, termImpactGroupShallowRefreshesAfterRefine) {
  const int32_t N = 5 * Postings::DOCS_BLOCK_SIZE;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    int32_t tf = block == 0 ? 40 : block == 2 ? 3 : 2;
    int32_t len = block == 0 ? tf : 120;
    std::string text;
    for (int32_t i = 0; i < tf; i++) text += "refreshimpact ";
    for (int32_t i = tf; i < len; i++) text += "filler ";
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "refreshimpact");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  int32_t target = 2 * Postings::DOCS_BLOCK_SIZE + 7;
  int32_t blockEnd = 3 * Postings::DOCS_BLOCK_SIZE - 1;
  ASSERT_EQ(scorer->advanceShallow(target), N - 1);
  EXPECT_EQ(SkipStats::impactL0GroupParses, 0);
  EXPECT_GT(SkipStats::impactGroupShallowAnswers, 0);

  float cheapMax = scorer->getMaxScore(blockEnd);
  EXPECT_EQ(SkipStats::impactL0GroupParses, 0);

  float refinedMax = scorer->refineMaxScore(blockEnd);
  EXPECT_EQ(SkipStats::impactRefinesTriggered, 1);
  EXPECT_EQ(SkipStats::impactL0GroupParses, 1);
  EXPECT_LT(refinedMax, cheapMax);

  int64_t cacheHitsBefore = SkipStats::shallowCacheHits;
  EXPECT_EQ(scorer->advanceShallow(target), blockEnd);
  EXPECT_GT(SkipStats::shallowCacheHits, cacheHitsBefore);
  EXPECT_FLOAT_EQ(scorer->getMaxScore(blockEnd), refinedMax);

  SkipStats::enabled = savedStats;
}

TEST_F(TermScorerTest, termImpactFrontierIsExactBlockMax) {
  const int32_t postingCount = 7 * Postings::DOCS_BLOCK_SIZE + 19;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  addAntiCorrelatedFrontierDocs(f, postingCount);
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  TermQuery frontierQuery("body_w", "frontier", 1.0f, true);
  TermQuery cornerQuery("body_w", "frontier", 1.0f, false);
  TermQuery actualQuery("body_w", "frontier", 1.0f, true);
  auto* frontierWeight = frontierQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* cornerWeight = cornerQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* actualWeight = actualQuery.createWeight(qContext, Query::NEED_SCORES);

  auto* frontierScorer = dynamic_cast<TermQuery::Scorer*>(
      frontierWeight->createScorer(testIndex.pool, segment));
  auto* cornerScorer = dynamic_cast<TermQuery::Scorer*>(
      cornerWeight->createScorer(testIndex.pool, segment));
  auto* actualScorer = dynamic_cast<TermQuery::Scorer*>(
      actualWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(frontierScorer, nullptr);
  ASSERT_NE(cornerScorer, nullptr);
  ASSERT_NE(actualScorer, nullptr);
  ASSERT_EQ(frontierScorer->impacts.blockCount(), cornerScorer->impacts.blockCount());

  std::vector<float> brute((size_t) frontierScorer->impacts.blockCount(), 0.0f);
  int32_t block = 0;
  for (int32_t doc = actualScorer->next(); doc != PostingsReader::END; doc = actualScorer->next()) {
    while (block + 1 < frontierScorer->impacts.blockCount()
           && doc > frontierScorer->impacts.lastDoc(block)) {
      block++;
    }
    ASSERT_LE(doc, frontierScorer->impacts.lastDoc(block));
    brute[(size_t) block] = std::max(brute[(size_t) block], actualScorer->score());
  }

  bool sawTighterBlock = false;
  for (int32_t i = 0; i < frontierScorer->impacts.blockCount(); i++) {
    EXPECT_FLOAT_EQ(frontierScorer->impacts.impact(i), brute[(size_t) i]) << "block=" << i;
    EXPECT_LE(frontierScorer->impacts.impact(i), cornerScorer->impacts.impact(i) + 1e-6f)
        << "block=" << i;
    sawTighterBlock |= frontierScorer->impacts.impact(i) + 1e-6f < cornerScorer->impacts.impact(i);
  }
  EXPECT_TRUE(sawTighterBlock);
}

TEST_F(TermScorerTest, cachedPostingsStateKeepsCurrentTermFrontierAfterTermsEnumSeek) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "alpha alpha filler");
  f.add(1, "alpha filler");
  f.add(2, "omega omega omega omega filler");
  f.add(3, "omega filler");
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  TermsEnum direct = f.createTermsEnum();
  ASSERT_TRUE(direct.seek("alpha"));
  std::vector<int32_t> alphaNorms;
  std::vector<int32_t> alphaTfs;
  ASSERT_GT(direct.readTermImpactFrontier(alphaNorms, alphaTfs), 0);
  ASSERT_TRUE(direct.seek("omega"));
  std::vector<int32_t> omegaNorms;
  std::vector<int32_t> omegaTfs;
  ASSERT_GT(direct.readTermImpactFrontier(omegaNorms, omegaTfs), 0);
  ASSERT_TRUE(alphaNorms != omegaNorms || alphaTfs != omegaTfs);

  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery alphaQuery("body_w", "alpha");
  auto* alphaWeight = alphaQuery.createWeight(qContext, Query::NEED_SCORES);
  TermQuery omegaQuery("body_w", "omega");
  auto* omegaWeight = omegaQuery.createWeight(qContext, Query::NEED_SCORES);
  unused(omegaWeight);

  auto* alphaScorer = dynamic_cast<TermQuery::Scorer*>(
      alphaWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(alphaScorer, nullptr);
  std::vector<int32_t> clonedNorms;
  std::vector<int32_t> clonedTfs;
  ASSERT_GT(alphaScorer->docsEnum.readTermImpactFrontier(clonedNorms, clonedTfs), 0);
  EXPECT_EQ(clonedNorms, alphaNorms);
  EXPECT_EQ(clonedTfs, alphaTfs);

  auto* secondAlphaScorer = dynamic_cast<TermQuery::Scorer*>(
      alphaWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(secondAlphaScorer, nullptr);
  clonedNorms.clear();
  clonedTfs.clear();
  ASSERT_GT(secondAlphaScorer->docsEnum.readTermImpactFrontier(clonedNorms, clonedTfs), 0);
  EXPECT_EQ(clonedNorms, alphaNorms);
  EXPECT_EQ(clonedTfs, alphaTfs);
}

TEST_F(TermScorerTest, lazyImpactsMatchForcedEagerFrontierOracle) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 55;
  constexpr int32_t TERM_COUNT = 5;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  std::array<std::string, TERM_COUNT> terms = {"r0", "r1", "r2", "r3", "r4"};
  uint32_t state = 0x5eed1234u;
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    int32_t used = 0;
    for (int32_t t = 0; t < TERM_COUNT; t++) {
      state = state * 1664525u + 1013904223u;
      int32_t tf = 1 + (int32_t) ((state >> 16) % 9);
      appendRepeatedTerm(body, terms[(size_t) t], tf);
      used += tf;
    }
    int32_t len = used + (int32_t) (state % 8);
    appendRepeatedTerm(body, "filler", len - used);
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  for (const std::string& term : terms) {
    SCOPED_TRACE(term);
    TermQuery lazyQuery("body_w", term, 1.7f, true);
    auto* lazyWeight = lazyQuery.createWeight(qContext, Query::NEED_SCORES);
    auto* lazy = dynamic_cast<TermQuery::Scorer*>(
        lazyWeight->createScorer(testIndex.pool, segment));

    ForceEagerImpactsGuard eagerGuard(true);
    TermQuery eagerQuery("body_w", term, 1.7f, true);
    auto* eagerWeight = eagerQuery.createWeight(qContext, Query::NEED_SCORES);
    auto* eager = dynamic_cast<TermQuery::Scorer*>(
        eagerWeight->createScorer(testIndex.pool, segment));
    ASSERT_NE(lazy, nullptr);
    ASSERT_NE(eager, nullptr);
    ASSERT_TRUE(lazy->hasImpacts());
    ASSERT_TRUE(eager->hasImpacts());
    ASSERT_EQ(lazy->impacts.blockCount(), eager->impacts.blockCount());
    ASSERT_EQ(lazy->impacts.numGroups(), eager->impacts.numGroups());

    EXPECT_FLOAT_EQ(lazy->impacts.globalMaxImpact(), eager->impacts.globalMaxImpact());
    EXPECT_FLOAT_EQ(lazy->impacts.maxGroupImpactFrom(0),
                    lazy->impacts.globalMaxImpact());

    for (int32_t g = 0; g < lazy->impacts.numGroups(); g++) {
      EXPECT_FLOAT_EQ(lazy->impacts.maxGroupImpactInRange(g, g),
                      eager->impacts.maxGroupImpactInRange(g, g)) << "group=" << g;
    }
    for (int32_t target : {N - 1, 0, 127, 128, DocsEnumMeta::L1_DOCS + 3,
                           N / 2, PostingsReader::END}) {
      EXPECT_EQ(lazy->impacts.blockContaining(target), eager->impacts.blockContaining(target))
          << "target=" << target;
    }
    for (int32_t target : {0, Postings::DOCS_BLOCK_SIZE + 5, DocsEnumMeta::L1_DOCS + 9}) {
      for (float minScore : {0.0f, lazy->impacts.globalMaxImpact() * 0.5f,
                             lazy->impacts.globalMaxImpact() + 0.001f}) {
        int64_t lazySkipped = 0;
        int64_t eagerSkipped = 0;
        auto lazyTarget = lazy->impacts.firstCompetitiveTarget(target, minScore, lazySkipped);
        auto eagerTarget = eager->impacts.firstCompetitiveTarget(target, minScore, eagerSkipped);
        EXPECT_EQ(lazyTarget.doc, eagerTarget.doc)
            << "target=" << target << " minScore=" << minScore;
        EXPECT_EQ(lazyTarget.lastDoc, eagerTarget.lastDoc)
            << "target=" << target << " minScore=" << minScore;
        EXPECT_FLOAT_EQ(lazyTarget.impact, eagerTarget.impact)
            << "target=" << target << " minScore=" << minScore;
      }
    }
  }
}

TEST_F(TermScorerTest, termImpactGroupBoundsMatchBlockBoundsOnGroupAlignedRanges) {
  const int32_t postingCount =
      (int32_t)scaleTestWork(1) * DocsEnumMeta::L1_DOCS + 19;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  addAntiCorrelatedFrontierDocs(f, postingCount);
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "frontier", 1.0f, true);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);
  ASSERT_TRUE(scorer->hasImpacts());

  int32_t groupCount = scorer->impacts.groupContainingFrom(-1, PostingsReader::END);
  ASSERT_GT(groupCount, 1);
  for (int32_t g = 0; g < groupCount; g++) {
    int32_t fromBlock = g * DocsEnumMeta::L1_PERIOD;
    int32_t toBlock = std::min(scorer->impacts.blockCount() - 1,
                               fromBlock + DocsEnumMeta::L1_PERIOD - 1);
    float blockBound = fromBlock == scorer->impacts.blockCount() - 1
        ? scorer->impacts.maxImpactFrom(fromBlock)
        : scorer->impacts.maxImpactInRange(fromBlock, toBlock);
    float groupBound = g == groupCount - 1
        ? scorer->impacts.maxGroupImpactFrom(g)
        : scorer->impacts.maxGroupImpactInRange(g, g);
    EXPECT_FLOAT_EQ(groupBound, blockBound) << "group=" << g;
  }
}

TEST_F(TermScorerTest, termImpactGroupBoundsCoverUnalignedBlockRanges) {
  // One structural group plus scalable repeated groups. Effort 1 covers
  // within-group and cross-group ranges; effort 2 uses a 3-group corpus.
  const int32_t fullGroups = 1 + (int32_t)scaleTestWork(1);
  const int32_t postingCount = fullGroups * DocsEnumMeta::L1_DOCS + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  addAntiCorrelatedFrontierDocs(f, postingCount);
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "frontier", 1.0f, true);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  std::vector<std::pair<int32_t, int32_t>> ranges = {
    {1, DocsEnumMeta::L1_PERIOD - 2},
    {3, DocsEnumMeta::L1_PERIOD + 5},
    {DocsEnumMeta::L1_PERIOD + 7, 2 * DocsEnumMeta::L1_PERIOD - 2}
  };
  for (int32_t group = 2; group < fullGroups; group++) {
    int32_t boundary = group * DocsEnumMeta::L1_PERIOD;
    ranges.emplace_back(boundary - DocsEnumMeta::L1_PERIOD + 7,
                        boundary + 1);
    ranges.emplace_back(boundary + 3,
                        boundary + DocsEnumMeta::L1_PERIOD - 2);
  }
  for (auto [fromBlock, toBlock] : ranges) {
    ASSERT_LT(fromBlock, toBlock);
    float blockBound = scorer->impacts.maxImpactInRange(fromBlock, toBlock);
    int32_t fromGroup = fromBlock / DocsEnumMeta::L1_PERIOD;
    int32_t toGroup = toBlock / DocsEnumMeta::L1_PERIOD;
    float groupBound = scorer->impacts.maxGroupImpactInRange(fromGroup, toGroup);
    EXPECT_GE(groupBound + 1e-6f, blockBound)
        << "fromBlock=" << fromBlock << " toBlock=" << toBlock;
  }
}

TEST_F(TermScorerTest, maxScoreSetupUsesGroupBoundsWithoutL0Parse) {
  // Each term has one full L1 group plus a tail posting, so setup bounds
  // the first window with a non-tail group range.
  const int32_t nDocs = DocsEnumMeta::L1_DOCS + 1;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    field.add(doc, "mt0 mt1");
  }
  testIndex.flush();
  field.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  SkipStatsGuard stats;
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery shapeQuery("body_w", "mt0");
  auto* shapeScorer = dynamic_cast<TermQuery::Scorer*>(
      shapeQuery.createWeight(qContext, Query::NEED_SCORES)
          ->createScorer(testIndex.pool, segment));
  ASSERT_NE(shapeScorer, nullptr);
  ASSERT_EQ(shapeScorer->impacts.numGroups(), 2);

  std::vector<std::string> terms;
  std::vector<TermQuery> queries;
  std::vector<Query*> optional;
  auto* weight = createDenseDisjunctionWeight(qContext, 2, terms, queries, optional);
  auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(
      supplier->bulkScorer(testIndex.pool));
  ASSERT_NE(bulk, nullptr);
  ScoreWindow out;
  ASSERT_NE(bulk->scoreNextWindow(out, nullptr, 0, segment.maxDoc(),
                                  std::numeric_limits<float>::lowest()),
            PostingsReader::END);

  EXPECT_GT(SkipStats::impactGroupBoundNoL0, 0);
  EXPECT_EQ(SkipStats::impactL0GroupParses, 0);
}

TEST_F(TermScorerTest, lazyHeaderParsesStayBelowDenseTermGroupCount) {
  const int32_t nDocs =
      (int32_t)scaleTestWork(4) * DocsEnumMeta::L1_DOCS + 19;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    f.add(doc, "dense_header filler");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  SkipStatsGuard stats;
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "dense_header");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);
  ASSERT_TRUE(scorer->hasImpacts());
  ASSERT_GT(scorer->impacts.numGroups(), 4);

  EXPECT_EQ(SkipStats::impactGroupHeaderParses, 0);
  EXPECT_TRUE(std::isfinite(scorer->getMaxScoreForSetup(PostingsReader::END)));
  EXPECT_EQ(SkipStats::impactGroupHeaderParses, 0);
  EXPECT_EQ(scorer->advanceShallowForSetup(0), DocsEnumMeta::L1_DOCS - 1);
  EXPECT_GT(SkipStats::impactGroupHeaderParses, 0);
  EXPECT_LT(SkipStats::impactGroupHeaderParses, scorer->impacts.numGroups());

  int64_t parsedBeforeDeath = SkipStats::impactGroupHeaderParses;
  int64_t skipped = 0;
  EXPECT_EQ(scorer->impacts.firstCompetitiveTarget(
                0, scorer->impacts.globalMaxImpact() + 1.0f, skipped).doc,
            DocsEnumMeta::END);
  EXPECT_EQ(SkipStats::impactGroupHeaderParses, parsedBeforeDeath);
}

TEST_F(TermScorerTest, lazySetupAndMainShallowCursorsCanInterleaveNonMonotone) {
  const int32_t nDocs = 3 * DocsEnumMeta::L1_DOCS + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    appendRepeatedTerm(body, "interleave", 1 + (doc % 5));
    appendRepeatedTerm(body, "filler", 4 + (doc % 3));
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "interleave");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  auto groupEnd = [&](int32_t target) {
    if (target >= nDocs) return PostingsReader::END;
    int32_t group = target / DocsEnumMeta::L1_DOCS;
    return std::min(nDocs - 1, (group + 1) * DocsEnumMeta::L1_DOCS - 1);
  };

  EXPECT_EQ(scorer->advanceShallowForSetup(2 * DocsEnumMeta::L1_DOCS + 7),
            groupEnd(2 * DocsEnumMeta::L1_DOCS + 7));
  EXPECT_EQ(scorer->advanceShallow(3), groupEnd(3));
  EXPECT_EQ(scorer->advanceShallowForSetup(DocsEnumMeta::L1_DOCS + 9),
            groupEnd(DocsEnumMeta::L1_DOCS + 9));
  EXPECT_EQ(scorer->advanceShallow(2 * DocsEnumMeta::L1_DOCS + 11),
            groupEnd(2 * DocsEnumMeta::L1_DOCS + 11));
  EXPECT_EQ(scorer->advanceShallow(10), groupEnd(10));
}

TEST_F(TermScorerTest, groupSetupPulsedTermBehaviorUnchanged) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(7, "pulse only");
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  TermsEnum tenum = f.createTermsEnum();
  ASSERT_TRUE(tenum.seek("pulse"));
  DocsFreqEnum denum(tenum);
  EXPECT_TRUE(denum.hasTermImpacts());
  EXPECT_EQ(denum.numImpactBlocks(), 0);
  std::vector<int32_t> norms;
  std::vector<int32_t> tfs;
  EXPECT_GT(denum.readTermImpactFrontier(norms, tfs), 0);

  // Both build paths must degrade identically on a pulsed term (no doc
  // stream): the eager path's group walk sees zero groups.
  for (bool forceEager : {false, true}) {
    SCOPED_TRACE(forceEager ? "eager" : "lazy");
    ForceEagerImpactsGuard eagerGuard(forceEager);
    Query::Context qContext(testIndex.pool, *testIndex.reader);
    auto& segment = qContext.topReader.segments()[0];
    TermQuery query("body_w", "pulse");
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = dynamic_cast<TermQuery::Scorer*>(weight->createScorer(testIndex.pool, segment));
    ASSERT_NE(scorer, nullptr);
    EXPECT_FALSE(scorer->hasImpacts());
    EXPECT_TRUE(std::isinf(scorer->getMaxScore(PostingsReader::END)));
    EXPECT_TRUE(std::isinf(scorer->getMaxScoreForSetup(PostingsReader::END)));
    EXPECT_EQ(scorer->advanceShallow(0), PostingsReader::END);
    EXPECT_EQ(scorer->advanceShallowForSetup(0), PostingsReader::END);
  }
}

TEST_F(TermScorerTest, mandOptSingleOptionalSparseAndDenseTopKMatchesExhaustive) {
  const int32_t nDocs =
      (int32_t)scaleTestWork(1) * DocsEnumMeta::L1_DOCS + 113;
  for (bool denseOpt : {false, true}) {
    SCOPED_TRACE(denseOpt ? "dense optional" : "sparse optional");
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    for (int32_t doc = 0; doc < nDocs; doc++) {
      bool hot = doc < 24;
      int32_t reqTf = hot ? 8 - (doc % 3) : 1;
      int32_t len = hot ? reqTf + 2 : 8 + (doc % 3);
      std::string body;
      int32_t used = 0;
      appendRepeatedTerm(body, "mand_req", reqTf);
      used += reqTf;
      bool hasOpt = denseOpt || (doc % 257) == 17;
      if (hasOpt) {
        int32_t optTf = denseOpt ? 1 : 12;
        appendRepeatedTerm(body, denseOpt ? "mand_opt_dense" : "mand_opt_sparse", optTf);
        used += optTf;
      }
      if (len < used + 2) {
        len = used + 2;
      }
      appendRepeatedTerm(body, "filler", len - used);
      f.add(doc, body);
    }
    testIndex.flush();
    f.startReading();

    TermQuery req("body_w", "mand_req");
    TermQuery opt("body_w", denseOpt ? "mand_opt_dense" : "mand_opt_sparse");
    std::vector<Query*> mandatory = {&req};
    std::vector<Query*> optional = {&opt};

    auto expected = runBooleanTopK(*testIndex.reader, mandatory, optional, 10, false);
    SkipStatsGuard stats;
    auto actual = runBooleanTopK(*testIndex.reader, mandatory, optional, 10, true);
    assertSameTopKDocs(expected, actual, 10);
    EXPECT_GT(SkipStats::mandOptWindowEvals, 0);
    if (!denseOpt) {
      EXPECT_GT(SkipStats::mandOptWindowSkips, 0);
    }
  }
}

TEST_F(TermScorerTest, mandOptWindowWalkBacksOffWhenNothingSkips) {
  // Uniform mandatory scores keep every window competitive and never
  // conjunctive; a periodic optional fragments windows at each of its docs.
  // The walk must stop evaluating windows after its patience runs out.
  const int32_t nDocs = 4 * Postings::DOCS_BLOCK_SIZE + 60;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "bk_req";
    if ((doc % 2) == 0) {
      body += " bk_opt";
    }
    body += " filler filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery req("body_w", "bk_req");
  TermQuery opt("body_w", "bk_opt");
  std::vector<Query*> mandatory = {&req};
  std::vector<Query*> optional = {&opt};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);
  // Theta below the mandatory clause's own max: every window stays
  // competitive on the required score alone (no skips, no conjunctions).
  float reqMax;
  {
    TermQuery reqOnly("body_w", "bk_req");
    auto* reqWeight = reqOnly.createWeight(qContext, Query::NEED_SCORES);
    auto* reqScorer = reqWeight->createScorer(testIndex.pool, segment);
    ASSERT_NE(reqScorer, nullptr);
    reqMax = reqScorer->getMaxScoreForSetup(PostingsReader::END);
  }
  ASSERT_TRUE(std::isfinite(reqMax));

  SkipStatsGuard stats;
  scorer->setMinCompetitiveScore(reqMax * 0.5f);
  int32_t seen = 0;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    seen++;
  }
  EXPECT_EQ(seen, nDocs);  // uniform scores: nothing skippable
  EXPECT_EQ(SkipStats::mandOptWindowSkips, 0);
  EXPECT_EQ(SkipStats::mandOptConjunctionWindows, 0);
  // ~286 optional docs fragment ~286 windows; the walk must give up once its
  // patience runs out.
  EXPECT_GT(SkipStats::mandOptWindowEvals, 0);
  EXPECT_LE(SkipStats::mandOptWindowEvals, 70);
}

TEST_F(TermScorerTest, mandOptConjunctionTransitionReturnsIntersectionUntilOptionalExhausts) {
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 31;
  const std::vector<int32_t> optDocs = {
    17,
    Postings::DOCS_BLOCK_SIZE + 11,
    DocsEnumMeta::L1_DOCS + 23
  };
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    appendRepeatedTerm(body, "conj_req", 1);
    used++;
    if (std::find(optDocs.begin(), optDocs.end(), doc) != optDocs.end()) {
      appendRepeatedTerm(body, "conj_opt", 12);
      used += 12;
    }
    appendRepeatedTerm(body, "filler", std::max(1, 8 - used));
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery reqForMax("body_w", "conj_req");
  auto* reqMaxWeight = reqForMax.createWeight(qContext, Query::NEED_SCORES);
  auto* reqMaxScorer = reqMaxWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(reqMaxScorer, nullptr);
  float theta = std::nextafter(reqMaxScorer->getMaxScore(PostingsReader::END),
                               std::numeric_limits<float>::infinity());
  ASSERT_TRUE(std::isfinite(theta));

  TermQuery req("body_w", "conj_req");
  TermQuery opt("body_w", "conj_opt");
  std::vector<Query*> mandatory = {&req};
  std::vector<Query*> optional = {&opt};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  SkipStatsGuard stats;
  scorer->setMinCompetitiveScore(theta);
  EXPECT_EQ(collectDocIds(scorer), optDocs);
  EXPECT_GT(SkipStats::mandOptWindowEvals, 0);
  EXPECT_GT(SkipStats::mandOptConjunctionWindows, 0);
}

TEST_F(TermScorerTest, mandOptThresholdRiseReclassifiesCurrentWindow) {
  const int32_t nDocs = DocsEnumMeta::L1_DOCS + 19;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    appendRepeatedTerm(body, "rise_req", 1);
    if ((doc % 200) == 17) {
      appendRepeatedTerm(body, "rise_opt", 8);
    }
    appendRepeatedTerm(body, "filler", 8);
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery reqForMax("body_w", "rise_req");
  auto* reqMaxWeight = reqForMax.createWeight(qContext, Query::NEED_SCORES);
  auto* reqMaxScorer = reqMaxWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(reqMaxScorer, nullptr);
  float theta = std::nextafter(reqMaxScorer->getMaxScore(PostingsReader::END),
                               std::numeric_limits<float>::infinity());
  ASSERT_TRUE(std::isfinite(theta));

  TermQuery req("body_w", "rise_req");
  TermQuery opt("body_w", "rise_opt");
  std::vector<Query*> mandatory = {&req};
  std::vector<Query*> optional = {&opt};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  SkipStatsGuard stats;
  scorer->setMinCompetitiveScore(reqMaxScorer->getMaxScore(PostingsReader::END) * 0.5f);
  ASSERT_EQ(scorer->next(), 0);
  scorer->setMinCompetitiveScore(theta);
  EXPECT_EQ(scorer->next(), 17);
  EXPECT_GT(SkipStats::mandOptConjunctionWindows, 0);
}

TEST_F(TermScorerTest, mandOptMinScoreZeroBypassesWindowWalk) {
  const int32_t nDocs = Postings::DOCS_BLOCK_SIZE + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "zero_req";
    if ((doc % 11) == 3) {
      body += " zero_opt";
    }
    body += " filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery req("body_w", "zero_req");
  TermQuery opt("body_w", "zero_opt");
  std::vector<Query*> mandatory = {&req};
  std::vector<Query*> optional = {&opt};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  SkipStatsGuard stats;
  scorer->setMinCompetitiveScore(0.0f);
  EXPECT_EQ((int32_t) collectDocIds(scorer).size(), nDocs);
  EXPECT_EQ(SkipStats::mandOptWindowEvals, 0);
  EXPECT_EQ(SkipStats::mandOptConjunctionWindows, 0);
  EXPECT_EQ(SkipStats::mandOptWindowSkips, 0);
}

TEST_F(TermScorerTest, mandOptFilterRequiredPushesThetaToRequiredOptional) {
  const int32_t nDocs = 3 * Postings::DOCS_BLOCK_SIZE + 11;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "filter_req ";
    if (doc < 6) {
      body += "alpha beta";
    } else {
      body += "alpha beta ";
      appendRepeatedTerm(body, "filler", 20 + (doc % 3));
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  float highScore = 0.0f;
  float weakScore = 0.0f;
  {
    PhraseQuery phraseForScores("body_w", terms, positions);
    auto* phraseWeight = phraseForScores.createWeight(qContext, Query::NEED_SCORES);
    auto* phraseScorer = phraseWeight->createScorer(testIndex.pool, segment);
    ASSERT_NE(phraseScorer, nullptr);
    for (int32_t doc = phraseScorer->next(); doc != PostingsReader::END; doc = phraseScorer->next()) {
      if (doc == 0) {
        highScore = phraseScorer->score();
      } else if (doc == 6) {
        weakScore = phraseScorer->score();
        break;
      }
    }
  }
  ASSERT_GT(highScore, weakScore);
  float theta = (highScore + weakScore) * 0.5f;
  ASSERT_GT(theta, weakScore);

  TermQuery filterTerm("body_w", "filter_req");
  PhraseQuery phrase("body_w", terms, positions);
  std::vector<Query*> optional = {&phrase};
  std::vector<Query*> filter = {&filterTerm};
  BooleanQuery query({}, optional, {}, filter);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  SkipStatsGuard stats;
  scorer->setMinCompetitiveScore(theta);
  int32_t seen = 0;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    EXPECT_LT(doc, 6);
    EXPECT_GE(scorer->score(), theta);
    seen++;
  }
  EXPECT_GT(seen, 0);
  EXPECT_GT(SkipStats::mandOptConjunctionWindows, 0);
  EXPECT_GT(SkipStats::phraseBoundRejects, 0);
}

TEST_F(TermScorerTest, mandOptTwoPhaseMandatoryPhraseMatchesExhaustive) {
  const int32_t nDocs = 5 * Postings::DOCS_BLOCK_SIZE + 29;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    bool decoy = (doc % 6) == 0;
    int32_t phraseRepeats = doc < 20 ? 6 - (doc % 3) : 1;
    if (decoy) {
      body = "alpha pad beta ";
    } else {
      for (int32_t i = 0; i < phraseRepeats; i++) {
        body += "alpha beta ";
      }
    }
    if ((doc % 97) == 7) {
      appendRepeatedTerm(body, "phrase_boost", 40);
    }
    appendRepeatedTerm(body, "filler", doc < 20 ? 3 : 240 + (doc % 31));
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  TermQuery opt("body_w", "phrase_boost");
  std::vector<Query*> mandatory = {&phrase};
  std::vector<Query*> optional = {&opt};

  auto expected = runBooleanTopK(*testIndex.reader, mandatory, optional, 8, false);
  SkipStatsGuard stats;
  auto actual = runBooleanTopK(*testIndex.reader, mandatory, optional, 8, true);
  assertSameTopKDocs(expected, actual, 8);
  EXPECT_GT(SkipStats::mandOptWindowEvals, 0);
}

TEST_F(TermScorerTest, mandOptExternalPhraseApproximationRetainsRequiredOptional) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "alpha beta gamma delta");
  f.add(1, "alpha beta gamma pad delta");
  f.add(2, "alpha pad beta gamma delta");
  f.add(3, "alpha beta gamma delta");
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};

  PhraseQuery phraseForMax("body_w", terms, positions);
  auto* phraseWeight = phraseForMax.createWeight(qContext, Query::NEED_SCORES);
  auto* phraseScorer = phraseWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(phraseScorer, nullptr);
  float theta = std::nextafter(
      phraseScorer->getMaxScoreForSetup(PostingsReader::END),
      std::numeric_limits<float>::infinity());

  PhraseQuery phrase("body_w", terms, positions);
  std::vector<std::string_view> optTerms = {"gamma", "delta"};
  std::vector<int32_t> optPositions = {0, 1};
  PhraseQuery bonus("body_w", optTerms, optPositions);
  std::vector<Query*> mandatory = {&phrase};
  std::vector<Query*> optional = {&bonus};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);
  scorer->setMinCompetitiveScore(theta);

  auto enums = scorer->approximationEnums();
  ASSERT_EQ(2, enums.size());
  auto positionEnums = [&](int32_t doc) {
    for (DocsPosEnum* docsEnum : enums) {
      ASSERT_LT(docsEnum->docId(), doc);
      EXPECT_EQ(doc, docsEnum->advance(doc));
    }
  };

  SkipStatsGuard stats;
  positionEnums(0);
  EXPECT_TRUE(scorer->matchesAt(0));
  EXPECT_GT(scorer->score(), 0.0f);
  EXPECT_GT(scorer->score(), 0.0f);
  EXPECT_EQ(1, SkipStats::mandOptOptionalVerifies);

  positionEnums(1);
  EXPECT_FALSE(scorer->matchesAt(1));
  EXPECT_EQ(2, SkipStats::mandOptOptionalVerifies);

  positionEnums(2);
  EXPECT_FALSE(scorer->matchesAt(2));
  EXPECT_EQ(2, SkipStats::mandOptOptionalVerifies);

  positionEnums(3);
  EXPECT_TRUE(scorer->matchesAt(3));
  EXPECT_GT(scorer->score(), 0.0f);
  EXPECT_GT(scorer->score(), 0.0f);
  EXPECT_EQ(3, SkipStats::mandOptOptionalVerifies);
}

TEST_F(TermScorerTest, mandOptTwoPhaseOptionalScoresOnlyConfirmedMatches) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "req alpha pad beta filler filler");
  f.add(1, "req alpha beta filler");
  f.add(2, "req filler filler filler");
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<float> reqScores(3, 0.0f);
  {
    TermQuery reqOnly("body_w", "req");
    auto* weight = reqOnly.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    ASSERT_NE(scorer, nullptr);
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      reqScores[(size_t) doc] = scorer->score();
    }
  }

  TermQuery req("body_w", "req");
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  std::vector<Query*> mandatory = {&req};
  std::vector<Query*> optional = {&phrase};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = weight->createScorer(testIndex.pool, segment);
  ASSERT_NE(scorer, nullptr);

  ASSERT_EQ(scorer->next(), 0);
  EXPECT_FLOAT_EQ(scorer->score(), reqScores[0]);
  ASSERT_EQ(scorer->next(), 1);
  EXPECT_GT(scorer->score(), reqScores[1]);
  ASSERT_EQ(scorer->next(), 2);
  EXPECT_FLOAT_EQ(scorer->score(), reqScores[2]);
  EXPECT_EQ(scorer->next(), PostingsReader::END);
}

TEST_F(TermScorerTest, mandOptBulkMatchesPullAcrossClauseCountsFiltersAndDeletes) {
  const int32_t segDocs = DocsEnumMeta::L1_DOCS + 257;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  for (int32_t seg = 0; seg < 3; seg++) {
    field.startIndexing();
    for (int32_t local = 0; local < segDocs; local++) {
      int32_t doc = seg * segDocs + local;
      std::string body;
      appendRepeatedTerm(body, "mob_mand_dense", 1 + (doc % 5));
      if ((doc % 17) == 0) appendRepeatedTerm(body, "mob_mand_sparse", 2 + (doc % 3));
      if ((local % 4) != 1) appendRepeatedTerm(body, "mob_opt_a", 1 + (doc % 4));
      if ((doc % 10) == 0) appendRepeatedTerm(body, "mob_opt_b", 9);
      if (seg < 2 && (local % 257) == 17) appendRepeatedTerm(body, "mob_opt_c", 25);
      if (seg == 0 && (local % 31) == 3) appendRepeatedTerm(body, "mob_opt_d", 17);
      appendRepeatedTerm(body, "mob_filler", 4 + (doc % 3));
      field.add(local, body);
      if ((doc % 29) == 11) {
        testIndex.deleteDoc(local);
      }
    }
    testIndex.flush();
  }
  field.startReading();
  auto reader = testIndex.reader;

  TermQuery mandDense("body_w", "mob_mand_dense");
  TermQuery mandSparse("body_w", "mob_mand_sparse");
  TermQuery optA("body_w", "mob_opt_a");
  TermQuery optB("body_w", "mob_opt_b");
  TermQuery optC("body_w", "mob_opt_c");
  TermQuery optD("body_w", "mob_opt_d");
  std::array<Query*, 2> mandQueries = {&mandDense, &mandSparse};
  std::array<Query*, 4> optQueries = {&optA, &optB, &optC, &optD};

  int64_t bulkWindows = 0;
  int64_t bulkSweeps = 0;
  for (Query* mand : mandQueries) {
    std::span<Query*> mandatory(&mand, (size_t) 1);
    for (int32_t optCount : {1, 2, 4}) {
      std::span<Query*> optional(optQueries.data(), (size_t) optCount);
      for (int32_t filterMode : {0, 1, 2}) {
        int32_t filterStep = filterMode == 0 ? 0 : filterMode == 1 ? 3 : 5;
        bool arrayFilter = filterMode == 2;
        bool liveOnly = filterMode != 0;
        bool sawBulk = false;
        auto expected = runMandOptSupplierTopK(
            *reader, mandatory, optional, 13, true, filterStep, arrayFilter, liveOnly);
        SkipStatsGuard stats;
        auto actual = runMandOptSupplierTopK(
            *reader, mandatory, optional, 13, false, filterStep, arrayFilter, liveOnly,
            true, &sawBulk);
        EXPECT_TRUE(sawBulk) << "optCount=" << optCount << " filterMode=" << filterMode;
        assertSameTopKDocs(expected, actual, 13);
        bulkWindows += SkipStats::mandOptBulkWindows;
        bulkSweeps += SkipStats::mandOptBulkSweeps;
      }
    }
  }
  EXPECT_GT(bulkWindows, 0);
  EXPECT_GT(bulkSweeps, 0);
}

TEST_F(TermScorerTest, mandOptBulkZeroAndOneSurvivingOptionalScorers) {
  CollectionHelper helper("mand_opt_zero_one_surviving_optional");
  helper.index(flatdoc("id", "z0", "body_w", "mob_zero_mand"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "z1", "body_w", "mob_zero_mand mob_one_opt"), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();

  MemPool pool;
  Query::Context qContext(pool, *reader);
  TermQuery mand("body_w", "mob_zero_mand");
  TermQuery presentOpt("body_w", "mob_one_opt");
  TermQuery absentOpt("body_w", "mob_absent_opt");
  std::array<Query*, 1> mandatory = {&mand};
  std::array<Query*, 2> optional = {&presentOpt, &absentOpt};
  std::span<Query*> mandatorySpan(mandatory.data(), mandatory.size());
  std::span<Query*> optionalSpan(optional.data(), optional.size());
  std::span<Query*> empty;
  BooleanQuery query(mandatorySpan, optionalSpan, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto segments = qContext.topReader.segments();
  ASSERT_EQ(segments.size(), 2u);

  auto* seg0Supplier = weight->scorerSupplier(pool, segments[0]);
  ASSERT_NE(seg0Supplier, nullptr);
  EXPECT_EQ(seg0Supplier->bulkScorer(pool), nullptr);
  auto* seg0Scorer = buildScorerForTests(
      pool, *seg0Supplier, std::numeric_limits<int64_t>::max());
  ASSERT_NE(seg0Scorer, nullptr);
  EXPECT_EQ(collectDocIds(seg0Scorer), std::vector<int32_t>({0}));

  auto* seg1Supplier = weight->scorerSupplier(pool, segments[1]);
  ASSERT_NE(seg1Supplier, nullptr);
  auto* bulk = seg1Supplier->bulkScorer(pool);
  EXPECT_NE(dynamic_cast<BooleanQuery::MandOptBulkScorer*>(bulk), nullptr);
  helper.clear();
}

TEST_F(TermScorerTest, mandOptBulkFallbackRoutingAndTwoPhaseChildren) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "route_mand route_opt route_filter alpha beta");
  f.add(1, "route_mand route_opt route_block alpha pad beta");
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery mand("body_w", "route_mand");
  TermQuery opt("body_w", "route_opt");
  TermQuery filterTerm("body_w", "route_filter");
  TermQuery prohibited("body_w", "route_block");
  std::vector<std::string_view> phraseTerms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery phrase("body_w", phraseTerms, positions);
  ForcePrepareQuery wrappedPhrase(&phrase);
  std::span<Query*> empty;

  auto expectNoBulk = [&](std::span<Query*> mandatory, std::span<Query*> optional,
                          std::span<Query*> prohibitedSpan, std::span<Query*> filter,
                          int minShouldMatch) {
    BooleanQuery query(mandatory, optional, prohibitedSpan, filter, minShouldMatch);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    EXPECT_EQ(supplier->bulkScorer(testIndex.pool), nullptr);
  };

  std::array<Query*, 1> mandOnly = {&mand};
  std::array<Query*, 1> optOnly = {&opt};
  std::array<Query*, 1> filterOnly = {&filterTerm};
  std::array<Query*, 1> prohibitedOnly = {&prohibited};
  {
    // A dense direct-term filter routes MandOpt to the window-mask bulk.
    BooleanQuery query(std::span<Query*>(mandOnly), optOnly, empty, filterOnly, 0);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    EXPECT_NE(supplier->bulkScorer(testIndex.pool), nullptr);
  }
  expectNoBulk(mandOnly, optOnly, prohibitedOnly, empty, 0);
  expectNoBulk(mandOnly, optOnly, empty, empty, 1);
  expectNoBulk(mandOnly, optOnly, prohibitedOnly, filterOnly, 0);

  std::array<Query*, 1> phraseMand = {&phrase};
  expectNoBulk(phraseMand, optOnly, empty, empty, 0);
  std::array<Query*, 1> phraseOpt = {&phrase};
  expectNoBulk(mandOnly, phraseOpt, empty, empty, 0);
  expectNoBulk(mandOnly, phraseOpt, empty, filterOnly, 0);
  std::array<Query*, 1> wrappedOpt = {&wrappedPhrase};
  expectNoBulk(mandOnly, wrappedOpt, empty, empty, 0);

  {
    SkipStatsGuard stats;
    BooleanQuery query(mandOnly, optOnly, empty, filterOnly, 1);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    unused(supplier->bulkScorer(testIndex.pool));
    EXPECT_EQ(0, SkipStats::exactFilteredMandOptCompositions);
  }
}

TEST_F(TermScorerTest, mandOptBulkHybridEngagesForDenseMandSparseOptHighTheta) {
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 31;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  std::vector<int32_t> optDocs;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    appendRepeatedTerm(body, "hyb_mand", 1);
    if ((doc % 257) == 17) {
      appendRepeatedTerm(body, "hyb_opt", 8);
      optDocs.push_back(doc);
    }
    appendRepeatedTerm(body, "hyb_filler", 6 + (doc % 3));
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  float mandMax = 0.0f;
  {
    TermQuery mandOnly("body_w", "hyb_mand");
    auto* weight = mandOnly.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    ASSERT_NE(scorer, nullptr);
    mandMax = scorer->getMaxScoreForSetup(PostingsReader::END);
  }
  ASSERT_TRUE(std::isfinite(mandMax));
  float theta = std::nextafter(mandMax * 1.05f, std::numeric_limits<float>::infinity());

  TermQuery mand("body_w", "hyb_mand");
  TermQuery opt("body_w", "hyb_opt");
  std::array<Query*, 1> mandatory = {&mand};
  std::array<Query*, 1> optional = {&opt};
  auto* bulk = createMandOptBulkScorer(
      testIndex.pool, qContext, segment,
      std::span<Query*>(mandatory.data(), mandatory.size()),
      std::span<Query*>(optional.data(), optional.size()));
  ASSERT_NE(dynamic_cast<BooleanQuery::MandOptBulkScorer*>(bulk), nullptr);

  SkipStatsGuard stats;
  std::vector<int32_t> docs;
  for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
    ScoreWindow window;
    int32_t next = bulk->scoreNextWindow(window, nullptr, cursor, segment.maxDoc(), theta);
    for (int32_t i = 0; i < window.size; i++) {
      docs.push_back(window.docs[(size_t) i]);
      EXPECT_GE(window.scores[(size_t) i], theta);
    }
    if (next == PostingsReader::END) break;
    ASSERT_GT(next, cursor);
    cursor = next;
  }
  EXPECT_EQ(docs, optDocs);
  EXPECT_GT(SkipStats::mandOptBulkOptDrivenWindows, 0);
}

TEST_F(TermScorerTest, mandOptBulkInfiniteMaxOptionalDegradesSafely) {
  const int32_t nDocs = 3 * Postings::DOCS_BLOCK_SIZE + 9;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "inf_mand";
    if (doc == 7) {
      body += " inf_opt";
    }
    body += " inf_filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  TermQuery mand("body_w", "inf_mand");
  TermQuery opt("body_w", "inf_opt");
  std::array<Query*, 1> mandatory = {&mand};
  std::array<Query*, 1> optional = {&opt};
  bool sawBulk = false;
  auto expected = runMandOptSupplierTopK(
      *testIndex.reader, mandatory, optional, 20, true);
  SkipStatsGuard stats;
  auto actual = runMandOptSupplierTopK(
      *testIndex.reader, mandatory, optional, 20, false, 0, false, false, true, &sawBulk);
  EXPECT_TRUE(sawBulk);
  assertSameTopKDocs(expected, actual, 20);
  EXPECT_GT(SkipStats::mandOptBulkWindows, 0);
  EXPECT_EQ(SkipStats::mandOptBulkWindowSkips, 0);
}

TEST_F(TermScorerTest, mandOptBulkThetaZeroBypassesWindowWalk) {
  const int32_t nDocs = Postings::DOCS_BLOCK_SIZE + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "zero_bulk_mand";
    if ((doc % 11) == 3) body += " zero_bulk_opt";
    body += " zero_bulk_filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery mand("body_w", "zero_bulk_mand");
  TermQuery opt("body_w", "zero_bulk_opt");
  std::array<Query*, 1> mandatory = {&mand};
  std::array<Query*, 1> optional = {&opt};
  auto* bulk = createMandOptBulkScorer(
      testIndex.pool, qContext, segment,
      std::span<Query*>(mandatory.data(), mandatory.size()),
      std::span<Query*>(optional.data(), optional.size()));
  ASSERT_NE(bulk, nullptr);

  int32_t seen = 0;
  SkipStatsGuard stats;
  for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
    ScoreWindow window;
    int32_t next = bulk->scoreNextWindow(window, nullptr, cursor, segment.maxDoc(), 0.0f);
    seen += window.size;
    if (next == PostingsReader::END) break;
    ASSERT_GT(next, cursor);
    cursor = next;
  }
  EXPECT_EQ(seen, nDocs);
  EXPECT_EQ(SkipStats::mandOptBulkWindowSkips, 0);
  EXPECT_EQ(SkipStats::shallowCursorMoves, 0);
}

TEST_F(TermScorerTest, mandOptKeepsThetaOutOfMandatoryTermScorer) {
  const int32_t nDocs = 3 * Postings::DOCS_BLOCK_SIZE + 11;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "theta_mand filler";
    if ((doc % 3) == 0) {
      body += " theta_opt";
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  for (bool bulkPath : {false, true}) {
    SCOPED_TRACE(bulkPath ? "bulk" : "pull");
    MemPool pool;
    Query::Context qContext(pool, *testIndex.reader);
    auto& segment = qContext.topReader.segments()[0];
    TermQuery mand("body_w", "theta_mand", 4.0f);
    TermQuery opt("body_w", "theta_opt");
    std::array<Query*, 1> mandatory = {&mand};
    std::array<Query*, 1> optional = {&opt};
    std::span<Query*> empty;
    BooleanQuery query(mandatory, optional, empty, empty);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);

    SkipStatsGuard stats;
    if (bulkPath) {
      auto* bulk = supplier->bulkScorer(pool);
      ASSERT_NE(dynamic_cast<BooleanQuery::MandOptBulkScorer*>(bulk), nullptr);
      ScoreWindow window;
      bulk->scoreNextWindow(window, nullptr, 0, segment.maxDoc(), 100.0f);
      EXPECT_GT(SkipStats::mandOptBulkWindowSkips, 0);
    } else {
      auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
          buildScorerForTests(
              pool, *supplier, std::numeric_limits<int64_t>::max()));
      ASSERT_NE(scorer, nullptr);
      scorer->setMinCompetitiveScore(100.0f);
      EXPECT_EQ(scorer->next(), PostingsReader::END);
      EXPECT_GT(SkipStats::mandOptWindowEvals, 0);
    }
    EXPECT_EQ(SkipStats::impactCertificateInvalidations, 0);
    EXPECT_EQ(SkipStats::impactCertificateSurvivedRises, 0);
  }
}

TEST_F(TermScorerTest, mandOptBulkCapacityWindowAndCountDomainUseMandDocs) {
  const int32_t nDocs = DocsEnumMeta::L1_DOCS;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "cap_mand";
    if ((doc % 2) == 0) body += " cap_opt";
    body += " cap_filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery mand("body_w", "cap_mand");
  TermQuery opt("body_w", "cap_opt");
  std::array<Query*, 1> mandatory = {&mand};
  std::array<Query*, 1> optional = {&opt};

  {
    auto* bulk = createMandOptBulkScorer(
        testIndex.pool, qContext, segment,
        std::span<Query*>(mandatory.data(), mandatory.size()),
        std::span<Query*>(optional.data(), optional.size()));
    ASSERT_NE(bulk, nullptr);
    ScoreWindow window;
    int32_t next = bulk->scoreNextWindow(
        window, nullptr, 0, segment.maxDoc(), std::numeric_limits<float>::lowest());
    EXPECT_EQ(next, PostingsReader::END);
    EXPECT_EQ(window.min, 0);
    EXPECT_EQ(window.max, nDocs);
    EXPECT_EQ(window.size, nDocs);
  }

  for (bool arrayDocSet : {false, true}) {
    MemPool countPool;
    Query::Context countContext(countPool, *testIndex.reader);
    auto& countSegment = countContext.topReader.segments()[0];
    TermQuery countMand("body_w", "cap_mand");
    TermQuery countOpt("body_w", "cap_opt");
    std::array<Query*, 1> countMandatory = {&countMand};
    std::array<Query*, 1> countOptional = {&countOpt};
    auto* bulk = createMandOptBulkScorer(
        countPool, countContext, countSegment,
        std::span<Query*>(countMandatory.data(), countMandatory.size()),
        std::span<Query*>(countOptional.data(), countOptional.size()));
    ASSERT_NE(bulk, nullptr);
    auto filter = makeEveryNthSegmentDocSet(countSegment, arrayDocSet ? 5 : 3,
                                            arrayDocSet, false);
    DocSetBuilder builder(countSegment.maxDoc());
    int64_t count = 0;
    for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < countSegment.maxDoc(); ) {
      int32_t next = bulk->countNextWindow(count, &builder, filter.get(),
                                           cursor, countSegment.maxDoc());
      if (next == PostingsReader::END) break;
      ASSERT_GT(next, cursor);
      cursor = next;
    }
    auto actual = builder.build();

    DocSetBuilder expectedBuilder(countSegment.maxDoc());
    for (int32_t doc = 0; doc < countSegment.maxDoc(); doc++) {
      if (filter->get(doc)) {
        expectedBuilder.add(doc);
      }
    }
    auto expected = expectedBuilder.build();
    EXPECT_EQ(count, expected->card()) << "arrayDocSet=" << arrayDocSet;
    expectDocSetEqual(actual.get(), expected.get(), countSegment.maxDoc());
  }
}

TEST_F(TermScorerTest, mandOptBulkPreparedSourcesCanRouteToBulk) {
  CollectionHelper helper("main");
  helper.index(flatdoc("id", "prep0", "body_w", "prep_mand prep_opt"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "prep1", "body_w", "prep_mand"), UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();

  MemPool pool;
  Query::Context qContext(pool, *reader);
  TermQuery mandTerm("body_w", "prep_mand");
  TermQuery optTerm("body_w", "prep_opt");
  ForcePrepareQuery mand(&mandTerm);
  ForcePrepareQuery opt(&optTerm);
  std::array<Query*, 1> mandatory = {&mand};
  std::array<Query*, 1> optional = {&opt};
  std::span<Query*> empty;
  BooleanQuery query(mandatory, optional, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  ASSERT_TRUE(weight->needsPrepare());
  Query::Weight::PrepareContext pctx{*reader, std::span<DocSet* const>{}, false};
  auto prepared = weight->prepare(pctx);
  auto& segment = qContext.topReader.segments()[0];
  auto* supplier = prepared->scorerSupplier(pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = supplier->bulkScorer(pool);
  EXPECT_NE(dynamic_cast<BooleanQuery::MandOptBulkScorer*>(bulk), nullptr);
}

TEST_F(TermScorerTest, disjunctionBoundsAreFiniteConservativeAndRefinable) {
  const int32_t nDocs = 3 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    appendRepeatedTerm(body, "bound_a", 1 + (doc % 5));
    used += 1 + (doc % 5);
    if ((doc % 3) == 0) {
      appendRepeatedTerm(body, "bound_b", 1 + ((doc / 3) % 7));
      used += 1 + ((doc / 3) % 7);
    }
    if ((doc % 5) == 2) {
      appendRepeatedTerm(body, "bound_c", 1 + ((doc / 5) % 3));
      used += 1 + ((doc / 5) % 3);
    }
    appendRepeatedTerm(body, "filler", 60 + (doc % 19) - std::min(used, 60 + (doc % 19)));
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<const char*, 3> termNames = {"bound_a", "bound_b", "bound_c"};
  std::array<std::vector<float>, 3> termScores;
  for (size_t t = 0; t < termNames.size(); t++) {
    termScores[t].assign((size_t) nDocs, 0.0f);
    TermQuery query("body_w", termNames[t]);
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    ASSERT_NE(scorer, nullptr);
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      termScores[t][(size_t) doc] = scorer->score();
    }
  }

  auto* scorers = testIndex.pool.make_arr<Query::Scorer*>(termNames.size());
  std::array<TermQuery, 3> queries = {
    TermQuery("body_w", "bound_a"),
    TermQuery("body_w", "bound_b"),
    TermQuery("body_w", "bound_c")
  };
  for (size_t t = 0; t < queries.size(); t++) {
    auto* weight = queries[t].createWeight(qContext, Query::NEED_SCORES);
    scorers[t] = weight->createScorer(testIndex.pool, segment);
    ASSERT_NE(scorers[t], nullptr);
  }
  auto* disj = testIndex.pool.make<BooleanQuery::DisjunctionScorer>(
      testIndex.pool, std::span<Query::Scorer*>(scorers, termNames.size()),
      singlePhaseScorersForTests(testIndex.pool, termNames.size()), true);

  int32_t target = Postings::DOCS_BLOCK_SIZE + 5;
  int32_t upTo = disj->advanceShallow(target);
  ASSERT_GE(upTo, target);
  float cheap = disj->getMaxScore(upTo);
  float refined = disj->refineMaxScore(upTo);
  ASSERT_TRUE(std::isfinite(cheap));
  ASSERT_TRUE(std::isfinite(refined));

  float brute = 0.0f;
  for (int32_t doc = target; doc <= upTo && doc < nDocs; doc++) {
    float sum = 0.0f;
    for (size_t t = 0; t < termNames.size(); t++) {
      sum += termScores[t][(size_t) doc];
    }
    brute = std::max(brute, sum);
  }
  EXPECT_GE(cheap + 1e-6f, brute);
  EXPECT_GE(refined + 1e-6f, brute);
  EXPECT_LE(refined, cheap + 1e-6f);
}

TEST_F(TermScorerTest, mandOptNearThetaRefineSkipsCoarseCompetitiveGroup) {
  const int32_t nDocs = 5 * Postings::DOCS_BLOCK_SIZE;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    int32_t reqTf = block == 0 ? 50 : 1;
    int32_t len = block == 0 ? reqTf + 2 : 220;
    std::string body;
    int32_t used = 0;
    appendRepeatedTerm(body, "near_req_probe", reqTf);
    appendRepeatedTerm(body, "near_req", reqTf);
    used += 2 * reqTf;
    appendRepeatedTerm(body, "near_opt_probe", 1);
    appendRepeatedTerm(body, "near_opt", 1);
    used += 2;
    if (len < used + 2) {
      len = used + 2;
    }
    appendRepeatedTerm(body, "filler", len - used);
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  int32_t target = 2 * Postings::DOCS_BLOCK_SIZE + 7;
  float theta;
  {
    TermQuery req("body_w", "near_req_probe");
    TermQuery opt("body_w", "near_opt_probe");
    std::vector<Query*> mandatory = {&req};
    std::vector<Query*> optional = {&opt};
    BooleanQuery query(mandatory, optional, {}, {});
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
        weight->createScorer(testIndex.pool, segment));
    ASSERT_NE(scorer, nullptr);
    int32_t upTo = scorer->advanceShallow(target);
    float cheap = scorer->getMaxScore(upTo);
    float refined = scorer->refineMaxScore(upTo);
    ASSERT_TRUE(std::isfinite(cheap));
    ASSERT_TRUE(std::isfinite(refined));
    ASSERT_LT(refined, cheap);
    theta = std::nextafter(
        std::max(refined, (float) (BooleanQuery::kRefineBeta * (double) cheap)),
        std::numeric_limits<float>::infinity());
    ASSERT_GT(theta, refined);
    ASSERT_LT(theta, cheap);
  }

  TermQuery req("body_w", "near_req");
  TermQuery opt("body_w", "near_opt");
  std::vector<Query*> mandatory = {&req};
  std::vector<Query*> optional = {&opt};
  BooleanQuery query(mandatory, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<BooleanQuery::MandOptScorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  SkipStatsGuard stats;
  scorer->setMinCompetitiveScore(theta);
  EXPECT_NE(scorer->advance(target), target);
  EXPECT_GT(SkipStats::mandOptWindowSkips, 0);
  EXPECT_GT(SkipStats::impactRefinesTriggered, 0);
}

TEST_F(TermScorerTest, termImpactGroupBoundsHandleFinalPartialGroup) {
  const int32_t postingCount = DocsEnumMeta::L1_DOCS + Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  addAntiCorrelatedFrontierDocs(f, postingCount);
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "frontier", 1.0f, true);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  int32_t groupCount = scorer->impacts.groupContainingFrom(-1, PostingsReader::END);
  ASSERT_EQ(groupCount, 2);
  int32_t fromBlock = DocsEnumMeta::L1_PERIOD;
  int32_t toBlock = scorer->impacts.blockCount() - 1;
  ASSERT_LT(fromBlock, toBlock);
  EXPECT_FLOAT_EQ(scorer->impacts.maxGroupImpactFrom(1),
                  scorer->impacts.maxImpactInRange(fromBlock, toBlock));
}

TEST_F(TermScorerTest, termImpactGroupBoundsHandleFreqOnlyScalarHeaders) {
  const int32_t N = DocsEnumMeta::L1_DOCS + 19;
  RAMDir dir;
  MemPool pool;
  PostingsWriter postingsWriter(dir, 0, N + 16);
  {
    TextWriter writer(postingsWriter);
    auto& finfo = postingsWriter.addField("f");
    finfo.type = FieldType::TEXT;
    finfo.flags = FieldType::INDEX_DOCS_FREQS;
    writer.startField(&finfo);
    TermRef hot(pool, "hot", 3);
    writer.startTerm(hot);
    for (int32_t doc = 0; doc < N; doc++) {
      writer.addDoc(doc, 1 + (doc % 9));
    }
    writer.endTerm(hot);
    writer.endField();
  }
  postingsWriter.finish();

  PostingsReader reader(dir, 0);
  FieldReader fieldReader(reader);
  ASSERT_TRUE(fieldReader.readNextField());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, reader, fieldInfo);
  ASSERT_TRUE(tenum.seek("hot"));
  DocsFreqEnum denum(tenum);
  EXPECT_FALSE(denum.hasTermImpacts());

  Similarity::FieldStats fieldStats;
  fieldStats.sumTotalTermFreq = tenum.sumTotalTermFreq();
  fieldStats.sumDocFreq = tenum.sumDocFreq();
  fieldStats.docsWithField = tenum.docsWithField();
  fieldStats.maxDoc = reader.maxDoc();
  Similarity::TermStats termStats;
  termStats.docFreq = denum.numDocs();
  termStats.totalTermFreq = denum.totalTermFreq();
  Similarity sim;
  auto simScorer = sim.getScorer(1.0f, fieldStats, termStats);
  SkipStatsGuard stats;
  ImpactsIndex impacts;
  impacts.build(pool, denum, simScorer, 1.0f, true);
  ASSERT_FALSE(impacts.empty());
  EXPECT_FLOAT_EQ(impacts.maxGroupImpactFrom(0),
                  impacts.maxImpactInRange(0, impacts.blockCount() - 1));
  EXPECT_EQ(SkipStats::impactGroupHeaderParses, 0);
}

TEST_F(TermScorerTest, termImpactFrontierTopKMatchesCornerAndExhaustive) {
  const int32_t postingCount = 14 * Postings::DOCS_BLOCK_SIZE;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  addAntiCorrelatedFrontierDocs(f, postingCount);
  testIndex.flush();
  f.startReading();

  for (int32_t k : {3, 25}) {
    TermImpactTopKRun exhaustive = runSingleTermFrontierTopK(*testIndex.reader, k, true, false);
    TermImpactTopKRun corner = runSingleTermFrontierTopK(*testIndex.reader, k, false, true);
    TermImpactTopKRun frontier = runSingleTermFrontierTopK(*testIndex.reader, k, true, true);

    ASSERT_EQ(corner.topDocs.size(), exhaustive.topDocs.size()) << "k=" << k;
    ASSERT_EQ(frontier.topDocs.size(), exhaustive.topDocs.size()) << "k=" << k;
    for (size_t i = 0; i < exhaustive.topDocs.size(); i++) {
      EXPECT_EQ(corner.topDocs[i].doc, exhaustive.topDocs[i].doc) << "k=" << k << " i=" << i;
      EXPECT_FLOAT_EQ(corner.topDocs[i].score, exhaustive.topDocs[i].score) << "k=" << k << " i=" << i;
      EXPECT_EQ(frontier.topDocs[i].doc, exhaustive.topDocs[i].doc) << "k=" << k << " i=" << i;
      EXPECT_FLOAT_EQ(frontier.topDocs[i].score, exhaustive.topDocs[i].score) << "k=" << k << " i=" << i;
    }
    EXPECT_LE(frontier.visited, corner.visited) << "k=" << k;
    EXPECT_GE(frontier.skippedBlocks, corner.skippedBlocks) << "k=" << k;
    if (k == 3) {
      EXPECT_LT(frontier.visited, corner.visited) << "k=" << k;
      EXPECT_GT(frontier.skippedBlocks, corner.skippedBlocks) << "k=" << k;
    }
  }
}

TEST_F(TermScorerTest, termImpactTopKSkippingMatchesExhaustive) {
  const int32_t N = 6 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    int32_t tf;
    int32_t len;
    if (doc < Postings::DOCS_BLOCK_SIZE) {
      tf = 24 + (doc % 29);
      len = tf + (doc % 11);
    } else {
      tf = 1;
      len = 180 + (doc % 37);
    }
    std::string text;
    for (int32_t i = 0; i < tf; i++) {
      text += "impactskip ";
    }
    for (int32_t i = tf; i < len; i++) {
      text += "filler ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  for (int32_t k : {1, 5, 50}) {
    TermQuery exhaustiveQuery("body_w", "impactskip");
    TermQuery prunedQuery("body_w", "impactskip");
    auto* exhaustiveWeight = exhaustiveQuery.createWeight(qContext, Query::NEED_SCORES);
    auto* prunedWeight = prunedQuery.createWeight(qContext, Query::NEED_SCORES);

    auto* exhaustiveScorer = dynamic_cast<TermQuery::Scorer*>(
        exhaustiveWeight->createScorer(testIndex.pool, segment));
    ASSERT_NE(exhaustiveScorer, nullptr);
    TopDocsCollector exhaustiveCollector(k);
    for (int32_t doc = exhaustiveScorer->next(); doc != PostingsReader::END; doc = exhaustiveScorer->next()) {
      exhaustiveCollector.collect(0, doc, exhaustiveScorer->score());
    }
    ASSERT_EQ(exhaustiveCollector.totalHits(), N);

    auto* prunedScorer = dynamic_cast<TermQuery::Scorer*>(
        prunedWeight->createScorer(testIndex.pool, segment));
    ASSERT_NE(prunedScorer, nullptr);
    TopDocsCollector prunedCollector(k);
    collectTopK(0, prunedScorer, nullptr, nullptr, prunedCollector);

    auto expected = exhaustiveCollector.sort();
    auto actual = prunedCollector.sort();
    ASSERT_EQ(actual.size(), expected.size()) << "k=" << k;
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(actual[i].doc, expected[i].doc) << "k=" << k << " i=" << i;
      EXPECT_FLOAT_EQ(actual[i].score, expected[i].score) << "k=" << k << " i=" << i;
    }
    EXPECT_LT(prunedCollector.totalHits(), exhaustiveCollector.totalHits()) << "k=" << k;
  }
}

TEST_F(TermScorerTest, phraseImpactShallowStillUsesBlockGranularity) {
  const int32_t N = 5 * Postings::DOCS_BLOCK_SIZE;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    std::string text = "alpha beta ";
    for (int32_t i = 0; i < 20 + (doc % 7); i++) {
      text += "pad ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};
  PhraseQuery query("body_w", terms, positions);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  auto* scorer = dynamic_cast<PhraseQuery::Scorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);
  int32_t target = 2 * Postings::DOCS_BLOCK_SIZE + 3;
  int32_t blockEnd = 3 * Postings::DOCS_BLOCK_SIZE - 1;
  EXPECT_EQ(scorer->advanceShallow(target), blockEnd);
  EXPECT_GT(SkipStats::impactL0GroupParses, 0);
  EXPECT_EQ(SkipStats::impactGroupShallowAnswers, 0);
  EXPECT_TRUE(std::isfinite(scorer->getMaxScore(blockEnd)));

  SkipStats::enabled = savedStats;
}

TEST_F(TermScorerTest, phraseRepeatedTermLazyBoundsCoverScores) {
  const int32_t N = DocsEnumMeta::L1_DOCS + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string text = "a b a ";
    appendRepeatedTerm(text, "a", doc % 4);
    appendRepeatedTerm(text, "filler", 20 + (doc % 31));
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<std::string_view> terms = {"a", "b", "a"};
  std::vector<int32_t> positions = {0, 1, 2};
  PhraseQuery query("body_w", terms, positions);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);

  auto* boundScorer = dynamic_cast<PhraseQuery::Scorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(boundScorer, nullptr);
  int32_t upTo = boundScorer->advanceShallow(0);
  ASSERT_NE(upTo, PostingsReader::END);
  float bound = boundScorer->getMaxScore(upTo);
  ASSERT_TRUE(std::isfinite(bound));
  EXPECT_EQ(boundScorer->advanceShallow(DocsEnumMeta::L1_DOCS + 5), N - 1);
  EXPECT_EQ(boundScorer->advanceShallow(3), upTo);

  auto* scoreScorer = dynamic_cast<PhraseQuery::Scorer*>(
      weight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scoreScorer, nullptr);
  for (int32_t doc = scoreScorer->next(); doc != PostingsReader::END && doc <= upTo;
       doc = scoreScorer->next()) {
    EXPECT_LE(scoreScorer->score(), bound * 1.000001f) << "doc=" << doc;
  }
}

// Phrase pruning: block 0 holds the strong phrase docs (high phrase tf, short
// docs) so the top-k threshold rises past what any later doc's per-doc bound
// (min term tf with its norm) allows; later blocks are long low-tf docs plus
// decoys where both terms appear non-adjacent (conjunction candidates that
// fail position verification).  The pruned run must return the exact
// exhaustive top-k while position-verifying far fewer candidates - the
// pre-position bound in doMatches rejects the rest before touching positions.
TEST_F(TermScorerTest, phraseImpactTopKMatchesExhaustive) {
  const int32_t N = 14 * Postings::DOCS_BLOCK_SIZE + 23;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string text;
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    if (block == 0 || block == 8) {
      // Two separated strong regions: the threshold set in block 0 forces the
      // pruned run to hop the weak ranges between them (not just early-exit).
      int32_t tf = (block == 0 ? 6 : 5) - (doc % 3);
      for (int32_t i = 0; i < tf; i++) text += "alpha beta ";
      for (int32_t i = 0; i < 4 + (doc % 5); i++) text += "pad ";
    } else if (doc % 7 == 3) {
      text = "alpha pad beta ";  // decoy: candidate, no phrase
      for (int32_t i = 0; i < 250 + (doc % 37); i++) text += "pad ";
    } else {
      text = "alpha beta ";
      for (int32_t i = 0; i < 250 + (doc % 37); i++) text += "pad ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<std::string_view> terms = {"alpha", "beta"};
  std::vector<int32_t> positions = {0, 1};

  for (int32_t k : {1, 5}) {
    PhraseQuery exhaustiveQuery("body_w", terms, positions);
    PhraseQuery prunedQuery("body_w", terms, positions);
    auto* exhaustiveWeight = exhaustiveQuery.createWeight(qContext, Query::NEED_SCORES);
    auto* prunedWeight = prunedQuery.createWeight(qContext, Query::NEED_SCORES);

    int64_t exhaustiveMatchCalls;
    TopDocsCollector exhaustiveCollector(k);
    {
      PhraseMatchCountGuard guard(true);
      auto* scorer = exhaustiveWeight->createScorer(testIndex.pool, segment);
      ASSERT_NE(scorer, nullptr);
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        exhaustiveCollector.collect(0, doc, scorer->score());
      }
      exhaustiveMatchCalls = guard.calls();
    }

    int64_t boundRejects;
    int64_t verifies;
    TopDocsCollector prunedCollector(k);
    {
      bool statsWereEnabled = SkipStats::enabled;
      SkipStats::enabled = true;
      int64_t rejectsBefore = SkipStats::phraseBoundRejects;
      int64_t verifiesBefore = SkipStats::phraseVerifies;
      auto* scorer = dynamic_cast<PhraseQuery::Scorer*>(
          prunedWeight->createScorer(testIndex.pool, segment));
      ASSERT_NE(scorer, nullptr);
      collectTopK(0, scorer, nullptr, nullptr, prunedCollector);
      boundRejects = SkipStats::phraseBoundRejects - rejectsBefore;
      verifies = SkipStats::phraseVerifies - verifiesBefore;
      SkipStats::enabled = statsWereEnabled;
    }

    auto expected = exhaustiveCollector.sort();
    auto actual = prunedCollector.sort();
    ASSERT_EQ(actual.size(), expected.size()) << "k=" << k;
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(actual[i].doc, expected[i].doc) << "k=" << k << " i=" << i;
      EXPECT_EQ(std::bit_cast<uint32_t>(actual[i].score),
                std::bit_cast<uint32_t>(expected[i].score)) << "k=" << k << " i=" << i;
    }
    // Every conjunction candidate reaches doMatches, but under the risen
    // threshold most are rejected by the per-doc bound before position work.
    EXPECT_GT(boundRejects, 0) << "k=" << k;
    EXPECT_LT(verifies, exhaustiveMatchCalls) << "k=" << k;
  }
}

// docFreq-sorted phrase execution is an internal permutation: match sets and
// scores must be bit-identical to text-order execution.  Shapes covered: the
// rare term after the lead, a position gap with the rare term late, and a
// repeated term (independent enums over the same postings).
TEST_F(TermScorerTest, phraseSortedExecutionMatchesTextOrder) {
  const int32_t N = 3 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    switch (doc % 7) {
      case 0: f.add(doc, "common rare pad pad"); break;    // "common rare" adjacent
      case 1: f.add(doc, "common pad rare pad"); break;    // gap phrase {0,2}
      case 2: f.add(doc, "common common pad pad"); break;  // repeated-term phrase
      case 3: f.add(doc, "rare common common pad"); break; // repeated term, offset start
      default: f.add(doc, "common pad pad pad"); break;    // df(common) >> df(rare)
    }
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  std::vector<std::string_view> adjacent = {"common", "rare"};
  std::vector<int32_t> adjacentPos = {0, 1};
  std::vector<std::string_view> gapped = {"common", "rare"};
  std::vector<int32_t> gappedPos = {0, 2};
  std::vector<std::string_view> repeated = {"common", "common"};
  std::vector<int32_t> repeatedPos = {0, 1};

  struct Hit { int32_t doc; uint32_t scoreBits; };
  auto run = [&](std::span<std::string_view> terms, std::span<const int32_t> positions,
                 bool sorted) {
    bool saved = PhraseQuery::Scorer::disableSortForTests;
    PhraseQuery::Scorer::disableSortForTests = !sorted;
    PhraseQuery phrase("body_w", terms, positions);
    auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    std::vector<Hit> hits;
    if (scorer != nullptr) {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        hits.push_back({doc, std::bit_cast<uint32_t>(scorer->score())});
      }
    }
    PhraseQuery::Scorer::disableSortForTests = saved;
    return hits;
  };

  struct Case { std::span<std::string_view> terms; std::span<const int32_t> pos; const char* label; };
  for (auto& c : std::initializer_list<Case>{{adjacent, adjacentPos, "adjacent"},
                                             {gapped, gappedPos, "gapped"},
                                             {repeated, repeatedPos, "repeated"}}) {
    auto textOrder = run(c.terms, c.pos, false);
    auto sorted = run(c.terms, c.pos, true);
    ASSERT_FALSE(textOrder.empty()) << c.label;
    ASSERT_EQ(sorted.size(), textOrder.size()) << c.label;
    for (size_t i = 0; i < textOrder.size(); i++) {
      EXPECT_EQ(sorted[i].doc, textOrder[i].doc) << c.label << " i=" << i;
      EXPECT_EQ(sorted[i].scoreBits, textOrder[i].scoreBits) << c.label << " i=" << i;
    }
  }
}

// Repeated-term phrases share one postings enum per distinct term; the
// dedup-off hook is the oracle. Shapes: repeat at lead and tail, two repeat
// groups ("t1 t2 t3 t4 t1 t2"), gapped repeats, overlapping matches, and a
// term frequency past POSITIONS_BLOCK_SIZE so the drain crosses a position
// block boundary.
TEST_F(TermScorerTest, phraseRepeatedTermDedupMatchesPerOccurrenceEnums) {
  const int32_t N = 3 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  std::string big;
  for (int32_t k = 0; k < Postings::POSITIONS_BLOCK_SIZE + 40; k++) {
    big += "w x ";
  }
  for (int32_t doc = 0; doc < N; doc++) {
    switch (doc % 14) {
      case 0: f.add(doc, "a b a pad"); break;         // match at 0
      case 1: f.add(doc, "a b b a"); break;           // miss
      case 2: f.add(doc, "a a b pad"); break;         // lead-repeat match
      case 3: f.add(doc, "b a b a b a"); break;       // overlapping matches
      case 4: f.add(doc, "t1 t2 t3 t4 t1 t2"); break; // two repeat groups
      case 5: f.add(doc, "t1 t2 t3 t4 t1 pad"); break; // 6-gram miss
      case 6: f.add(doc, big); break;                 // tf(w) crosses a pos block
      case 7: f.add(doc, "a pad a pad"); break;       // gapped repeat match
      case 8: f.add(doc, "a pad pad a"); break;       // gapped repeat miss
      case 9: f.add(doc, "x w x pad"); break;
      // A failed candidate must not strand the lead repeat slot past a
      // middle "a" that starts the real match ("a x a x a" cases): the
      // per-slot cursors resume mid-buffer after the base re-kicks.
      case 10: f.add(doc, "a y a x a x a"); break;    // match at 2 after 0 fails
      case 11: f.add(doc, "a x a y a x a x a"); break; // match at 4 after 0,2 fail
      case 12: f.add(doc, "a y a x a"); break;        // all candidates fail
      default: f.add(doc, "pad a b pad"); break;
    }
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  struct Hit { int32_t doc; uint32_t scoreBits; };
  // Clause order matters for coverage: the docFreq sort usually leads with
  // the rarest term, which can keep the repeated term out of the lead slot;
  // text order (sorted=false) forces the repeated term to lead in the
  // "a x a x a" shapes. Every case runs both ways.
  auto run = [&](std::span<std::string_view> terms, std::span<const int32_t> positions,
                 bool dedup, bool sorted, int64_t* decodes = nullptr) {
    bool savedDedup = PhraseQuery::Scorer::disableRepeatDedupForTests;
    bool savedSort = PhraseQuery::Scorer::disableSortForTests;
    PhraseQuery::Scorer::disableRepeatDedupForTests = !dedup;
    PhraseQuery::Scorer::disableSortForTests = !sorted;
    SkipStatsGuard stats;
    PhraseQuery phrase("body_w", terms, positions);
    auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    std::vector<Hit> hits;
    if (scorer != nullptr) {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        hits.push_back({doc, std::bit_cast<uint32_t>(scorer->score())});
      }
    }
    if (decodes != nullptr) {
      *decodes = SkipStats::docBlocksDecoded + SkipStats::posBlocksDecoded;
    }
    PhraseQuery::Scorer::disableRepeatDedupForTests = savedDedup;
    PhraseQuery::Scorer::disableSortForTests = savedSort;
    return hits;
  };

  std::vector<std::string_view> aba = {"a", "b", "a"};
  std::vector<int32_t> pos012 = {0, 1, 2};
  std::vector<std::string_view> aab = {"a", "a", "b"};
  std::vector<std::string_view> sixGram = {"t1", "t2", "t3", "t4", "t1", "t2"};
  std::vector<int32_t> pos6 = {0, 1, 2, 3, 4, 5};
  std::vector<std::string_view> wxw = {"w", "x", "w"};
  std::vector<std::string_view> aGapA = {"a", "a"};
  std::vector<int32_t> pos02 = {0, 2};
  std::vector<std::string_view> axaxa = {"a", "x", "a", "x", "a"};
  std::vector<int32_t> pos5 = {0, 1, 2, 3, 4};

  struct Case { std::span<std::string_view> terms; std::span<const int32_t> pos; const char* label; };
  for (auto& c : std::initializer_list<Case>{{aba, pos012, "a b a"},
                                             {aab, pos012, "a a b"},
                                             {sixGram, pos6, "t1 t2 t3 t4 t1 t2"},
                                             {wxw, pos012, "w x w"},
                                             {aGapA, pos02, "a _ a"},
                                             {axaxa, pos5, "a x a x a"}}) {
    for (bool sorted : {true, false}) {
      auto expected = run(c.terms, c.pos, false, sorted);
      auto actual = run(c.terms, c.pos, true, sorted);
      ASSERT_FALSE(expected.empty()) << c.label << " sorted=" << sorted;
      ASSERT_EQ(expected.size(), actual.size()) << c.label << " sorted=" << sorted;
      for (size_t i = 0; i < expected.size(); i++) {
        EXPECT_EQ(expected[i].doc, actual[i].doc) << c.label << " sorted=" << sorted << " i=" << i;
        EXPECT_EQ(expected[i].scoreBits, actual[i].scoreBits)
            << c.label << " sorted=" << sorted << " i=" << i;
      }
    }
  }

  // Ground truth independent of the oracle: the 6-gram matches exactly the
  // doc % 14 == 4 docs, and "a x a x a" matches exactly the mid-start docs
  // (cases 10 and 11), not the all-candidates-fail case 12.
  auto sixHits = run(sixGram, pos6, true, true);
  ASSERT_FALSE(sixHits.empty());
  size_t expectSix = 0;
  for (int32_t d = 0; d < N; d++) {
    if (d % 14 == 4) expectSix++;
  }
  for (auto& h : sixHits) {
    EXPECT_EQ(4, h.doc % 14);
  }
  EXPECT_EQ(expectSix, sixHits.size());

  auto axaxaHits = run(axaxa, pos5, true, false);
  ASSERT_FALSE(axaxaHits.empty());
  size_t expectAxaxa = 0;
  for (int32_t d = 0; d < N; d++) {
    if (d % 14 == 10 || d % 14 == 11) expectAxaxa++;
  }
  for (auto& h : axaxaHits) {
    int32_t c = h.doc % 14;
    EXPECT_TRUE(c == 10 || c == 11) << "doc " << h.doc;
  }
  EXPECT_EQ(expectAxaxa, axaxaHits.size());

  // The shared enum decodes each duplicated term's blocks once: strictly
  // fewer doc+position block decodes than per-occurrence enums.
  int64_t dedupDecodes = 0, dupDecodes = 0;
  run(wxw, pos012, true, true, &dedupDecodes);
  run(wxw, pos012, false, true, &dupDecodes);
  EXPECT_LT(dedupDecodes, dupDecodes);
}

// Phrase frequency semantics: score() drains the full per-doc match count
// (doMatches stops at the first alignment), and alignment starts may
// overlap, matching Lucene's exact-phrase freq.
TEST_F(TermScorerTest, phraseScoreUsesFullOverlappingFrequency) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "a b pad pad pad pad");   // freq 1
  f.add(1, "a b a b pad pad");       // freq 2, same length as doc 0
  f.add(2, "a x a x a x");           // self-overlapping pattern host
  f.add(3, "a a a pad pad pad");     // "a a" freq 2 (overlapping, repeat term)
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  auto runFreqs = [&](std::span<std::string_view> terms, std::span<const int32_t> positions) {
    PhraseQuery phrase("body_w", terms, positions);
    auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    std::vector<std::pair<int32_t, float>> hits;  // doc -> score
    std::vector<int32_t> freqs;
    if (scorer != nullptr) {
      auto* phraseScorer = (PhraseQuery::Scorer*) scorer;
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        float s = scorer->score();
        hits.push_back({doc, s});
        freqs.push_back(phraseScorer->numMatches());
      }
    }
    return std::pair(hits, freqs);
  };

  std::vector<std::string_view> ab = {"a", "b"};
  std::vector<int32_t> pos01 = {0, 1};
  auto [abHits, abFreqs] = runFreqs(ab, pos01);
  ASSERT_EQ(2u, abHits.size());
  EXPECT_EQ(1, abFreqs[0]);
  EXPECT_EQ(2, abFreqs[1]);
  // Same length docs, freq 2 vs 1: BM25 must rank doc 1 higher.
  EXPECT_GT(abHits[1].second, abHits[0].second);

  std::vector<std::string_view> axax = {"a", "x", "a", "x"};
  std::vector<int32_t> pos0123 = {0, 1, 2, 3};
  auto [axHits, axFreqs] = runFreqs(axax, pos0123);
  ASSERT_EQ(1u, axHits.size());
  EXPECT_EQ(2, axHits[0].first);
  EXPECT_EQ(2, axFreqs[0]);  // bases 0 and 2 overlap

  std::vector<std::string_view> aa = {"a", "a"};
  auto [aaHits, aaFreqs] = runFreqs(aa, pos01);
  ASSERT_FALSE(aaHits.empty());
  bool sawDoc3 = false;
  for (size_t i = 0; i < aaHits.size(); i++) {
    if (aaHits[i].first == 3) {
      sawDoc3 = true;
      EXPECT_EQ(2, aaFreqs[i]);  // "a a a": bases 0 and 1, repeat term
    }
  }
  EXPECT_TRUE(sawDoc3);
}

// PhraseScorer's exact score contract includes these production float bits.
// Default/absent slop and explicit slop=0 must preserve IDF, frequency, norm,
// and score operation order.
TEST_F(TermScorerTest, phraseExactScoreBitGoldens) {
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  f.add(0, "a b pad pad pad pad");
  f.add(1, "a b a b pad pad");
  f.add(2, "a b a b a b");
  f.add(3, "a a a pad pad pad");
  f.add(4, "a pad b pad pad pad");
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  auto run = [&](std::span<std::string_view> terms, std::span<const int32_t> positions,
                 bool explicitZero = false) {
    auto collect = [&](PhraseQuery& phrase) {
      auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
      auto* scorer = weight->createScorer(testIndex.pool, segment);
      std::vector<std::pair<int32_t, uint32_t>> hits;
      if (scorer != nullptr) {
        for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
          hits.emplace_back(doc, std::bit_cast<uint32_t>(scorer->score()));
        }
      }
      return hits;
    };
    if (explicitZero) {
      PhraseQuery phrase("body_w", terms, positions, 0);
      return collect(phrase);
    }
    PhraseQuery phrase("body_w", terms, positions);
    return collect(phrase);
  };

  std::vector<std::string_view> ab = {"a", "b"};
  std::vector<int32_t> adjacent = {0, 1};
  std::vector<int32_t> gapped = {0, 2};
  std::vector<std::string_view> aa = {"a", "a"};

  const std::vector<std::pair<int32_t, uint32_t>> adjacentGolden = {
      {0, 1043228443}, {1, 1047514566}, {2, 1049167839}};
  const std::vector<std::pair<int32_t, uint32_t>> gappedGolden = {{4, 1043228443}};
  const std::vector<std::pair<int32_t, uint32_t>> repeatedGolden = {
      {3, 1038008262}};

  EXPECT_EQ(adjacentGolden, run(ab, adjacent));
  EXPECT_EQ(adjacentGolden, run(ab, adjacent, true));
  EXPECT_EQ(gappedGolden, run(ab, gapped));
  EXPECT_EQ(gappedGolden, run(ab, gapped, true));
  EXPECT_EQ(repeatedGolden, run(aa, adjacent));
  EXPECT_EQ(repeatedGolden, run(aa, adjacent, true));
}

// Fuzz the repeat-dedup scorer against the per-occurrence oracle: random
// docs over a tiny alphabet (repeats collide constantly), random phrases
// with repeated terms and gaps, both execution orders. Docs and score bits
// must agree exactly.
TEST_F(TermScorerTest, phraseRepeatedTermDedupFuzzMatchesOracle) {
  int32_t kDocs = (int32_t)scaleTestDimension(250, 2);
  int32_t kPhrases = (int32_t)scaleTestDimension(50, 2);
  static constexpr std::string_view kVocab[] = {"a", "b", "c", "d"};
  constexpr size_t kVocabSize = std::size(kVocab);

  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < kDocs; doc++) {
    int32_t len = (int32_t) rng.rint(3, 30);
    std::string body;
    for (int32_t k = 0; k < len; k++) {
      if (!body.empty()) body.push_back(' ');
      // skew: "a" twice as likely, so repeated-"a" phrases hit often
      size_t pick = (size_t) rng.rint(kVocabSize + 2);
      body.append(kVocab[pick >= kVocabSize ? 0 : pick]);
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  struct Hit { int32_t doc; uint32_t scoreBits; };
  auto run = [&](std::span<std::string_view> terms, std::span<const int32_t> positions,
                 bool dedup, bool sorted) {
    bool savedDedup = PhraseQuery::Scorer::disableRepeatDedupForTests;
    bool savedSort = PhraseQuery::Scorer::disableSortForTests;
    PhraseQuery::Scorer::disableRepeatDedupForTests = !dedup;
    PhraseQuery::Scorer::disableSortForTests = !sorted;
    PhraseQuery phrase("body_w", terms, positions);
    auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    std::vector<Hit> hits;
    if (scorer != nullptr) {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        hits.push_back({doc, std::bit_cast<uint32_t>(scorer->score())});
      }
    }
    PhraseQuery::Scorer::disableRepeatDedupForTests = savedDedup;
    PhraseQuery::Scorer::disableSortForTests = savedSort;
    return hits;
  };

  bool sawRepeatedTerm = false;
  bool sawGap = false;
  for (int32_t q = 0; q < kPhrases; q++) {
    int32_t len = (int32_t) rng.rint(2, 7);
    std::vector<std::string_view> terms;
    std::vector<int32_t> positions;
    int32_t pos = 0;
    std::string label;
    for (int32_t k = 0; k < len; k++) {
      size_t pick = (size_t) rng.rint(kVocabSize + 2);
      terms.push_back(kVocab[pick >= kVocabSize ? 0 : pick]);
      sawRepeatedTerm |=
          std::find(terms.begin(), terms.end() - 1, terms.back())
          != terms.end() - 1;
      positions.push_back(pos);
      label += terms.back();
      label += " ";
      bool gap = rng.rint(5) == 0;
      sawGap |= gap;
      pos += 1 + (int32_t)gap;  // ~20% gapped slot
    }
    for (bool sorted : {true, false}) {
      auto expected = run(terms, positions, false, sorted);
      auto actual = run(terms, positions, true, sorted);
      ASSERT_EQ(expected.size(), actual.size())
          << "phrase '" << label << "' sorted=" << sorted;
      for (size_t i = 0; i < expected.size(); i++) {
        ASSERT_EQ(expected[i].doc, actual[i].doc)
            << "phrase '" << label << "' sorted=" << sorted << " i=" << i;
        ASSERT_EQ(expected[i].scoreBits, actual[i].scoreBits)
            << "phrase '" << label << "' sorted=" << sorted << " i=" << i;
      }
    }
  }
  EXPECT_TRUE(sawRepeatedTerm);
  EXPECT_TRUE(sawGap);
}

// Block-max conjunction: the summed clause bounds let a scored "+a +b" skip
// doc-block ranges that cannot beat the threshold.  Two separated strong
// regions (blocks 0 and 8) force real range hops, weak long docs in between;
// pruned top-k must equal exhaustive exactly while visiting fewer docs.
TEST_F(TermScorerTest, blockMaxConjunctionTopKMatchesExhaustive) {
  const int32_t N = 14 * Postings::DOCS_BLOCK_SIZE + 23;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string text;
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    if (block == 0 || block == 8) {
      int32_t tf = (block == 0 ? 6 : 5) - (doc % 3);
      for (int32_t i = 0; i < tf; i++) text += "cja cjb ";
      for (int32_t i = 0; i < 4 + (doc % 5); i++) text += "pad ";
    } else {
      text = "cja cjb ";
      for (int32_t i = 0; i < 25 + (doc % 5); i++) text += "pad ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery a("body_w", "cja");
  TermQuery b("body_w", "cjb");
  std::vector<Query*> mand = {&a, &b};

  for (int32_t k : {1, 5}) {
    BooleanQuery exhaustiveQ(mand, {}, {}, {});
    BooleanQuery prunedQ(mand, {}, {}, {});
    auto* exhaustiveWeight = exhaustiveQ.createWeight(qContext, Query::NEED_SCORES);
    auto* prunedWeight = prunedQ.createWeight(
        qContext, Query::NEED_SCORES | Query::ALLOW_PRUNING);

    auto* exhaustiveScorer = exhaustiveWeight->createScorer(testIndex.pool, segment);
    ASSERT_NE(exhaustiveScorer, nullptr);
    TopDocsCollector exhaustiveCollector(k);
    for (int32_t d = exhaustiveScorer->next(); d != PostingsReader::END;
         d = exhaustiveScorer->next()) {
      exhaustiveCollector.collect(0, d, exhaustiveScorer->score());
    }
    ASSERT_EQ(exhaustiveCollector.totalHits(), N);

    auto* prunedScorer = dynamic_cast<BooleanQuery::ConjunctionScorer*>(
        prunedWeight->createScorer(testIndex.pool, segment));
    ASSERT_NE(prunedScorer, nullptr);
    TopDocsCollector prunedCollector(k);
    collectTopK(0, prunedScorer, nullptr, nullptr, prunedCollector);

    auto expected = sortedCollectorDocs(exhaustiveCollector);
    auto actual = sortedCollectorDocs(prunedCollector);
    ASSERT_EQ(actual.size(), expected.size()) << "k=" << k;
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(actual[i].doc, expected[i].doc) << "k=" << k << " i=" << i;
      EXPECT_EQ(std::bit_cast<uint32_t>(actual[i].score),
                std::bit_cast<uint32_t>(expected[i].score)) << "k=" << k << " i=" << i;
    }
    EXPECT_LT(prunedCollector.totalHits(), exhaustiveCollector.totalHits()) << "k=" << k;
    EXPECT_GT(prunedScorer->skippedRanges(), 0) << "k=" << k;
  }
}

TEST_F(TermScorerTest, conjunctionFailedEvalBackoffEngages) {
  SkipStatsGuard stats;
  const int32_t N = 70 * Postings::DOCS_BLOCK_SIZE + 7;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    f.add(doc, "backa backb");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery a("body_w", "backa");
  TermQuery b("body_w", "backb");
  std::vector<Query*> mand = {&a, &b};
  BooleanQuery backoffQ(mand, {}, {}, {});
  auto* backoffWeight = backoffQ.createWeight(
      qContext, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  auto* backoffScorer = dynamic_cast<BooleanQuery::ConjunctionScorer*>(
      backoffWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(backoffScorer, nullptr);
  backoffScorer->setMinCompetitiveScore(std::numeric_limits<float>::denorm_min());
  int32_t visited = 0;
  while (backoffScorer->next() != PostingsReader::END) visited++;
  EXPECT_EQ(visited, N);
  EXPECT_EQ(backoffScorer->skippedRanges(), 0);
  EXPECT_GT(SkipStats::conjEvalBackoffs, 0);
}

TEST_F(TermScorerTest, negatedTopKMatchesExhaustiveAtBothDepths) {
  const int32_t nDocs = 18 * Postings::DOCS_BLOCK_SIZE + 37;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addNegatedThetaDocs(field, nDocs);
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;

  TermQuery mand("body_w", "mand");
  TermQuery excludeOne("body_w", "excludeone");
  TermQuery excludeTwo("body_w", "excludetwo");
  std::vector<Query*> mandatory = {&mand};
  std::vector<Query*> prohibitedOne = {&excludeOne};
  std::vector<Query*> prohibitedTwo = {&excludeOne, &excludeTwo};
  BooleanQuery oneExclusion(mandatory, {}, prohibitedOne, {});
  BooleanQuery twoExclusions(mandatory, {}, prohibitedTwo, {});

  for (auto* query : std::array<Query*, 2>{&oneExclusion, &twoExclusions}) {
    for (int32_t topK : {10, 100}) {
      SCOPED_TRACE(::testing::Message() << "topK=" << topK);
      auto expected = runQueryTopK(*reader, *query, topK, false);
      auto actual = runQueryTopK(*reader, *query, topK, true);
      assertQueryTopKExact(expected, actual);
      EXPECT_LT(actual.visited, expected.visited);
    }
  }
}

TEST_F(TermScorerTest, negatedBoundsComposeThroughNestedScorers) {
  const int32_t nDocs = 16 * Postings::DOCS_BLOCK_SIZE + 29;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addNegatedThetaDocs(field, nDocs);
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;

  TermQuery mand("body_w", "mand");
  TermQuery join("body_w", "join");
  TermQuery conjoin("body_w", "conjoin");
  TermQuery bonus("body_w", "bonus");
  TermQuery excludeOne("body_w", "excludeone");
  std::vector<Query*> conjunctionMand = {&mand, &conjoin};
  std::vector<Query*> prohibited = {&excludeOne};
  BooleanQuery negatedConjunction(conjunctionMand, {}, prohibited, {});
  std::vector<Query*> outerMand = {&negatedConjunction, &join};
  BooleanQuery nestedConjunction(outerMand, {}, {}, {});

  std::vector<std::string_view> phraseTerms = {"quick", "fox"};
  std::vector<int32_t> phrasePositions = {0, 1};
  PhraseQuery phrase("body_w", phraseTerms, phrasePositions);
  std::vector<Query*> phraseMand = {&phrase};
  BooleanQuery negatedPhrase(phraseMand, {}, prohibited, {});

  std::vector<Query*> mandOptMand = {&mand};
  std::vector<Query*> mandOptOptional = {&bonus};
  BooleanQuery mandOpt(mandOptMand, mandOptOptional, {}, {});
  std::vector<Query*> wrappedMandOpt = {&mandOpt};
  BooleanQuery negatedMandOpt(wrappedMandOpt, {}, prohibited, {});

  for (auto* query : std::array<Query*, 3>{
           &nestedConjunction, &negatedPhrase, &negatedMandOpt}) {
    for (int32_t topK : {10, 100}) {
      auto expected = runQueryTopK(*reader, *query, topK, false);
      auto actual = runQueryTopK(*reader, *query, topK, true);
      assertQueryTopKExact(expected, actual);
    }
  }
}

TEST_F(TermScorerTest, negatedOptionalChildComposesWithWindowedAndGlobalMaxScore) {
  const int32_t nDocs = 18 * Postings::DOCS_BLOCK_SIZE + 41;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addNegatedThetaDocs(field, nDocs);
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;

  TermQuery mand("body_w", "mand");
  TermQuery excludeOne("body_w", "excludeone");
  TermQuery bonus("body_w", "bonus");
  TermQuery rareBonus("body_w", "rarebonus");
  std::vector<Query*> mandatory = {&mand};
  std::vector<Query*> prohibited = {&excludeOne};
  BooleanQuery negated(mandatory, {}, prohibited, {});
  std::array<Query*, 3> optional = {&negated, &bonus, &rareBonus};

  SkipStatsGuard stats;
  for (int32_t topK : {10, 100}) {
    auto expected = runComposedMaxScoreTopK(
        *reader, optional, topK, std::numeric_limits<int32_t>::max(), false);
    auto global = runComposedMaxScoreTopK(
        *reader, optional, topK, std::numeric_limits<int32_t>::max(), true);
    auto windowed = runComposedMaxScoreTopK(*reader, optional, topK, 256, true);
    assertTopKEquivalent(expected.topDocs, global.topDocs);
    assertTopKEquivalent(expected.topDocs, windowed.topDocs);
  }

  int32_t ahead = 2 * Postings::DOCS_BLOCK_SIZE + 7;
  auto positionedExpected = runComposedMaxScoreTopK(
      *reader, optional, 10, std::numeric_limits<int32_t>::max(), false, ahead);
  auto positionedWindowed = runComposedMaxScoreTopK(
      *reader, optional, 10, 256, true, ahead);
  assertTopKEquivalent(positionedExpected.topDocs, positionedWindowed.topDocs);
  EXPECT_EQ(SkipStats::maxScoreSetupFallbackBlockBounds, 0);
}

TEST_F(TermScorerTest, termCompetitiveCertificatesTrackThetaAndBlockTransitions) {
  const int32_t nDocs = DocsEnumMeta::L1_DOCS + 2 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addNegatedThetaDocs(field, nDocs);
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;

  {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = context.topReader.segments()[0];
    TermQuery query("body_w", "mand");
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto* scorer = dynamic_cast<TermQuery::Scorer*>(weight->createScorer(pool, segment));
    ASSERT_NE(scorer, nullptr);
    ASSERT_TRUE(scorer->hasImpacts());

    SkipStatsGuard stats;
    float weakTheta = std::numeric_limits<float>::denorm_min();
    scorer->setMinCompetitiveScore(weakTheta);
    EXPECT_EQ(SkipStats::impactCertificateInvalidations, 1);
    int32_t doc = scorer->next();
    ASSERT_NE(doc, PostingsReader::END);
    EXPECT_EQ(SkipStats::impactCompetitiveColdLookups, 1);
    int32_t firstLastDoc = scorer->competitiveUpTo;
    float firstBound = scorer->competitiveBound;
    ASSERT_GT(firstBound, weakTheta);

    float survivedTheta = std::nextafter(weakTheta, std::numeric_limits<float>::infinity());
    scorer->setMinCompetitiveScore(survivedTheta);
    scorer->setMinCompetitiveScore(
        std::nextafter(survivedTheta, std::numeric_limits<float>::infinity()));
    EXPECT_EQ(SkipStats::impactCertificateInvalidations, 1);
    EXPECT_EQ(SkipStats::impactCertificateSurvivedRises, 2);
    while (doc < firstLastDoc) {
      doc = scorer->next();
    }
    ASSERT_EQ(doc, firstLastDoc);
    EXPECT_EQ(SkipStats::impactCompetitiveColdLookups, 1);
    ASSERT_NE(scorer->next(), PostingsReader::END);
    EXPECT_EQ(SkipStats::impactCompetitiveColdLookups, 2);

    float breakingTheta = std::nextafter(scorer->competitiveBound,
                                         std::numeric_limits<float>::infinity());
    scorer->setMinCompetitiveScore(breakingTheta);
    EXPECT_EQ(SkipStats::impactCertificateInvalidations, 2);
    scorer->next();
    EXPECT_GE(SkipStats::impactCompetitiveColdLookups, 3);

    SkipStats::reset();
    Query::Context weakContext(pool, *reader);
    auto* weakWeight = query.createWeight(weakContext, Query::NEED_SCORES);
    auto* weakScorer = dynamic_cast<TermQuery::Scorer*>(
        weakWeight->createScorer(pool, weakContext.topReader.segments()[0]));
    ASSERT_NE(weakScorer, nullptr);
    weakScorer->setMinCompetitiveScore(weakTheta);
    int32_t visited = 0;
    while (weakScorer->next() != PostingsReader::END) visited++;
    EXPECT_EQ(visited, nDocs);
    EXPECT_EQ(weakScorer->skippedBlocks(), 0);
    EXPECT_EQ(SkipStats::impactCompetitiveColdLookups,
              (int64_t) weakScorer->impacts.blockCount());

    int32_t lastImpactDoc = weakScorer->impacts.groupLastDoc(
        weakScorer->impacts.numGroups() - 1);
    EXPECT_EQ(weakScorer->skipNonCompetitiveBlocks(lastImpactDoc + 1),
              lastImpactDoc + 1);
    EXPECT_EQ(weakScorer->competitiveUpTo, PostingsReader::END);
    EXPECT_TRUE(std::isinf(weakScorer->competitiveBound));

    SkipStats::reset();
    Query::Context noScoreContext(pool, *reader);
    auto* noScoreWeight = query.createWeight(noScoreContext, 0);
    auto* noImpactScorer = dynamic_cast<TermQuery::Scorer*>(
        noScoreWeight->createScorer(pool, noScoreContext.topReader.segments()[0]));
    ASSERT_NE(noImpactScorer, nullptr);
    ASSERT_FALSE(noImpactScorer->hasImpacts());
    noImpactScorer->setMinCompetitiveScore(1.0f);
    while (noImpactScorer->next() != PostingsReader::END) {}
    EXPECT_EQ(SkipStats::impactCompetitiveColdLookups, 0);
    EXPECT_EQ(noImpactScorer->competitiveUpTo, PostingsReader::END);
    EXPECT_TRUE(std::isinf(noImpactScorer->competitiveBound));
  }

  {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = context.topReader.segments()[0];
    TermQuery query("body_w", "mand");
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = supplier->bulkScorer(pool);
    ASSERT_NE(bulk, nullptr);

    SkipStatsGuard stats;
    const float theta = std::numeric_limits<float>::denorm_min();
    int64_t visited = 0;
    int32_t cursor = 0;
    while (cursor != PostingsReader::END) {
      ScoreWindow window;
      int32_t next =
          bulk->scoreNextWindow(window, nullptr, cursor, segment.maxDoc(), theta);
      visited += window.size;
      if (next == PostingsReader::END) {
        break;
      }
      ASSERT_GT(next, cursor);
      cursor = next;
    }

    EXPECT_EQ(visited, nDocs);
    const int64_t impactBlocks =
        (nDocs + Postings::DOCS_BLOCK_SIZE - 1) / Postings::DOCS_BLOCK_SIZE;
    EXPECT_EQ(SkipStats::impactCompetitiveColdLookups, impactBlocks);
  }
}

// The bulk conjunction path must produce the same top-k as the pull
// ConjunctionScorer (tie-group equivalent), and with pruning disabled
// (exact-count mode pins the threshold at lowest) it must emit every match.
TEST_F(TermScorerTest, conjunctionBulkScorerMatchesPull) {
  const int32_t N = 14 * Postings::DOCS_BLOCK_SIZE + 23;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  int32_t bothCount = 0;
  for (int32_t doc = 0; doc < N; doc++) {
    std::string text;
    int32_t block = doc / Postings::DOCS_BLOCK_SIZE;
    bool hasA = (doc % 2) == 0;
    bool hasB = (doc % 3) != 1;
    if (block == 0 || block == 8) {
      int32_t tf = (block == 0 ? 6 : 5) - (doc % 3);
      for (int32_t i = 0; i < tf; i++) {
        if (hasA) text += "bca ";
        if (hasB) text += "bcb ";
      }
      for (int32_t i = 0; i < 4 + (doc % 5); i++) text += "pad ";
    } else {
      if (hasA) text += "bca ";
      if (hasB) text += "bcb ";
      for (int32_t i = 0; i < 25 + (doc % 5); i++) text += "pad ";
    }
    if (hasA && hasB) bothCount++;
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery a("body_w", "bca");
  TermQuery b("body_w", "bcb");
  std::vector<Query*> mand = {&a, &b};

  for (int32_t k : {1, 5, 100}) {
    BooleanQuery pullQ(mand, {}, {}, {});
    BooleanQuery bulkQ(mand, {}, {}, {});
    auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
    auto* bulkWeight = bulkQ.createWeight(qContext, Query::NEED_SCORES);

    auto* pullScorer = pullWeight->createScorer(testIndex.pool, segment);
    ASSERT_NE(pullScorer, nullptr);
    TopDocsCollector pullCollector(k);
    for (int32_t d = pullScorer->next(); d != PostingsReader::END; d = pullScorer->next()) {
      pullCollector.collect(0, d, pullScorer->score());
    }
    ASSERT_EQ(pullCollector.totalHits(), bothCount);

    auto* supplier = bulkWeight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = supplier->bulkScorer(testIndex.pool);
    ASSERT_NE(bulk, nullptr) << "pure scored conjunction should get the bulk path";
    TopDocsCollector bulkCollector(k);
    collectTopKWindowed(0, bulk, nullptr, bulkCollector, nullptr, segment.maxDoc());

    auto expected = sortedCollectorDocs(pullCollector);
    auto actual = sortedCollectorDocs(bulkCollector);
    assertTopKEquivalent(expected, actual);
  }

  // Exhaustive mode (theta pinned): the bulk path must visit and emit every match.
  BooleanQuery exactQ(mand, {}, {}, {});
  auto* exactWeight = exactQ.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = exactWeight->scorerSupplier(testIndex.pool, segment);
  auto* bulk = supplier->bulkScorer(testIndex.pool);
  ASSERT_NE(bulk, nullptr);
  TopDocsCollector exactCollector(10);
  collectTopKWindowed(0, bulk, nullptr, exactCollector, nullptr, segment.maxDoc(),
                      /*allowPruning=*/false);
  EXPECT_EQ(exactCollector.totalHits(), bothCount);

  BooleanQuery countQ(mand, {}, {}, {});
  auto* countWeight = countQ.createWeight(qContext, Query::NEED_SCORES);
  auto* countSupplier = countWeight->scorerSupplier(testIndex.pool, segment);
  auto* countBulk = countSupplier->bulkScorer(testIndex.pool);
  ASSERT_NE(countBulk, nullptr);
  int64_t counted = 0;
  for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
    int32_t next = countBulk->countNextWindow(counted, nullptr, nullptr, cursor, segment.maxDoc());
    if (next == PostingsReader::END) {
      break;
    }
    ASSERT_GT(next, cursor);
    cursor = next;
  }
  EXPECT_EQ(counted, bothCount);
}

TEST_F(TermScorerTest, countBulkFillMatchesPullOnRandomFullAndTailBlocks) {
  const int32_t N = 5 * Postings::DOCS_BLOCK_SIZE + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  uint32_t state = 0x5eed1234u;
  auto nextRand = [&]() {
    state = state * 1664525u + 1013904223u;
    return state;
  };
  for (int32_t doc = 0; doc < N; doc++) {
    uint32_t r = nextRand();
    bool a = (r & 0x3u) != 0;
    bool b = ((r >> 4) & 0x7u) < 3;
    bool c = ((r >> 9) & 0xfu) == 0;
    addBulkFillTermDoc(f, doc, a, b, c);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 3> terms = {"bf_a", "bf_b", "bf_c"};

  int64_t pull = countPullTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  int64_t bulk = countBulkTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  EXPECT_EQ(bulk, pull);
}

TEST_F(TermScorerTest, countBulkFillBitsetFilterMatchesPull) {
  const int32_t N = 4 * Postings::DOCS_BLOCK_SIZE + 19;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    bool a = (doc % 2) == 0;
    bool b = (doc % 5) < 2;
    bool c = (doc % 17) == 3;
    addBulkFillTermDoc(f, doc, a, b, c);
  }
  testIndex.flush();
  f.startReading();

  RAMBitDocSet filter(N);
  for (int32_t doc = 0; doc < N; doc++) {
    if ((doc % 3) != 1) {
      filter.mutableBits().set(doc);
    }
  }

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 3> terms = {"bf_a", "bf_b", "bf_c"};

  BulkDomainDriveGuard guard(true);
  int64_t pull = countPullTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, &filter);
  int64_t bulk = countBulkTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, &filter);
  EXPECT_EQ(bulk, pull);
}

TEST_F(TermScorerTest, countBulkFillContiguousDenseBlocks) {
  const int32_t N = 3 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    appendRepeatedTerm(body, "densefill", 1 + (doc % 3));
    if (doc % 50 == 0) appendRepeatedTerm(body, "denseother", 1);
    appendRepeatedTerm(body, "filler", 4);
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 2> terms = {"densefill", "denseother"};

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  int64_t bulk = countBulkTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  EXPECT_EQ(bulk, N);
  EXPECT_GT(SkipStats::countBulkFillContiguousBlocks, 0);
  EXPECT_GT(SkipStats::docsOnlyFreqBlocksSkipped, 0);
  SkipStats::enabled = savedStats;
}

// Dense-but-not-contiguous postings store as bitset words; intoBitSet must OR
// whole word blocks straight from the stream (no decode) and agree with the
// indexed doc list across window boundaries that straddle blocks.
TEST_F(TermScorerTest, intoBitSetWordBlocksMatchIteration) {
  const int32_t N = 10000;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  uint32_t state = 0xb17b17u;
  auto nextRand = [&]() {
    state = state * 1664525u + 1013904223u;
    return state;
  };
  std::vector<int32_t> expected;
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    if ((nextRand() & 0x3u) != 3) {  // ~3/4 density -> unary word blocks
      body += "wordy ";
      expected.push_back(doc);
    }
    body += "filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  auto& segment = testIndex.reader->segments()[0];
  PostingsReader& postingsReader = segment.postingsReader();
  FieldReader fieldReader(postingsReader);
  ASSERT_TRUE(fieldReader.seek("body_w"));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(testIndex.pool, postingsReader, fieldInfo);
  ASSERT_TRUE(tenum.seek("wordy"));

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();

  // Contiguous windows with boundaries that land inside word blocks (4097 is
  // deliberately off the 128-doc grid).
  DocsOnlyEnum denum(tenum);
  const int32_t bounds[] = {0, 1000, 4097, 6000, N + 100};
  std::vector<int32_t> got;
  for (size_t w = 0; w + 1 < std::size(bounds); w++) {
    const int32_t from = bounds[w], to = bounds[w + 1];
    std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
    denum.intoBitSet(bits, from, to);
    for (int32_t i = 0; i < to - from; i++) {
      if (bits[(size_t) i >> 6] & (1ULL << (i & 63))) {
        got.push_back(from + i);
      }
    }
  }
  EXPECT_EQ(got, expected);
  EXPECT_GT(SkipStats::countBulkFillWordBlocks, 0);
  SkipStats::enabled = savedStats;

  // A window opening past the enum position: leading docs are consumed
  // unrecorded and the first word block is clipped.
  DocsOnlyEnum denum2(tenum);
  const int32_t from = 50, to = 700;
  std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
  denum2.intoBitSet(bits, from, to);
  std::vector<int32_t> got2, want2;
  for (int32_t i = 0; i < to - from; i++) {
    if (bits[(size_t) i >> 6] & (1ULL << (i & 63))) {
      got2.push_back(from + i);
    }
  }
  for (int32_t d : expected) {
    if (d >= from && d < to) {
      want2.push_back(d);
    }
  }
  EXPECT_EQ(got2, want2);
}

TEST_F(TermScorerTest, docsTierAdvanceCoversBlockEncodingsAndSkips) {
  const int32_t N = 3 * DocsEnumMeta::L1_DOCS + 257;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  uint32_t state = 0x9e3779b9u;
  auto nextRand = [&]() {
    state = state * 1664525u + 1013904223u;
    return state;
  };
  std::vector<int32_t> contigExpected;
  std::vector<int32_t> wordExpected;
  std::vector<int32_t> packedExpected;
  contigExpected.reserve((size_t) N);
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "ado_contig";
    contigExpected.push_back(doc);
    if ((nextRand() & 0x3u) != 0) {
      body += " ado_word";
      wordExpected.push_back(doc);
    }
    if ((doc % 10) == 0) {
      body += " ado_packed";
      packedExpected.push_back(doc);
    }
    body += " filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  auto& segment = testIndex.reader->segments()[0];
  PostingsReader& postingsReader = segment.postingsReader();
  FieldReader fieldReader(postingsReader);
  ASSERT_TRUE(fieldReader.seek("body_w"));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);

  auto checkAdvance = [&](std::string_view term, const std::vector<int32_t>& expected,
                          std::initializer_list<int32_t> targets) {
    TermsEnum tenum(testIndex.pool, postingsReader, fieldInfo);
    ASSERT_TRUE(tenum.seek(term));
    DocsOnlyEnum denum(tenum);
    int32_t last = -1;
    for (int32_t target : targets) {
      ASSERT_GT(target, last) << term;
      int32_t got = denum.advance(target);
      auto it = std::lower_bound(expected.begin(), expected.end(), target);
      int32_t want = it == expected.end() ? PostingsReader::END : *it;
      EXPECT_EQ(got, want) << term << " target=" << target;
      last = got;
      if (got == PostingsReader::END) {
        break;
      }
    }
  };

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  checkAdvance("ado_contig", contigExpected, {0, 64, 127, 128, 4096, 9000, N + 1});
  checkAdvance("ado_word", wordExpected, {0, 64, 127, 128, 4096, 9000, N + 1});
  checkAdvance("ado_packed", packedExpected, {0, 9, 129, 4096, 9000, N + 1});

  TermsEnum tenum(testIndex.pool, postingsReader, fieldInfo);
  ASSERT_TRUE(tenum.seek("ado_word"));
  DocsOnlyEnum denum(tenum);
  auto firstWindowDoc = std::lower_bound(wordExpected.begin(), wordExpected.end(), 4096);
  ASSERT_NE(firstWindowDoc, wordExpected.end());
  int32_t from = *firstWindowDoc;
  int32_t to = std::min(from + 777, N);
  ASSERT_EQ(denum.advance(from), from);
  std::vector<uint64_t> bits((size_t) (to - from + 63) / 64, 0);
  denum.intoBitSet(bits, from, to);
  std::vector<int32_t> got;
  for (int32_t i = 0; i < to - from; i++) {
    if (bits[(size_t) i >> 6] & (1ULL << (i & 63))) {
      got.push_back(from + i);
    }
  }
  std::vector<int32_t> want;
  for (int32_t doc : wordExpected) {
    if (doc >= from && doc < to) {
      want.push_back(doc);
    }
  }
  EXPECT_EQ(got, want);
  EXPECT_GT(SkipStats::docsOnlyFreqBlocksSkipped, 0);
  SkipStats::enabled = savedStats;
}

TEST_F(TermScorerTest, conjunctionDenseCountMatchesPullWithFilters) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 321;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    if ((doc % 17) != 5) appendRepeatedTerm(body, "dca", 1 + (doc % 3));
    if ((doc % 19) != 7) appendRepeatedTerm(body, "dcb", 1 + (doc % 5));
    if ((doc % 23) != 11) appendRepeatedTerm(body, "dcc", 1 + (doc % 2));
    appendRepeatedTerm(body, "filler", 3);
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 2> terms = {"dca", "dcb"};
  auto bitsetFilter = makeEveryNthDocSet(N, 3, false);
  auto arrayFilter = makeEveryNthDocSet(N, 5, true);

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  int64_t pull = countPullTermConjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  int64_t bulk = countBulkTermConjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  EXPECT_EQ(bulk, pull);
  EXPECT_GT(SkipStats::conjDenseCountWindows, 0);
  EXPECT_GT(SkipStats::countBulkFillWordBlocks, 0);

  DocSetBuilder countedDocs(N);
  int64_t firstWindowCount = 0;
  int32_t countNext;
  {
    auto queries = makeTermQueries(terms);
    auto mandatory = queryPointers(queries);
    BooleanQuery query(mandatory, {}, {}, {});
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* countScorer = supplier->bulkScorer(testIndex.pool);
    ASSERT_NE(countScorer, nullptr);
    countNext = countScorer->countNextWindow(
        firstWindowCount, &countedDocs, nullptr, 0, segment.maxDoc());
  }
  {
    auto queries = makeTermQueries(terms);
    auto mandatory = queryPointers(queries);
    BooleanQuery query(mandatory, {}, {}, {});
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* matchScorer = supplier->bulkScorer(testIndex.pool);
    ASSERT_NE(matchScorer, nullptr);
    ASSERT_TRUE(matchScorer->supportsMatchWindows());
    ScoreWindow matches;
    int32_t matchNext = matchScorer->matchNextWindow(
        matches, nullptr, 0, segment.maxDoc());
    EXPECT_EQ(matchNext, countNext);
    ASSERT_EQ(matches.size, firstWindowCount);
    auto domain = countedDocs.build();
    ASSERT_EQ(domain->card(), firstWindowCount);
    for (int32_t i = 0; i < matches.size; i++) {
      EXPECT_TRUE(domain->get(matches.docs[(size_t) i]));
    }
  }

  pull = countPullTermConjunctionSegment(testIndex.pool, qContext, segment, terms, bitsetFilter.get());
  bulk = countBulkTermConjunctionSegment(testIndex.pool, qContext, segment, terms, bitsetFilter.get());
  EXPECT_EQ(bulk, pull);

  pull = countPullTermConjunctionSegment(testIndex.pool, qContext, segment, terms, arrayFilter.get());
  bulk = countBulkTermConjunctionSegment(testIndex.pool, qContext, segment, terms, arrayFilter.get());
  EXPECT_EQ(bulk, pull);
  SkipStats::enabled = savedStats;
}

TEST_F(TermScorerTest, conjunctionDenseCountThreeClauseLeapfrogMatchesPull) {
  const int32_t N = 3 * DocsEnumMeta::L1_DOCS + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    if ((doc % 8) == 0) appendRepeatedTerm(body, "lfa", 1 + (doc % 3));
    if ((doc % 9) == 0) appendRepeatedTerm(body, "lfb", 1 + (doc % 4));
    if ((doc % 7) == 0) appendRepeatedTerm(body, "lfc", 1 + (doc % 2));
    appendRepeatedTerm(body, "filler", 2);
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 3> terms = {"lfa", "lfb", "lfc"};

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  int64_t pull = countPullTermConjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  int64_t bulk = countBulkTermConjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  EXPECT_EQ(bulk, pull);
  EXPECT_GT(SkipStats::conjDenseCountWindows, 0);
  EXPECT_LT(SkipStats::countBulkFillCalls,
            SkipStats::conjDenseCountWindows * (int64_t) terms.size());
  SkipStats::enabled = savedStats;
}

TEST_F(TermScorerTest,
       exactFilteredTermCountHonorsDomainAndPartialMaxResume) {
  const int32_t N = 5 * DocsEnumMeta::L1_DOCS + 83;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  std::vector<int32_t> expected;
  std::vector<int32_t> expectedAttached;
  for (int32_t doc = 0; doc < N; doc++) {
    bool requiredLead = (doc % 8) == 0;
    bool overlap = (doc % 256) == 0;
    bool independentTail = (doc % 8) != 0 && (doc % 6) == 0;
    std::string body = "candidate_dense";
    if (requiredLead) body += " candidate_lead";
    if (independentTail || overlap) body += " candidate_filter";
    if ((doc % 7) == 0) body += " attached_filter";
    f.add(doc, body);
    if (overlap && (doc % 5) == 0) {
      expected.push_back(doc);
    }
    if (overlap && (doc % 7) == 0) {
      expectedAttached.push_back(doc);
    }
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery lead("body_w", "candidate_lead");
  TermQuery dense("body_w", "candidate_dense");
  TermQuery filterTerm("body_w", "candidate_filter");
  std::array<Query*, 2> mandatory{&lead, &dense};
  std::array<Query*, 1> filters{&filterTerm};
  BooleanQuery query(mandatory, {}, {}, filters);
  SkipStatsGuard stats;
  auto* weight = query.createWeight(qContext, 0);
  auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(nullptr, supplier);
  auto* bulk = supplier->bulkScorer(testIndex.pool);
  ASSERT_NE(nullptr, bulk);

  auto domain = makeEveryNthDocSet(N, 5, true);
  DocSetBuilder matchedDocs(N);
  int64_t count = 0;
  int32_t cursor = 0;
  auto countTo = [&](int32_t max) {
    while (cursor != PostingsReader::END && cursor < max) {
      int32_t next = bulk->countNextWindow(
          count, &matchedDocs, domain.get(), cursor, max);
      if (next == PostingsReader::END) {
        cursor = next;
        break;
      }
      ASSERT_GT(next, cursor);
      cursor = next;
    }
  };

  countTo(2 * DocsEnumMeta::L1_DOCS + 181);
  ASSERT_NE(PostingsReader::END, cursor);
  countTo(N);
  EXPECT_EQ((int64_t) expected.size(), count);
  auto actual = matchedDocs.build();
  ASSERT_EQ((int32_t) expected.size(), actual->card());
  for (int32_t doc : expected) {
    EXPECT_TRUE(actual->get(doc)) << doc;
  }
  EXPECT_GT(SkipStats::exactTermCountBatches, 1);
  EXPECT_EQ(1, SkipStats::exactTermCountEngagements);
  EXPECT_EQ(0, SkipStats::filteredConjBatchCountWindows);
  EXPECT_EQ(0, SkipStats::filteredCountCandidateAdmits);
  EXPECT_EQ(0, SkipStats::filteredCountCandidateDenseLatchBacks);
  EXPECT_EQ(0, SkipStats::conjDenseCountWindows);
  EXPECT_EQ(0, SkipStats::tfreqBlocksDecoded);

  // An empty prefix must preserve the first unconsumed lead posting so the
  // next nonempty range resumes without skipping it.
  SkipStats::reset();
  auto* prefixWeight = query.createWeight(qContext, 0);
  auto* prefixSupplier = prefixWeight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(nullptr, prefixSupplier);
  auto* prefixBulk = prefixSupplier->bulkScorer(testIndex.pool);
  ASSERT_NE(nullptr, prefixBulk);
  int64_t prefixCount = 0;
  int32_t prefixNext = prefixBulk->countNextWindow(
      prefixCount, nullptr, nullptr, 1, 7);
  EXPECT_EQ(8, prefixNext);
  EXPECT_EQ(0, prefixCount);
  EXPECT_EQ(0, SkipStats::filteredCountCandidateAdmits);
  EXPECT_EQ(0, SkipStats::filteredCountCandidateDenseLatchBacks);
  while (prefixNext != PostingsReader::END && prefixNext < N) {
    int32_t next = prefixBulk->countNextWindow(
        prefixCount, nullptr, nullptr, prefixNext, N);
    if (next == PostingsReader::END) {
      break;
    }
    ASSERT_GT(next, prefixNext);
    prefixNext = next;
  }
  EXPECT_EQ((N - 1) / 256, prefixCount);
  EXPECT_EQ(0, SkipStats::filteredCountCandidateAdmits);
  EXPECT_EQ(1, SkipStats::exactTermCountEngagements);
  EXPECT_GT(SkipStats::exactTermCountBatches, 1);

  // A full lead batch reports only the interval it exhausted, not the caller's
  // larger max. The next call must resume at that exact boundary.
  auto* matchWeight = query.createWeight(qContext, 0);
  auto* matchSupplier = matchWeight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(nullptr, matchSupplier);
  auto* matchBulk = matchSupplier->bulkScorer(testIndex.pool);
  ASSERT_NE(nullptr, matchBulk);
  ScoreWindow matches;
  int32_t matchNext = matchBulk->matchNextWindow(
      matches, nullptr, 0, N);
  EXPECT_EQ(1024 * 8, matchNext);
  EXPECT_EQ(matchNext, matches.max);
  EXPECT_EQ(0, matches.min);
  for (int32_t i = 0; i < matches.size; i++) {
    EXPECT_LT(matches.docs[(size_t) i], matches.max);
  }

  // WindowFilter is a separate BulkScorer contract from the call-time
  // DocSet domain. Attaching one before iteration must select the dense arm,
  // which prepares and intersects the filter for every production window.
  SkipStats::reset();
  auto* attachedWeight = query.createWeight(qContext, 0);
  auto* attachedSupplier = attachedWeight->scorerSupplier(
      testIndex.pool, segment);
  ASSERT_NE(nullptr, attachedSupplier);
  BulkScorer* attachedBulk;
  {
    ExactTermCountGuard exactGuard(true);
    attachedBulk = attachedSupplier->bulkScorer(testIndex.pool);
  }
  ASSERT_NE(nullptr, attachedBulk);
  TermQuery attachedTerm("body_w", "attached_filter");
  auto* windowWeight = attachedTerm.createWeight(qContext, 0);
  auto* windowScorer = windowWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(nullptr, windowScorer);
  std::array<Query::Scorer*, 1> windowScorers{windowScorer};
  WindowFilter windowFilter(
      testIndex.pool, windowScorers, false, N / 7);
  ASSERT_TRUE(attachedBulk->attachWindowFilter(&windowFilter));
  EXPECT_TRUE(attachedBulk->willCountDense());

  int64_t attachedCount = 0;
  int32_t attachedCursor = 0;
  while (attachedCursor != PostingsReader::END && attachedCursor < N) {
    int32_t next = attachedBulk->countNextWindow(
        attachedCount, nullptr, nullptr, attachedCursor, N);
    if (next == PostingsReader::END) {
      break;
    }
    ASSERT_GT(next, attachedCursor);
    attachedCursor = next;
  }
  EXPECT_EQ((int64_t) expectedAttached.size(), attachedCount);
  EXPECT_EQ(0, SkipStats::filteredConjBatchCountWindows);
  EXPECT_GT(SkipStats::conjDenseCountWindows, 0);
  EXPECT_EQ(0, SkipStats::tfreqBlocksDecoded);
}

TEST_F(TermScorerTest, conjunctionDenseScoredLatchBackResumesExactScoring) {
  DenseScoredGuard denseScoredGuard(false);
  SkipStatsGuard stats;

  const int32_t N = 10 * DocsEnumMeta::L1_DOCS + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    f.add(doc, "latch_a latch_b filler");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 2> terms = {"latch_a", "latch_b"};
  auto queries = makeTermQueries(terms);
  auto mandatory = queryPointers(queries);
  std::span<Query*> empty;
  constexpr int32_t topK = 100;

  BooleanQuery pullQuery(
      std::span<Query*>(mandatory.data(), mandatory.size()),
      empty, empty, empty);
  auto* pullWeight = pullQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* pullScorer = pullWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(pullScorer, nullptr);
  TopDocsCollector pullCollector(topK);
  collectTopK(0, pullScorer, nullptr, nullptr, pullCollector,
              /*allowPruning=*/false);

  BooleanQuery bulkQuery(
      std::span<Query*>(mandatory.data(), mandatory.size()),
      empty, empty, empty);
  auto* bulkWeight = bulkQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = bulkWeight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = dynamic_cast<BooleanQuery::ConjunctionBulkScorer*>(
      supplier->bulkScorer(testIndex.pool));
  ASSERT_NE(bulk, nullptr);
  TopDocsCollector bulkCollector(topK);
  collectTopKWindowed(0, bulk, nullptr, bulkCollector, nullptr,
                      segment.maxDoc(), /*allowPruning=*/false);

  EXPECT_EQ(bulk->denseScoredSampleWindowsForTests(), 8);
  EXPECT_FALSE(bulk->denseScoredCostRejectedForTests());
  EXPECT_TRUE(bulk->denseScoredLatchedBackForTests());
  EXPECT_EQ(SkipStats::conjDenseScoredWindows, 8);
  EXPECT_EQ(SkipStats::conjDenseScoredAdmits, 0);
  EXPECT_EQ(SkipStats::conjDenseScoredLatchBacks, 1);
  EXPECT_EQ(SkipStats::conjDenseScoredCostRejects, 0);
  EXPECT_EQ(bulkCollector.totalHits(), pullCollector.totalHits());
  assertTopKEquivalent(
      sortedCollectorDocs(pullCollector), sortedCollectorDocs(bulkCollector));
}

TEST_F(TermScorerTest, conjunctionDenseScoredRejectsAsymmetricClauseCosts) {
  DenseScoredGuard denseScoredGuard(false);
  SkipStatsGuard stats;

  const int32_t N = DocsEnumMeta::L1_DOCS + 37;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "producer";
    if ((doc % 10) == 0) body += " lead";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 2> terms = {"lead", "producer"};
  auto queries = makeTermQueries(terms);
  auto mandatory = queryPointers(queries);
  std::span<Query*> empty;
  constexpr int32_t topK = 100;

  BooleanQuery pullQuery(
      std::span<Query*>(mandatory.data(), mandatory.size()),
      empty, empty, empty);
  auto* pullWeight = pullQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* pullScorer = pullWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(pullScorer, nullptr);
  TopDocsCollector pullCollector(topK);
  collectTopK(0, pullScorer, nullptr, nullptr, pullCollector,
              /*allowPruning=*/false);

  BooleanQuery bulkQuery(
      std::span<Query*>(mandatory.data(), mandatory.size()),
      empty, empty, empty);
  auto* bulkWeight = bulkQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = bulkWeight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = dynamic_cast<BooleanQuery::ConjunctionBulkScorer*>(
      supplier->bulkScorer(testIndex.pool));
  ASSERT_NE(bulk, nullptr);
  TopDocsCollector bulkCollector(topK);
  collectTopKWindowed(0, bulk, nullptr, bulkCollector, nullptr,
                      segment.maxDoc(), /*allowPruning=*/false);

  EXPECT_TRUE(bulk->denseScoredCostRejectedForTests());
  EXPECT_EQ(bulk->denseScoredSampleWindowsForTests(), 0);
  EXPECT_FALSE(bulk->denseScoredLatchedBackForTests());
  EXPECT_EQ(SkipStats::conjDenseScoredWindows, 0);
  EXPECT_EQ(SkipStats::conjDenseScoredAdmits, 0);
  EXPECT_EQ(SkipStats::conjDenseScoredLatchBacks, 0);
  EXPECT_EQ(SkipStats::conjDenseScoredCostRejects, 1);
  EXPECT_EQ(bulkCollector.totalHits(), pullCollector.totalHits());
  assertTopKEquivalent(
      sortedCollectorDocs(pullCollector), sortedCollectorDocs(bulkCollector));
}

TEST_F(TermScorerTest, conjunctionScoredProbeKeepsBlockWorkSurvivorBounded) {
  DenseScoredGuard denseScoredGuard(false);
  ScoredProbeGuard scoredProbeGuard(false);

  const int32_t N = 40000;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "probe_producer";
    if ((doc % 3) == 0) {
      appendRepeatedTerm(body, "probe_word", 1 + (doc % 5));
    }
    if ((doc % 257) == 0) {
      appendRepeatedTerm(body, "probe_lead", 1 + (doc % 7));
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 3> terms = {
      "probe_lead", "probe_word", "probe_producer"
  };
  auto queries = makeTermQueries(terms);
  auto mandatory = queryPointers(queries);
  std::span<Query*> empty;
  BooleanQuery query(
      std::span<Query*>(mandatory.data(), mandatory.size()),
      empty, empty, empty);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = dynamic_cast<BooleanQuery::ConjunctionBulkScorer*>(
      supplier->bulkScorer(testIndex.pool));
  ASSERT_NE(bulk, nullptr);

  SkipStatsGuard stats;
  TopDocsCollector collector(100);
  collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                      segment.maxDoc(), /*allowPruning=*/false);

  EXPECT_TRUE(bulk->denseScoredCostRejectedForTests());
  EXPECT_GT(SkipStats::scoredProbeAdvances, 0);
  EXPECT_GT(SkipStats::scoredWordProbeAdvances, 0);
  EXPECT_GT(SkipStats::scoredProbeSurvivorBlocks, 0);
  EXPECT_LT(SkipStats::scoredProbeFreqDecodes,
            SkipStats::scoredProbeAdvances);
  EXPECT_EQ(SkipStats::scoredProbeFreqDecodes,
            SkipStats::scoredProbeSurvivorBlocks);
  EXPECT_EQ(SkipStats::scoredProbeWordExpansions, 0);
  EXPECT_EQ(SkipStats::tfreqBlocksDecoded,
            SkipStats::conjScoredLeadFreqDecodes
                + SkipStats::scoredProbeFreqDecodes);
  EXPECT_LE(SkipStats::tfreqBlocksDecoded,
            SkipStats::conjScoredLeadFreqDecodes
                + SkipStats::scoredProbeSurvivorBlocks);
}

TEST_F(TermScorerTest, conjunctionScoredProbeResumeOwnsPrepositionedTermFreq) {
  DenseScoredGuard denseScoredGuard(true);
  ScoredProbeGuard scoredProbeGuard(false);

  constexpr int32_t N = 8192;
  constexpr int32_t failedDoc = DocsEnumMeta::L1_DOCS;
  constexpr int32_t resumeDoc = failedDoc + 1;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    if (doc <= resumeDoc) {
      body = "resume_lead";
    }
    if (doc != failedDoc) {
      appendRepeatedTerm(body, "resume_probe", doc == resumeDoc ? 17 : 1);
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 2> terms = {
      "resume_lead", "resume_probe"
  };

  auto pullQueries = makeTermQueries(terms);
  auto pullMandatory = queryPointers(pullQueries);
  BooleanQuery pullQuery(pullMandatory, {}, {}, {});
  auto* pullWeight = pullQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* pull = pullWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(pull, nullptr);
  ASSERT_EQ(pull->advance(resumeDoc), resumeDoc);
  const uint32_t expectedScore = std::bit_cast<uint32_t>(pull->score());

  auto bulkQueries = makeTermQueries(terms);
  auto bulkMandatory = queryPointers(bulkQueries);
  BooleanQuery bulkQuery(bulkMandatory, {}, {}, {});
  auto* bulkWeight = bulkQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* supplier = bulkWeight->scorerSupplier(testIndex.pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = dynamic_cast<BooleanQuery::ConjunctionBulkScorer*>(
      supplier->bulkScorer(testIndex.pool));
  ASSERT_NE(bulk, nullptr);
  auto termScorers = bulk->termScorersForTests();
  ASSERT_EQ(termScorers.size(), 2);

  SkipStatsGuard stats;
  ScoreWindow first;
  int32_t next = bulk->scoreNextWindow(
      first, nullptr, 0, failedDoc, std::numeric_limits<float>::lowest());
  ASSERT_EQ(next, failedDoc);
  ASSERT_EQ(first.size, failedDoc);

  const int64_t decodesBeforeFailure = SkipStats::scoredProbeFreqDecodes;
  const int64_t survivorsBeforeFailure =
      SkipStats::scoredProbeSurvivorBlocks;
  ScoreWindow failed;
  next = bulk->scoreNextWindow(
      failed, nullptr, failedDoc, resumeDoc,
      std::numeric_limits<float>::lowest());
  ASSERT_EQ(next, resumeDoc);
  ASSERT_EQ(failed.size, 0);
  EXPECT_EQ(SkipStats::scoredProbeFreqDecodes, decodesBeforeFailure);
  EXPECT_EQ(SkipStats::scoredProbeSurvivorBlocks, survivorsBeforeFailure);
  ASSERT_EQ(termScorers[1]->docId(), resumeDoc);

  ScoreWindow resumed;
  next = bulk->scoreNextWindow(
      resumed, nullptr, resumeDoc, resumeDoc + 1,
      std::numeric_limits<float>::lowest());
  ASSERT_EQ(next, resumeDoc + 1);
  ASSERT_EQ(resumed.size, 1);
  EXPECT_EQ(resumed.docs[0], resumeDoc);
  EXPECT_EQ(std::bit_cast<uint32_t>(resumed.scores[0]), expectedScore);
  EXPECT_EQ(SkipStats::scoredProbeFreqDecodes, decodesBeforeFailure + 1);
  EXPECT_EQ(SkipStats::scoredProbeSurvivorBlocks,
            survivorsBeforeFailure + 1);
}

TEST_F(TermScorerTest, exactConjunctionDecodesTermFreqsOnlyOnSurvival) {
  constexpr int32_t N = 40000;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "exact_filler";
    if ((doc % 257) == 0) body += " exact_gate";
    if ((doc % 2) == 0) {
      appendRepeatedTerm(body, "exact_two", 2 + (doc % 5));
    }
    if ((doc % 3) == 0) {
      appendRepeatedTerm(body, "exact_three", 3 + (doc % 7));
    }
    if ((doc % 5) == 0) {
      appendRepeatedTerm(body, "exact_five", 4 + (doc % 11));
    }
    if ((doc % 7) == 0) {
      appendRepeatedTerm(body, "exact_seven", 5 + (doc % 13));
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  struct Run {
    std::vector<TopDocsCollector::ScoreDoc> topDocs;
    int64_t exactEngagements;
    int64_t probeAdvances;
    int64_t survivorBlocks;
    int64_t freqDecodes;
    int64_t tfreqBlocksDecoded;
  };
  auto run = [&](bool disabled) {
    auto poolFree = testIndex.pool.rewindScopeGuard();
    ExactFreqOnSurvivalGuard freqGuard(disabled);
    SkipStatsGuard stats;
    Query::Context qContext(testIndex.pool, *testIndex.reader);
    auto& segment = qContext.topReader.segments()[0];
    std::array<std::string_view, 5> terms = {
        "exact_gate", "exact_two", "exact_three", "exact_five",
        "exact_seven"
    };
    auto queries = makeTermQueries(terms);
    auto mandatory = queryPointers(queries);
    BooleanQuery query(mandatory, {}, {}, {});
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(testIndex.pool, segment);
    EXPECT_NE(scorer, nullptr);

    TopDocsCollector collector(100);
    if (scorer != nullptr) {
      collectTopK(0, scorer, nullptr, nullptr, collector,
                  /*allowPruning=*/false);
    }
    return Run{
      sortedCollectorDocs(collector),
      SkipStats::conjExactDirectApproxEngagements,
      SkipStats::scoredProbeAdvances,
      SkipStats::scoredProbeSurvivorBlocks,
      SkipStats::scoredProbeFreqDecodes,
      SkipStats::tfreqBlocksDecoded,
    };
  };

  Run enabled = run(false);
  Run disabled = run(true);
  assertTopKEquivalent(disabled.topDocs, enabled.topDocs);
  EXPECT_EQ(enabled.topDocs.size(), 1);
  EXPECT_GT(enabled.exactEngagements, 0);
  EXPECT_GT(disabled.exactEngagements, 0);
  EXPECT_GT(enabled.probeAdvances, 0);
  EXPECT_EQ(disabled.probeAdvances, 0);
  EXPECT_GT(enabled.survivorBlocks, 0);
  EXPECT_EQ(enabled.freqDecodes, enabled.survivorBlocks);
  EXPECT_EQ(enabled.tfreqBlocksDecoded, enabled.freqDecodes);
  EXPECT_GT(disabled.tfreqBlocksDecoded,
            enabled.tfreqBlocksDecoded * 10);
}

TEST_F(TermScorerTest, conjunctionSparseCountFallbackMatchesPull) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 200;
  // Keep srare's docFreq below the dense gate so the leapfrog fallback runs.
  const int32_t rareMax =
      N / BooleanQuery::ConjunctionBulkScorer::kDenseThresholdInverse - 1;
  ASSERT_GT(rareMax, 1);
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  int32_t rareCount = 0;
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "scommon";
    if (rareCount < rareMax && (doc % 500) == 0) {
      body += " srare";
      rareCount++;
    }
    body += " filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  std::array<std::string_view, 2> terms = {"srare", "scommon"};

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  int64_t pull = countPullTermConjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  int64_t bulk = countBulkTermConjunctionSegment(testIndex.pool, qContext, segment, terms, nullptr);
  EXPECT_EQ(bulk, pull);
  EXPECT_EQ(SkipStats::conjDenseCountWindows, 0);
  EXPECT_GT(SkipStats::conjCountFallbacks, 0);
  SkipStats::enabled = savedStats;
}

TEST_F(TermScorerTest, docsFreqEnumDocsOnlyProtocolAssertsOnFreqAccess) {
#ifndef NDEBUG
  const int32_t N = 8;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    f.add(doc, "protocol hot filler");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  auto& segment = testIndex.reader->segments()[0];
  PostingsReader& postingsReader = segment.postingsReader();
  FieldReader fieldReader(postingsReader);
  ASSERT_TRUE(fieldReader.seek("body_w"));
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(testIndex.pool, postingsReader, fieldInfo);
  ASSERT_TRUE(tenum.seek("protocol"));

  DocsFreqEnum denum(tenum);
  auto docs = denum.peekDocBlock();
  ASSERT_FALSE(docs.empty());
  denum.consumeDocOnlyBlock(1);
  ASSERT_DEATH({ (void) denum.termFreq(); }, "");
#endif
}

TEST_F(TermScorerTest, countBulkFillWithDeletesMatchesPull) {
  const int32_t N = 4 * Postings::DOCS_BLOCK_SIZE + 41;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    bool a = (doc % 2) == 0;
    bool b = (doc % 7) < 4;
    bool c = (doc % 19) == 5;
    addBulkFillTermDoc(f, doc, a, b, c);
    if ((doc % 11) == 0) {
      testIndex.deleteDoc(doc);
    }
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  ASSERT_NE(segment.liveDocs(), nullptr);
  DocSet* liveDocs = &segment.liveDocs()->docset();
  std::array<std::string_view, 3> terms = {"bf_a", "bf_b", "bf_c"};

  BulkDomainDriveGuard guard(true);
  int64_t pull = countPullTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, liveDocs);
  int64_t bulk = countBulkTermDisjunctionSegment(testIndex.pool, qContext, segment, terms, liveDocs);
  EXPECT_EQ(bulk, pull);
}

TEST_F(TermScorerTest, bulkCountDomainDisjunctionMatchesPullAcrossFiltersAndDeletes) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 97;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    if ((doc % 2) == 0) appendRepeatedTerm(body, "bda", 1 + (doc % 3));
    if ((doc % 3) != 1) appendRepeatedTerm(body, "bdb", 1 + (doc % 5));
    if ((doc % 11) == 0) appendRepeatedTerm(body, "bdc", 1);
    appendRepeatedTerm(body, "filler", 2);
    f.add(doc, body);
    if ((doc % 17) == 0) {
      testIndex.deleteDoc(doc);
    }
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  ASSERT_NE(segment.liveDocs(), nullptr);
  auto bitsetFilter = makeEveryNthDocSet(segment.maxDoc(), 4, false);
  auto arrayFilter = makeEveryNthDocSet(segment.maxDoc(), 5, true);

  std::array<std::string_view, 3> terms = {"bda", "bdb", "bdc"};
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;

  auto check = [&](DocSet* filter, bool disableDomainDrive) {
    BulkDomainDriveGuard guard(disableDomainDrive);
    BooleanQuery bulkQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
    BooleanQuery pullQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
    auto* bulkWeight = bulkQ.createWeight(qContext, Query::NEED_SCORES);
    auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
    expectBulkDomainMatchesPull(bulkWeight, pullWeight, testIndex.pool, segment, filter);
  };

  check(nullptr, true);
  check(&segment.liveDocs()->docset(), true);
  check(bitsetFilter.get(), true);
  check(arrayFilter.get(), true);
}

TEST_F(TermScorerTest, constantTopKAndDomainMatchesExhaustivePullWithFilters) {
  const int32_t N = 256;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    f.add(doc, (doc & 1) == 0 ? "needle filler" : "filler");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  TermQuery query("body_w", "needle");
  auto* weight = query.createWeight(qContext, 0);
  ASSERT_TRUE(weight->isConstantScoring());
  auto bitsetFilter = makeEveryNthDocSet(N, 3, false);
  auto arrayFilter = makeEveryNthDocSet(N, 5, true);

  for (DocSet* filter : {bitsetFilter.get(), arrayFilter.get()}) {
    DocSetBuilder expectedBuilder(N);
    TopDocsCollector expectedCollector(10);
    auto* expectedScorer = weight->createScorer(testIndex.pool, segment);
    ASSERT_NE(expectedScorer, nullptr);
    collectTopK(0, expectedScorer, filter, &expectedBuilder,
                expectedCollector, false);
    auto expectedDomain = expectedBuilder.build();

    DocSetBuilder actualBuilder(N);
    TopDocsCollector actualCollector(10);
    auto* actualScorer = weight->createScorer(testIndex.pool, segment);
    ASSERT_NE(actualScorer, nullptr);
    collectConstantTopKAndDomain(
        0, actualScorer, filter, actualBuilder, actualCollector);
    auto actualDomain = actualBuilder.build();

    EXPECT_EQ(actualCollector.totalHits(), expectedCollector.totalHits());
    auto expectedTop = sortedCollectorDocs(expectedCollector);
    auto actualTop = sortedCollectorDocs(actualCollector);
    ASSERT_EQ(actualTop.size(), expectedTop.size());
    for (size_t i = 0; i < expectedTop.size(); i++) {
      EXPECT_EQ(actualTop[i].doc, expectedTop[i].doc);
      EXPECT_FLOAT_EQ(actualTop[i].score, expectedTop[i].score);
    }
    expectDocSetEqual(actualDomain.get(), expectedDomain.get(), N);
  }
}

TEST_F(TermScorerTest, filterOnlyBulkDomainsMatchPull) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 97;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "filler";
    if ((doc % 3) != 1) body += " filter_a";
    if ((doc % 5) < 3) body += " filter_b";
    f.add(doc, body);
    if ((doc % 17) == 0) {
      testIndex.deleteDoc(doc);
    }
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  ASSERT_NE(segment.liveDocs(), nullptr);
  DocSet* liveDocs = &segment.liveDocs()->docset();
  TermQuery filterA("body_w", "filter_a");
  TermQuery filterB("body_w", "filter_b");
  std::array<Query*, 2> filters = {&filterA, &filterB};
  std::span<Query*> empty;

  for (size_t clauseCount : {1u, 2u}) {
    std::span<Query*> activeFilters(filters.data(), clauseCount);
    BooleanQuery bulkCountQ(empty, empty, empty, activeFilters);
    BooleanQuery pullCountQ(empty, empty, empty, activeFilters);
    auto* bulkCountWeight = bulkCountQ.createWeight(qContext, Query::NEED_SCORES);
    auto* pullCountWeight = pullCountQ.createWeight(qContext, Query::NEED_SCORES);
    expectBulkDomainMatchesPull(
        bulkCountWeight, pullCountWeight, testIndex.pool, segment, liveDocs);

    BooleanQuery pullScoreQ(empty, empty, empty, activeFilters);
    auto* pullScoreWeight = pullScoreQ.createWeight(qContext, Query::NEED_SCORES);
    DocSetBuilder pullBuilder(segment.maxDoc());
    TopDocsCollector pullCollector(10);
    auto* pullScorer = pullScoreWeight->createScorer(testIndex.pool, segment);
    ASSERT_NE(pullScorer, nullptr);
    collectTopK(0, pullScorer, liveDocs, &pullBuilder, pullCollector,
                /*allowPruning=*/false);
    auto pullDomain = pullBuilder.build();

    BooleanQuery bulkScoreQ(empty, empty, empty, activeFilters);
    auto* bulkScoreWeight = bulkScoreQ.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = bulkScoreWeight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = supplier->bulkScorer(testIndex.pool);
    ASSERT_NE(bulk, nullptr);
    DocSetBuilder bulkBuilder(segment.maxDoc());
    int64_t bulkCount = countMatchesWindowed(
        bulk, liveDocs, &bulkBuilder, segment.maxDoc());

    auto* rankingSupplier = bulkScoreWeight->scorerSupplier(testIndex.pool, segment);
    ASSERT_NE(rankingSupplier, nullptr);
    auto* rankingBulk = rankingSupplier->bulkScorer(testIndex.pool);
    ASSERT_NE(rankingBulk, nullptr);
    TopDocsCollector bulkCollector(10);
    collectTopKWindowed(0, rankingBulk, liveDocs, bulkCollector, nullptr,
                        segment.maxDoc(), /*allowPruning=*/false);
    ASSERT_EQ(bulkCount, bulkCollector.totalHits());
    auto bulkDomain = bulkBuilder.build();

    EXPECT_EQ(bulkCollector.totalHits(), pullCollector.totalHits());
    auto bulkTop = sortedCollectorDocs(bulkCollector);
    auto pullTop = sortedCollectorDocs(pullCollector);
    ASSERT_EQ(bulkTop.size(), pullTop.size());
    for (size_t i = 0; i < pullTop.size(); i++) {
      EXPECT_EQ(bulkTop[i].doc, pullTop[i].doc);
      EXPECT_FLOAT_EQ(bulkTop[i].score, pullTop[i].score);
    }
    expectDocSetEqual(bulkDomain.get(), pullDomain.get(), segment.maxDoc());
  }
}

void checkBulkCountDomainDisjunctionDomainDriveMatchesPull(
    IndexReader& reader, int32_t numTerms) {
  const int32_t filterStep = 512;

  MemPool pool;
  Query::Context qContext(pool, reader);
  auto& segment = qContext.topReader.segments()[0];
  std::vector<std::string> termStrings = makeMtTermStrings(numTerms);
  std::vector<std::string_view> termViews;
  termViews.reserve(termStrings.size());
  for (auto& term : termStrings) termViews.push_back(term);
  auto queries = makeTermQueries(termViews);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;

  for (bool arrayDocSet : {false, true}) {
    auto filter = makeEveryNthDocSet(segment.maxDoc(), filterStep, arrayDocSet);
    BooleanQuery bulkQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
    BooleanQuery pullQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
    auto* bulkWeight = bulkQ.createWeight(qContext, Query::NEED_SCORES);
    auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
    auto expected = pullDomain(pullWeight, pool, segment, filter.get());

    DocSetBuilder builder(segment.maxDoc());
    int64_t count = 0;
    auto* supplier = bulkWeight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = supplier->bulkScorer(pool);
    ASSERT_NE(bulk, nullptr);
    auto* maxScoreBulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(bulk);
    ASSERT_NE(maxScoreBulk, nullptr);
    for (int32_t cursor = 0; cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
      int32_t next = bulk->countNextWindow(count, &builder, filter.get(), cursor, segment.maxDoc());
      if (next == PostingsReader::END) break;
      ASSERT_GT(next, cursor);
      cursor = next;
    }
    ASSERT_GT(maxScoreBulk->domainDriveWindowCount(), 0) << "arrayDocSet=" << arrayDocSet;
    EXPECT_EQ(count, expected.count);
    EXPECT_EQ(count, builder.card());
    auto actual = builder.build();
    expectDocSetEqual(actual.get(), expected.domain.get(), segment.maxDoc());
  }
}

TEST_F(TermScorerTest, bulkCountDomainConjunctionDenseAndSparseMatchPull) {
  {
    const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 321;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    for (int32_t doc = 0; doc < N; doc++) {
      std::string body;
      if ((doc % 17) != 5) appendRepeatedTerm(body, "bcda", 1 + (doc % 3));
      if ((doc % 19) != 7) appendRepeatedTerm(body, "bcdb", 1 + (doc % 5));
      appendRepeatedTerm(body, "filler", 2);
      f.add(doc, body);
    }
    testIndex.flush();
    f.startReading();

    auto poolFree = testIndex.pool.rewindScopeGuard();
    Query::Context qContext(testIndex.pool, *testIndex.reader);
    auto& segment = qContext.topReader.segments()[0];
    auto bitsetFilter = makeEveryNthDocSet(segment.maxDoc(), 3, false);
    auto arrayFilter = makeEveryNthDocSet(segment.maxDoc(), 5, true);
    std::array<std::string_view, 2> terms = {"bcda", "bcdb"};
    auto queries = makeTermQueries(terms);
    auto mandatory = queryPointers(queries);
    std::span<Query*> empty;

    std::array<DocSet*, 3> filters = {nullptr, bitsetFilter.get(), arrayFilter.get()};
    for (DocSet* filter : filters) {
      BooleanQuery bulkQ(std::span<Query*>(mandatory.data(), mandatory.size()), empty, empty, empty);
      BooleanQuery pullQ(std::span<Query*>(mandatory.data(), mandatory.size()), empty, empty, empty);
      auto* bulkWeight = bulkQ.createWeight(qContext, Query::NEED_SCORES);
      auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
      expectBulkDomainMatchesPull(bulkWeight, pullWeight, testIndex.pool, segment, filter);
    }
  }

  {
    const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 200;
    const int32_t rareMax =
        N / BooleanQuery::ConjunctionBulkScorer::kDenseThresholdInverse - 1;
    TestIndex testIndex;
    TestField f(testIndex, "body_w");
    f.startIndexing();
    int32_t rareCount = 0;
    for (int32_t doc = 0; doc < N; doc++) {
      std::string body = "bcscommon";
      if (rareCount < rareMax && (doc % 500) == 0) {
        body += " bcsrare";
        rareCount++;
      }
      body += " filler";
      f.add(doc, body);
    }
    testIndex.flush();
    f.startReading();

    auto poolFree = testIndex.pool.rewindScopeGuard();
    Query::Context qContext(testIndex.pool, *testIndex.reader);
    auto& segment = qContext.topReader.segments()[0];
    auto filter = makeEveryNthDocSet(segment.maxDoc(), 2, false);
    std::array<std::string_view, 2> terms = {"bcsrare", "bcscommon"};
    auto queries = makeTermQueries(terms);
    auto mandatory = queryPointers(queries);
    std::span<Query*> empty;

    bool savedStats = SkipStats::enabled;
    SkipStats::enabled = true;
    SkipStats::reset();
    BooleanQuery bulkQ(std::span<Query*>(mandatory.data(), mandatory.size()), empty, empty, empty);
    BooleanQuery pullQ(std::span<Query*>(mandatory.data(), mandatory.size()), empty, empty, empty);
    auto* bulkWeight = bulkQ.createWeight(qContext, Query::NEED_SCORES);
    auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
    expectBulkDomainMatchesPull(bulkWeight, pullWeight, testIndex.pool, segment, filter.get());
    EXPECT_GT(SkipStats::conjCountFallbacks, 0);
    SkipStats::enabled = savedStats;
  }
}

TEST_F(TermScorerTest, queryPrepMaterializeBulkDomainMatchesPull) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 77;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body;
    if ((doc % 2) == 0) body += "qp_a ";
    if ((doc % 3) == 0) body += "qp_b ";
    if ((doc % 5) == 0) body += "qp_c ";
    body += "filler";
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  auto filter = makeEveryNthDocSet(segment.maxDoc(), 4, true);
  std::array<std::string_view, 3> terms = {"qp_a", "qp_b", "qp_c"};
  auto queries = makeTermQueries(terms);
  auto optional = queryPointers(queries);
  std::span<Query*> empty;

  BooleanQuery pullQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
  auto expected = pullDomain(pullWeight, testIndex.pool, segment, filter.get());

  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  BooleanQuery materializeQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
  auto* materializeWeight = materializeQ.createWeight(qContext, Query::NEED_SCORES);
  auto actual = QueryPrep::materialize(*materializeWeight, nullptr, segment, filter.get());
  EXPECT_GT(SkipStats::bulkDomainWindowsFed, 0);
  SkipStats::enabled = savedStats;

  expectDocSetEqual(actual.get(), expected.domain.get(), segment.maxDoc());
}

TEST_F(TermScorerTest, queryPrepDirectTermMaterializationMatchesWindows) {
  const int32_t N = 2 * DocsEnumMeta::L1_DOCS + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    std::string body = "filler";
    if (doc == 0 || doc == 127 || doc == 128
        || doc == DocsEnumMeta::L1_DOCS - 1
        || doc == DocsEnumMeta::L1_DOCS) {
      body += " sparse";
    }
    if ((doc & 7) == 0) {
      body += " promoted";
    }
    f.add(doc, body);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  bool savedDirectDisable =
      QueryPrep::disableDirectPostingsMaterializationForTests;
  for (std::string_view term : {"sparse", "promoted", "missing"}) {
    TermQuery query("body_w", term);
    auto* directWeight = query.createWeight(qContext, 0);
    QueryPrep::disableDirectPostingsMaterializationForTests = false;
    auto direct = QueryPrep::materialize(
        *directWeight, nullptr, segment, nullptr);

    auto* windowWeight = query.createWeight(qContext, 0);
    QueryPrep::disableDirectPostingsMaterializationForTests = true;
    auto window = QueryPrep::materialize(
        *windowWeight, nullptr, segment, nullptr);

    expectDocSetEqual(direct.get(), window.get(), segment.maxDoc());
    EXPECT_EQ(direct->type, window->type);
    EXPECT_EQ(direct->type, term == "promoted"
        ? DocSet::BITSET : DocSet::ARRAY);
  }
  QueryPrep::disableDirectPostingsMaterializationForTests =
      savedDirectDisable;
}

// "+a b" without scores: the optional clause is a pure score add under a
// mandatory clause, so a non-scoring weight drops it - membership is
// unchanged and the single-clause count() shortcut engages (Lucene's
// BooleanWeight simplification).
TEST_F(TermScorerTest, nonScoringBooleanDropsOptionalUnderMandatory) {
  const int32_t N = 1500;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  int32_t dfA = 0;
  for (int32_t d = 0; d < N; d++) {
    std::string text;
    if (d % 3 == 0) { text += "aterm "; dfA++; }
    if (d % 5 == 0) { text += "bterm "; }
    text += "pad";
    f.add(d, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  TermQuery a("body_w", "aterm");
  TermQuery b("body_w", "bterm");
  std::vector<Query*> mand = {&a};
  std::vector<Query*> opt = {&b};
  BooleanQuery q(mand, opt, {}, {});

  // Non-scoring: optional dropped -> O(1) exact count, and iteration = df(a).
  auto* countWeight = q.createWeight(qContext, 0);
  EXPECT_EQ(countWeight->count(segment), dfA);
  auto* countScorer = countWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(countScorer, nullptr);
  int32_t iterated = 0;
  for (int32_t d = countScorer->next(); d != PostingsReader::END; d = countScorer->next()) {
    EXPECT_EQ(d % 3, 0);
    iterated++;
  }
  EXPECT_EQ(iterated, dfA);

  // Scoring: optional kept -> no scalar count arm, same match set.
  Query::Context qContext2(testIndex.pool, *testIndex.reader);
  BooleanQuery q2(mand, opt, {}, {});
  auto* scoredWeight = q2.createWeight(qContext2, Query::NEED_SCORES);
  EXPECT_EQ(scoredWeight->count(segment), -1);
  auto* scoredScorer = scoredWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(scoredScorer, nullptr);
  int32_t scoredCount = 0;
  for (int32_t d = scoredScorer->next(); d != PostingsReader::END; d = scoredScorer->next()) {
    scoredCount++;
  }
  EXPECT_EQ(scoredCount, dfA);

  // min_match=1 makes the optional group a membership constraint: the
  // non-scoring path must NOT drop it (found by BooleanFuzzTest as a filter
  // clause matching docs with none of its optionals).
  Query::Context qContext3(testIndex.pool, *testIndex.reader);
  BooleanQuery q3(mand, opt, {}, {}, 1);
  auto* mmWeight = q3.createWeight(qContext3, 0);
  auto* mmScorer = mmWeight->createScorer(testIndex.pool, segment);
  ASSERT_NE(mmScorer, nullptr);
  int32_t mmCount = 0;
  for (int32_t d = mmScorer->next(); d != PostingsReader::END; d = mmScorer->next()) {
    EXPECT_EQ(d % 15, 0) << "doc must hold aterm AND bterm";
    mmCount++;
  }
  EXPECT_EQ(mmCount, (N + 14) / 15);
}

TEST_F(TermScorerTest, WindowFilterIntersectsDirectTermsAcrossWindowJumps) {
  CollectionHelper helper("main");
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 512; doc++) {
    std::string body = "pad";
    if ((doc % 2) == 0) body += " filter_a";
    if ((doc % 3) == 0) body += " filter_b";
    docs.push_back(flatdoc("id", "wf_" + std::to_string(doc),
                           "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();

  MemPool pool;
  Query::Context context(pool, *reader);
  auto& segment = context.topReader.segments()[0];
  TermQuery filterA("body_w", "filter_a");
  TermQuery filterB("body_w", "filter_b");
  auto* supplierA = filterA.createWeight(context, 0)->scorerSupplier(pool, segment);
  auto* supplierB = filterB.createWeight(context, 0)->scorerSupplier(pool, segment);
  auto scorers = pool.make_span<Query::Scorer*>(2);
  scorers[0] = buildScorerForTests(
      pool, *supplierA, std::numeric_limits<int64_t>::max());
  scorers[1] = buildScorerForTests(
      pool, *supplierB, std::numeric_limits<int64_t>::max());
  WindowFilter filter(pool, scorers);

  EXPECT_EQ(filter.prepare(1, 6), 0);
  for (int32_t doc = 1; doc < 6; doc++) {
    EXPECT_FALSE(filter.accepts(doc));
  }

  int32_t expectedCard = 0;
  for (int32_t doc = 211; doc < 389; doc++) {
    expectedCard += (doc % 6) == 0;
  }
  EXPECT_EQ(filter.prepare(211, 389), expectedCard);
  for (int32_t doc = 211; doc < 389; doc++) {
    EXPECT_EQ(filter.accepts(doc), (doc % 6) == 0) << "doc=" << doc;
  }

  EXPECT_TRUE(filter.acceptsProbe(390));
  EXPECT_FALSE(filter.acceptsProbe(391));
  EXPECT_TRUE(filter.acceptsProbe(396));

  expectedCard = 0;
  for (int32_t doc = 397; doc < 449; doc++) {
    expectedCard += (doc % 6) == 0;
  }
  EXPECT_EQ(filter.prepare(397, 449), expectedCard);
  for (int32_t doc = 397; doc < 449; doc++) {
    EXPECT_EQ(filter.accepts(doc), (doc % 6) == 0) << "doc=" << doc;
  }
}

TEST_F(TermScorerTest, FilterMaskProbeAndFillProduceEquivalentScoredResults) {
  CollectionHelper helper("filter_mask_probe_score");
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "pad";
    if ((doc % 2) == 0) body += " keep";
    if ((doc % 37) == 0) appendRepeatedTerm(body, "term_sparse", 1 + doc % 5);
    if ((doc % 601) == 0) appendRepeatedTerm(body, "conj_sparse", 1 + doc % 7);
    if ((doc % 3) == 0) body += " conj_dense group_dense";
    if ((doc % 97) == 0) appendRepeatedTerm(body, "union_a", 1 + doc % 3);
    if ((doc % 103) == 0) appendRepeatedTerm(body, "union_b", 1 + doc % 4);
    docs.push_back(flatdoc("id", "probe_score_" + std::to_string(doc),
                           "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = std::make_shared<IndexReader>(helper.getIndexWriter()->dir);
  ASSERT_EQ(reader->maxDoc(), nDocs);
  ASSERT_GT(reader->maxDoc(), DocsEnumMeta::L1_DOCS);

  TermQuery keep("body_w", "keep");
  std::vector<Query*> filters = {&keep};

  TermQuery termSparse("body_w", "term_sparse");
  std::vector<Query*> termMandatory = {&termSparse};
  BooleanQuery termQuery(termMandatory, {}, {}, filters);

  TermQuery conjSparse("body_w", "conj_sparse");
  TermQuery conjDense("body_w", "conj_dense");
  std::vector<Query*> conjunctionMandatory = {&conjSparse, &conjDense};
  BooleanQuery conjunctionQuery(conjunctionMandatory, {}, {}, filters);

  TermQuery unionA("body_w", "union_a");
  TermQuery unionB("body_w", "union_b");
  TermQuery groupDense("body_w", "group_dense");
  std::vector<Query*> unionOptional = {&unionA, &unionB};
  BooleanQuery unionBody({}, unionOptional, {}, {}, 1);
  std::vector<Query*> groupedMandatory = {&unionBody, &groupDense};
  BooleanQuery groupedConjunctionQuery(groupedMandatory, {}, {}, filters);

  struct ScoredRun {
    QueryTopKRun result;
    int64_t filterFillCalls;
  };
  auto run = [&](Query& query, bool forceFill, std::type_index expectedType) {
    FilteredConjunctionBatchGuard candidateGuard(true);
    FilterMaskProbeGuard probeGuard(forceFill);
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto& segment = context.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_NE(bulk, nullptr);
    if (bulk != nullptr) {
      EXPECT_EQ(std::type_index(typeid(*bulk)), expectedType);
    }
    TopDocsCollector collector(2048);
    if (bulk != nullptr) {
      collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                          segment.maxDoc(), true);
    }
    return ScoredRun{
      {collector.totalHits(), sortedCollectorDocs(collector)},
      SkipStats::countBulkFillCalls
    };
  };

  auto assertProbeFillEquivalent = [&](Query& query, std::type_index expectedType) {
    auto probe = run(query, false, expectedType);
    auto fill = run(query, true, expectedType);
    EXPECT_EQ(probe.filterFillCalls, 0);
    EXPECT_GT(fill.filterFillCalls, 0);
    EXPECT_EQ(probe.result.visited, fill.result.visited);
    assertQueryTopKExact(probe.result, fill.result);
  };

  assertProbeFillEquivalent(termQuery, typeid(TermQuery::TermBulkScorer));
  assertProbeFillEquivalent(conjunctionQuery,
                            typeid(BooleanQuery::ConjunctionBulkScorer));
  assertProbeFillEquivalent(groupedConjunctionQuery,
                            typeid(BooleanQuery::ConjunctionBulkScorer));
}

TEST_F(TermScorerTest, FilterMaskProbeAndFillProduceEquivalentSparseCounts) {
  CollectionHelper helper("filter_mask_probe_count");
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 129;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "pad";
    if ((doc % 2) == 0) body += " keep";
    if ((doc % 37) == 0) body += " term_sparse";
    if ((doc % 601) == 0) body += " conj_sparse";
    if ((doc % 3) == 0) body += " conj_dense";
    docs.push_back(flatdoc("id", "probe_count_" + std::to_string(doc),
                           "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = std::make_shared<IndexReader>(helper.getIndexWriter()->dir);
  ASSERT_EQ(reader->maxDoc(), nDocs);
  ASSERT_GT(reader->maxDoc(), DocsEnumMeta::L1_DOCS);

  TermQuery keep("body_w", "keep");
  std::vector<Query*> filters = {&keep};
  TermQuery termSparse("body_w", "term_sparse");
  std::vector<Query*> termMandatory = {&termSparse};
  BooleanQuery termQuery(termMandatory, {}, {}, filters);
  TermQuery conjSparse("body_w", "conj_sparse");
  TermQuery conjDense("body_w", "conj_dense");
  std::vector<Query*> conjunctionMandatory = {&conjSparse, &conjDense};
  BooleanQuery conjunctionQuery(conjunctionMandatory, {}, {}, filters);

  struct CountRun {
    int64_t count;
    std::unique_ptr<DocSet> domain;
    int64_t fillCalls;
  };
  auto run = [&](Query& query, bool forceFill, std::type_index expectedType) {
    FilteredConjunctionBatchGuard candidateGuard(true);
    FilterMaskProbeGuard probeGuard(forceFill);
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto& segment = context.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_NE(bulk, nullptr);
    if (bulk != nullptr) {
      EXPECT_EQ(std::type_index(typeid(*bulk)), expectedType);
    }
    DocSetBuilder builder(segment.maxDoc());
    int64_t count = bulk == nullptr
        ? 0 : countMatchesWindowed(bulk, nullptr, &builder, segment.maxDoc());
    return CountRun{count, builder.build(), SkipStats::countBulkFillCalls};
  };

  auto assertProbeFillEquivalent = [&](Query& query, std::type_index expectedType) {
    auto probe = run(query, false, expectedType);
    auto fill = run(query, true, expectedType);
    EXPECT_GT(fill.fillCalls, probe.fillCalls);
    EXPECT_EQ(probe.count, fill.count);
    expectDocSetEqual(probe.domain.get(), fill.domain.get(), nDocs);
  };

  {
    SCOPED_TRACE("term");
    assertProbeFillEquivalent(termQuery, typeid(TermQuery::TermBulkScorer));
  }
  {
    SCOPED_TRACE("conjunction");
    assertProbeFillEquivalent(conjunctionQuery,
                              typeid(BooleanQuery::ConjunctionBulkScorer));
  }
}

TEST_F(TermScorerTest, NumericRangeFiltersMatchPullAcrossScoredBodyShapes) {
  CollectionHelper helper("range_filter_shapes");
  SchemaBuilder schema;
  auto& rangeField = schema.field("range_i");
  rangeField.type = api::FieldDef::FieldClass::INT;
  rangeField.index = api::FieldDef::IndexMode::RANGE;
  schema.set(helper.collection());
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "pad";
    if ((doc % 2) == 0) body += " term_filter";
    if ((doc % 29) == 0) {
      appendRepeatedTerm(body, "term_body", 1 + (doc / 29) % 11);
    }
    if ((doc % 47) == 0) {
      appendRepeatedTerm(body, "union_a", 1 + (doc / 47) % 7);
    }
    if ((doc % 53) == 0) {
      appendRepeatedTerm(body, "union_b", 1 + (doc / 53) % 9);
    }
    if ((doc % 31) == 0) {
      appendRepeatedTerm(body, "conj_sparse", 1 + (doc / 31) % 13);
    }
    if ((doc % 3) == 0) body += " conj_dense";
    docs.push_back(flatdoc("id", "range_shape_" + std::to_string(doc),
                           "body_w", body, "range_i",
                           (int64_t)(doc % 1000)));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = std::make_shared<IndexReader>(helper.getIndexWriter()->dir);
  ASSERT_EQ(reader->maxDoc(), nDocs);
  ASSERT_GT(reader->maxDoc(), DocsEnumMeta::L1_DOCS);

  NumericRangeQuery fatRange("range_i", 0, 799);
  TermQuery termFilter("body_w", "term_filter");
  std::vector<Query*> rangeFilter = {&fatRange};
  std::vector<Query*> termRangeFilters = {&fatRange, &termFilter};

  TermQuery termBody("body_w", "term_body");
  std::vector<Query*> termMandatory = {&termBody};
  BooleanQuery termRange(termMandatory, {}, {}, rangeFilter);
  BooleanQuery termTermRange(termMandatory, {}, {}, termRangeFilters);

  TermQuery unionA("body_w", "union_a");
  TermQuery unionB("body_w", "union_b");
  std::vector<Query*> unionOptional = {&unionA, &unionB};
  BooleanQuery unionRange({}, unionOptional, {}, rangeFilter, 1);
  BooleanQuery unionTermRange({}, unionOptional, {}, termRangeFilters, 1);

  TermQuery conjSparse("body_w", "conj_sparse");
  TermQuery conjDense("body_w", "conj_dense");
  std::vector<Query*> conjunctionMandatory = {&conjSparse, &conjDense};
  BooleanQuery conjunctionRange(
      conjunctionMandatory, {}, {}, rangeFilter);
  BooleanQuery conjunctionTermRange(
      conjunctionMandatory, {}, {}, termRangeFilters);

  struct ShapeCase {
    const char* name;
    Query* query;
    std::type_index bulkType;
    bool probeReachable;
  };
  std::array cases = {
    ShapeCase{"term/range", &termRange,
              typeid(TermQuery::TermBulkScorer), true},
    ShapeCase{"term/term+range", &termTermRange,
              typeid(TermQuery::TermBulkScorer), true},
    ShapeCase{"union/range", &unionRange,
              typeid(BooleanQuery::MaxScoreBulkScorer), false},
    ShapeCase{"union/term+range", &unionTermRange,
              typeid(BooleanQuery::MaxScoreBulkScorer), false},
    ShapeCase{"conjunction/range", &conjunctionRange,
              typeid(BooleanQuery::ConjunctionBulkScorer), true},
    ShapeCase{"conjunction/term+range", &conjunctionTermRange,
              typeid(BooleanQuery::ConjunctionBulkScorer), true},
  };

  struct Run {
    std::vector<segdoc> docs;
    std::type_index bulkType;
    int64_t fillCalls;
  };
  auto run = [&](Query& query, bool pull, bool forceFill,
                 bool disableShapes) {
    FilteredScoredBulkGuard bulkGuard(pull);
    FilterMaskProbeGuard probeGuard(forceFill);
    NumericRangeShapeGuard shapeGuard(disableShapes);
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = context.topReader.segments()[0];
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_EQ(bulk == nullptr, pull || disableShapes);
    TopDocsCollector collector(50);
    if (bulk != nullptr) {
      collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                          segment.maxDoc(), true);
    } else if (supplier != nullptr) {
      auto* scorer = buildScorerForTests(
          pool, *supplier, std::numeric_limits<int64_t>::max());
      if (scorer != nullptr) {
        collectTopK(0, scorer, nullptr, nullptr, collector, true);
      }
    }
    std::vector<segdoc> resultDocs;
    for (const auto& hit : sortedCollectorDocs(collector)) {
      resultDocs.push_back(hit.doc);
    }
    std::sort(resultDocs.begin(), resultDocs.end());
    return Run{std::move(resultDocs),
               bulk == nullptr ? std::type_index(typeid(void))
                               : std::type_index(typeid(*bulk)),
               SkipStats::countBulkFillCalls};
  };

  for (const auto& shape : cases) {
    SCOPED_TRACE(shape.name);
    Run pull = run(*shape.query, true, false, true);
    Run attached = run(*shape.query, false, false, false);
    Run disabled = run(*shape.query, false, false, true);
    EXPECT_EQ(attached.bulkType, shape.bulkType);
    EXPECT_EQ(attached.docs, pull.docs);
    EXPECT_EQ(disabled.bulkType, std::type_index(typeid(void)));
    EXPECT_EQ(disabled.docs, pull.docs);
    if (shape.probeReachable) {
      Run fill = run(*shape.query, false, true, false);
      EXPECT_EQ(fill.bulkType, shape.bulkType);
      EXPECT_EQ(fill.docs, pull.docs);
      EXPECT_GT(fill.fillCalls, attached.fillCalls);
    }
  }
}

TEST_F(TermScorerTest, ScoredDirectTermFiltersRouteToAttachedBulks) {
  CollectionHelper helper("main");
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 1024; doc++) {
    std::string body = "quick fox pad";
    if ((doc % 2) == 0) body += " keep";
    if ((doc % 32) == 2) body += " mid_filter";
    if ((doc % 512) == 2) body += " selective";  // below the mask-density gate
    if ((doc % 3) == 0) appendRepeatedTerm(body, "body_a", 1 + doc % 5);
    if ((doc % 5) == 0) body += " body_b";
    if ((doc % 7) == 0) body += " bonus";
    if ((doc % 8) == 1 || (doc % 512) == 2) {
      appendRepeatedTerm(body, "union_a", 1 + doc % 4);
    }
    if ((doc % 8) == 2) appendRepeatedTerm(body, "union_b", 1 + doc % 6);
    docs.push_back(flatdoc("id", "route_" + std::to_string(doc),
                           "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  constexpr int32_t topK = 20;

  auto runBulk = [&](Query& query, std::type_index expectedType) {
    ExactFilteredMandOptCompositionGuard compositionGuard(true);
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto& segment = context.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_NE(bulk, nullptr);
    if (bulk != nullptr) {
      EXPECT_EQ(std::type_index(typeid(*bulk)), expectedType);
    }
    TopDocsCollector collector(topK);
    if (bulk != nullptr) {
      collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                          segment.maxDoc(), true);
    }
    return QueryTopKRun{collector.totalHits(), sortedCollectorDocs(collector)};
  };

  auto countBulk = [&](Query& query) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto& segment = context.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_NE(bulk, nullptr);
    int64_t count = 0;
    for (int32_t cursor = 0;
         bulk != nullptr && cursor != PostingsReader::END
             && cursor < segment.maxDoc(); ) {
      int32_t next = bulk->countNextWindow(
          count, nullptr, nullptr, cursor, segment.maxDoc());
      if (next == PostingsReader::END) break;
      EXPECT_GT(next, cursor);
      cursor = next;
    }
    return count;
  };

  TermQuery bodyA("body_w", "body_a");
  TermQuery bodyB("body_w", "body_b");
  TermQuery bonus("body_w", "bonus");
  TermQuery unionA("body_w", "union_a");
  TermQuery unionB("body_w", "union_b");
  TermQuery keep("body_w", "keep");
  TermQuery midFilter("body_w", "mid_filter");
  TermQuery selective("body_w", "selective");
  TermQuery missingOptional("body_w", "missing_optional");

  std::vector<Query*> oneMandatory = {&bodyA};
  std::vector<Query*> twoMandatory = {&bodyA, &bodyB};
  std::vector<Query*> oneOptional = {&bonus};
  std::vector<Query*> unionOptional = {&unionA, &unionB};
  std::vector<Query*> oneMissingOptional = {&missingOptional};
  std::vector<Query*> filters = {&keep};
  std::vector<Query*> midFilters = {&midFilter};
  BooleanQuery termQuery(oneMandatory, {}, {}, filters);
  BooleanQuery conjunctionQuery(twoMandatory, {}, {}, filters);
  BooleanQuery mandOptQuery(oneMandatory, oneOptional, {}, filters);
  BooleanQuery unionQuery({}, unionOptional, {}, filters, 1);
  BooleanQuery midMandOptQuery(oneMandatory, oneOptional, {}, midFilters);
  std::vector<Query*> selectiveMandatory = {&selective};
  BooleanQuery termLeadMandOptQuery(
      selectiveMandatory, unionOptional, {}, midFilters);
  BooleanQuery missingOptionalMandOptQuery(
      oneMandatory, oneMissingOptional, {}, midFilters);
  BooleanQuery wrappedMandOptBody(oneMandatory, oneOptional, {}, {});
  BoostQuery boostedWrappedMandOptBody(&wrappedMandOptBody, 3.0f);
  std::vector<Query*> wrappedMandOptClause = {&boostedWrappedMandOptBody};
  BooleanQuery wrappedMidMandOptQuery(
      wrappedMandOptClause, {}, {}, midFilters);
  BooleanQuery compoundMandatory({}, unionOptional, {}, {}, 1);
  BoostQuery boostedCompoundMandatory(&compoundMandatory, 3.0f);
  std::vector<Query*> compoundMandatoryClause = {&boostedCompoundMandatory};
  BooleanQuery midCompoundMandOpt(
      compoundMandatoryClause, oneOptional, {}, midFilters);
  BooleanQuery denseCompoundMandOpt(
      compoundMandatoryClause, oneOptional, {}, filters);
  BooleanQuery compoundMandOptBody(
      compoundMandatoryClause, oneOptional, {}, {});
  std::vector<Query*> compoundBodyClause = {&compoundMandOptBody};
  BooleanQuery wrappedMidCompoundMandOpt(
      compoundBodyClause, {}, {}, midFilters);
  std::vector<Query*> denseCompoundBodyClause = {&denseCompoundMandOpt};
  BooleanQuery multiLevelCompoundMandOpt(
      denseCompoundBodyClause, {}, {}, midFilters);

  auto expectedTerm = runQueryTopK(*reader, termQuery, topK, false);
  auto actualTerm = runBulk(termQuery, typeid(TermQuery::TermBulkScorer));
  assertTopKEquivalent(expectedTerm.topDocs, actualTerm.topDocs);

  auto expectedConjunction = runQueryTopK(
      *reader, conjunctionQuery, topK, false);
  auto actualConjunction = runBulk(
      conjunctionQuery, typeid(BooleanQuery::ConjunctionBulkScorer));
  assertTopKEquivalent(expectedConjunction.topDocs, actualConjunction.topDocs);

  auto expectedMandOpt = runQueryTopK(*reader, mandOptQuery, topK, false);
  auto actualMandOpt = runBulk(
      mandOptQuery, typeid(BooleanQuery::MandOptBulkScorer));
  assertTopKEquivalent(expectedMandOpt.topDocs, actualMandOpt.topDocs);

  // The 32-doc filter is above the general /256 mask knee but below the /16
  // scalar-fill knee. A direct term mandatory retains bulk; a compound
  // mandatory using Scorer's scalar fill falls back to pull. Dense filters
  // still keep the compound bulk route.
  EXPECT_FALSE(BooleanQuery::filterDensityBelow(1, 15, 16));
  EXPECT_FALSE(BooleanQuery::filterDensityBelow(1, 16, 16));
  EXPECT_TRUE(BooleanQuery::filterDensityBelow(1, 17, 16));
  ASSERT_FALSE(BooleanQuery::filterDensityBelow(
      32, 1024, BooleanQuery::kMaskFilterDensityInverse));
  ASSERT_TRUE(BooleanQuery::filterDensityBelow(
      32, 1024, BooleanQuery::kMandOptScalarFillDensityInverse));
  auto expectedMidTerm = runQueryTopK(
      *reader, midMandOptQuery, topK, false);
  auto actualMidTerm = runBulk(
      midMandOptQuery, typeid(BooleanQuery::MandOptBulkScorer));
  assertTopKEquivalent(expectedMidTerm.topDocs, actualMidTerm.topDocs);

  struct ExactMandOptRun {
    QueryTopKRun result;
    std::type_index bulkType;
    int64_t compositions;
    int64_t termFeeds;
  };
  auto runExactMandOpt = [&](Query& query, bool disableComposition) {
    ExactFilteredMandOptCompositionGuard compositionGuard(
        disableComposition);
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    auto& segment = context.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    auto* bulk = supplier == nullptr ? nullptr : supplier->bulkScorer(pool);
    EXPECT_NE(bulk, nullptr);
    TopDocsCollector collector(topK);
    if (bulk != nullptr) {
      collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                          segment.maxDoc(), false);
    }
    return ExactMandOptRun{
        QueryTopKRun{collector.totalHits(), sortedCollectorDocs(collector)},
        bulk == nullptr ? std::type_index(typeid(void))
                        : std::type_index(typeid(*bulk)),
        SkipStats::exactFilteredMandOptCompositions,
        SkipStats::candidateTermFeedEngagements};
  };

  ExactMandOptRun filterLeadOff = runExactMandOpt(midMandOptQuery, true);
  ExactMandOptRun filterLeadOn = runExactMandOpt(midMandOptQuery, false);
  EXPECT_EQ(filterLeadOff.bulkType,
            std::type_index(typeid(BooleanQuery::MandOptBulkScorer)));
  EXPECT_EQ(filterLeadOn.bulkType,
            std::type_index(typeid(BooleanQuery::OptionalScoreBulkScorer)));
  EXPECT_EQ(0, filterLeadOff.compositions);
  EXPECT_EQ(1, filterLeadOn.compositions);
  EXPECT_EQ(0, filterLeadOn.termFeeds);
  assertQueryTopKExact(filterLeadOff.result, filterLeadOn.result);

  ExactMandOptRun termLeadOff = runExactMandOpt(
      termLeadMandOptQuery, true);
  ExactMandOptRun termLeadOn = runExactMandOpt(
      termLeadMandOptQuery, false);
  EXPECT_EQ(termLeadOn.bulkType,
            std::type_index(typeid(BooleanQuery::OptionalScoreBulkScorer)));
  EXPECT_EQ(1, termLeadOn.compositions);
  EXPECT_EQ(1, termLeadOn.termFeeds);
  EXPECT_EQ(2, termLeadOn.result.visited);
  EXPECT_EQ(2u, termLeadOn.result.topDocs.size());
  assertQueryTopKExact(termLeadOff.result, termLeadOn.result);

  QueryTopKRun missingOptionalExpected = runQueryTopK(
      *reader, missingOptionalMandOptQuery, topK, false);
  ExactMandOptRun missingOptionalOn = runExactMandOpt(
      missingOptionalMandOptQuery, false);
  EXPECT_EQ(missingOptionalOn.bulkType,
            std::type_index(typeid(BooleanQuery::ConjunctionBulkScorer)));
  EXPECT_EQ(1, missingOptionalOn.compositions);
  EXPECT_FALSE(missingOptionalOn.result.topDocs.empty());
  assertQueryTopKExact(
      missingOptionalExpected, missingOptionalOn.result);

  // TopDocs folds an external filter around the scored body. Lifting a sole
  // boosted MandOpt child must expose that filter without changing score bits.
  ExactMandOptRun wrappedOff = runExactMandOpt(
      wrappedMidMandOptQuery, true);
  ExactMandOptRun wrappedOn = runExactMandOpt(
      wrappedMidMandOptQuery, false);
  EXPECT_EQ(wrappedOff.bulkType,
            std::type_index(typeid(BooleanQuery::MandOptBulkScorer)));
  EXPECT_EQ(wrappedOn.bulkType,
            std::type_index(typeid(BooleanQuery::OptionalScoreBulkScorer)));
  EXPECT_EQ(0, wrappedOff.compositions);
  EXPECT_EQ(1, wrappedOn.compositions);
  assertQueryTopKExact(wrappedOff.result, wrappedOn.result);

  auto expectedMidCompound = runQueryTopK(
      *reader, midCompoundMandOpt, topK, false);
  {
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = context.topReader.segments()[0];
    auto* weight = midCompoundMandOpt.createWeight(
        context, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    EXPECT_EQ(nullptr, supplier->bulkScorer(pool));
    EXPECT_EQ(1, SkipStats::mandOptBulkScalarFillFallbacks);
  }
  {
    FilteredMandOptFillGateGuard gateGuard(true);
    auto legacyMidCompound = runBulk(
        midCompoundMandOpt, typeid(BooleanQuery::MandOptBulkScorer));
    assertTopKEquivalent(
        expectedMidCompound.topDocs, legacyMidCompound.topDocs);
  }

  // Prepared op-level filters wrap the complete MandOpt body as one child.
  // The enclosing supplier must pass its filter context into that child's
  // bulk admission instead of relying on its own syntactic body shape.
  auto expectedWrappedMidCompound = runQueryTopK(
      *reader, wrappedMidCompoundMandOpt, topK, false);
  {
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = context.topReader.segments()[0];
    auto* weight = wrappedMidCompoundMandOpt.createWeight(
        context, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    EXPECT_EQ(nullptr, supplier->bulkScorer(pool));
    EXPECT_EQ(1, SkipStats::mandOptBulkScalarFillFallbacks);
  }
  {
    FilteredMandOptFillGateGuard gateGuard(true);
    auto legacyWrappedMidCompound = runBulk(
        wrappedMidCompoundMandOpt,
        typeid(BooleanQuery::MandOptBulkScorer));
    assertTopKEquivalent(
        expectedWrappedMidCompound.topDocs,
        legacyWrappedMidCompound.topDocs);
  }

  // A sole required child lifts its rank-only optional and merges both filter
  // layers. The resulting compound mandatory still declines scalar-fill bulk
  // under the selective combined filter.
  auto expectedMultiLevelCompound = runQueryTopK(
      *reader, multiLevelCompoundMandOpt, topK, false);
  EXPECT_FALSE(expectedMultiLevelCompound.topDocs.empty());
  {
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto& segment = context.topReader.segments()[0];
    auto* weight = multiLevelCompoundMandOpt.createWeight(
        context, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    EXPECT_EQ(nullptr, supplier->bulkScorer(pool));
    EXPECT_EQ(1, SkipStats::mandOptBulkScalarFillFallbacks);
  }

  auto expectedDenseCompound = runQueryTopK(
      *reader, denseCompoundMandOpt, topK, false);
  auto actualDenseCompound = runBulk(
      denseCompoundMandOpt, typeid(BooleanQuery::MandOptBulkScorer));
  assertTopKEquivalent(
      expectedDenseCompound.topDocs, actualDenseCompound.topDocs);

  auto expectedUnion = runQueryTopK(*reader, unionQuery, topK, false);
  {
    SkipStatsGuard stats;
    auto actualUnion = runBulk(
        unionQuery, typeid(BooleanQuery::MaxScoreBulkScorer));
    assertTopKEquivalent(expectedUnion.topDocs, actualUnion.topDocs);
    EXPECT_GT(SkipStats::maxScoreOuterWindows, 0);
    EXPECT_GT(SkipStats::maxScoreInnerWindows, 0);
  }
  EXPECT_EQ(countBulk(unionQuery), expectedUnion.visited);

  // The routing assertion below is only meaningful while this filter stays
  // under the mask-density gate; fail loudly if the constant moves past it.
  ASSERT_LT(2, 1024 / (int32_t) BooleanQuery::kMaskFilterDensityInverse + 1);
  std::vector<Query*> selectiveFilter = {&selective};
  BooleanQuery selectiveQuery(oneMandatory, {}, {}, selectiveFilter);
  auto expectedSelective = runQueryTopK(
      *reader, selectiveQuery, topK, false);
  auto actualSelective = runBulk(
      selectiveQuery, typeid(BooleanQuery::ConjunctionBulkScorer));
  assertTopKEquivalent(
      expectedSelective.topDocs, actualSelective.topDocs);

  std::vector<std::string_view> phraseTerms = {"quick", "fox"};
  std::vector<int32_t> phrasePositions = {0, 1};
  PhraseQuery phraseFilter("body_w", phraseTerms, phrasePositions);
  std::vector<Query*> phraseFilters = {&phraseFilter};
  BooleanQuery phraseFilterQuery(oneMandatory, {}, {}, phraseFilters);
  {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = phraseFilterQuery.createWeight(context, Query::NEED_SCORES);
    auto* supplier = weight->scorerSupplier(
        pool, context.topReader.segments()[0]);
    ASSERT_NE(supplier, nullptr);
    EXPECT_EQ(supplier->bulkScorer(pool), nullptr);
  }
}

TEST_F(TermScorerTest, SparseFilteredTermUnionWandMatchesDisjunctionPull) {
  const int32_t nDocs = 2 * DocsEnumMeta::L1_DOCS + 257;
  TestIndex testIndex;
  TestField bodyField(testIndex, "body_w");
  TestField rangeField(testIndex, "wand_filter_i");
  bodyField.startIndexing();
  rangeField.startIndexing();
  int32_t filterCount = 0;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "wand_common";
    bool matchesFilter = (doc % 300) == 0;  // stays under the mask-density gate
    if (matchesFilter) {
      body += " wand_filter";
      int32_t filterOrd = filterCount++;
      if (filterOrd < 120) {
        appendRepeatedTerm(body, "wand_peak_a", 2 + filterOrd % 13);
      }
      if ((filterOrd % 3) == 0) {
        appendRepeatedTerm(body, "wand_peak_b", 2 + filterOrd % 9);
      }
    }
    appendRepeatedTerm(body, "wand_pad", 4 + doc % 3);
    bodyField.add(doc, body);
    if (matchesFilter) rangeField.add(doc, 1);
  }
  testIndex.flush();
  testIndex.initReader();
  auto reader = testIndex.reader;
  ASSERT_EQ(reader->maxDoc(), nDocs);
  ASSERT_GT(reader->maxDoc(), DocsEnumMeta::L1_DOCS);
  ASSERT_LT(filterCount,
            reader->maxDoc() / BooleanQuery::kMaskFilterDensityInverse);

  TermQuery common("body_w", "wand_common");
  TermQuery peakA("body_w", "wand_peak_a");
  TermQuery peakB("body_w", "wand_peak_b");
  TermQuery filter("body_w", "wand_filter");
  NumericRangeQuery rangeFilter("wand_filter_i", 1, 1);
  std::vector<Query*> optional = {&common, &peakA, &peakB};

  struct Run {
    std::vector<TopDocsCollector::ScoreDoc> topDocs;
    int64_t advancePrunes;
    int64_t candidatePrunes;
  };
  auto run = [&](BooleanQuery& query, bool disableWand) {
    FilteredUnionWandGuard wandGuard(disableWand);
    SkipStatsGuard stats;
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(
        context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto& segment = context.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    EXPECT_NE(supplier, nullptr);
    EXPECT_EQ(supplier == nullptr ? nullptr : supplier->bulkScorer(pool),
              nullptr);
    auto* scorer = supplier == nullptr
        ? nullptr
        : buildScorerForTests(
              pool, *supplier, std::numeric_limits<int64_t>::max());
    EXPECT_NE(dynamic_cast<BooleanQuery::ConjunctionScorer*>(scorer), nullptr);
    TopDocsCollector collector(20);
    if (scorer != nullptr) {
      collectTopK(0, scorer, nullptr, nullptr, collector, true);
    }
    return Run{sortedCollectorDocs(collector),
               SkipStats::wandAdvancePrunes,
               SkipStats::wandCandidatePrunes};
  };

  for (Query* filterQuery : std::array<Query*, 2>{&filter, &rangeFilter}) {
    SCOPED_TRACE(filterQuery == &filter ? "term filter" : "range filter");
    std::vector<Query*> filters = {filterQuery};
    BooleanQuery query({}, optional, {}, filters, 1);
    Run disjunction = run(query, true);
    Run wand = run(query, false);
    ASSERT_EQ(disjunction.topDocs.size(), wand.topDocs.size());
    std::vector<segdoc> disjunctionDocs;
    std::vector<segdoc> wandDocs;
    for (const auto& hit : disjunction.topDocs) {
      disjunctionDocs.push_back(hit.doc);
    }
    for (const auto& hit : wand.topDocs) wandDocs.push_back(hit.doc);
    std::sort(disjunctionDocs.begin(), disjunctionDocs.end());
    std::sort(wandDocs.begin(), wandDocs.end());
    EXPECT_EQ(disjunctionDocs, wandDocs);
    EXPECT_EQ(disjunction.advancePrunes, 0);
    EXPECT_EQ(disjunction.candidatePrunes, 0);
    EXPECT_GT(wand.advancePrunes, 0);
  }
}

TEST_F(TermScorerTest, FilteredConjunctionClampsSparseProductionWindows) {
  const int32_t nDocs = 4 * DocsEnumMeta::L1_DOCS + 37;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body = "pad";
    if ((doc % 2) == 0) body += " keep";
    if ((doc % 3) == 0) body += " body_a";
    if ((doc % 5) == 0) body += " body_b";
    field.add(doc, body);
  }
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;

  TermQuery bodyA("body_w", "body_a");
  TermQuery bodyB("body_w", "body_b");
  TermQuery keep("body_w", "keep");
  std::vector<Query*> mandatory = {&bodyA, &bodyB};
  std::vector<Query*> filters = {&keep};
  BooleanQuery query(mandatory, {}, {}, filters);
  constexpr int32_t topK = 100;
  auto expected = runQueryTopK(*reader, query, topK, false);

  MemPool pool;
  Query::Context context(pool, *reader);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  auto& segment = context.topReader.segments()[0];
  auto* supplier = weight->scorerSupplier(pool, segment);
  ASSERT_NE(supplier, nullptr);
  auto* bulk = supplier->bulkScorer(pool);
  ASSERT_NE(dynamic_cast<BooleanQuery::ConjunctionBulkScorer*>(bulk), nullptr);

  TopDocsCollector collector(topK);
  collectTopKWindowed(0, bulk, nullptr, collector, nullptr,
                      segment.maxDoc(), true);
  QueryTopKRun actual{collector.totalHits(), sortedCollectorDocs(collector)};
  assertTopKEquivalent(expected.topDocs, actual.topDocs);
}

TEST_F(TermScorerTest, maxScoreDisjunctionPathsMatchReferences) {
  CollectionHelper helper("main");
  addMaxScoreDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t totalDocs = 3 * kMaxScoreDisjunctionSegDocs;
  std::array<std::string_view, 3> terms = {"common", "medium", "rare"};

  {
    for (int32_t k : {3, totalDocs + 10}) {
      auto expected = runExhaustiveDisjunctionTopK(*reader, k);
      auto actual = runMaxScoreDisjunctionTopK(*reader, k);
      assertSameTopKDocs(expected, actual, k);
      if (k == 3) {
        EXPECT_LT(actual.visited, expected.visited);
        // The demoted-clause probe filter can legitimately drive
        // nonEssentialLookups to zero: candidates whose essential sum plus the
        // demoted bounds cannot compete are abandoned before any probe.
      } else {
        EXPECT_EQ(actual.visited, expected.visited);
        EXPECT_EQ(actual.nonEssentialLookups, 0);
      }
    }
  }

  {
    for (int32_t k : {3, totalDocs + 10}) {
      auto expected = runExhaustiveDisjunctionTopK(*reader, k);
      auto global = runMaxScoreDisjunctionTopK(
          *reader, k, std::numeric_limits<int32_t>::max());
      auto windowed = runMaxScoreDisjunctionTopK(*reader, k, 256);
      assertSameTopKDocs(expected, global, k);
      assertSameTopKDocs(expected, windowed, k);
      assertSameTopKDocs(global, windowed, k);
      if (k == 3) {
        EXPECT_LT(windowed.visited, expected.visited);
        EXPECT_LE(windowed.visited, global.visited);
      } else {
        EXPECT_EQ(windowed.visited, expected.visited);
        EXPECT_EQ(windowed.nonEssentialLookups, 0);
      }
    }
  }

  {
    for (int32_t k : {3, totalDocs + 10}) {
      auto exhaustive = runExhaustiveDisjunctionTopK(*reader, k);
      auto baseline = runMaxScoreDisjunctionTopK(*reader, k);
      auto bulk = runBulkTermDisjunctionTopK(*reader, terms, k, false, nullptr);
      assertSameTopKDocs(exhaustive, bulk, k);
      assertSameTopKDocs(baseline, bulk, k);
    }
  }

  {
    const int32_t k = 3;
    MaxScoreAccumulator accumulator;
    auto exhaustive = runExhaustiveDisjunctionTopK(*reader, k);
    auto baseline = runMaxScoreDisjunctionTopK(*reader, k);
    auto bulk = runBulkTermDisjunctionTopK(*reader, terms, k, true, &accumulator);
    assertSameTopKDocs(exhaustive, bulk, k);
    assertSameTopKDocs(baseline, bulk, k);
    ASSERT_GT(accumulator.get(), std::numeric_limits<float>::lowest());
  }

  {
    MemPool pool;
    Query::Context qContext(pool, *reader);
    auto& segment = qContext.topReader.segments()[0];
    auto queries = makeTermQueries(terms);
    auto optional = queryPointers(queries);
    std::span<Query*> empty;
    const int32_t topK = 3;

    BooleanQuery pullQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
    auto* pullWeight = pullQ.createWeight(qContext, Query::NEED_SCORES);
    DocSetBuilder pullBuilder(segment.maxDoc());
    TopDocsCollector pullCollector(topK);
    auto* pullScorer = pullWeight->createScorer(pool, segment);
    ASSERT_NE(pullScorer, nullptr);
    collectTopK(0, pullScorer, nullptr, &pullBuilder, pullCollector,
                /*allowPruning=*/false);
    auto pullDomainSet = pullBuilder.build();

    BooleanQuery bulkQ(empty, std::span<Query*>(optional.data(), optional.size()), empty, empty);
    auto* bulkWeight = bulkQ.createWeight(qContext, Query::NEED_SCORES);
    auto* supplier = bulkWeight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = supplier->bulkScorer(pool);
    ASSERT_NE(bulk, nullptr);
    DocSetBuilder bulkBuilder(segment.maxDoc());
    int64_t bulkCount = countMatchesWindowed(
        bulk, nullptr, &bulkBuilder, segment.maxDoc());

    auto* rankingSupplier = bulkWeight->scorerSupplier(pool, segment);
    ASSERT_NE(rankingSupplier, nullptr);
    auto* rankingBulk = rankingSupplier->bulkScorer(pool);
    ASSERT_NE(rankingBulk, nullptr);
    TopDocsCollector bulkCollector(topK);
    MaxScoreAccumulator accumulator;
    int64_t before = bulkCollector.totalHits();
    collectTopKWindowed(0, rankingBulk, nullptr, bulkCollector, &accumulator,
                        segment.maxDoc(), /*allowPruning=*/true);
    int64_t after = bulkCollector.totalHits();
    ASSERT_GE(bulkCount, after - before);
    bulkCollector.hitCount += bulkCount - (after - before);
    auto bulkDomainSet = bulkBuilder.build();

    EXPECT_EQ(bulkCollector.totalHits(), pullCollector.totalHits());
    assertTopKEquivalent(sortedCollectorDocs(pullCollector),
                         sortedCollectorDocs(bulkCollector));
    expectDocSetEqual(bulkDomainSet.get(), pullDomainSet.get(), segment.maxDoc());
  }
}

TEST_F(TermScorerTest, CompetitiveScoreThresholdSeededMatchesReference) {
  struct ThresholdCase {
    float mcs;
    double factor;
    double bound;
  };

  std::vector<ThresholdCase> cases = {
    {0.0f, 1.0, 0.0},
    {-1.0f, 1.0, 0.0},
    {1.0f, 1.0, 0.0},
    {std::numeric_limits<float>::lowest(), 1.0, 0.0},
    {std::numeric_limits<float>::max(), 1.0, 0.0},
    {std::numeric_limits<float>::denorm_min(), 1.0 + 0x1p-24, 0.0},
    {-std::numeric_limits<float>::denorm_min(), 1.0 + 0x1p-24, 0.0},
    {std::numeric_limits<float>::infinity(), 1.0, 0.0},
    {-std::numeric_limits<float>::infinity(), 1.0, 0.0},
    {17.0f, std::numeric_limits<double>::infinity(), 0.0},
    {17.0f, 1.0, std::numeric_limits<double>::infinity()},
    {17.0f, 1.0, -std::numeric_limits<double>::infinity()},
    {42.0f, 1.0 + 0x1p-24, 42.0 / (1.0 + 0x1p-24)},
    {42.0f, 1.0 + 0x1p-24, 42.0 / (1.0 + 0x1p-24) - 0x1p-40},
    {42.0f, 1.0 + 0x1p-24, 42.0 / (1.0 + 0x1p-24) + 0x1p-40},
    {-42.0f, 3.5, -42.0 / 3.5 - 0x1p-30},
    {std::numeric_limits<float>::max(), 0x1p-64, 0.0},
    {-std::numeric_limits<float>::max(), 0x1p-64, 0.0}
  };

  uint32_t state = 0xdeadbeefU;
  auto nextUnit = [&]() {
    state = state * 1664525U + 1013904223U;
    return (double) (state >> 8) / (double) (1U << 24);
  };
  for (int32_t i = 0; i < 200; i++) {
    int32_t exp = -80 + (int32_t) (nextUnit() * 160.0);
    double raw = (nextUnit() * 2.0 - 1.0) * std::ldexp(1.0, exp);
    float mcs = (float) raw;
    double factor = 0.25 + nextUnit() * 8.0;
    double cancellation = (double) mcs / factor;
    double offset = (nextUnit() * 2.0 - 1.0) * std::ldexp(1.0, exp - 24);
    cases.push_back({mcs, factor, cancellation + offset});
    cases.push_back({mcs, factor, offset});
  }

  for (const auto& testCase : cases) {
    float expected = competitiveScoreThresholdReference(testCase.mcs, testCase.factor,
                                                        testCase.bound);
    float actual = competitiveScoreThreshold(testCase.mcs, testCase.factor, testCase.bound);
    EXPECT_EQ(std::bit_cast<uint32_t>(actual), std::bit_cast<uint32_t>(expected))
        << "mcs=" << testCase.mcs << " factor=" << testCase.factor
        << " bound=" << testCase.bound;
  }

  int32_t docs[] = {1, 2, 3, 4};
  float scores[] = {
    std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f, 2.0f
  };
  int32_t strictDocs[] = {1, 2, 3, 4};
  float strictScores[] = {
    std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f, 2.0f
  };
  int32_t strictKept = compactByScoreThreshold(strictDocs, strictScores, 4, 1.0f);
  EXPECT_EQ(strictKept, 2);
  EXPECT_EQ(strictDocs[0], 3);
  EXPECT_EQ(strictDocs[1], 4);

  int32_t finishKept = compactByScoreNotLessThanThreshold(docs, scores, 4, 1.0f);
  EXPECT_EQ(finishKept, 3);
  EXPECT_EQ(docs[0], 1);
  EXPECT_TRUE(std::isnan(scores[0]));
  EXPECT_EQ(docs[1], 3);
  EXPECT_EQ(docs[2], 4);
}

void checkMaxScoreBulkScorerBufferSweepsMatchExhaustiveAcrossShapes(
    IndexReader& reader, int32_t maxClauses, int32_t effort) {
  auto termStrings = makeSweepTermStrings(maxClauses);
  auto views = termViews(termStrings);

  int64_t sweepWindows = 0;
  int64_t compactionDrops = 0;
  auto check = [&](int32_t clauses, int32_t topK) {
    std::span<const std::string_view> terms(views.data(), (size_t) clauses);
    auto expected = runFilteredExhaustiveTermDisjunctionTopK(reader, terms, topK);
    SkipStatsGuard stats;
    auto actual = runFilteredBulkTermDisjunctionTopK(reader, terms, topK);
    sweepWindows += SkipStats::maxScoreSweepWindows;
    compactionDrops += SkipStats::maxScoreBufferCompactions;
    assertSameTopKDocs(expected, actual, topK);
  };
  if (effort == 1) {
    for (int32_t clauses : {2, 3, 6}) {
      check(clauses, 11);
    }
    for (int32_t topK : {5, 3 * DocsEnumMeta::L1_DOCS + 500}) {
      check(6, topK);
    }
  } else {
    for (int32_t clauses : {2, 3, 6}) {
      for (int32_t topK : {5, 11, 3 * DocsEnumMeta::L1_DOCS + 500}) {
        check(clauses, topK);
      }
    }
  }

  EXPECT_GT(sweepWindows, 0);
  EXPECT_GT(compactionDrops, 0);
}

void checkMaxScoreBulkScorerCostAwareOrderIsGuardedAndExact(IndexReader& reader) {
  std::vector<TermQuery> queries;
  queries.emplace_back("body_w", "sweep0", 8.0f);
  queries.emplace_back("body_w", "sweep1");
  queries.emplace_back("body_w", "sweep2");
  queries.emplace_back("body_w", "sweep3");
  auto optional = queryPointers(queries);
  std::span<Query*> empty;

  for (int32_t clauses : {3, 4}) {
    std::span<Query*> selected(optional.data(), (size_t) clauses);
    auto expected = runBooleanTopK(reader, empty, selected, 11, false);
    bool usedCostOrder = false;
    auto actual = runBulkBooleanTopK(reader, selected, 11, &usedCostOrder);
    EXPECT_EQ(clauses >= 4, usedCostOrder);
    assertSameTopKDocs(expected, actual, 11);
  }
}

TEST_F(TermScorerTest, MaxScoreBulkScorerWindowDispatchMatchesDisabledAcrossRandomizedUnions) {
  for (bool tinySegment : {true, false}) {
    const int32_t maxClauses = 8;
    int32_t nDocs = tinySegment ? 997 : 2 * DocsEnumMeta::L1_DOCS + 333;
    TestIndex testIndex;
    TestField field(testIndex, "body_w");
    field.startIndexing();
    addWindowDispatchRandomDocs(field, nDocs, maxClauses);
    if (!tinySegment) {
      for (int32_t doc = 5; doc < nDocs; doc += 13) {
        testIndex.deleteDoc(doc);
      }
    }
    testIndex.flush();
    field.startReading();
    auto reader = testIndex.reader;
    auto termStrings = makeWindowDispatchTermStrings(maxClauses);
    auto views = termViews(termStrings);
    bool liveOnly = !tinySegment;

    BulkDomainDriveGuard domainGuard(true);
    std::vector<int32_t> clauseCounts;
    if (effort == 1) {
      clauseCounts = {2, 3, 4, maxClauses};
    } else {
      for (int32_t clauses = 2; clauses <= maxClauses; clauses++) {
        clauseCounts.push_back(clauses);
      }
    }
    for (int32_t clauses : clauseCounts) {
      std::span<const std::string_view> terms(views.data(), (size_t) clauses);
      for (int32_t mode = 0; mode < 3; mode++) {
        int32_t filterStep = mode == 0 ? 0 : mode == 1 ? 7 : 11;
        bool arrayDocSet = mode == 2;
        int32_t topK = clauses + 5;
        auto disabled = runFilteredBulkTermDisjunctionTopK(
            *reader, terms, topK, filterStep, arrayDocSet, liveOnly, true, true);
        auto enabled = runFilteredBulkTermDisjunctionTopK(
            *reader, terms, topK, filterStep, arrayDocSet, liveOnly, true, false);
        assertSameTopKDocs(disabled, enabled, topK);

        auto pull = runFilteredExhaustiveTermDisjunctionTopK(
            *reader, terms, topK, filterStep, arrayDocSet, liveOnly);
        assertSameTopKDocs(pull, enabled, topK);

        auto disabledExhaustive = runFilteredBulkTermDisjunctionTopK(
            *reader, terms, topK, filterStep, arrayDocSet, liveOnly, false, true);
        auto enabledExhaustive = runFilteredBulkTermDisjunctionTopK(
            *reader, terms, topK, filterStep, arrayDocSet, liveOnly, false, false);
        assertSameTopKDocs(disabledExhaustive, enabledExhaustive, topK);

        EXPECT_EQ(countBulkTermDisjunctionAll(*reader, terms, filterStep,
                                              arrayDocSet, liveOnly),
                  countPullTermDisjunctionAll(*reader, terms, filterStep,
                                              arrayDocSet, liveOnly))
            << "tiny=" << tinySegment << " clauses=" << clauses << " mode=" << mode;
      }
    }
  }
}

TEST_F(TermScorerTest, MaxScoreBulkScorerWindowDispatchBoundaryBehavior) {
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addWindowDispatchBoundaryDocs(field);
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;
  const int32_t maxDoc = 3 * DocsEnumMeta::L1_DOCS + 100;

  auto assertParity = [&](std::span<const std::string_view> terms,
                          int32_t minDoc, int32_t max, float theta) {
    auto disabled = collectBulkWindows(*reader, terms, minDoc, max, theta, true);
    auto enabled = collectBulkWindows(*reader, terms, minDoc, max, theta, false);
    expectBulkWindowRunsEqual(disabled, enabled);
    return enabled;
  };

  std::array<std::string_view, 2> top1AtWindowEnd = {"wd_at_end_a", "wd_at_end_b"};
  auto atWindowEnd = assertParity(top1AtWindowEnd, 0, maxDoc, 0.0f);
  ASSERT_EQ(atWindowEnd.docs.size(), 2);
  EXPECT_EQ(atWindowEnd.docs[0], DocsEnumMeta::L1_DOCS);
  EXPECT_EQ(atWindowEnd.docs[1], 2 * DocsEnumMeta::L1_DOCS);

  std::array<std::string_view, 2> top1AtOuterEnd = {"wd_at_end_a", "wd_at_end_b"};
  auto atOuterEnd = assertParity(top1AtOuterEnd, 0, DocsEnumMeta::L1_DOCS, 0.0f);
  EXPECT_TRUE(atOuterEnd.docs.empty());

  std::array<std::string_view, 2> top2AtWindowEnd = {"wd_top2_end_a", "wd_top2_end_b"};
  auto atTop2End = assertParity(top2AtWindowEnd, 0, maxDoc, 0.0f);
  EXPECT_GT(atTop2End.top2Conversions, 0);

  std::array<std::string_view, 2> sameTop = {"wd_same_a", "wd_same_b"};
  auto sameTopRun = assertParity(sameTop, 0, maxDoc, 0.0f);
  EXPECT_EQ(sameTopRun.top2Conversions, 0);

  std::array<std::string_view, 2> allEnd = {"wd_same_a", "wd_same_b"};
  auto allEndRun = assertParity(allEnd, 2 * DocsEnumMeta::L1_DOCS + 10, maxDoc, 0.0f);
  EXPECT_TRUE(allEndRun.docs.empty());

  std::array<std::string_view, 2> exhausting = {"wd_exhaust_a", "wd_exhaust_b"};
  auto exhaustingRun = assertParity(exhausting, 0, maxDoc, 0.0f);
  ASSERT_EQ(exhaustingRun.docs.size(), 3);
  EXPECT_EQ(exhaustingRun.docs[0], 1000);
  EXPECT_EQ(exhaustingRun.docs[1], 7000);
  EXPECT_EQ(exhaustingRun.docs[2], 11000);
  EXPECT_GT(exhaustingRun.anchorJumps, 0);
  EXPECT_GT(exhaustingRun.top2Conversions, 0);

  std::array<std::string_view, 2> convertedDifferentDriver = {
    "wd_common_late", "wd_rare_driver"
  };
  auto convertedRun = assertParity(convertedDifferentDriver, 0, maxDoc, 0.0f);
  EXPECT_GT(convertedRun.top2Conversions, 0);
  ASSERT_FALSE(convertedRun.docs.empty());
  EXPECT_EQ(convertedRun.docs[0], 1000);

  std::array<std::string_view, 2> halfExact = {"wd_half_exact_a", "wd_half_exact_b"};
  auto halfExactRun = assertParity(halfExact, 0, maxDoc, 0.0f);
  EXPECT_GT(halfExactRun.halfWindowClips, 0);

  std::array<std::string_view, 2> halfInside = {"wd_half_inside_a", "wd_half_inside_b"};
  auto halfInsideRun = assertParity(halfInside, 0, maxDoc, 0.0f);
  EXPECT_EQ(halfInsideRun.halfWindowClips, 0);

  std::array<std::string_view, 2> halfGap = {"wd_half_gap_a", "wd_half_gap_b"};
  auto halfGapRun = assertParity(halfGap, 0, maxDoc, 0.0f);
  EXPECT_GT(halfGapRun.halfWindowClips, 0);

  auto tinyRun = assertParity(halfExact, 0, 100, 0.0f);
  EXPECT_EQ(tinyRun.halfWindowClips, 0);

  {
    std::array<std::string_view, 3> terms = {
        "wd_latch_low", "wd_latch_a", "wd_latch_b"
    };

    auto setup = [&](MemPool& pool, Query::Context& qContext) {
      auto& segment = qContext.topReader.segments()[0];
      auto* bulk = createBulkTermDisjunctionScorer(pool, qContext, segment, terms);
      auto* maxScoreBulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(bulk);
      EXPECT_NE(maxScoreBulk, nullptr);
      ScoreWindow window;
      int32_t next = maxScoreBulk->scoreNextWindow(window, nullptr, 0, maxDoc, 0.0f);
      EXPECT_NE(next, PostingsReader::END);
      EXPECT_LT(next, maxScoreBulk->outerWindowEndForTests());
      return std::pair<BooleanQuery::MaxScoreBulkScorer*, int32_t>(maxScoreBulk, next);
    };

    float boundary = 0.0f;
    {
      PartitionLatchGuard latchEnabled(false);
      SkipStatsGuard stats;
      MemPool pool;
      Query::Context qContext(pool, *reader);
      auto [bulk, next] = setup(pool, qContext);
      ASSERT_NE(bulk, nullptr);
      boundary = bulk->nextPartitionMcsForTests();
      ASSERT_TRUE(std::isfinite(boundary));
      float belowBoundary =
          std::nextafter(boundary, -std::numeric_limits<float>::infinity());
      ASSERT_GT(belowBoundary, 0.0f);
      ScoreWindow window;
      bulk->scoreNextWindow(window, nullptr, next, maxDoc, belowBoundary);
      EXPECT_GT(SkipStats::maxScorePartitionLatchReuses, 0);
      EXPECT_EQ(SkipStats::maxScorePartitionLatchBreaks, 0);
    }

    {
      PartitionLatchGuard latchEnabled(false);
      SkipStatsGuard stats;
      MemPool pool;
      Query::Context qContext(pool, *reader);
      auto [bulk, next] = setup(pool, qContext);
      ASSERT_NE(bulk, nullptr);
      ASSERT_EQ(std::bit_cast<uint32_t>(bulk->nextPartitionMcsForTests()),
                std::bit_cast<uint32_t>(boundary));
      ScoreWindow window;
      bulk->scoreNextWindow(window, nullptr, next, maxDoc, boundary);
      EXPECT_GT(SkipStats::maxScorePartitionLatchBreaks, 0);
    }

    {
      PartitionLatchGuard latchEnabled(false);
      SkipStatsGuard stats;
      MemPool pool;
      Query::Context qContext(pool, *reader);
      auto& segment = qContext.topReader.segments()[0];
      auto* bulk = createBulkTermDisjunctionScorer(pool, qContext, segment, terms);
      auto* maxScoreBulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(bulk);
      ASSERT_NE(maxScoreBulk, nullptr);
      ScoreWindow window;
      int32_t next = maxScoreBulk->scoreNextWindow(window, nullptr, 0, maxDoc, boundary);
      ASSERT_NE(next, PostingsReader::END);
      ASSERT_LT(next, maxScoreBulk->outerWindowEndForTests());
      float nextBoundary = maxScoreBulk->nextPartitionMcsForTests();
      ASSERT_TRUE(std::isfinite(nextBoundary));
      float belowNextBoundary =
          std::nextafter(nextBoundary, -std::numeric_limits<float>::infinity());
      ASSERT_GT(belowNextBoundary, boundary);
      maxScoreBulk->scoreNextWindow(window, nullptr, next, maxDoc, belowNextBoundary);
      EXPECT_GT(SkipStats::maxScorePartitionLatchReuses, 0);
      EXPECT_GT(SkipStats::maxScoreThresholdRefreshes, 0);
    }
  }

  {
    std::array<std::string_view, 2> deadTerms = {
        "wd_dead_dense_a", "wd_dead_dense_b"
    };
    auto deadRun = collectBulkWindows(*reader, deadTerms, 0, maxDoc, 1.0e30f, false);
    EXPECT_TRUE(deadRun.docs.empty());
    EXPECT_GT(deadRun.deadOuterJumps, 0);

    std::array<std::string_view, 2> anchoredTerms = {
        "wd_exhaust_a", "wd_exhaust_b"
    };
    auto anchoredRun =
        collectBulkWindows(*reader, anchoredTerms, 0, maxDoc, 0.0f, false);
    EXPECT_GT(anchoredRun.anchorJumps, 0);
    EXPECT_GT(anchoredRun.top2Conversions, 0);

    std::array<std::string_view, 2> halfTerms = {
        "wd_half_exact_a", "wd_half_exact_b"
    };
    auto halfRun = collectBulkWindows(*reader, halfTerms, 0, maxDoc, 0.0f, false);
    EXPECT_GT(halfRun.halfWindowClips, 0);

    std::array<std::string_view, 3> latchTerms = {
        "wd_latch_low", "wd_latch_a", "wd_latch_b"
    };
    {
      PartitionLatchGuard latchEnabled(false);
      SkipStatsGuard stats;
      auto latchRun = runFilteredBulkTermDisjunctionTopK(*reader, latchTerms, 3);
      unused(latchRun);
      EXPECT_GT(SkipStats::maxScorePartitionLatchReuses, 0);
    }
  }

}

void checkMaxScoreBulkScorerPartitionLatchMatchesDisabledAcrossRisingTheta(
    IndexReader& reader, int32_t maxClauses) {
  auto termStrings = makeSweepTermStrings(maxClauses);
  auto views = termViews(termStrings);

  BulkDomainDriveGuard domainGuard(true);
  for (int32_t clauses = 2; clauses <= maxClauses; clauses++) {
    std::span<const std::string_view> terms(views.data(), (size_t) clauses);
    PartitionLatchGuard disabledLatch(true);
    auto disabled = runFilteredBulkTermDisjunctionTopK(reader, terms, clauses + 3);
    {
      PartitionLatchGuard enabledLatch(false);
      SkipStatsGuard stats;
      auto enabled = runFilteredBulkTermDisjunctionTopK(reader, terms, clauses + 3);
      assertSameTopKDocs(disabled, enabled, clauses + 3);
      auto exhaustive = runFilteredExhaustiveTermDisjunctionTopK(reader, terms, clauses + 3);
      assertSameTopKDocs(exhaustive, enabled, clauses + 3);
    }
  }
}

TEST_F(TermScorerTest, MaxScoreBulkScorerSweepCorpusChecks) {
  constexpr int32_t maxClauses = 8;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addSweepDisjunctionDocs(field, maxClauses);
  testIndex.flush();
  field.startReading();

  {
    SCOPED_TRACE("BufferSweepsMatchExhaustiveAcrossShapes");
    checkMaxScoreBulkScorerBufferSweepsMatchExhaustiveAcrossShapes(
        *testIndex.reader, 6, effort);
  }
  {
    SCOPED_TRACE("CostAwareOrderIsGuardedAndExact");
    checkMaxScoreBulkScorerCostAwareOrderIsGuardedAndExact(*testIndex.reader);
  }
  {
    SCOPED_TRACE("PartitionLatchMatchesDisabledAcrossRisingTheta");
    checkMaxScoreBulkScorerPartitionLatchMatchesDisabledAcrossRisingTheta(
        *testIndex.reader, maxClauses);
  }
}

TEST_F(TermScorerTest, ScoredWordProbeApplyToCandidatesMatchesPerDocAdvanceAcrossBlockShapes) {
  CollectionHelper helper("main");
  auto postings = makeMixedProbePostings();
  indexProbeTermDocs(helper, "probe_mix", postings, "probe_mix");
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t packedOrd = Postings::DOCS_BLOCK_SIZE;
  const int32_t wordOrd = 2 * Postings::DOCS_BLOCK_SIZE;

  std::vector<int32_t> candidates = {
    0, 1, 63, 127,
    postings[(size_t) packedOrd],
    postings[(size_t) packedOrd] + 1,
    postings[(size_t) packedOrd + 1],
    postings[(size_t) packedOrd + 22],
    postings[(size_t) packedOrd + 22] + 1,
    postings[(size_t) wordOrd],
    postings[(size_t) wordOrd] + 1,
    postings[(size_t) wordOrd + 1],
    postings[(size_t) wordOrd + 5],
    postings[(size_t) wordOrd + 34],
    postings[(size_t) wordOrd + 70],
    postings[(size_t) wordOrd + 127],
    postings.back() + 1
  };
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

  for (bool required : {false, true}) {
    auto expected = runPerDocTermCandidateSweep(*reader, "probe_mix", candidates, required);
    auto actual = runApplyTermCandidateSweep(*reader, "probe_mix", candidates,
                                             required, !required);
    expectCandidateSweepEqual(expected, actual);
    if (!required) {
      EXPECT_GT(actual.scoredWordProbes, 0);
    }
  }
}

TEST_F(TermScorerTest, CandidateLeapfrogPreservesBatchedScoreAndMatchCompaction) {
  CollectionHelper helper("main");
  std::vector<int32_t> postings = {
    DocsEnumMeta::L1_DOCS + 904,
    DocsEnumMeta::L1_DOCS + 906
  };
  const int32_t batchSize = DocsEnumMeta::L1_DOCS;
  indexProbeTermDocs(
      helper, "leapfrog", postings, "leapfrog",
      2 * batchSize - postings.back());
  auto reader = helper.getIndexWriter()->getIndexReader();
  std::vector<int32_t> candidates((size_t) (2 * batchSize));
  std::iota(candidates.begin(), candidates.end(), 0);

  auto runScored = [&](bool disabled, bool required) {
    CandidateLeapfrogGuard guard(disabled);
    MemPool pool;
    Query::Context qContext(pool, *reader);
    TermQuery query("body_w", "leapfrog");
    auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
    auto& segment = qContext.topReader.segments()[0];
    auto* scorer = weight->createScorer(pool, segment);
    EXPECT_NE(scorer, nullptr);

    CandidateSweepRun run;
    run.docs = candidates;
    run.scores = initialCandidateScores((int32_t) candidates.size());
    int32_t firstSize = scorer->applyToCandidates(
        run.docs.data(), run.scores.data(), batchSize, required);
    int32_t secondSize = scorer->applyToCandidates(
        run.docs.data() + batchSize, run.scores.data() + batchSize,
        batchSize, required);
    if (required) {
      run.docs.erase(
          run.docs.begin() + firstSize,
          run.docs.begin() + batchSize);
      run.scores.erase(
          run.scores.begin() + firstSize,
          run.scores.begin() + batchSize);
      run.docs.resize((size_t) (firstSize + secondSize));
      run.scores.resize((size_t) (firstSize + secondSize));
    }
    return run;
  };

  for (bool required : {false, true}) {
    auto expected = runScored(true, required);
    auto actual = runScored(false, required);
    expectCandidateSweepEqual(expected, actual);
    if (required) {
      EXPECT_EQ(actual.docs, postings);
    }
  }

  auto runMatches = [&](bool disabled) {
    CandidateLeapfrogGuard guard(disabled);
    MemPool pool;
    Query::Context qContext(pool, *reader);
    TermQuery query("body_w", "leapfrog");
    auto* weight = query.createWeight(qContext, 0);
    auto& segment = qContext.topReader.segments()[0];
    auto* scorer = (TermQuery::Scorer*) weight->createScorer(pool, segment);
    EXPECT_NE(scorer, nullptr);
    std::vector<uint64_t> matched((size_t) (2 * batchSize / 64));
    scorer->addMatchesToCandidates(
        candidates.data(), batchSize,
        std::span<uint64_t>(matched.data(), (size_t) (batchSize / 64)));
    scorer->addMatchesToCandidates(
        candidates.data() + batchSize, batchSize,
        std::span<uint64_t>(
            matched.data() + batchSize / 64, (size_t) (batchSize / 64)));
    return matched;
  };

  EXPECT_EQ(runMatches(false), runMatches(true));
}

TEST_F(TermScorerTest, RetainMatchesCompactsAcrossPostingsBlockShapesAndBatches) {
  CollectionHelper helper("main");
  auto postings = makeMixedProbePostings();
  indexProbeTermDocs(helper, "retain_mix", postings, "retain_mix");
  auto reader = helper.getIndexWriter()->getIndexReader();

  MemPool pool;
  Query::Context qContext(pool, *reader);
  TermQuery query("body_w", "retain_mix");
  auto* weight = query.createWeight(qContext, 0);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      weight->createScorer(pool, segment));
  ASSERT_NE(scorer, nullptr);

  const int32_t packedStart = postings[Postings::DOCS_BLOCK_SIZE];
  const int32_t wordStart = postings[2 * Postings::DOCS_BLOCK_SIZE];
  std::array<int32_t, 4> boundaries = {
    0, packedStart + 73, wordStart + 100, postings.back() + 2
  };
  std::vector<int32_t> actual;
  for (size_t batch = 1; batch < boundaries.size(); batch++) {
    std::vector<int32_t> candidates(
        (size_t) (boundaries[batch] - boundaries[batch - 1]));
    std::iota(candidates.begin(), candidates.end(), boundaries[batch - 1]);
    int32_t size = scorer->retainMatchesToCandidates(
        candidates.data(), (int32_t) candidates.size());
    actual.insert(actual.end(), candidates.begin(), candidates.begin() + size);
  }
  EXPECT_EQ(postings, actual);

  std::array<int32_t, 2> afterEnd = {
    postings.back() + 2, postings.back() + 3
  };
  EXPECT_EQ(0, scorer->retainMatchesToCandidates(
      afterEnd.data(), (int32_t) afterEnd.size()));
}

// fillScoreBlock is count-driven: a call that stops on count (not upTo) must
// leave the enum positioned so the next call resumes exactly, on both the
// block-span path (mcs == 0) and the impact-skipping scalar path (mcs > 0).
TEST_F(TermScorerTest, FillScoreBlockCountLimitedCallsResumeOnBothFillPaths) {
  CollectionHelper helper("main");
  auto postings = makeMixedProbePostings();
  indexProbeTermDocs(helper, "count_limited", postings, "count_limited");
  auto reader = helper.getIndexWriter()->getIndexReader();

  int32_t start = postings[5];
  int32_t upTo = postings.back() + 1;
  int32_t unlimitedCount = (int32_t) postings.size() + 16;
  std::array<int32_t, 1> unlimited = {unlimitedCount};
  std::array<int32_t, 2> limited = {73, unlimitedCount};

  for (float minCompetitiveScore : {0.0f, std::numeric_limits<float>::denorm_min()}) {
    auto expected = fillTermScoreBlockCalls(*reader, "count_limited", start, upTo,
                                            unlimited, minCompetitiveScore);
    auto actual = fillTermScoreBlockCalls(*reader, "count_limited", start, upTo,
                                          limited, minCompetitiveScore);
    SCOPED_TRACE(::testing::Message() << "mcs=" << minCompetitiveScore);
    expectFillRunNear(expected, actual);
    ASSERT_FALSE(actual.docs.empty());
    EXPECT_EQ(actual.docs.front(), start);
    EXPECT_EQ(actual.docs.back(), postings.back());
  }
}

TEST_F(TermScorerTest, ScoredWordProbeRankAndEarlyCompactionKeepsCursorCoherent) {
  CollectionHelper helper("main");
  auto postings = makeWordProbeBlock(0);
  indexProbeTermDocs(helper, "rank_probe", postings, "rank_probe");
  auto reader = helper.getIndexWriter()->getIndexReader();

  std::vector<int32_t> rankCandidates = {
    postings[0],
    postings[1],
    postings[2],
    postings[1] + 1,
    postings[31],
    postings[63],
    postings[96],
    postings[127]
  };
  std::sort(rankCandidates.begin(), rankCandidates.end());
  auto expected = runPerDocTermCandidateSweep(*reader, "rank_probe", rankCandidates, false);
  auto actual = runApplyTermCandidateSweep(*reader, "rank_probe", rankCandidates, false, true);
  expectCandidateSweepEqual(expected, actual);
  EXPECT_GT(actual.scoredWordProbes, 0);

  MemPool pool;
  Query::Context qContext(pool, *reader);
  TermQuery query("body_w", "rank_probe");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = weight->createScorer(pool, segment);
  ASSERT_NE(scorer, nullptr);

  std::vector<int32_t> missDocs = {1, 2};
  auto missScores = initialCandidateScores((int32_t) missDocs.size());
  bool savedStats = SkipStats::enabled;
  SkipStats::enabled = true;
  SkipStats::reset();
  int32_t kept = scorer->applyToCandidates(missDocs.data(), missScores.data(),
                                           (int32_t) missDocs.size(), true);
  EXPECT_EQ(kept, 0);
  EXPECT_GT(SkipStats::scoredWordProbeAdvances, 0);
  SkipStats::enabled = savedStats;

  FillRun filled;
  int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
  float blockScores[Postings::DOCS_BLOCK_SIZE];
  int32_t upTo = postings[12] + 1;
  int32_t n = 0;
  while ((n = scorer->fillScoreBlock(blockDocs, blockScores,
                                     Postings::DOCS_BLOCK_SIZE, upTo)) > 0) {
    for (int32_t i = 0; i < n; i++) {
      filled.docs.push_back(blockDocs[i]);
      filled.scores.push_back(blockScores[i]);
    }
  }
  auto oracle = collectTermPerDoc(*reader, "rank_probe", postings[1], upTo);
  expectFillRunNear(oracle, filled);
}

TEST_F(TermScorerTest, ScoredWordProbeSweepThenEssentialFillAcrossWindowBoundaryMatchesOracle) {
  CollectionHelper helper("main");
  auto postings = makeWordProbeBlock(0);
  indexProbeTermDocs(helper, "flip_probe", postings, "flip_probe");
  auto reader = helper.getIndexWriter()->getIndexReader();

  MemPool pool;
  Query::Context qContext(pool, *reader);
  TermQuery query("body_w", "flip_probe");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];
  auto* scorer = weight->createScorer(pool, segment);
  ASSERT_NE(scorer, nullptr);

  std::vector<int32_t> windowNCandidates = {
    1,
    postings[10],
    postings[10] + 1,
    postings[20]
  };
  auto scores = initialCandidateScores((int32_t) windowNCandidates.size());
  int32_t kept = scorer->applyToCandidates(windowNCandidates.data(), scores.data(),
                                           (int32_t) windowNCandidates.size(), false);
  EXPECT_EQ(kept, (int32_t) windowNCandidates.size());

  int32_t windowStart = postings[60];
  int32_t upTo = postings[92] + 1;
  if (scorer->docId() < windowStart) {
    scorer->advance(windowStart);
  }

  FillRun filled;
  int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
  float blockScores[Postings::DOCS_BLOCK_SIZE];
  int32_t n = 0;
  while ((n = scorer->fillScoreBlock(blockDocs, blockScores,
                                     Postings::DOCS_BLOCK_SIZE, upTo)) > 0) {
    for (int32_t i = 0; i < n; i++) {
      filled.docs.push_back(blockDocs[i]);
      filled.scores.push_back(blockScores[i]);
    }
  }

  auto oracle = collectTermPerDoc(*reader, "flip_probe", windowStart, upTo);
  expectFillRunNear(oracle, filled);
}

TEST_F(TermScorerTest, ScoredWordProbeSkipStatsSeparateFromPackedFallback) {
  {
    CollectionHelper helper("main");
    auto postings = makeWordProbeBlock(0);
    indexProbeTermDocs(helper, "word_stats_probe", postings, "word_stats_probe");
    auto reader = helper.getIndexWriter()->getIndexReader();
    std::vector<int32_t> candidates = {
      postings[0],
      postings[1] + 1,
      postings[32],
      postings[80]
    };
    auto actual = runApplyTermCandidateSweep(*reader, "word_stats_probe", candidates,
                                             false, true);
    EXPECT_GT(actual.scoredWordProbes, 0);
    helper.clear();
  }

  {
    CollectionHelper helper("main");
    auto postings = makePackedProbeBlock(0);
    indexProbeTermDocs(helper, "packed_stats_probe", postings, "packed_stats_probe");
    auto reader = helper.getIndexWriter()->getIndexReader();
    std::vector<int32_t> candidates = {0, 1, 8, 64, 65, postings.back()};
    auto expected = runPerDocTermCandidateSweep(*reader, "packed_stats_probe",
                                                candidates, false);
    auto actual = runApplyTermCandidateSweep(*reader, "packed_stats_probe",
                                             candidates, false, true);
    expectCandidateSweepEqual(expected, actual);
    EXPECT_EQ(actual.scoredWordProbes, 0);
  }
}

void addHighThetaTwoClauseDocs(CollectionHelper& helper,
                               int32_t windowStart, int32_t windowEnd) {
  const int32_t nDocs = windowEnd + 31;
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t local = doc - windowStart;
    if (doc >= windowStart && doc < windowEnd) {
      if ((local % 2) == 0) appendRepeatedTerm(body, "high_a", 1);
      if ((local % 3) == 0) appendRepeatedTerm(body, "high_b", 1);
    } else {
      appendRepeatedTerm(body, "high_a", 1);
      appendRepeatedTerm(body, "high_b", 1);
    }
    appendRepeatedTerm(body, "high_pad", 3);
    docs.push_back(flatdoc("id", "high_" + std::to_string(doc), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);
}

void checkMaxScoreBulkScorerSingleEssentialDirectFillMatchesFilteredWindow(
    IndexReader& reader, std::span<const std::string_view> terms,
    int32_t windowStart, int32_t windowEnd, float theta) {
  BulkDomainDriveGuard domainDriveGuard(true);
  auto unfiltered = runSingleEssentialBulkWindow(reader, terms, windowStart, windowEnd,
                                                 theta, false);
  auto filtered = runSingleEssentialBulkWindow(reader, terms, windowStart, windowEnd,
                                               theta, true);

  EXPECT_GT(unfiltered.directFills, 0);
  EXPECT_EQ(filtered.directFills, 0);
  EXPECT_GT(unfiltered.sweepWindows, 0);
  EXPECT_GT(filtered.sweepWindows, 0);
  ASSERT_EQ(unfiltered.docs, filtered.docs);
  ASSERT_EQ(unfiltered.scores.size(), filtered.scores.size());
  for (size_t i = 0; i < unfiltered.scores.size(); i++) {
    EXPECT_EQ(std::bit_cast<uint32_t>(unfiltered.scores[i]),
              std::bit_cast<uint32_t>(filtered.scores[i])) << "i=" << i;
  }
}

void checkMaxScoreBulkScorerRequiredPromotionMakesHighThetaTwoClauseConjunction(
    IndexReader& reader, std::span<const std::string_view> terms,
    std::span<const WindowScore> scores,
    int32_t windowStart, int32_t windowEnd, float theta) {
  std::vector<int32_t> expectedDocs;
  for (auto hit : scores) {
    if (hit.score >= theta) {
      expectedDocs.push_back(hit.doc);
    }
  }
  ASSERT_FALSE(expectedDocs.empty());

  MemPool pool;
  Query::Context qContext(pool, reader);
  auto& segment = qContext.topReader.segments()[0];
  auto* bulk = createBulkTermDisjunctionScorer(pool, qContext, segment, terms);
  ASSERT_NE(bulk, nullptr);

  ScoreWindow window;
  SkipStatsGuard stats;
  int32_t next = bulk->scoreNextWindow(window, nullptr, windowStart, windowEnd, theta);
  EXPECT_EQ(next, PostingsReader::END);
  std::vector<int32_t> actualDocs;
  actualDocs.reserve((size_t) window.size);
  for (int32_t i = 0; i < window.size; i++) {
    actualDocs.push_back(window.docs[(size_t) i]);
    EXPECT_GE(window.scores[(size_t) i], theta);
    EXPECT_EQ((window.docs[(size_t) i] - windowStart) % 6, 0);
  }
  EXPECT_EQ(actualDocs, expectedDocs);
  EXPECT_GT(SkipStats::maxScoreSweepWindows, 0);
  EXPECT_GT(SkipStats::maxScoreRequiredSweeps, 0);
  EXPECT_GT(SkipStats::maxScoreBufferCompactions, 0);

  MemPool countPool;
  Query::Context countContext(countPool, reader);
  auto& countSegment = countContext.topReader.segments()[0];
  EXPECT_EQ(countBulkTermDisjunctionSegment(countPool, countContext, countSegment, terms, nullptr),
            countPullTermDisjunctionSegment(countPool, countContext, countSegment, terms, nullptr));
}

TEST_F(TermScorerTest, MaxScoreBulkScorerHighThetaTwoClauseCorpusChecks) {
  constexpr int32_t windowStart = DocsEnumMeta::L1_DOCS;
  constexpr int32_t windowEnd = 2 * DocsEnumMeta::L1_DOCS;
  CollectionHelper helper("main");
  addHighThetaTwoClauseDocs(helper, windowStart, windowEnd);
  auto reader = helper.getIndexWriter()->getIndexReader();
  std::array<std::string_view, 2> terms = {"high_a", "high_b"};

  auto scores = exhaustiveWindowScores(*reader, terms, windowStart, windowEnd);
  float maxSingle = std::numeric_limits<float>::lowest();
  float minBoth = std::numeric_limits<float>::infinity();
  for (auto hit : scores) {
    int32_t local = hit.doc - windowStart;
    if ((local % 6) == 0) {
      minBoth = std::min(minBoth, hit.score);
    } else {
      maxSingle = std::max(maxSingle, hit.score);
    }
  }
  ASSERT_GT(minBoth, maxSingle);
  float theta = (maxSingle + minBoth) * 0.5f;

  {
    SCOPED_TRACE("SingleEssentialDirectFillMatchesFilteredWindow");
    checkMaxScoreBulkScorerSingleEssentialDirectFillMatchesFilteredWindow(
        *reader, terms, windowStart, windowEnd, theta);
  }
  {
    SCOPED_TRACE("RequiredPromotionMakesHighThetaTwoClauseConjunction");
    checkMaxScoreBulkScorerRequiredPromotionMakesHighThetaTwoClauseConjunction(
        *reader, terms, scores, windowStart, windowEnd, theta);
  }
}

TEST_F(TermScorerTest, MaxScoreBulkScorerBufferSweepsRespectFiltersAndDeletes) {
  const int32_t clauses = 5;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addSweepDisjunctionDocs(field, clauses);
  for (int32_t doc = 0; doc < 3 * DocsEnumMeta::L1_DOCS + 211; doc += 13) {
    testIndex.deleteDoc(doc);
  }
  testIndex.flush();
  field.startReading();
  auto reader = testIndex.reader;
  auto termStrings = makeSweepTermStrings(clauses);
  auto views = termViews(termStrings);
  std::span<const std::string_view> terms(views.data(), views.size());

  BulkDomainDriveGuard guard(true);
  for (bool arrayDocSet : {false, true}) {
    auto expected = runFilteredExhaustiveTermDisjunctionTopK(
        *reader, terms, 40, 3, arrayDocSet, true);
    SkipStatsGuard stats;
    auto actual = runFilteredBulkTermDisjunctionTopK(
        *reader, terms, 40, 3, arrayDocSet, true);
    EXPECT_GT(SkipStats::maxScoreSweepWindows, 0) << "arrayDocSet=" << arrayDocSet;
    assertSameTopKDocs(expected, actual, 40);
  }
}

void checkMaxScoreBulkScorerBs1BitsetFilterMatchesPull(
    IndexReader& reader, int32_t numTerms) {
  const int32_t topK = 100;

  auto pull = runDenseFilteredPullTopK(reader, numTerms, topK);
  BulkDomainDriveGuard guard(true);
  auto bulk = runDenseFilteredBulkTopK(reader, numTerms, topK);
  ASSERT_GT(bulk.bs1Windows, 0);
  assertSameTopKDocs(pull, bulk, topK);
}

void checkMaxScoreBulkScorerBs1OnlyForAllEssentialWindows(
    IndexReader& reader, int32_t numTerms) {
  float partialThreshold = 0.0f;
  {
    MemPool pool;
    Query::Context qContext(pool, reader);
    std::vector<std::string> terms;
    std::vector<TermQuery> queries;
    std::vector<Query*> optional;
    auto* weight = createDenseDisjunctionWeight(qContext, numTerms, terms, queries, optional);
    auto& segment = qContext.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(supplier->bulkScorer(pool));
    ASSERT_NE(bulk, nullptr);

    ScoreWindow window;
    bulk->scoreNextWindow(window, nullptr, 0, segment.maxDoc(),
                          std::numeric_limits<float>::lowest());
    EXPECT_EQ(bulk->bs1WindowCount(), 1);
    EXPECT_GT(window.size, 0);
    partialThreshold = bulk->nextPartitionMcsForTests();
    ASSERT_TRUE(std::isfinite(partialThreshold));
  }

  {
    MemPool pool;
    Query::Context qContext(pool, reader);
    std::vector<std::string> terms;
    std::vector<TermQuery> queries;
    std::vector<Query*> optional;
    auto* weight = createDenseDisjunctionWeight(qContext, numTerms, terms, queries, optional);
    auto& segment = qContext.topReader.segments()[0];
    auto* supplier = weight->scorerSupplier(pool, segment);
    ASSERT_NE(supplier, nullptr);
    auto* bulk = dynamic_cast<BooleanQuery::MaxScoreBulkScorer*>(supplier->bulkScorer(pool));
    ASSERT_NE(bulk, nullptr);

    ScoreWindow window;
    bulk->scoreNextWindow(window, nullptr, 0, segment.maxDoc(), partialThreshold);
    EXPECT_EQ(bulk->bs1WindowCount(), 0);
    EXPECT_GT(window.size, 0);  // Some clauses are still essential; the window is not dead.
  }
}

void checkMaxScoreBulkScorerSelectiveDomainDriveMatchesStream(
    IndexReader& reader, int32_t numTerms) {
  const int32_t topK = 50;
  const int32_t filterStep = 512;

  for (bool arrayDocSet : {false, true}) {
    auto pull = runDenseFilteredPullTopK(
        reader, numTerms, topK, filterStep, arrayDocSet);
    DisjunctionTopKRun stream;
    {
      BulkDomainDriveGuard guard(true);
      stream = runDenseFilteredBulkTopK(
          reader, numTerms, topK, filterStep, arrayDocSet);
    }
    auto drive = runDenseFilteredBulkTopK(
        reader, numTerms, topK, filterStep, arrayDocSet);

    ASSERT_GT(drive.domainDriveWindows, 1) << "arrayDocSet=" << arrayDocSet;
    assertSameTopKDocs(stream, drive, topK);
    assertSameTopKDocs(pull, drive, topK);
  }
}

TEST_F(TermScorerTest, MaxScoreBulkScorerDenseCorpusChecks) {
  constexpr int32_t numTerms = 32;
  const int32_t fullGroups = 1 + (int32_t)scaleTestWork(1);
  const int32_t nDocs = fullGroups * DocsEnumMeta::L1_DOCS + 37;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addDenseManyClauseDisjunctionDocs(field, nDocs, numTerms);
  testIndex.flush();
  field.startReading();

  {
    SCOPED_TRACE("bulkCountDomainDisjunctionDomainDriveMatchesPull");
    checkBulkCountDomainDisjunctionDomainDriveMatchesPull(
        *testIndex.reader, numTerms);
  }
  {
    SCOPED_TRACE("Bs1BitsetFilterMatchesPull");
    checkMaxScoreBulkScorerBs1BitsetFilterMatchesPull(
        *testIndex.reader, numTerms);
  }
  {
    SCOPED_TRACE("Bs1OnlyForAllEssentialWindows");
    checkMaxScoreBulkScorerBs1OnlyForAllEssentialWindows(
        *testIndex.reader, numTerms);
  }
  {
    SCOPED_TRACE("SelectiveDomainDriveMatchesStream");
    checkMaxScoreBulkScorerSelectiveDomainDriveMatchesStream(
        *testIndex.reader, numTerms);
  }
}

TEST_F(TermScorerTest, MaxScoreBulkScorerFilteredDeletedTopKMatchesPull) {
  const int32_t numTerms = 16;
  const int32_t nDocs =
      (int32_t)scaleTestWork(2) * DocsEnumMeta::L1_DOCS + 53;
  const int32_t topK = 75;
  TestIndex testIndex;
  TestField field(testIndex, "body_w");
  field.startIndexing();
  addDenseManyClauseDisjunctionDocs(field, nDocs, numTerms);
  for (int32_t doc = 0; doc < nDocs; doc += 11) {
    testIndex.deleteDoc(doc);
  }
  testIndex.flush();
  field.startReading();

  auto pull = runDenseFilteredPullTopK(*testIndex.reader, numTerms, topK, 3, false);
  BulkDomainDriveGuard guard(true);
  auto bulk = runDenseFilteredBulkTopK(*testIndex.reader, numTerms, topK, 3, false);
  assertSameTopKDocs(pull, bulk, topK);
}

TEST_F(TermScorerTest, MaxScoreBulkScorerTiesMatchExhaustive) {
  CollectionHelper helper("main");
  addBulkTieDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t k = 9;
  std::array<std::string_view, 2> terms = {"tie_a", "tie_b"};

  auto expected = runExhaustiveTermDisjunctionTopK(*reader, terms, k);
  auto bulk = runBulkTermDisjunctionTopK(*reader, terms, k, false, nullptr);
  assertSameTopKDocs(expected, bulk, k);
}

TEST_F(TermScorerTest, WandMinShouldMatchTopKMatchesExhaustive) {
  CollectionHelper helper("main");
  addWandMsmDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t totalDocs = 8 * Postings::DOCS_BLOCK_SIZE + 73;

  for (int32_t minMatch : {2, 3}) {
    auto expectedMatches = runMsmMatches(*reader, minMatch, false);
    auto actualMatches = runMsmMatches(*reader, minMatch, true);
    assertSameMsmMatches(expectedMatches, actualMatches, minMatch);

    for (int32_t k : {5, totalDocs + 10}) {
      auto expected = runMsmTopK(*reader, minMatch, k, false);
      auto actual = runMsmTopK(*reader, minMatch, k, true);
      assertSameMsmTopK(expected, actual, minMatch, k);
      if (k == 5) {
        EXPECT_LT(actual.visited, expected.visited) << "minMatch=" << minMatch;
        EXPECT_GT(actual.wandVisited, 0) << "minMatch=" << minMatch;
      } else {
        EXPECT_EQ(actual.visited, expected.visited) << "minMatch=" << minMatch;
      }
    }
  }
}

TEST_F(TermScorerTest, WandManyClauseMsmMatchesExhaustive) {
  CollectionHelper helper("main");
  const int32_t numTerms = 12;
  addManyTermMsmDocs(helper, numTerms);
  auto reader = helper.getIndexWriter()->getIndexReader();

  for (int32_t minMatch : {3, 6}) {
    for (int32_t k : {5, 200}) {
      auto expected = runManyTermMsmTopK(*reader, numTerms, minMatch, k, false);
      auto actual = runManyTermMsmTopK(*reader, numTerms, minMatch, k, true);
      assertSameMsmTopK(expected, actual, minMatch, k);
      if (k == 5) {
        EXPECT_LT(actual.visited, expected.visited) << "minMatch=" << minMatch;
        EXPECT_GT(actual.wandVisited, 0) << "minMatch=" << minMatch;
      }
    }
  }
}

TEST_F(TermScorerTest, MaxScoreAccumulatorConcurrentMax) {
  MaxScoreAccumulator accumulator;
  float prev = accumulator.get();
  for (float score : {0.5f, 0.25f, 3.0f, 2.0f, 7.5f, 6.0f}) {
    accumulator.accumulate(score);
    float cur = accumulator.get();
    EXPECT_GE(cur, prev);
    prev = cur;
  }
  EXPECT_FLOAT_EQ(accumulator.get(), 7.5f);

  MaxScoreAccumulator concurrent;
  std::array<std::vector<float>, 4> values = {
    std::vector<float>{1.0f, 4.0f, 12.0f, 8.0f},
    std::vector<float>{2.0f, 18.0f, 3.0f},
    std::vector<float>{5.0f, 99.5f, 11.0f},
    std::vector<float>{0.5f, 42.0f, 77.0f}
  };
  std::vector<std::thread> threads;
  threads.reserve(values.size());
  for (size_t i = 0; i < values.size(); i++) {
    threads.emplace_back([&concurrent, &values, i]() {
      for (float score : values[i]) {
        concurrent.accumulate(score);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_FLOAT_EQ(concurrent.get(), 99.5f);
}

void checkCrossSegmentAccumulatorRealOpMatchesExhaustive(
    IndexReader& reader,
    const std::vector<std::vector<std::string>>& idsBySeg) {
  const int32_t k = 5;

  auto expected = runCrossSegmentTermTopK(reader, k, false);

  auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("body_w", "needle").fields({"id"}).limit(k).getScores();
  req->execute(true);
  ASSERT_EQ(req->responses.size(), 1u) << req->toString();
  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();

  auto ids = localResultIds(*req);
  auto scores = localResultScores(*req);
  ASSERT_EQ(ids.size(), expected.topDocs.size());
  ASSERT_EQ(scores.size(), expected.topDocs.size());
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    auto seg = (size_t) expected.topDocs[i].doc.segment();
    auto doc = (size_t) expected.topDocs[i].doc.docId();
    ASSERT_LT(seg, idsBySeg.size());
    ASSERT_LT(doc, idsBySeg[seg].size());
    EXPECT_EQ(ids[i], idsBySeg[seg][doc]) << "i=" << i;
    EXPECT_FLOAT_EQ(scores[i], expected.topDocs[i].score) << "i=" << i;
  }
}

void checkCrossSegmentAccumulatorPropagatesThresholdSequential(IndexReader& reader) {
  const int32_t k = 5;

  MaxScoreAccumulator accumulator;
  TopDocsCollector seg0Collector(k);
  collectCrossSegmentTermSegment(reader, 0, k, true, &accumulator, seg0Collector);
  ASSERT_GT(accumulator.get(), std::numeric_limits<float>::lowest());

  TopDocsCollector seg1SharedCollector(k);
  collectCrossSegmentTermSegment(reader, 1, k, true, &accumulator, seg1SharedCollector);

  TopDocsCollector seg1LocalCollector(k);
  collectCrossSegmentTermSegment(reader, 1, k, true, nullptr, seg1LocalCollector);

  EXPECT_LT(seg1SharedCollector.totalHits(), seg1LocalCollector.totalHits());

  TopDocsCollector merged(k);
  merged.merge(seg0Collector);
  merged.merge(seg1SharedCollector);

  DisjunctionTopKRun actual;
  actual.visited = seg0Collector.totalHits() + seg1SharedCollector.totalHits();
  actual.topDocs = sortedCollectorDocs(merged);
  auto expected = runCrossSegmentTermTopK(reader, k, false, 2);
  assertSameTopKDocs(expected, actual, k);
}

TEST_F(TermScorerTest, CrossSegmentAccumulatorCorpusChecks) {
  CollectionHelper helper("main");
  std::vector<std::vector<std::string>> idsBySeg;
  addCrossSegmentAccumulatorDocs(helper, idsBySeg, 3);
  auto reader = helper.getIndexWriter()->getIndexReader();

  {
    SCOPED_TRACE("RealOpMatchesExhaustive");
    checkCrossSegmentAccumulatorRealOpMatchesExhaustive(*reader, idsBySeg);
  }
  {
    SCOPED_TRACE("PropagatesThresholdSequential");
    checkCrossSegmentAccumulatorPropagatesThresholdSequential(*reader);
  }
}

// Regression for the 1f bug: impact block skipping under-counts the total hit count
// (matches / get_number), since skipped docs are never visited.  collectTopK must keep
// pruning OFF (allowPruning=false) when an exact count is needed, so every match is
// visited; only then does totalHits() equal the true docfreq.
TEST_F(TermScorerTest, getNumberDisablesImpactSkipping) {
  const int32_t N = 6 * Postings::DOCS_BLOCK_SIZE + 17;  // multi-block common term
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    int32_t tf = 1 + ((doc / Postings::DOCS_BLOCK_SIZE) * 3 + (doc % 5)) % 17;
    int32_t len = 4 + ((doc * 11) % 90);
    if (len < tf) len = tf;
    std::string text;
    for (int32_t i = 0; i < tf; i++) text += "needle ";
    for (int32_t i = tf; i < len; i++) text += "filler ";
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  const int32_t k = 5;  // small k: with pruning ON the threshold would rise and skip blocks.

  // allowPruning=false (the get_number path): pruning is disabled, so every match is
  // visited and totalHits() is the exact docfreq (== N).  "needle" is in every doc.
  // This is a valid guard: skipping DOES fire on this corpus when allowed, so a broken
  // gate (pruning despite allowPruning=false) would drop the count below N and fail here.
  TermQuery exactQuery("body_w", "needle");
  auto* exactWeight = exactQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* exactScorer = dynamic_cast<TermQuery::Scorer*>(exactWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(exactScorer, nullptr);
  TopDocsCollector exactCollector(k);
  collectTopK(0, exactScorer, nullptr, nullptr, exactCollector, /*allowPruning=*/false);
  EXPECT_EQ(exactCollector.totalHits(), (int64_t) N);
}

// CachedTermInfo holds immutable positioned-term state. Every scorer must start
// independently without another dictionary seek.
TEST_F(TermScorerTest, interleavedScorersForSameTermAreIndependent) {
  const int32_t N = 3 * Postings::DOCS_BLOCK_SIZE + 7;  // multi-block, "needle" in every doc
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    f.add(doc, "needle filler filler");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  auto countAll = [&](const char* term) {
    TermQuery q("body_w", term);
    auto* w = q.createWeight(qContext, Query::NEED_SCORES);
    auto* s = dynamic_cast<TermQuery::Scorer*>(w->createScorer(testIndex.pool, segment));
    EXPECT_NE(s, nullptr);
    int64_t n = 0;
    for (int32_t doc = s->next(); doc != PostingsReader::END; doc = s->next()) {
      n++;
    }
    return n;
  };

  // First weight+scorer for "needle", fully consumed.
  EXPECT_EQ(countAll("needle"), (int64_t) N);
  // Second weight+scorer for the SAME term, created and run AFTER the first finished.
  // Pre-fix this cloned the exhausted cached enum and counted far fewer than N.
  EXPECT_EQ(countAll("needle"), (int64_t) N);
}
