#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <tbb/task_group.h>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/TopKAssert.h"
#include "test/LocalReq.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/TermQuery.h"
#include "solux/reader/SkipStats.h"
#include "solux/search/Collector.h"

using namespace solux;
using namespace solux::test;

//
// Full-text field faceting benchmark.
//
// FacetBM already covers faceting on synthetic _s / _i column fields.  This
// benchmark targets the path FacetBM cannot reach: faceting ON a full-text field
// (FullTextFacetReq, term-driven counting over the inverted index), across a
// range of domain densities.
//
// The domain is produced by a full-text query, and the queried term's document
// frequency sets the domain size.  body_w is Zipfian over a 50K vocab (rank 0 =
// most frequent token), so the query term string dials density directly:
//
//   "all"    match-all          -> 1M docs
//   "t0"     head term (dense)   -> ~790K docs (many full 64K roaring buckets)
//   "t100"   mid term            -> ~16K docs
//   "t10000" tail term (sparse)  -> ~170 docs (sparse ArrDocSet)
//
// 1M docs in shape "5555" expands to ~5 segments of ~181K docs down to tiny
// sparse segments, so the per-segment count + cross-segment merge is exercised
// across every container regime.
//

namespace {

// Precomputed Zipf CDF: rank r (1-based) has weight 1/r^s.  O(vocab) to build,
// O(log vocab) per draw.  Shared read-only across the parallel segment builders.
struct ZipfTable {
  std::vector<double> cdf;  // cumulative weights, cdf.back() == total
  double total = 0;

  ZipfTable(int vocab, double s) {
    cdf.resize(vocab);
    double sum = 0;
    for (int r = 1; r <= vocab; r++) {
      sum += 1.0 / std::pow((double)r, s);
      cdf[r - 1] = sum;
    }
    total = sum;
  }

  // Map a uniform 64-bit value to a Zipf-distributed rank in [0, vocab).
  // Rank 0 is the most frequent token, so "t0" is the densest query term.
  int sample(uint64_t bits) const {
    double u = (double)(bits >> 11) * 0x1.0p-53;  // [0, 1)
    double target = u * total;
    auto it = std::lower_bound(cdf.begin(), cdf.end(), target);
    int idx = (int)(it - cdf.begin());
    if (idx >= (int)cdf.size()) idx = (int)cdf.size() - 1;
    return idx;
  }
};

constexpr int kBodyVocab = 50000;  // body_w vocabulary size
constexpr double kZipfS = 1.0;     // ~natural-language exponent

using ScoreDoc = TopDocsCollector::ScoreDoc;

struct ScoreTopKResult {
  int64_t visited = 0;
  int64_t skippedBlocks = 0;
  int64_t nonEssentialLookups = 0;
  int64_t bulkSegments = 0;
  int64_t bulkFallbackSegments = 0;
  int64_t fp = 0;
  std::vector<ScoreDoc> topDocs;
};

enum class DisjunctionMaxScoreMode {
  Exhaustive,
  Global,
  Windowed
};

enum class CrossSegmentAccumulatorMode {
  Local,
  Shared
};

enum class MsmWandMode {
  Exhaustive,
  Wand
};

enum class FrontierBoundMode {
  Corner,
  Frontier
};

enum class FrontierCorpus {
  AntiCorrelated,
  Clustered,
  Zipf
};

enum class BulkDisjunctionCorpus {
  Few,
  Dense
};

void sortScoreDocs(std::vector<ScoreDoc>& docs) {
  std::sort(docs.begin(), docs.end(), [](const ScoreDoc& a, const ScoreDoc& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.doc < b.doc;
  });
}

int64_t scoreTopKFingerprint(std::span<const ScoreDoc> docs) {
  int64_t ret = (int64_t) docs.size();
  for (const auto& doc : docs) {
    uint32_t scoreBits = std::bit_cast<uint32_t>(doc.score);
    ret = ret * 31 + doc.doc.segment();
    ret = ret * 31 + doc.doc.docId();
    ret = ret * 31 + (int64_t) scoreBits;
  }
  return ret;
}

ScoreTopKResult runFullTextScoreTopK(IndexReader& reader, std::string_view qterm,
                                     int32_t topK, bool skip,
                                     bool useFrontierBound = true) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", qterm, 1.0f, useFrontierBound);
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  int64_t skippedBlocks = 0;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    if (skip) {
      scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
      collectTopK(segnum, scorer, nullptr, nullptr, collector);
    } else {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        collector.collect(segnum, doc, scorer->score());
      }
    }
    if (auto* termScorer = dynamic_cast<TermQuery::Scorer*>(scorer)) {
      skippedBlocks += termScorer->skippedBlocks();
    }
  }

  ScoreTopKResult result;
  result.visited = collector.totalHits();
  result.skippedBlocks = skippedBlocks;
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

ScoreTopKResult runFullTextScoreTopKDisjunction(IndexReader& reader,
                                                std::string_view commonTerm,
                                                std::string_view rareTerm,
                                                int32_t topK,
                                                bool skip) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery common("body_w", commonTerm);
  TermQuery rare("body_w", rareTerm);
  std::vector<Query*> optional = {&common, &rare};
  BooleanQuery query({}, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  int64_t nonEssentialLookups = 0;
  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    if (skip) {
      scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
      collectTopK(segnum, scorer, nullptr, nullptr, collector);
    } else {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        collector.collect(segnum, doc, scorer->score());
      }
    }
    if (auto* maxScore = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(scorer)) {
      nonEssentialLookups += maxScore->nonEssentialLookupCount();
    }
  }

  ScoreTopKResult result;
  result.visited = collector.totalHits();
  result.nonEssentialLookups = nonEssentialLookups;
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

ScoreTopKResult runCrossSegmentAccumulatorTopK(IndexReader& reader, int32_t topK,
                                               bool allowPruning,
                                               MaxScoreAccumulator* accumulator) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery query("body_w", "needle");
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector merged(topK);
  int64_t visited = 0;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    TopDocsCollector segmentCollector(topK);
    collectTopK(segnum, scorer, nullptr, nullptr, segmentCollector, allowPruning, accumulator);
    visited += segmentCollector.totalHits();
    merged.merge(segmentCollector);
  }

  ScoreTopKResult result;
  result.visited = visited;
  auto topDocs = merged.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

ScoreTopKResult runMsmWandTopK(IndexReader& reader, int32_t topK, MsmWandMode mode) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery a("body_w", "wand_a");
  TermQuery b("body_w", "wand_b");
  TermQuery c("body_w", "wand_c");
  TermQuery d("body_w", "wand_d");
  TermQuery e("body_w", "wand_e");
  std::array<Query::Weight*, 5> weights = {
    a.createWeight(qContext, Query::NEED_SCORES),
    b.createWeight(qContext, Query::NEED_SCORES),
    c.createWeight(qContext, Query::NEED_SCORES),
    d.createWeight(qContext, Query::NEED_SCORES),
    e.createWeight(qContext, Query::NEED_SCORES)
  };
  constexpr int32_t minMatch = 2;
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count < minMatch) {
      continue;
    }

    std::span<Query::Scorer*> span(arr, (size_t) count);
    Query::Scorer* scorer = nullptr;
    if (count == minMatch) {
      scorer = pool.make<BooleanQuery::ConjunctionScorer>(pool, span, span);
    } else if (mode == MsmWandMode::Wand) {
      scorer = pool.make<BooleanQuery::MinShouldMatchWandScorer>(pool, span, minMatch);
    } else {
      scorer = pool.make<BooleanQuery::MinShouldMatchScorer>(pool, span, minMatch);
    }

    collectTopK(segnum, scorer, nullptr, nullptr, collector, mode == MsmWandMode::Wand);
  }

  ScoreTopKResult result;
  result.visited = collector.totalHits();
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

