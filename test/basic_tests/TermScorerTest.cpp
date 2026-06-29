#include <solux/query/AllQuery.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/query/TermQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/search/Collector.h"


using namespace solux;
using namespace solux::test;

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

std::vector<TopDocsCollector::ScoreDoc> sortedCollectorDocs(TopDocsCollector& collector) {
  auto docs = collector.sort();
  std::vector<TopDocsCollector::ScoreDoc> out(docs.begin(), docs.end());
  std::sort(out.begin(), out.end(), TopDocsCollector::scoreAndDocComp);
  return out;
}

void appendRepeatedTerm(std::string& body, std::string_view term, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(term);
  }
}

void antiCorrelatedTfLen(int32_t postingOrd, int32_t& tf, int32_t& len) {
  int32_t block = postingOrd / Postings::DOCS_BLOCK_SIZE;
  int32_t local = postingOrd % Postings::DOCS_BLOCK_SIZE;
  if (block == 0) {
    tf = 180 - (local % 37);
    len = tf + 2 + (local % 3);
  } else if (local == 0) {
    tf = 170 - std::min(block, 40);
    len = 900 + block * 17;
  } else if (local == 1) {
    tf = 1;
    len = 2;
  } else {
    tf = 1 + (local % 3 == 0 ? 1 : 0);
    len = 260 + (local % 41);
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
      appendRepeatedTerm(body, "filler", 20 + (doc % 11));
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
  helper.clear();

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
                                              int32_t windowSize = DocsEnum::L1_DOCS) {
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
          pool, std::span<Query::Scorer*>(arr, (size_t) count));
    collectTopK(segnum, scorer, nullptr, nullptr, collector, false);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

void assertSameTopKDocs(const DisjunctionTopKRun& expected, const DisjunctionTopKRun& actual, int32_t topK) {
  ASSERT_EQ(actual.topDocs.size(), expected.topDocs.size()) << "k=" << topK;
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(actual.topDocs[i].doc, expected.topDocs[i].doc) << "k=" << topK << " i=" << i;
    EXPECT_FLOAT_EQ(actual.topDocs[i].score, expected.topDocs[i].score) << "k=" << topK << " i=" << i;
  }
}

void assertSameTopKDocsExact(const DisjunctionTopKRun& expected, const DisjunctionTopKRun& actual, int32_t topK) {
  ASSERT_EQ(actual.topDocs.size(), expected.topDocs.size()) << "k=" << topK;
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(actual.topDocs[i].doc, expected.topDocs[i].doc) << "k=" << topK << " i=" << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(actual.topDocs[i].score),
              std::bit_cast<uint32_t>(expected.topDocs[i].score))
      << "k=" << topK << " i=" << i;
  }
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