ScoreTopKResult runClusteredDisjunctionTopK(IndexReader& reader, int32_t topK,
                                            DisjunctionMaxScoreMode mode) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery common("body_w", "common");
  TermQuery alpha("body_w", "alpha");
  TermQuery beta("body_w", "beta");
  std::array<Query::Weight*, 3> weights = {
    common.createWeight(qContext, Query::NEED_SCORES),
    alpha.createWeight(qContext, Query::NEED_SCORES),
    beta.createWeight(qContext, Query::NEED_SCORES)
  };
  TopDocsCollector collector(topK);
  int64_t nonEssentialLookups = 0;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count == 0) {
      continue;
    }

    Query::Scorer* scorer = nullptr;
    if (count == 1) {
      scorer = arr[0];
    } else if (mode == DisjunctionMaxScoreMode::Exhaustive) {
      scorer = pool.make<BooleanQuery::DisjunctionScorer>(
        pool, std::span<Query::Scorer*>(arr, (size_t) count));
    } else {
      int32_t windowSize = mode == DisjunctionMaxScoreMode::Global
        ? std::numeric_limits<int32_t>::max()
        : DocsEnum::L1_DOCS;
      scorer = pool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
        pool, std::span<Query::Scorer*>(arr, (size_t) count),
        segments[segnum].maxDoc(), windowSize);
    }

    if (mode == DisjunctionMaxScoreMode::Exhaustive) {
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        collector.collect(segnum, doc, scorer->score());
      }
    } else {
      scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
      collectTopK(segnum, scorer, nullptr, nullptr, collector);
      if (auto* maxScore = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(scorer)) {
        nonEssentialLookups += maxScore->nonEssentialLookupCount();
      }
    }
  }

  ScoreTopKResult result;
  result.visited = collector.totalHits();
  result.nonEssentialLookups = nonEssentialLookups;
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

// Execution paths are not required to produce bit-identical sums (accepted
// policy): compare tie-group-aware, per TopKAssert.h.
void assertSameTopK(const ScoreTopKResult& expected, const ScoreTopKResult& actual) {
  solux::test::assertTopKEquivalent(expected.topDocs, actual.topDocs);
}

void buildClusteredScoreTopKIndex(IndexWriter& iw, int64_t nDocs) {
  Inverter& inverter = iw.obtainInverter();
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

  std::string body;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    int tokenCount = 8 + (int) ((int64_t) doc * 192 / nDocs);
    // "hot" in every other doc so docFreq < docsWithField and idf > 0.  A term in
    // EVERY doc has idf ~= 0, which zeroes the BM25 scores and removes the score
    // variance that makes the per-block norm bound matter - defeating the measurement.
    bool hasHot = (doc % 2) == 0;
    body.assign(hasHot ? "hot" : "pad");
    for (int token = 1; token < tokenCount; token++) {
      body.append(" pad");
    }

    inverter.startDoc();
    hBody.index(inverter, body);
    inverter.finishDoc();
  }

  iw.releaseInverter(inverter, true);
  iw.commit();
}

void appendTerm(std::string& body, std::string_view term, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(term);
  }
}

void frontierBenchTfLen(int64_t postingOrd, int32_t& tf, int32_t& len) {
  int32_t block = (int32_t) (postingOrd / Postings::DOCS_BLOCK_SIZE);
  int32_t local = (int32_t) (postingOrd % Postings::DOCS_BLOCK_SIZE);
  if (block == 0) {
    tf = 260 - local;
    len = tf + 2;
  } else if (local == 0) {
    tf = 240 - std::min(block, 120);
    len = 1100 + block * 3;
  } else if (local == 1) {
    tf = 1;
    len = 2;
  } else {
    tf = 1 + (local % 5 == 0 ? 1 : 0);
    len = 260 + (local % 53);
  }
  if (len < tf) {
    len = tf;
  }
}

void buildAntiCorrelatedFrontierBenchIndex(CollectionHelper& helper, int64_t nDocs) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

  std::string body;
  int64_t postingOrd = 0;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    body.clear();
    if ((doc % 2) == 0) {
      int32_t tf = 0;
      int32_t len = 0;
      frontierBenchTfLen(postingOrd++, tf, len);
      appendTerm(body, "frontier", tf);
      appendTerm(body, "filler", len - tf);
    } else {
      appendTerm(body, "filler", 32 + (int32_t) (doc % 17));
    }

    inverter.startDoc();
    hId.index(inverter, std::to_string(doc));
    hBody.index(inverter, body);
    inverter.finishDoc();
  }

  iw->releaseInverter(inverter, true);
  helper.commit();
}

void makeCrossSegmentAccumulatorBody(std::string& body, int32_t seg, int32_t local,
                                     int32_t docsInSeg) {
  int32_t tf;
  int32_t len;
  if (seg == 0 && local < 160) {
    tf = 220 - local;
    if (tf < 30) tf = 30;
    len = 240;
  } else if (seg == 0) {
    tf = 2;
    len = 260;
  } else {
    tf = 1;
    len = 300 - (local * 180 / std::max(1, docsInSeg - 1)) + seg * 8;
  }

  body.clear();
  appendTerm(body, "needle", tf);
  appendTerm(body, "filler", len - tf);
}

void makeMsmWandBenchBody(std::string& body, int64_t doc, int32_t hotLimit) {
  int32_t used = 0;
  auto add = [&](std::string_view term, int32_t count) {
    appendTerm(body, term, count);
    used += count;
  };

  body.clear();
  bool hot = doc < hotLimit;
  add("wand_a", 1);
  if (hot || (doc % 2) == 0) add("wand_b", 1);
  if (hot || (doc % 3) == 0) add("wand_c", 1);
  if (hot) {
    add("wand_d", hotLimit + 40 - (int32_t) (doc / 2));
    add("wand_e", hotLimit + 20 - (int32_t) (doc / 3));
  } else {
    if ((doc % 31) == 0) add("wand_d", 1);
    if ((doc % 47) == 0) add("wand_e", 1);
  }

  int32_t len = hot ? 2 * hotLimit + 220 : 140 + (int32_t) (doc % 113);
  if (len < used) len = used;
  add("filler", len - used);
}

void buildMsmWandBenchIndex(CollectionHelper& helper, int64_t nDocs) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");
  int32_t hotLimit = solux::unit_tests ? 128 : 512;

  std::string body;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    makeMsmWandBenchBody(body, doc, hotLimit);

    inverter.startDoc();
    hId.index(inverter, std::to_string(doc));
    hBody.index(inverter, body);
    inverter.finishDoc();
  }

  iw->releaseInverter(inverter, true);
  helper.commit();
}

void buildCrossSegmentAccumulatorBenchIndex(CollectionHelper& helper,
                                            std::span<const int32_t> docsPerSeg) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  int64_t id = 0;

  for (size_t segnum = 0; segnum < docsPerSeg.size(); segnum++) {
    Inverter& inverter = iw->obtainInverter();
    Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
    Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");
    int32_t docsInSeg = docsPerSeg[segnum];

    std::string body;
    for (int32_t local = 0; local < docsInSeg; local++) {
      makeCrossSegmentAccumulatorBody(body, (int32_t) segnum, local, docsInSeg);

      inverter.startDoc();
      hId.index(inverter, std::to_string(id++));
      hBody.index(inverter, body);
      inverter.finishDoc();
    }
    iw->releaseInverter(inverter, true);
  }

  helper.commit();
}

void buildClusteredDisjunctionBenchIndex(CollectionHelper& helper, int64_t nDocs) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

  std::string body;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    // alpha's high-impact docs are clustered in window 0 with STRICTLY DECREASING
    // tf, each padded to a constant length, so every top-k score is distinct with
    // a clear gap (no ties, and no sub-ULP boundary flip between the heap-order
    // exhaustive sum and the stable-order MaxScore sum) - which keeps the exact
    // doc-id bench guard valid.  alpha also appears at tf 1 scattered through every
    // later window, so its GLOBAL max stays high (the window-0 cluster) and global
    // MaxScore must keep it essential everywhere; windowed MaxScore demotes it in
    // the later windows where its per-window max is just tf 1.  One clustered clause
    // is enough to separate windowed from global; common/beta are demoted disjuncts.
    int32_t alphaHotTf = 0;
    if (doc < DocsEnum::L1_DOCS && (doc % 2) == 0) {
      alphaHotTf = (int32_t) std::max<int64_t>(2, 132 - doc / 2);
    }

    body.clear();
    appendTerm(body, "common", 1);
    if (alphaHotTf > 0) {
      appendTerm(body, "alpha", alphaHotTf);
      appendTerm(body, "filler", 134 - alphaHotTf);  // constant length: only tf drives the score
    } else {
      // Scattered alpha keeps alpha's global max high (so global MaxScore drives
      // it through every later window); beta is a frequent, low-idf disjunct that
      // both global and windowed demote once the threshold rises.  windowed also
      // demotes alpha in these later windows (per-window max is just tf 1), so it
      // skips the bulk that global must scan.
      if ((doc % 3) == 0) appendTerm(body, "alpha", 1);
      if ((doc % 2) == 1) appendTerm(body, "beta", 1);
      appendTerm(body, "filler", 80);
    }

    inverter.startDoc();
    hId.index(inverter, std::to_string(doc));
    hBody.index(inverter, body);
    inverter.finishDoc();
  }

  iw->releaseInverter(inverter, true);
  helper.commit();
}

// Multi-term version of the anti-correlated frontier corpus: numTerms terms,
// each round-robin over docids so each gets its own anti-correlated block
// structure (loose corner, tight frontier).  Used to measure whether the T2
// frontier bound tightens the MULTI-term windowed-MaxScore window bound, not
// just the single-term block skip.
std::vector<std::string> makeMtTerms(int32_t numTerms) {
  std::vector<std::string> terms;
  terms.reserve((size_t) numTerms);
  for (int32_t term = 0; term < numTerms; term++) {
    terms.push_back("mt" + std::to_string(term));
  }
  return terms;
}

void buildMultiTermAntiCorrelatedIndex(CollectionHelper& helper, int64_t nDocs, int32_t numTerms) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

  std::vector<int64_t> postingOrd((size_t) numTerms, 0);
  std::string body;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    int32_t term = (int32_t) (doc % numTerms);
    int32_t tf = 0;
    int32_t len = 0;
    frontierBenchTfLen(postingOrd[(size_t) term]++, tf, len);
    body.clear();
    appendTerm(body, "mt" + std::to_string(term), tf);
    appendTerm(body, "filler", len - tf);
    inverter.startDoc();
    hId.index(inverter, std::to_string(doc));
    hBody.index(inverter, body);
    inverter.finishDoc();
  }
  iw->releaseInverter(inverter, true);
  helper.commit();
}

void buildDenseManyClauseIndex(CollectionHelper& helper, int64_t nDocs, int32_t numTerms) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");
  std::vector<std::string> terms = makeMtTerms(numTerms);

  std::string body;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    body.clear();
    int32_t used = 0;
    for (int32_t term = 0; term < numTerms; term++) {
      if (((doc + term) & 1) != 0) {
        continue;
      }
      int32_t tf = 1 + (int32_t) ((doc * 31 + term * 17) % 4);
      appendTerm(body, terms[(size_t) term], tf);
      used += tf;
    }
    int32_t len = used + 24 + (int32_t) (doc % 37);
    appendTerm(body, "filler", len - used);

    inverter.startDoc();
    hId.index(inverter, std::to_string(doc));
    hBody.index(inverter, body);
    inverter.finishDoc();
  }

  iw->releaseInverter(inverter, true);
  helper.commit();
}

ScoreTopKResult runMultiTermDisjunctionTopK(IndexReader& reader,
                                            const std::vector<std::string>& terms,
                                            int32_t topK, bool useFrontier,
                                            bool skip = true) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::vector<TermQuery> queries;
  queries.reserve(terms.size());
  for (const auto& t : terms) {
    queries.emplace_back("body_w", t, 1.0f, useFrontier);
  }
  std::vector<Query*> optional;
  optional.reserve(terms.size());
  for (auto& q : queries) optional.push_back(&q);
  BooleanQuery query({}, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) {
      continue;
    }
    if (skip) {
      scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
      collectTopK(segnum, scorer, nullptr, nullptr, collector);
    } else {
      // Exhaustive: no threshold feedback, so nothing prunes and every block is
      // decoded -- the "blocks total" denominator for skip-effectiveness.
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        collector.collect(segnum, doc, scorer->score());
      }
    }
  }

  ScoreTopKResult result;
  result.visited = collector.totalHits();
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

std::unique_ptr<DocSet> makeModuloBitsetDomain(int32_t maxDoc, int32_t step) {
  if (step <= 1) return nullptr;
  auto domain = std::make_unique<RAMBitDocSet>(maxDoc);
  for (int32_t doc = 0; doc < maxDoc; doc += step) {
    domain->mutableBits().set(doc);
  }
  return domain;
}

std::unique_ptr<DocSet> makeModuloArrayDomain(int32_t maxDoc, int32_t step) {
  if (step <= 1) return nullptr;
  std::vector<int32_t> docs;
  docs.reserve((size_t)((maxDoc + step - 1) / step));
  for (int32_t doc = 0; doc < maxDoc; doc += step) {
    docs.push_back(doc);
  }
  return std::make_unique<ArrDocSet>(std::move(docs));
}

ScoreTopKResult runBulkOrPullDisjunctionTopK(IndexReader& reader,
                                             const std::vector<std::string>& terms,
                                             int32_t topK, bool useBulk,
                                             int32_t domainStep = 0,
                                             bool domainArray = false) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  std::vector<TermQuery> queries;
  queries.reserve(terms.size());
  for (const auto& t : terms) {
    queries.emplace_back("body_w", t);
  }
  std::vector<Query*> optional;
  optional.reserve(terms.size());
  for (auto& q : queries) optional.push_back(&q);
  BooleanQuery query({}, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  ScoreTopKResult result;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto& seg = segments[segnum];
    auto domain = domainArray
      ? makeModuloArrayDomain(seg.maxDoc(), domainStep)
      : makeModuloBitsetDomain(seg.maxDoc(), domainStep);
    DocSet* filter = domain.get();
    if (useBulk) {
      auto* supplier = weight->scorerSupplier(pool, seg);
      if (supplier == nullptr) {
        continue;
      }
      auto* bulk = supplier->bulkScorer(pool);
      if (bulk != nullptr) {
        result.bulkSegments++;
        collectTopKWindowed(segnum, bulk, filter, collector, nullptr, seg.maxDoc());
        continue;
      }
      result.bulkFallbackSegments++;
      LOG_ERROR("MaxScore bulk scorer unexpectedly null for segment {}", segnum);
      auto* scorer = supplier->get(pool, std::numeric_limits<int64_t>::max());
      if (scorer == nullptr) {
        continue;
      }
      scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
      collectTopK(segnum, scorer, filter, nullptr, collector);
    } else {
      auto* scorer = weight->createScorer(pool, seg);
      if (scorer == nullptr) {
        continue;
      }
      scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
      collectTopK(segnum, scorer, filter, nullptr, collector);
    }
  }

  result.visited = collector.totalHits();
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

bool phraseFilterBenchAdjacent(int64_t doc, int32_t filterStep, int32_t adjacencyStep) {
  int64_t block = doc / filterStep;
  int32_t local = (int32_t)(doc % filterStep);
  int32_t adjacentResidue = (block % adjacencyStep) == 0 ? 0 : adjacencyStep - 1;
  return (local % adjacencyStep) == adjacentResidue;
}