void addDenseManyClauseDisjunctionDocs(CollectionHelper& helper, int32_t nDocs, int32_t numTerms) {
  helper.clear();
  std::vector<std::string> terms = makeMtTermStrings(numTerms);
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);

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
    int32_t len = used + 24 + (doc % 37);
    appendRepeatedTerm(body, "filler", len - used);
    docs.push_back(flatdoc("id", "bs1_" + std::to_string(doc), "body_w", body));
  }

  helper.indexAll(docs, UpdateMessage::COMMIT);
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
          pool, std::span<Query::Scorer*>(arr, (size_t) count));
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
    collectTopKWindowed(segnum, bulk, filter.get(), collector, nullptr, segments[segnum].maxDoc());
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
      collectTopKWindowed(segnum, bulk, nullptr, segmentCollector, accumulator, segments[segnum].maxDoc());
      visited += segmentCollector.totalHits();
      merged.merge(segmentCollector);
    } else {
      collectTopKWindowed(segnum, bulk, nullptr, single, accumulator, segments[segnum].maxDoc());
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

void addBulkTieDisjunctionDocs(CollectionHelper& helper) {
  helper.clear();
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

void appendRepeatedToken(std::string& body, std::string_view token, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(token);
  }
}

std::string makeCrossSegmentBody(int32_t tf, int32_t len) {
  std::string body;
  appendRepeatedToken(body, "needle", tf);
  appendRepeatedToken(body, "filler", len - tf);
  return body;
}

void addCrossSegmentAccumulatorDocs(CollectionHelper& helper, std::vector<std::vector<std::string>>& idsBySeg,
                                    int32_t segCount = 3) {
  const std::array<int32_t, 3> segDocs = {
    2 * Postings::DOCS_BLOCK_SIZE + 17,
    6 * Postings::DOCS_BLOCK_SIZE + 31,
    4 * Postings::DOCS_BLOCK_SIZE + 19
  };
  helper.clear();
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
                                           MaxScoreAccumulator* accumulator = nullptr) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", "needle");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
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
  helper.clear();
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);

  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    auto add = [&](std::string_view term, int32_t count) {
      appendRepeatedToken(body, term, count);
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
    return pool.make<BooleanQuery::ConjunctionScorer>(pool, span, span);
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
  helper.clear();
  std::vector<Doc> docs;
  docs.reserve((size_t) nDocs);
  for (int32_t doc = 0; doc < nDocs; doc++) {
    std::string body;
    int32_t used = 0;
    auto add = [&](const std::string& term, int32_t count) {
      appendRepeatedToken(body, term, count);
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

    DocsEnum denum(testIndex.pool, f.currentSegment()->postingsReader(), tenum);
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
      float score = 0.0f; // current expected score for an all-scorer is 0.0f
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
  auto expectedBlockLastDoc = [&](int32_t target) {
    if (target >= N) {
      return PostingsReader::END;
    }
    int32_t block = target / Postings::DOCS_BLOCK_SIZE;
    return std::min(N - 1, (block + 1) * Postings::DOCS_BLOCK_SIZE - 1);
  };
  for (int32_t target : {0, 1, 127, 128, 129, 3 * Postings::DOCS_BLOCK_SIZE + 5, N - 1}) {
    EXPECT_EQ(shallowScorer->advanceShallow(target), expectedBlockLastDoc(target)) << target;
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

TEST_F(TermScorerTest, termImpactShallowMaxScoreUsesWindowBlocks) {
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
  ASSERT_EQ(windowScorer->advanceShallow(ws), we);

  int32_t startBlock = windowScorer->blockContaining(ws);
  int32_t endBlock = windowScorer->blockContaining(we);
  ASSERT_LT(endBlock, windowScorer->impactBlockCount);
  float bruteMax = 0.0f;
  for (int32_t block = startBlock; block <= endBlock; block++) {
    bruteMax = std::max(bruteMax, windowScorer->blockImpact[block]);
  }

  float windowMax = windowScorer->getMaxScore(we);
  EXPECT_FLOAT_EQ(windowMax, bruteMax);
  EXPECT_LE(windowMax, globalMax);
  EXPECT_LT(windowMax, globalMax);
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
  ASSERT_EQ(frontierScorer->impactBlockCount, cornerScorer->impactBlockCount);

  std::vector<float> brute((size_t) frontierScorer->impactBlockCount, 0.0f);
  int32_t block = 0;
  for (int32_t doc = actualScorer->next(); doc != PostingsReader::END; doc = actualScorer->next()) {
    while (block + 1 < frontierScorer->impactBlockCount
           && doc > frontierScorer->impactLastDoc[block]) {
      block++;
    }
    ASSERT_LE(doc, frontierScorer->impactLastDoc[block]);
    brute[(size_t) block] = std::max(brute[(size_t) block], actualScorer->score());
  }

  bool sawTighterBlock = false;
  for (int32_t i = 0; i < frontierScorer->impactBlockCount; i++) {
    EXPECT_FLOAT_EQ(frontierScorer->blockImpact[i], brute[(size_t) i]) << "block=" << i;
    EXPECT_LE(frontierScorer->blockImpact[i], cornerScorer->blockImpact[i] + 1e-6f)
        << "block=" << i;
    sawTighterBlock |= frontierScorer->blockImpact[i] + 1e-6f < cornerScorer->blockImpact[i];
  }
  EXPECT_TRUE(sawTighterBlock);
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

TEST_F(TermScorerTest, maxScoreDisjunctionTopKMatchesExhaustive) {
  CollectionHelper helper("main");
  addMaxScoreDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t totalDocs = 3 * kMaxScoreDisjunctionSegDocs;

  for (int32_t k : {3, totalDocs + 10}) {
    auto expected = runExhaustiveDisjunctionTopK(*reader, k);
    auto actual = runMaxScoreDisjunctionTopK(*reader, k);
    assertSameTopKDocs(expected, actual, k);
    if (k == 3) {
      EXPECT_LT(actual.visited, expected.visited);
      EXPECT_GT(actual.nonEssentialLookups, 0);
    } else {
      EXPECT_EQ(actual.visited, expected.visited);
      EXPECT_EQ(actual.nonEssentialLookups, 0);
    }
  }
  helper.clear();
}

TEST_F(TermScorerTest, windowedMaxScoreDisjunctionTopKMatchesExhaustiveAndGlobal) {
  CollectionHelper helper("main");
  addMaxScoreDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t totalDocs = 3 * kMaxScoreDisjunctionSegDocs;

  for (int32_t k : {3, totalDocs + 10}) {
    auto expected = runExhaustiveDisjunctionTopK(*reader, k);
    auto global = runMaxScoreDisjunctionTopK(*reader, k, std::numeric_limits<int32_t>::max());
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
  helper.clear();
}

TEST_F(TermScorerTest, MaxScoreBulkScorerWindowedTopKMatchesBaseline) {
  CollectionHelper helper("main");
  addMaxScoreDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t totalDocs = 3 * kMaxScoreDisjunctionSegDocs;
  std::array<std::string_view, 3> terms = {"common", "medium", "rare"};

  for (int32_t k : {3, totalDocs + 10}) {
    auto exhaustive = runExhaustiveDisjunctionTopK(*reader, k);
    auto baseline = runMaxScoreDisjunctionTopK(*reader, k);
    auto bulk = runBulkTermDisjunctionTopK(*reader, terms, k, false, nullptr);
    assertSameTopKDocs(exhaustive, bulk, k);
    assertSameTopKDocsExact(baseline, bulk, k);
  }
  helper.clear();
}

TEST_F(TermScorerTest, MaxScoreBulkScorerSharedAccumulatorMatchesBaseline) {
  CollectionHelper helper("main");
  addMaxScoreDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t k = 3;
  std::array<std::string_view, 3> terms = {"common", "medium", "rare"};

  MaxScoreAccumulator accumulator;
  auto exhaustive = runExhaustiveDisjunctionTopK(*reader, k);
  auto baseline = runMaxScoreDisjunctionTopK(*reader, k);
  auto bulk = runBulkTermDisjunctionTopK(*reader, terms, k, true, &accumulator);
  assertSameTopKDocs(exhaustive, bulk, k);
  assertSameTopKDocsExact(baseline, bulk, k);
  ASSERT_GT(accumulator.get(), std::numeric_limits<float>::lowest());
  helper.clear();
}

TEST_F(TermScorerTest, MaxScoreBulkScorerBs1BitsetFilterMatchesPull) {
  CollectionHelper helper("main");
  const int32_t numTerms = 32;
  const int32_t nDocs = 12 * Postings::DOCS_BLOCK_SIZE + 37;
  const int32_t topK = 100;
  addDenseManyClauseDisjunctionDocs(helper, nDocs, numTerms);
  auto reader = helper.getIndexWriter()->getIndexReader();

  auto pull = runDenseFilteredPullTopK(*reader, numTerms, topK);
  BulkDomainDriveGuard guard(true);
  auto bulk = runDenseFilteredBulkTopK(*reader, numTerms, topK);
  ASSERT_GT(bulk.bs1Windows, 0);
  assertSameTopKDocsExact(pull, bulk, topK);
  helper.clear();
}

TEST_F(TermScorerTest, MaxScoreBulkScorerSelectiveDomainDriveMatchesStream) {
  CollectionHelper helper("main");
  const int32_t numTerms = 32;
  const int32_t nDocs = 3 * DocsEnum::L1_DOCS + 37;
  const int32_t topK = 50;
  const int32_t filterStep = 512;
  addDenseManyClauseDisjunctionDocs(helper, nDocs, numTerms);
  auto reader = helper.getIndexWriter()->getIndexReader();

  for (bool arrayDocSet : {false, true}) {
    auto pull = runDenseFilteredPullTopK(*reader, numTerms, topK, filterStep, arrayDocSet);
    DisjunctionTopKRun stream;
    {
      BulkDomainDriveGuard guard(true);
      stream = runDenseFilteredBulkTopK(*reader, numTerms, topK, filterStep, arrayDocSet);
    }
    auto drive = runDenseFilteredBulkTopK(*reader, numTerms, topK, filterStep, arrayDocSet);

    ASSERT_GT(drive.domainDriveWindows, 1) << "arrayDocSet=" << arrayDocSet;
    assertSameTopKDocsExact(stream, drive, topK);
    assertSameTopKDocsExact(pull, drive, topK);
  }
  helper.clear();
}

TEST_F(TermScorerTest, MaxScoreBulkScorerTiesMatchExhaustive) {
  CollectionHelper helper("main");
  addBulkTieDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t k = 9;
  std::array<std::string_view, 2> terms = {"tie_a", "tie_b"};

  auto expected = runExhaustiveTermDisjunctionTopK(*reader, terms, k);
  auto bulk = runBulkTermDisjunctionTopK(*reader, terms, k, false, nullptr);
  assertSameTopKDocsExact(expected, bulk, k);
  helper.clear();
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
  helper.clear();
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
  helper.clear();
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

TEST_F(TermScorerTest, CrossSegmentAccumulatorRealOpMatchesExhaustive) {
  CollectionHelper helper("main");
  std::vector<std::vector<std::string>> idsBySeg;
  addCrossSegmentAccumulatorDocs(helper, idsBySeg, 3);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t k = 5;

  auto expected = runCrossSegmentTermTopK(*reader, k, false);

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
  helper.clear();
}

TEST_F(TermScorerTest, CrossSegmentAccumulatorPropagatesThresholdSequential) {
  CollectionHelper helper("main");
  std::vector<std::vector<std::string>> idsBySeg;
  addCrossSegmentAccumulatorDocs(helper, idsBySeg, 2);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t k = 5;

  MaxScoreAccumulator accumulator;
  TopDocsCollector seg0Collector(k);
  collectCrossSegmentTermSegment(*reader, 0, k, true, &accumulator, seg0Collector);
  ASSERT_GT(accumulator.get(), std::numeric_limits<float>::lowest());

  TopDocsCollector seg1SharedCollector(k);
  collectCrossSegmentTermSegment(*reader, 1, k, true, &accumulator, seg1SharedCollector);

  TopDocsCollector seg1LocalCollector(k);
  collectCrossSegmentTermSegment(*reader, 1, k, true, nullptr, seg1LocalCollector);

  EXPECT_LT(seg1SharedCollector.totalHits(), seg1LocalCollector.totalHits());

  TopDocsCollector merged(k);
  merged.merge(seg0Collector);
  merged.merge(seg1SharedCollector);

  DisjunctionTopKRun actual;
  actual.visited = seg0Collector.totalHits() + seg1SharedCollector.totalHits();
  actual.topDocs = sortedCollectorDocs(merged);
  auto expected = runCrossSegmentTermTopK(*reader, k, false);
  assertSameTopKDocs(expected, actual, k);
  helper.clear();
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

// Regression: CachedTermInfo::useDocsEnum used to hand out the cached DocsEnum un-cloned
// when only one weight referenced the term (sharedCount==0).  Creating a SECOND weight
// for the same term (sharedCount->1) and a scorer AFTER the first scorer had already run
// then cloned the exhausted cached enum.  useDocsEnum now always clones, so interleaved
// createWeight / run / createWeight is safe.
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

  // First weight+scorer for "needle", fully consumed (its createWeight set sharedCount=0).
  EXPECT_EQ(countAll("needle"), (int64_t) N);
  // Second weight+scorer for the SAME term, created and run AFTER the first finished.
  // Pre-fix this cloned the exhausted cached enum and counted far fewer than N.
  EXPECT_EQ(countAll("needle"), (int64_t) N);
}