void buildPhraseFilterBenchIndex(CollectionHelper& helper, int64_t nDocs,
                                 int32_t filterStep, int32_t adjacencyStep) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::IndexHandler& hId = inverter.getIndexHandler("id");
  Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

  std::string body;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    body.clear();
    appendTerm(body, "alpha", 1);
    if (!phraseFilterBenchAdjacent(doc, filterStep, adjacencyStep)) {
      appendTerm(body, "gap", 1);
    }
    appendTerm(body, "beta", 1);
    appendTerm(body, "filler", 8 + (int32_t)(doc % 7));
    if ((doc % filterStep) == 0) {
      appendTerm(body, "needle", 1);
    }

    inverter.startDoc();
    hId.index(inverter, std::to_string(doc));
    hBody.index(inverter, body);
    inverter.finishDoc();
  }

  iw->releaseInverter(inverter, true);
  helper.commit();
}

ScoreTopKResult runPhraseFilterConjunctionTopK(IndexReader& reader, int32_t topK) {
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

  ScoreTopKResult result;
  result.visited = collector.totalHits();
  auto topDocs = collector.sort();
  result.topDocs.assign(topDocs.begin(), topDocs.end());
  sortScoreDocs(result.topDocs);
  result.fp = scoreTopKFingerprint(result.topDocs);
  return result;
}

ScoreTopKResult runPhraseFilterConjunctionTopKCounted(IndexReader& reader, int32_t topK,
                                                       int64_t& matchCalls) {
  PhraseQuery::Scorer::matchCallsForTests = 0;
  PhraseQuery::Scorer::countMatchesForTests = true;
  ScoreTopKResult result = runPhraseFilterConjunctionTopK(reader, topK);
  matchCalls = PhraseQuery::Scorer::matchCallsForTests;
  PhraseQuery::Scorer::countMatchesForTests = false;
  return result;
}

}  // namespace

namespace solux {

void buildFullTextBenchIndex(CollectionHelper& helper, int64_t nDocs, std::span<const int32_t> docsPerSeg) {
  unused(nDocs);
  helper.clear();

  // Built once; persists for the process.  ~400KB of doubles, read-only below.
  static const ZipfTable bodyZipf(kBodyVocab, kZipfS);

  auto iw = helper.getIndexWriter();

  // Pre-obtain all inverters we need for parallel segment building.
  std::vector<Inverter*> inverters;
  inverters.reserve(docsPerSeg.size());
  for (size_t i = 0; i < docsPerSeg.size(); i++) {
    inverters.push_back(&iw->obtainInverter());
  }

  // Calculate starting document ID for each segment.
  std::vector<int64_t> segmentStartIds;
  segmentStartIds.reserve(docsPerSeg.size());
  int64_t idNum = 0;
  for (size_t i = 0; i < docsPerSeg.size(); i++) {
    segmentStartIds.push_back(idNum);
    idNum += docsPerSeg[i];
  }

  tbb::task_group tg;
  for (size_t segnum = 0; segnum < docsPerSeg.size(); segnum++) {
    tg.run([&, segnum]() {
      int segDocs = docsPerSeg[segnum];
      Inverter& inverter = *inverters[segnum];
      int64_t localIdNum = segmentStartIds[segnum];

      Inverter::IndexHandler& hId   = inverter.getIndexHandler("id");
      Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

      std::string body;
      for (int i = 0; i < segDocs; i++) {
        SplitMix64 r(localIdNum);  // make each doc predictable

        inverter.startDoc();
        hId.index(inverter, std::to_string(localIdNum++));

        // Tweet-length body: 8..30 Zipfian tokens "t<rank>" joined by spaces.
        int nTok = 8 + (int)r.rint(23);
        body.resize(0);
        for (int t = 0; t < nTok; t++) {
          if (t) body.push_back(' ');
          body.push_back('t');
          body.append(std::to_string(bodyZipf.sample(r())));
        }
        hBody.index(inverter, body);

        inverter.finishDoc();
      }
      iw->releaseInverter(inverter, true);  // immediately request a flush of the segment.
    });
  }
  tg.wait();

  helper.commit();

  if (!solux::unit_tests) {
    malloc_trim(0);
    std::println(std::cerr, "Post buildFullTextBenchIndex - Peak RSS: {} KB, current RSS: {} KB", peakRSSKB(), currentRSSKB());
  }
}

}  // namespace solux

//
// Faceting ON the full-text field body_w (FullTextFacetReq, term-driven counting).
// qterm: "all" for a match-all domain, otherwise a body_w term whose document
//        frequency sets the domain density (e.g. "t0" dense, "t10000" sparse).
//
static void BM_FullTextFacet(benchmark::State& state, int64_t nDocs, std::string_view shape,
                             std::string_view qterm, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = 200;
  }

  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  // The node has a single shared collection, but benchmarks run in a predictable
  // order: these full-text variants are registered consecutively and share one
  // build, and the only other corpora (FacetBM/QueryBM) use a different shape, so
  // a shape match here is always our text corpus.  (If a same-shaped column
  // corpus is ever added, reuse would need a body_w field check too.)
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildFullTextBenchIndex(helper, nDocs, docsPerSeg);
  }

  RSSWatcher watcher;

  int64_t matches = 0;
  int64_t fp = -1;
  for (auto _ : state) {
    int64_t ret = 0;

    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
    req->collection("main");
    auto& topDocs = req->topDocs("q");

    if (qterm == "all") {
      topDocs.allQuery();
    } else {
      topDocs.matchQuery("body_w", qterm);
    }
    topDocs.getNumber(true).getScores(false);

    // facet on the full-text field itself
    auto& facet = topDocs.facet("f", "body_w");
    facet.limit(5);

    req->execute(para);

    const auto* qDocs = req->responses[0]->proto.ops.at("q")->docList();
    matches = qDocs->matches.value_or(0);
    const auto* facetResult = qDocs->ops.at("f")->facetResult();
    const auto& counts = facetResult->counts;
    // Text faceting buckets are terms (col_s).
    if (std::holds_alternative<solux::api::ColStr>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + java_string_hashcode(bucketIds.v[i]) + counts[i];
      }
    } else {
      LOG_ERROR("Unexpected bucket ids type in text facet result: {}", "<unknown>");
    }

    benchmark::DoNotOptimize(ret);

    if (fp != -1) {
      ASSERT_EQ(fp, ret);  // sanity check that we get the same result every time.
    }
    fp = ret;  // save the fingerprint for the next iteration
  }

  state.counters["fp"] = fp;            // sanity check.
  state.counters["matches"] = matches;  // domain size, so density is visible per variant.
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Single-term relevance top-k over the same body_w full-text corpus.  skip=true
// drives TermQuery through collectTopK so the collector threshold feeds the
// scorer and enables Step 1 block skipping; skip=false exhaustively scores every
// matching doc and never pushes a threshold.
//
static void BM_FullTextScoreTopK(benchmark::State& state, int64_t nDocs, std::string_view shape,
                                 std::string_view qterm, bool skip) {
  int mergeFactor = 10;  // TODO: actually get from IW?
  constexpr int32_t topK = 100;

  if (solux::unit_tests) {
    nDocs = 200;
  }

  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildFullTextBenchIndex(helper, nDocs, docsPerSeg);
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  ScoreTopKResult exhaustive = runFullTextScoreTopK(*reader, qterm, topK, false);
  ScoreTopKResult pruned = runFullTextScoreTopK(*reader, qterm, topK, true);
  assertSameTopK(exhaustive, pruned);

  RSSWatcher watcher;

  int64_t fp = -1;
  int64_t visited = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runFullTextScoreTopK(*reader, qterm, topK, skip);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["skip"] = skip ? 1 : 0;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Pure disjunction relevance top-k over the same Zipfian body_w corpus.
// skip=true lets MaxScore drop the frequent term from the essential union after
// the collector threshold exceeds that term's global max score. skip=false is
// the exhaustive OR baseline.
//
static void BM_FullTextScoreTopKDisjunction(benchmark::State& state, int64_t nDocs,
                                            std::string_view shape,
                                            std::string_view commonTerm,
                                            std::string_view rareTerm,
                                            bool skip) {
  int mergeFactor = 10;  // TODO: actually get from IW?
  constexpr int32_t topK = 100;

  if (solux::unit_tests) {
    nDocs = 200;
  }

  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildFullTextBenchIndex(helper, nDocs, docsPerSeg);
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  ScoreTopKResult exhaustive = runFullTextScoreTopKDisjunction(*reader, commonTerm, rareTerm, topK, false);
  ScoreTopKResult pruned = runFullTextScoreTopKDisjunction(*reader, commonTerm, rareTerm, topK, true);
  assertSameTopK(exhaustive, pruned);

  RSSWatcher watcher;

  int64_t fp = -1;
  int64_t visited = 0;
  int64_t nonEssentialLookups = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runFullTextScoreTopKDisjunction(*reader, commonTerm, rareTerm, topK, skip);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);
    benchmark::DoNotOptimize(result.nonEssentialLookups);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
    nonEssentialLookups = result.nonEssentialLookups;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["nonessential_lookups"] = nonEssentialLookups;
  state.counters["skip"] = skip ? 1 : 0;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Clustered disjunction workload for block-max windowed MaxScore.  The high-tf
// alpha and beta docs are clustered in early L1 windows; low-tf occurrences are
// spread through later windows.  Windowed MaxScore can demote those clauses in
// the later windows, while global MaxScore must keep them essential everywhere.
//
static void BM_FullTextScoreTopKDisjunctionClustered(benchmark::State& state,
                                                     DisjunctionMaxScoreMode mode) {
  int64_t clusteredDocs = solux::unit_tests ? 12'000 : 1'000'000;
  constexpr int32_t topK = 100;
  std::vector<int32_t> docsPerSeg = {(int32_t) clusteredDocs};

  CollectionHelper helper;
  static std::vector<int32_t> builtShape;
  bool reuseIndex = builtShape == docsPerSeg && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildClusteredDisjunctionBenchIndex(helper, clusteredDocs);
    builtShape = docsPerSeg;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  ScoreTopKResult exhaustive = runClusteredDisjunctionTopK(
    *reader, topK, DisjunctionMaxScoreMode::Exhaustive);
  ScoreTopKResult global = runClusteredDisjunctionTopK(
    *reader, topK, DisjunctionMaxScoreMode::Global);
  ScoreTopKResult windowed = runClusteredDisjunctionTopK(
    *reader, topK, DisjunctionMaxScoreMode::Windowed);
  assertSameTopK(exhaustive, global);
  assertSameTopK(exhaustive, windowed);

  RSSWatcher watcher;

  int64_t fp = -1;
  int64_t visited = 0;
  int64_t nonEssentialLookups = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runClusteredDisjunctionTopK(*reader, topK, mode);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);
    benchmark::DoNotOptimize(result.nonEssentialLookups);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
    nonEssentialLookups = result.nonEssentialLookups;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["nonessential_lookups"] = nonEssentialLookups;
  state.counters["mode"] = (int32_t) mode;
  state.counters["window"] = mode == DisjunctionMaxScoreMode::Windowed ? DocsEnum::L1_DOCS : 0;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Multi-segment relevance top-k for cross-segment competitive threshold sharing.
// Segment 0 owns the global winners, so its filled top-k threshold can prune the
// lower-scoring later segments when a shared MaxScoreAccumulator is enabled.
//
static void BM_FullTextScoreTopKCrossSegmentAccumulator(benchmark::State& state,
                                                        CrossSegmentAccumulatorMode mode) {
  constexpr int32_t topK = 100;
  std::vector<int32_t> docsPerSeg;
  if (solux::unit_tests) {
    docsPerSeg = {2048, 2048, 2048, 2048};
  } else {
    docsPerSeg.assign(8, 125000);
  }

  CollectionHelper helper;
  static std::vector<int32_t> builtShape;
  bool reuseIndex = builtShape == docsPerSeg && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildCrossSegmentAccumulatorBenchIndex(helper, docsPerSeg);
    builtShape = docsPerSeg;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  ScoreTopKResult exhaustive = runCrossSegmentAccumulatorTopK(*reader, topK, false, nullptr);
  ScoreTopKResult local = runCrossSegmentAccumulatorTopK(*reader, topK, true, nullptr);
  MaxScoreAccumulator guardAccumulator;
  ScoreTopKResult shared = runCrossSegmentAccumulatorTopK(*reader, topK, true, &guardAccumulator);
  assertSameTopK(exhaustive, local);
  assertSameTopK(exhaustive, shared);

  RSSWatcher watcher;

  int64_t fp = -1;
  int64_t visited = 0;
  for (auto _ : state) {
    MaxScoreAccumulator accumulator;
    ScoreTopKResult result = mode == CrossSegmentAccumulatorMode::Shared
      ? runCrossSegmentAccumulatorTopK(*reader, topK, true, &accumulator)
      : runCrossSegmentAccumulatorTopK(*reader, topK, true, nullptr);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["mode"] = (int32_t) mode;
  state.counters["shared"] = mode == CrossSegmentAccumulatorMode::Shared ? 1 : 0;
  state.counters["guard_exhaustive_visited"] = exhaustive.visited;
  state.counters["guard_local_visited"] = local.visited;
  state.counters["guard_shared_visited"] = shared.visited;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Min-should-match relevance top-k A/B for global-max WAND.  The first docs
// match the high-idf clauses with large, strictly decreasing term frequencies;
// the long tail matches mostly frequent low-idf combinations that WAND can stop
// driving once the top-k threshold rises.
//
static void BM_FullTextScoreTopKMsmWand(benchmark::State& state, MsmWandMode mode) {
  int64_t msmDocs = solux::unit_tests ? 10'000 : 1'000'003;
  constexpr int32_t topK = 100;
  std::vector<int32_t> docsPerSeg = {(int32_t) msmDocs};

  CollectionHelper helper;
  static int64_t builtMsmDocs = 0;
  bool reuseIndex = builtMsmDocs == msmDocs && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildMsmWandBenchIndex(helper, msmDocs);
    builtMsmDocs = msmDocs;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  ScoreTopKResult exhaustive = runMsmWandTopK(*reader, topK, MsmWandMode::Exhaustive);
  ScoreTopKResult wand = runMsmWandTopK(*reader, topK, MsmWandMode::Wand);
  assertSameTopK(exhaustive, wand);

  RSSWatcher watcher;

  int64_t fp = -1;
  int64_t visited = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runMsmWandTopK(*reader, topK, mode);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["mode"] = (int32_t) mode;
  state.counters["wand"] = mode == MsmWandMode::Wand ? 1 : 0;
  state.counters["guard_exhaustive_visited"] = exhaustive.visited;
  state.counters["guard_wand_visited"] = wand.visited;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Length-clustered single-term relevance top-k.  The term "hot" appears once in
// every doc, while doc length rises monotonically with docid.  Adjacent blocks
// have similar norms, so per-block minNorm should be a useful pruning bound.
//
static void BM_FullTextScoreTopKClustered(benchmark::State& state, bool skip) {
  int64_t clusteredDocs = solux::unit_tests ? 2000 : 1'000'000;
  constexpr int32_t topK = 100;

  RAMDir dir;
  IndexWriter iw(dir);
  buildClusteredScoreTopKIndex(iw, clusteredDocs);
  auto reader = iw.getIndexReader();

  ScoreTopKResult exhaustive = runFullTextScoreTopK(*reader, "hot", topK, false);
  ScoreTopKResult pruned = runFullTextScoreTopK(*reader, "hot", topK, true);
  assertSameTopK(exhaustive, pruned);

  RSSWatcher watcher;

  int64_t fp = -1;
  int64_t visited = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runFullTextScoreTopK(*reader, "hot", topK, skip);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["skip"] = skip ? 1 : 0;
  state.counters["nDocs"] = clusteredDocs;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Multi-term windowed-MaxScore disjunction over the anti-correlated frontier
// corpus, T2 vs T1 sub-clause bounds.  Tests whether the tighter T2 per-block
// bound flows into the multi-term window partition (each clause's
// getMaxScore(windowEnd)) and prunes more than the loose T1 corner.
//
static void BM_FullTextScoreTopKMultiTermFrontier(benchmark::State& state,
                                                  FrontierBoundMode mode, bool zipf) {
  constexpr int32_t topK = 100;
  int64_t nDocs = solux::unit_tests ? 16000 : 1'000'000;
  std::vector<int32_t> docsPerSeg = {(int32_t) nDocs};
  // anti-correlated mt0..mt3 (loose corner), vs realistic Zipfian mid-freq terms.
  std::vector<std::string> terms = zipf
    ? std::vector<std::string>{"t5", "t20", "t100", "t500"}
    : std::vector<std::string>{"mt0", "mt1", "mt2", "mt3"};

  CollectionHelper helper;
  static std::vector<int32_t> builtShape;
  static bool builtZipf = false;
  bool reuseIndex = builtShape == docsPerSeg && builtZipf == zipf && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    if (zipf) {
      buildFullTextBenchIndex(helper, nDocs, docsPerSeg);
    } else {
      buildMultiTermAntiCorrelatedIndex(helper, nDocs, (int32_t) terms.size());
    }
    builtShape = docsPerSeg;
    builtZipf = zipf;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();
  ScoreTopKResult t2 = runMultiTermDisjunctionTopK(*reader, terms, topK, true);
  ScoreTopKResult t1 = runMultiTermDisjunctionTopK(*reader, terms, topK, false);
  assertSameTopK(t2, t1);

  RSSWatcher watcher;
  int64_t fp = -1;
  int64_t visited = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runMultiTermDisjunctionTopK(*reader, terms, topK,
                                                         mode == FrontierBoundMode::Frontier);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);
    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
  }
  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["frontier"] = mode == FrontierBoundMode::Frontier ? 1 : 0;
  state.counters["guard_t1_visited"] = t1.visited;
  state.counters["guard_t2_visited"] = t2.visited;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

//
// Skip-effectiveness validation harness (the internal go/no-go for the
// skip-vs-skip top-k benchmark, and the level2 decision). NOT primarily a timing
// bench: the deliverable is the SkipStats counters. For a multi-term disjunction
// over the Zipfian corpus it reports, per query class (term count x k):
//   - blocks_decoded  : docs blocks decoded on the impact-pruned path (numerator)
//   - blocks_total    : docs blocks decoded exhaustively (denominator)
//   - pct_decoded     : the headline skip effectiveness
//   - l0_header_steps : per-block header walk within L1 groups
//   - l1_group_steps  : L1 group header walk -- THE level2 decision metric
//   - advance_calls   : leapfrog / impact-skip advance() drivers
// Run corner (T1) vs frontier (T2) to read bound tightness off pct_decoded.
// SkipStats is non-atomic, so the counted runs are single-thread by construction
// (one scorer chain per call); the timed loop runs with counters off.
//
enum class SkipQueryClass {
  TwoTerm,       // two dense-ish terms: both stay essential, modest skipping
  ThreeTerm,     // common + mid + rare
  TwoTermRare    // dense common + rare HIGH-idf: the rare term dominates scoring
                 // and stays essential, so the common term's long list is skipped
                 // via big advance jumps -- the workload that stresses the L1
                 // header walk (the level2 decision case).
};

static void BM_SkipEffectiveness(benchmark::State& state,
                                 SkipQueryClass queryClass,
                                 int32_t topK, bool useFrontier) {
  int64_t nDocs = solux::unit_tests ? 16000 : 1'000'000;
  std::vector<int32_t> docsPerSeg = {(int32_t) nDocs};
  // Zipfian body_w: rank 0 densest. t2 ~ head (many blocks), t50 mid, t500 tail,
  // t10000 ~ sparse high-idf tail.
  std::vector<std::string> terms;
  switch (queryClass) {
    case SkipQueryClass::TwoTerm:     terms = {"t2", "t500"}; break;
    case SkipQueryClass::ThreeTerm:   terms = {"t2", "t50", "t500"}; break;
    case SkipQueryClass::TwoTermRare: terms = {"t2", "t10000"}; break;
  }

  CollectionHelper helper;
  static std::vector<int32_t> builtShape;
  bool reuseIndex = builtShape == docsPerSeg && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildFullTextBenchIndex(helper, nDocs, docsPerSeg);
    builtShape = docsPerSeg;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  // Numerator (impact-pruned) and denominator (exhaustive), measured once outside
  // timing with counters on. Same top-k must fall out either way (byte-identical).
  SkipStats::enabled = true;
  SkipStats::reset();
  ScoreTopKResult pruned = runMultiTermDisjunctionTopK(*reader, terms, topK, useFrontier, true);
  int64_t blocksDecoded = SkipStats::docBlocksDecoded;
  int64_t l0Steps = SkipStats::l0HeaderSteps;
  int64_t l1Steps = SkipStats::l1GroupSteps;
  int64_t advanceCalls = SkipStats::advanceCalls;
  SkipStats::reset();
  ScoreTopKResult exhaustive = runMultiTermDisjunctionTopK(*reader, terms, topK, useFrontier, false);
  int64_t blocksTotal = SkipStats::docBlocksDecoded;
  SkipStats::enabled = false;
  assertSameTopK(pruned, exhaustive);

  // Timed loop: pruned path, counters off -> the timing carries no gate cost.
  for (auto _ : state) {
    ScoreTopKResult result = runMultiTermDisjunctionTopK(*reader, terms, topK, useFrontier, true);
    benchmark::DoNotOptimize(result.fp);
  }

  state.counters["terms"] = (double) terms.size();
  state.counters["k"] = topK;
  state.counters["frontier"] = useFrontier ? 1 : 0;
  state.counters["blocks_decoded"] = (double) blocksDecoded;
  state.counters["blocks_total"] = (double) blocksTotal;
  state.counters["pct_decoded"] =
    blocksTotal > 0 ? (double) blocksDecoded * 100.0 / (double) blocksTotal : 0.0;
  state.counters["l0_header_steps"] = (double) l0Steps;
  state.counters["l1_group_steps"] = (double) l1Steps;
  state.counters["advance_calls"] = (double) advanceCalls;
  state.counters["visited"] = (double) pruned.visited;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
}

static void BM_FullTextScoreTopKBulkDisjunction(benchmark::State& state,
                                                BulkDisjunctionCorpus corpus, bool useBulk,
                                                int32_t domainStep,
                                                bool domainArray) {
  constexpr int32_t topK = 100;
  int64_t nDocs = solux::unit_tests ? 2000 : 1'000'000;
  int32_t numTerms = corpus == BulkDisjunctionCorpus::Dense ? 32 : 5;
  std::vector<int32_t> docsPerSeg = {(int32_t) nDocs};
  std::vector<std::string> terms = makeMtTerms(numTerms);

  CollectionHelper helper;
  static BulkDisjunctionCorpus builtCorpus = BulkDisjunctionCorpus::Few;
  static int64_t builtDocs = 0;
  static int32_t builtTerms = 0;
  bool reuseIndex = builtCorpus == corpus && builtDocs == nDocs && builtTerms == numTerms
    && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    if (corpus == BulkDisjunctionCorpus::Dense) {
      buildDenseManyClauseIndex(helper, nDocs, numTerms);
    } else {
      buildMultiTermAntiCorrelatedIndex(helper, nDocs, numTerms);
    }
    builtCorpus = corpus;
    builtDocs = nDocs;
    builtTerms = numTerms;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();
  ScoreTopKResult pull = runBulkOrPullDisjunctionTopK(
    *reader, terms, topK, false, domainStep, domainArray);
  ScoreTopKResult bulk = runBulkOrPullDisjunctionTopK(
    *reader, terms, topK, true, domainStep, domainArray);
  ASSERT_EQ(0, bulk.bulkFallbackSegments);
  ASSERT_GT(bulk.bulkSegments, 0);
  assertSameTopK(pull, bulk);

  RSSWatcher watcher;
  int64_t fp = -1;
  int64_t visited = 0;
  int64_t bulkSegments = 0;
  int64_t bulkFallbackSegments = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runBulkOrPullDisjunctionTopK(
      *reader, terms, topK, useBulk, domainStep, domainArray);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);
    benchmark::DoNotOptimize(result.bulkSegments);
    benchmark::DoNotOptimize(result.bulkFallbackSegments);
    if (useBulk) {
      ASSERT_EQ(0, result.bulkFallbackSegments);
      ASSERT_GT(result.bulkSegments, 0);
    }

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
    bulkSegments = result.bulkSegments;
    bulkFallbackSegments = result.bulkFallbackSegments;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["bulk"] = useBulk ? 1 : 0;
  state.counters["dense"] = corpus == BulkDisjunctionCorpus::Dense ? 1 : 0;
  state.counters["domain_step"] = domainStep;
  state.counters["domain_array"] = domainArray ? 1 : 0;
  state.counters["terms"] = numTerms;
  state.counters["bulk_segments"] = bulkSegments;
  state.counters["bulk_fallback_segments"] = bulkFallbackSegments;
  state.counters["guard_pull_visited"] = pull.visited;
  state.counters["guard_bulk_visited"] = bulk.visited;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

// Opaque (noinline) read of run()'s returned fingerprint. A tight -O2 loop on
// this toolchain can misread the NRVO'd struct member when nothing else touches
// it between the return and the read; an out-of-line read returns the real
// value. The engine is correct -- this only hardens the bench's determinism guard.
SOLUX_NOINLINE static int64_t observeResultFp(const ScoreTopKResult& r) {
  return r.fp;
}

static void BM_FullTextScoreTopKPhraseFilterConjunction(benchmark::State& state, bool twoPhase) {
  constexpr int32_t topK = 100;
  constexpr int32_t filterStep = 64;
  constexpr int32_t adjacencyStep = 8;
  int64_t nDocs = solux::unit_tests ? 2000 : 1'000'000;
  std::vector<int32_t> docsPerSeg = {(int32_t) nDocs};

  CollectionHelper helper;
  static int64_t builtDocs = 0;
  static int32_t builtFilterStep = 0;
  static int32_t builtAdjacencyStep = 0;
  bool reuseIndex = builtDocs == nDocs
    && builtFilterStep == filterStep
    && builtAdjacencyStep == adjacencyStep
    && helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildPhraseFilterBenchIndex(helper, nDocs, filterStep, adjacencyStep);
    builtDocs = nDocs;
    builtFilterStep = filterStep;
    builtAdjacencyStep = adjacencyStep;
  }

  auto reader = helper.getIndexWriter()->getIndexReader();

  // Two-phase only changes WHEN positions are verified, not the match set, so it
  // must be byte-identical to the eager path. Verify that once, outside timing.
  int64_t tpGuardMatchCalls = 0;
  int64_t eagerGuardMatchCalls = 0;
  BooleanQuery::disableTwoPhaseForTests = false;
  ScoreTopKResult tpGuard = runPhraseFilterConjunctionTopKCounted(
    *reader, topK, tpGuardMatchCalls);
  BooleanQuery::disableTwoPhaseForTests = true;
  ScoreTopKResult eagerGuard = runPhraseFilterConjunctionTopKCounted(
    *reader, topK, eagerGuardMatchCalls);
  ASSERT_EQ(tpGuard.fp, eagerGuard.fp);
  ASSERT_GT(tpGuard.visited, 0);
  ASSERT_GT(eagerGuardMatchCalls, tpGuardMatchCalls);

  // Select the mode under test for the timed loop.
  BooleanQuery::disableTwoPhaseForTests = !twoPhase;
  int64_t guardFp = twoPhase ? tpGuard.fp : eagerGuard.fp;

  // Verify-count comes from the guard (measured once, outside timing). The timed
  // loop must NOT count: countMatchesForTests gates an increment in doMatches(),
  // and eager calls matches() ~7x more often, so counting inside the loop would
  // charge eager that per-call overhead and inflate the measured timing win.
  int64_t matchCalls = twoPhase ? tpGuardMatchCalls : eagerGuardMatchCalls;

  RSSWatcher watcher;
  int64_t fp = -1;
  for (auto _ : state) {
    ScoreTopKResult result = runPhraseFilterConjunctionTopK(*reader, topK);
    // result.fp must be read through observeResultFp(): a tight -O2 loop on this
    // toolchain misreads NRVO'd struct members read directly (result.visited read
    // directly comes back garbage), so visited is taken from the guard, not here.
    int64_t resultFp = observeResultFp(result);
    ASSERT_EQ(guardFp, resultFp);
    fp = resultFp;
  }

  state.counters["twophase"] = twoPhase ? 1 : 0;
  state.counters["fp"] = fp;
  state.counters["visited"] = tpGuard.visited;
  state.counters["match_calls"] = matchCalls;
  state.counters["guard_tp_match_calls"] = tpGuardMatchCalls;
  state.counters["guard_eager_match_calls"] = eagerGuardMatchCalls;
  state.counters["filter_step"] = filterStep;
  state.counters["adjacency_step"] = adjacencyStep;
  state.counters["guard_visited"] = tpGuard.visited;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
  BooleanQuery::disableTwoPhaseForTests = false;  // restore default for other benches
}

static void BM_FullTextScoreTopKFrontierBounds(benchmark::State& state,
                                               FrontierCorpus corpus,
                                               FrontierBoundMode mode) {
  int64_t corpusDocs = solux::unit_tests ? 6000 : 1'000'000;
  constexpr int32_t topK = 100;
  std::string_view qterm = "frontier";

  CollectionHelper helper;
  bool reuseIndex = false;
  if (corpus == FrontierCorpus::AntiCorrelated) {
    buildAntiCorrelatedFrontierBenchIndex(helper, corpusDocs);
  } else if (corpus == FrontierCorpus::Clustered) {
    helper.clear();
    buildClusteredScoreTopKIndex(*helper.getIndexWriter(), corpusDocs);
    qterm = "hot";
  } else {
    int mergeFactor = 10;
    constexpr const char* zipfShape = "5555";
    std::vector<int32_t> docsPerSeg;
    CollectionHelper::calcSegSizes(corpusDocs, mergeFactor, zipfShape, docsPerSeg);
    reuseIndex = helper.indexMatchesShape(docsPerSeg);
    if (!reuseIndex) {
      buildFullTextBenchIndex(helper, corpusDocs, docsPerSeg);
    }
    qterm = "t100";
  }

  auto reader = helper.getIndexWriter()->getIndexReader();
  ScoreTopKResult exhaustive = runFullTextScoreTopK(*reader, qterm, topK, false, true);
  ScoreTopKResult corner = runFullTextScoreTopK(*reader, qterm, topK, true, false);
  ScoreTopKResult frontier = runFullTextScoreTopK(*reader, qterm, topK, true, true);
  assertSameTopK(exhaustive, corner);
  assertSameTopK(exhaustive, frontier);

  RSSWatcher watcher;

  bool useFrontierBound = mode == FrontierBoundMode::Frontier;
  int64_t fp = -1;
  int64_t visited = 0;
  int64_t skippedBlocks = 0;
  for (auto _ : state) {
    ScoreTopKResult result = runFullTextScoreTopK(*reader, qterm, topK, true, useFrontierBound);
    benchmark::DoNotOptimize(result.fp);
    benchmark::DoNotOptimize(result.visited);
    benchmark::DoNotOptimize(result.skippedBlocks);

    if (fp != -1) {
      ASSERT_EQ(fp, result.fp);
    }
    fp = result.fp;
    visited = result.visited;
    skippedBlocks = result.skippedBlocks;
  }

  state.counters["fp"] = fp;
  state.counters["visited"] = visited;
  state.counters["skipped_blocks"] = skippedBlocks;
  state.counters["frontier"] = useFrontierBound ? 1 : 0;
  state.counters["corpus"] = corpus == FrontierCorpus::AntiCorrelated ? 0
                            : corpus == FrontierCorpus::Clustered ? 1
                            : 2;
  state.counters["nDocs"] = corpusDocs;
  state.counters["reused"] = reuseIndex;
  state.counters["guard_t1_visited"] = corner.visited;
  state.counters["guard_t2_visited"] = frontier.visited;
  state.counters["guard_t1_skipped_blocks"] = corner.skippedBlocks;
  state.counters["guard_t2_skipped_blocks"] = frontier.skippedBlocks;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}


constexpr int32_t nDocs = 1'000'000;
constexpr const char* shape = "5555";  // ~5 segs of ~181K docs down to tiny sparse segs

// Faceting ON the full-text field (body_w) via FullTextFacetReq term-driven
// counting, sweeping the domain density through the queried term's docFreq:
// match-all -> head (dense) -> mid -> tail (sparse), serial and parallel.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, all_body,       nDocs, shape, "all",    false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, all_body_para,  nDocs, shape, "all",    true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, head_body,      nDocs, shape, "t0",     false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, head_body_para, nDocs, shape, "t0",     true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, mid_body,       nDocs, shape, "t100",   false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, mid_body_para,  nDocs, shape, "t100",   true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, tail_body,      nDocs, shape, "t10000", false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, tail_body_para, nDocs, shape, "t10000", true);

// Single-term relevance top-k A/B over the same full-text corpus, measuring the
// Step 1 impact-skipping win on dense and mid-frequency terms.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopK, head_skip,   nDocs, shape, "t0",   true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopK, head_noskip, nDocs, shape, "t0",   false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopK, mid_skip,    nDocs, shape, "t100", true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopK, mid_noskip,  nDocs, shape, "t100", false);

// Disjunction MaxScore A/B. t0 is the frequent low-idf clause; t1000 is rare
// enough to lift the top-k threshold but common enough to supply k winners.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKDisjunction, disj_skip,   nDocs, shape, "t0", "t1000", true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKDisjunction, disj_noskip, nDocs, shape, "t0", "t1000", false);

// Clustered-disjunction A/B/C for block-max windowed MaxScore.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKDisjunctionClustered, clustered_windowed,
                        DisjunctionMaxScoreMode::Windowed);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKDisjunctionClustered, clustered_global,
                        DisjunctionMaxScoreMode::Global);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKDisjunctionClustered, clustered_exhaustive,
                        DisjunctionMaxScoreMode::Exhaustive);

// Cross-segment competitive threshold A/B.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKCrossSegmentAccumulator, shared,
                        CrossSegmentAccumulatorMode::Shared);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKCrossSegmentAccumulator, local,
                        CrossSegmentAccumulatorMode::Local);

// Min-should-match WAND A/B.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKMsmWand, wand, MsmWandMode::Wand);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKMsmWand, exhaustive, MsmWandMode::Exhaustive);

// Length-clustered relevance top-k A/B for the T1 minNorm pruning workload.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKClustered, clustered_skip,   true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKClustered, clustered_noskip, false);

// T2 frontier impact bound A/B. Corpus 0 is anti-correlated by construction;
// corpora 1 and 2 reuse the existing clustered and Zipfian workloads.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKFrontierBounds, anti_t2,
                        FrontierCorpus::AntiCorrelated, FrontierBoundMode::Frontier);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKFrontierBounds, anti_t1,
                        FrontierCorpus::AntiCorrelated, FrontierBoundMode::Corner);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKFrontierBounds, clustered_t2,
                        FrontierCorpus::Clustered, FrontierBoundMode::Frontier);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKFrontierBounds, clustered_t1,
                        FrontierCorpus::Clustered, FrontierBoundMode::Corner);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKFrontierBounds, zipf_t2,
                        FrontierCorpus::Zipf, FrontierBoundMode::Frontier);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKFrontierBounds, zipf_t1,
                        FrontierCorpus::Zipf, FrontierBoundMode::Corner);

// Multi-term windowed-MaxScore disjunction, T2 frontier vs T1 corner sub-clauses,
// on the anti-correlated corpus (where the corner is loose) and a realistic Zipfian one.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKMultiTermFrontier, multiterm_anti_t2,
                        FrontierBoundMode::Frontier, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKMultiTermFrontier, multiterm_anti_t1,
                        FrontierBoundMode::Corner, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKMultiTermFrontier, multiterm_zipf_t2,
                        FrontierBoundMode::Frontier, true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKMultiTermFrontier, multiterm_zipf_t1,
                        FrontierBoundMode::Corner, true);

// Skip-effectiveness validation harness. Frontier (T2) sweep over k for both
// query classes, plus a corner (T1) pair at k=10 to read bound tightness off
// pct_decoded. Report per class x k: pct_decoded (headline), l1_group_steps
// (level2 decision), l0_header_steps, advance_calls.
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_2term_k10,
                        SkipQueryClass::TwoTerm, 10, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_2term_k100,
                        SkipQueryClass::TwoTerm, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_2term_k1000,
                        SkipQueryClass::TwoTerm, 1000, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_3term_k10,
                        SkipQueryClass::ThreeTerm, 10, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_3term_k100,
                        SkipQueryClass::ThreeTerm, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_3term_k1000,
                        SkipQueryClass::ThreeTerm, 1000, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_2term_k10_corner,
                        SkipQueryClass::TwoTerm, 10, false);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_3term_k10_corner,
                        SkipQueryClass::ThreeTerm, 10, false);
// Common + rare high-idf: the aggressive-skip case that stresses the L1 header
// walk. Watch l1_group_steps relative to blocks_decoded here for the level2 call.
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_rare_k10,
                        SkipQueryClass::TwoTermRare, 10, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_rare_k100,
                        SkipQueryClass::TwoTermRare, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_SkipEffectiveness, skip_rare_k1000,
                        SkipQueryClass::TwoTermRare, 1000, true);

// Pull MaxScoreDisjunctionScorer vs the wired MaxScoreBulkScorer path. The dense
// many-clause corpus is the pre-BS1 case where most clauses stay essential.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, bulk_few,
                        BulkDisjunctionCorpus::Few, true, 0, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, pull_few,
                        BulkDisjunctionCorpus::Few, false, 0, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, bulk_dense,
                        BulkDisjunctionCorpus::Dense, true, 0, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, pull_dense,
                        BulkDisjunctionCorpus::Dense, false, 0, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, bulk_dense_domain,
                        BulkDisjunctionCorpus::Dense, true, 16, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, pull_dense_domain,
                        BulkDisjunctionCorpus::Dense, false, 16, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, bulk_dense_domain_sel,
                        BulkDisjunctionCorpus::Dense, true, 256, false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKBulkDisjunction, bulk_dense_domain_arr,
                        BulkDisjunctionCorpus::Dense, true, 256, true);

// Phrase approximation conjoined with a selective filter term. The filter leads
// the required conjunction, so two-phase phrase verification should only run on
// docs that survive approximation agreement.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKPhraseFilterConjunction, twophase, true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextScoreTopKPhraseFilterConjunction, eager,    false);
