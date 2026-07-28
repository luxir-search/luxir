#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "ImpactsIndex.h"
#include "Query.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/NormsReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/util/solux_util.h"

namespace solux {

class TermQuery final : public solux::Query {
protected:
  std::string_view field;
  std::string_view term;
  Similarity::TermStats injectedTermStats = {};
  float boost;
  bool useFrontierBound;
  bool hasInjectedTermStats = false;
public:
  class Scorer;
  class TermBulkScorer;

  TermQuery(std::string_view field, std::string_view term, float boost = 1.0f,
            bool useFrontierBound = true)
      : field(field), term(term), boost(boost), useFrontierBound(useFrontierBound) {}

  TermQuery(std::string_view field, std::string_view term,
            const Similarity::TermStats& injectedTermStats, float boost = 1.0f,
            bool useFrontierBound = true)
      : field(field), term(term), injectedTermStats(injectedTermStats), boost(boost),
        useFrontierBound(useFrontierBound), hasInjectedTermStats(true) {}

  std::string_view getField() const {
    return field;
  }

  std::string_view getTerm() const {
    return term;
  }

  float getBoost() const {
    return boost;
  }

  bool shouldUseFrontierBound() const {
    return useFrontierBound;
  }

  bool hasInjectedStats() const {
    return hasInjectedTermStats;
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::TERM);
    out.appendString(field);
    out.appendTerm(term);
    // Membership key: boost, the frontier-bound granularity flag, and
    // injected scoring stats all shape scores or skipping, never which docs
    // contain the term, so score-decorated variants share one entry.
    return FilterKeyScope::SEGMENT_STABLE;
  }

  const Similarity::TermStats& scoringTermStats(const CachedTermInfo& cachedTermInfo) const {
    return hasInjectedTermStats ? injectedTermStats : cachedTermInfo.termStats;
  }

  TermQuery::Weight* createWeight(Context& context, int32_t flags,
                                  float multiplier = 1.0f) override {
    return context.pool.make<TermQuery::Weight>(context, *this, flags, multiplier);
  }

  class Weight final : public Query::Weight {
  protected:
    TermQuery& query;
    solux::CachedFieldInfo* cachedFieldInfo = nullptr;
    solux::CachedTermInfo* cachedTermInfo = nullptr;
    solux::Similarity::BM25Scorer* simScorer = nullptr;
    float boost;
  public:
    Weight(Context& context, TermQuery& query, int32_t flags, float multiplier)
            : Query::Weight(context, flags), query(query),
              boost(checkedBoostProduct(multiplier, query.getBoost())) {
      bool needScores = (flags & NEED_SCORES) != 0;
      // Filter-style terms match normally but always score 0.
      if (!needScores) traits |= IS_CONSTANT_SCORING;
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
      if (cachedFieldInfo != nullptr) {
        cachedTermInfo = context.getCachedTerminfo(*cachedFieldInfo, query.getTerm());
      }
      // Only set up the BM25 sim scorer when this clause's score is actually
      // read. FuzzyQuery injects blended stats per clause, so those scorers are
      // weight-local; normal term queries keep sharing the cached scorer.
      if (needScores && cachedTermInfo != nullptr) {
        if (query.hasInjectedStats()) {
          simScorer = context.pool.make<solux::Similarity::BM25Scorer>(
              solux::Similarity().getScorer(
                  1.0f, cachedFieldInfo->fieldStats, query.scoringTermStats(*cachedTermInfo)));
        } else {
          if (cachedTermInfo->simScorer == nullptr) {
            cachedTermInfo->simScorer = context.pool.make<solux::Similarity::BM25Scorer>(
                solux::Similarity().getScorer(1.0f, cachedFieldInfo->fieldStats,
                                              cachedTermInfo->termStats));
          }
          simScorer = cachedTermInfo->simScorer;
        }
      }
    }


    Query::Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      if (cachedTermInfo == nullptr) {
        // term doesn't exist in any segment
        return nullptr;
      }
      solux::DocsFreqEnum* docsEnum = cachedTermInfo->useDocsEnum<DocsEnumTier::FREQS>(
          targetPool, segment);
      if (docsEnum == nullptr) {
        // term doesn't exist in this segment
        return nullptr;
      }

      if ((inputFlags & NEED_SCORES) == 0) {
        // Matching does not need norms or BM25 when score() is never read.
        return targetPool.make<TermQuery::Scorer>(
            *docsEnum, (solux::NormsReader*)nullptr,
            (solux::Similarity::BM25Scorer*)nullptr, boost);
      }

      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord]; // this segFieldInfo can't be null at this point
      solux::NormsReader* normsReader = nullptr;
      solux::IntColReader* valueReader = nullptr;
      solux::OrdColReader* ordReader = nullptr;
      if (segFieldInfo->type == FieldType::TEXT) {
        normsReader = targetPool.make<solux::NormsReader>(segment.postingsReader(), *segFieldInfo);
      } else if (segFieldInfo->ordFormat != SegFieldInfo::ORD_NONE) {
        ordReader = targetPool.make<solux::OrdColReader>(segment.postingsReader(), *segFieldInfo);
      } else if (segFieldInfo->columnLoc.offset() > 0) {
        valueReader = targetPool.make<solux::IntColReader>(segment.postingsReader(), *segFieldInfo);
      }
      BlockBounds::TermView sidecarTerm;
      if (const BlockBounds* bounds = segment.blockBounds(query.getField())) {
        sidecarTerm = bounds->find(docsEnum->termOrd());
      }
      if (ordReader != nullptr) {
        return targetPool.make<TermQuery::Scorer>(targetPool, *docsEnum, ordReader,
                                                  simScorer, boost,
                                                  query.shouldUseFrontierBound(), sidecarTerm);
      }
      return targetPool.make<TermQuery::Scorer>(targetPool, *docsEnum, normsReader,
                                                valueReader, simScorer, boost,
                                                query.shouldUseFrontierBound(), sidecarTerm);
    }

    // A term's exact match count is its docFreq - free from the term stats -
    // unless deletions could have removed some of its docs.
    int64_t count(solux::IndexReader::Segment& segment) override {
      if (segment.liveDocs() != nullptr) {
        return -1;
      }
      if (cachedTermInfo == nullptr) {
        return 0;
      }
      return cachedTermInfo->docFreq(segment.ord);
    }

    // Per-segment supplier that exposes the term's real cost (its number of docs
    // in this segment) so compound scorers can order leaders by cost. The
    // default supplier reports maxDoc for every clause, which is useless for
    // e.g. the min-should-match lead/tail split.
    class Supplier final : public Query::ScorerSupplier {
      TermQuery::Weight& weight;
      solux::IndexReader::Segment& segment;
    public:
      Supplier(TermQuery::Weight& weight, solux::IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override {
        if (weight.cachedTermInfo == nullptr) return 0;
        return weight.cachedTermInfo->docFreq(segment.ord);
      }

      Query::Scorer* get(solux::MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return weight.createScorer(targetPool, segment);
      }

      BulkScorer* bulkScorer(MemPool& targetPool) override;
    };

    Query::ScorerSupplier* scorerSupplier(solux::MemPool& targetPool,
                                          solux::IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }

  };

  class Scorer final : public Query::Scorer {
  public:
    solux::DocsFreqEnum& docsEnum;
    // Both absent when scores are not needed; score() is 0. Text fields use
    // normsIter; non-text term queries keep the existing column-backed lookup.
    std::optional<solux::NormsReader::Iterator> normsIter;
    std::optional<solux::IntColReader::Iterator> valueIter;
    solux::OrdColReader* ordReader = nullptr;
    const uint8_t* flatNormsBase = nullptr;
    solux::Similarity::BM25Scorer* simScorer;
    ImpactsIndex impacts;
    float minCompetitiveScore = 0.0f;
    enum class ShallowGranularity { NONE, GROUP, BLOCK };
    // Shallow cursor: the group or block containing the last advanceShallow
    // target, with its lastDoc/impact cached so repeated targets answer
    // without touching chunks.  GROUP means the group is still unparsed and
    // shallowImpact is the group frontier bound. BLOCK means the group is
    // parsed and shallowImpact is the exact block bound. shallowUpTo is -1
    // whenever the cursor is invalid.
    int32_t shallowBlock = -1;
    int32_t shallowMainGroup = -1;
    int32_t shallowUpTo = -1;
    int32_t shallowTarget = -1;
    float shallowImpact = 0.0f;
    ShallowGranularity shallowGranularity = ShallowGranularity::NONE;
    // Group cursor for the setup-only bound methods; independent of the
    // main shallow cursor above.
    int32_t shallowGroup = -1;
    // Query-time multiplier for boosted term clauses, e.g. fuzzy rewrites.
    float boost;
    int64_t skippedImpactBlocks = 0;
    int32_t competitiveUpTo = PostingsReader::END;
    float competitiveBound = 0.0f;

    Scorer(solux::DocsFreqEnum& docsEnum, solux::NormsReader* normsReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f,
           bool useFrontierBound = true)
            : Scorer(docsEnum, normsReader, nullptr, nullptr, simScorer, boost,
                     useFrontierBound) {
    }

    Scorer(solux::DocsFreqEnum& docsEnum, solux::NormsReader* normsReader,
           solux::IntColReader* valueReader, solux::Similarity::BM25Scorer* simScorer,
           float boost = 1.0f, bool useFrontierBound = true)
            : Scorer(docsEnum, normsReader, valueReader, nullptr, simScorer, boost,
                     useFrontierBound) {}

    Scorer(solux::DocsFreqEnum& docsEnum, solux::OrdColReader* ordReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f,
           bool useFrontierBound = true)
            : Scorer(docsEnum, nullptr, nullptr, ordReader, simScorer, boost,
                     useFrontierBound) {}

    Scorer(solux::DocsFreqEnum& docsEnum, solux::NormsReader* normsReader,
           solux::IntColReader* valueReader, solux::OrdColReader* ordReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f,
           bool useFrontierBound = true)
            : docsEnum(docsEnum), ordReader(ordReader), simScorer(simScorer), boost(boost) {
      unused(useFrontierBound);
      // Scoring needs both BM25 and an encoded norm/value lookup, or neither.
      assert((simScorer == nullptr) ==
             (normsReader == nullptr && valueReader == nullptr && ordReader == nullptr));
      assert((normsReader != nullptr) + (valueReader != nullptr) + (ordReader != nullptr) <= 1);
      if (normsReader != nullptr) {
        normsIter.emplace(*normsReader);
        flatNormsBase = normsReader->flatBase();
      }
      if (valueReader != nullptr) valueIter.emplace(*valueReader);
      competitiveBound = std::numeric_limits<float>::infinity();
    }

    Scorer(solux::MemPool& pool, solux::DocsFreqEnum& docsEnum, solux::NormsReader* normsReader,
           solux::Similarity::BM25Scorer* simScorer, float boost = 1.0f,
           bool useFrontierBound = true, BlockBounds::TermView sidecar = {})
            : Scorer(docsEnum, normsReader, simScorer, boost, useFrontierBound) {
      buildImpacts(pool, normsReader != nullptr, useFrontierBound, sidecar);
    }

    Scorer(solux::MemPool& pool, solux::DocsFreqEnum& docsEnum, solux::NormsReader* normsReader,
           solux::IntColReader* valueReader, solux::Similarity::BM25Scorer* simScorer,
           float boost = 1.0f, bool useFrontierBound = true,
           BlockBounds::TermView sidecar = {})
            : Scorer(docsEnum, normsReader, valueReader, simScorer, boost, useFrontierBound) {
      buildImpacts(pool, normsReader != nullptr || valueReader != nullptr,
                   useFrontierBound, sidecar);
    }

    Scorer(solux::MemPool& pool, solux::DocsFreqEnum& docsEnum,
           solux::OrdColReader* ordReader, solux::Similarity::BM25Scorer* simScorer,
           float boost = 1.0f, bool useFrontierBound = true,
           BlockBounds::TermView sidecar = {})
            : Scorer(docsEnum, ordReader, simScorer, boost, useFrontierBound) {
      buildImpacts(pool, ordReader != nullptr, useFrontierBound, sidecar);
    }

    bool hasImpacts() const {
      return !impacts.empty();
    }

    void buildImpacts(solux::MemPool& pool, bool hasNormLookup,
                      bool useFrontierBound = true,
                      BlockBounds::TermView sidecar = {}) {
      if (simScorer == nullptr || !hasNormLookup) {
        competitiveUpTo = PostingsReader::END;
        competitiveBound = std::numeric_limits<float>::infinity();
        return;
      }
      impacts.build(pool, docsEnum, *simScorer, boost, useFrontierBound, sidecar);
      competitiveUpTo = PostingsReader::END;
      competitiveBound = hasImpacts() ? 0.0f : std::numeric_limits<float>::infinity();
    }

    int32_t blockContaining(int32_t target) const {
      return impacts.blockContaining(target);
    }

    int32_t SOLUX_NOINLINE skipNonCompetitiveBlocks(int32_t doc) {
      if (!hasImpacts() || !(minCompetitiveScore > 0.0f)) {
        competitiveUpTo = PostingsReader::END;
        competitiveBound = hasImpacts() ? 0.0f : std::numeric_limits<float>::infinity();
        return doc;
      }
      // The impacts index resolves the whole hop internally (skipping dead
      // groups on their corner bounds without parsing them); the enum advances
      // ONCE per competitive landing rather than once per block.
      while (doc != PostingsReader::END) {
        skipCount(SkipStats::impactCompetitiveColdLookups);
        auto landing = impacts.firstCompetitiveTarget(doc, minCompetitiveScore,
                                                      skippedImpactBlocks);
        if (landing.doc == PostingsReader::END) {
          return PostingsReader::END;
        }
        competitiveUpTo = landing.lastDoc;
        competitiveBound = landing.impact;
        if (landing.doc == doc) {
          return doc;
        }
        doc = docsEnum.advance(landing.doc);
        if (doc <= competitiveUpTo) {
          return doc;
        }
      }
      return doc;
    }

    int32_t competitivePosting(int32_t doc) {
      return doc <= competitiveUpTo ? doc : skipNonCompetitiveBlocks(doc);
    }

    int32_t nextCompetitive() {
      return competitivePosting(docsEnum.nextDoc());
    }

    int32_t advanceCompetitive(int32_t target) {
      return competitivePosting(docsEnum.advance(target));
    }

    int32_t next() override {
      return nextCompetitive();
    }

    int32_t advance(int32_t target) override {
      return docsEnum.advance(target);
    }

    int32_t advanceScoredProbe(int32_t target) {
      return docsEnum.advanceScoredProbe(target);
    }

    int32_t nextScoredProbe() {
      return docsEnum.advanceScoredProbe(docsEnum.docId() + 1);
    }

    bool matchScoredProbe(int32_t target, float& score) SOLUX_INLINE {
      if (simScorer == nullptr) {
        int32_t doc = docsEnum.docId();
        if (doc < target) {
          doc = docsEnum.advanceScoredProbe(target);
        }
        score = 0.0f;
        return doc == target;
      }
      int32_t tf;
      if (!docsEnum.matchScoredProbe(target, tf)) {
        return false;
      }
      int64_t encodedNorm = flatNormsBase != nullptr ? flatNormsBase[target]
                                                     : advanceNorm(target);
      score = boost * simScorer->score((float) tf, encodedNorm);
      return true;
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docsEnum.docId();
    }

    // Keep the norms advance out-of-line at the per-doc score() call site.
    //
    // score() is the vtable target the per-doc disjunction path calls once per
    // matching doc. Keep norm lookup out-of-line here so scorer code size stays
    // stable. Block scoring (fillScoresFromSpans) deliberately does NOT use this
    // helper, so the block/throughput path still inlines the lookup.
    //
    // Without this NOINLINE, BM_FullTextScoreTopKBulkDisjunction/bulk_few
    // regressed +43.5% in a gcc-release A/B - and bulk_few does not even execute
    // the edit that triggered it; the inliner just re-evaluated the whole TU.
    // This is a fragile codegen workaround, not a fundamental constraint.
    // REVISIT with a future compiler: re-run that benchmark with and without
    // SOLUX_NOINLINE and drop it if the inliner no longer over-pulls.
    // Observed on: g++ (Ubuntu) 16.0.1 20260322 experimental (trunk r16-8246).
    int64_t lookupNorm(int32_t doc) {
      // Flat norms (the common text case) are a direct byte load - same
      // equivalence the block path (fillScoresFromSpans) already relies on.
      if (flatNormsBase != nullptr) {
        return flatNormsBase[doc];
      }
      if (normsIter) {
        int32_t normDoc = normsIter->advance(doc);
        assert(normDoc == doc);
        return normsIter->value();
      }
      if (ordReader != nullptr) return ordReader->firstOrdAt(doc);
      int32_t normDoc = valueIter->advance(doc);
      assert(normDoc == doc);
      return valueIter->value();
    }

    int64_t SOLUX_NOINLINE advanceNorm(int32_t doc) {
      return lookupNorm(doc);
    }

    float score() override {
      if (simScorer == nullptr) return 0.0f;
      auto docid = docsEnum.docId();
      int32_t tf = docsEnum.termFreq();
      // Keep the flat-norms load inline (it is one indexed byte read); the
      // NOINLINE advanceNorm wrapper stays for the sparse iterator walk only,
      // which is the code the inliner used to over-pull into score().
      int64_t encodedNorm = flatNormsBase != nullptr ? flatNormsBase[docid]
                                                     : advanceNorm(docid);
      return boost * simScorer->score((float) tf, encodedNorm);
    }

    static void markCandidate(std::span<uint64_t> matchedWords, int32_t index) {
      if (!matchedWords.empty()) {
        matchedWords[(size_t) (index >> 6)] |= 1ULL << (index & 63);
      }
    }

    int32_t applyToCandidatesImpl(int32_t* docs, float* scores,
                                  int32_t size, bool required,
                                  std::span<uint64_t> matchedWords) {
      assert(size >= 0);
      assert(matchedWords.empty()
          || matchedWords.size() * 64 >= (size_t) size);
      const int64_t advanceCallsBefore = SkipStats::advanceCalls;
      const int64_t wordProbeBeginsBefore = SkipStats::scoredWordProbeAdvances;
      int32_t matches = 0;
      int32_t write = 0;
      int32_t current = docsEnum.docId();
      int32_t i = 0;
      while (i < size) {
        int32_t target = docs[i];
        if (current < target) {
          current = docsEnum.advanceScoredProbe(target);
        }
        bool matched = current == target;
        matches += (int32_t) matched;
        if (matched) {
          markCandidate(matchedWords, i);
        }
        if (matched && simScorer != nullptr) {
          int32_t tf = docsEnum.termFreq();
          int64_t encodedNorm = flatNormsBase != nullptr ? flatNormsBase[target]
                                                         : advanceNorm(target);
          scores[i] += boost * simScorer->score((float) tf, encodedNorm);
        }
        if (required && matched) {
          if (write != i) {
            docs[write] = docs[i];
            scores[write] = scores[i];
          }
          write++;
        }
        i++;
      }
      if (SkipStats::enabled) {
        const int64_t advances = SkipStats::advanceCalls - advanceCallsBefore;
        const int64_t wordProbeBegins =
            SkipStats::scoredWordProbeAdvances - wordProbeBeginsBefore;
        SkipStats::applyToCandidatesCalls += 1;
        SkipStats::applyToCandidatesCandidates += size;
        SkipStats::applyToCandidatesAdvances += advances;
        SkipStats::applyToCandidatesMatches += matches;
        SkipStats::applyToCandidatesWordProbeBegins += wordProbeBegins;
        SkipStats::applyToCandidatesPlainAdvanceFallbacks += advances - wordProbeBegins;
      }
      return required ? write : size;
    }

    int32_t applyToCandidates(int32_t* docs, float* scores,
                              int32_t size, bool required) override {
      return applyToCandidatesImpl(docs, scores, size, required, {});
    }

    void addToCandidates(int32_t* docs, float* scores, int32_t size,
                         std::span<uint64_t> matchedWords) {
      applyToCandidatesImpl(docs, scores, size, false, matchedWords);
    }

    void addMatchesToCandidates(int32_t* docs, int32_t size,
                                std::span<uint64_t> matchedWords) {
      assert(size >= 0);
      assert(matchedWords.size() * 64 >= (size_t) size);
      int32_t current = docsEnum.docId();
      int32_t i = 0;
      while (i < size) {
        int32_t target = docs[i];
        if (current < target) {
          current = docsEnum.advanceDocOnly(target);
        }
        if (current == target) {
          markCandidate(matchedWords, i);
        }
        i++;
      }
    }

    int32_t fillScoreBlockScalar(int32_t* docs, float* scores, int32_t count, int32_t upTo,
                                 bool includeCurrent) {
      assert(count >= 0);
      int32_t filled = 0;
      int32_t doc = docsEnum.docId();
      if (doc < 0) {
        doc = nextCompetitive();
      } else if (!includeCurrent) {
        if (doc >= PostingsReader::END - 1) {
          return 0;
        }
        doc = advanceCompetitive(doc + 1);
      }
      while (filled < count && doc < upTo) {
        docs[filled] = doc;
        if (simScorer == nullptr) {
          scores[filled] = 0.0f;
        } else {
          int32_t tf = docsEnum.termFreq();
          auto encodedNorm = lookupNorm(doc);
          scores[filled] = boost * simScorer->score((float) tf, encodedNorm);
        }
        filled++;
        doc = nextCompetitive();
      }
      return filled;
    }

    void fillScoresFromSpans(int32_t* docs, float* scores, std::span<const int32_t> blockDocs,
                             std::span<const int32_t> blockFreqs, int32_t count) {
      for (int32_t i = 0; i < count; i++) {
        docs[i] = blockDocs[(size_t) i];
      }
      if (simScorer == nullptr) {
        std::fill(scores, scores + count, 0.0f);
        return;
      }
      if (flatNormsBase != nullptr) {
        assert(count <= Postings::DOCS_BLOCK_SIZE);
        uint8_t normBuf[Postings::DOCS_BLOCK_SIZE];
        for (int32_t i = 0; i < count; i++) {
          int32_t doc = blockDocs[(size_t) i];
          normBuf[i] = flatNormsBase[doc];
        }
        simScorer->scoreBlock(blockFreqs.data(), normBuf, boost, scores, count);
        return;
      }
      for (int32_t i = 0; i < count; i++) {
        int32_t doc = blockDocs[(size_t) i];
        auto encodedNorm = lookupNorm(doc);
        scores[i] = boost * simScorer->score((float) blockFreqs[(size_t) i], encodedNorm);
      }
    }

    int32_t fillScoreBlock(int32_t* docs, float* scores, int32_t count, int32_t upTo) override {
      assert(count >= 0);
      if (count <= 0) {
        return 0;
      }

      // The scalar path owns exact impact-threshold skipping. The block span
      // path is used by BS1, which does not push child term thresholds.
      if (hasImpacts() && minCompetitiveScore > 0.0f) {
        return fillScoreBlockScalar(docs, scores, count, upTo, true);
      }

      int32_t filled = 0;
      while (filled < count) {
        auto [blockDocs, blockFreqs] = docsEnum.peekDocFreqBlock();
        int32_t available = (int32_t) blockDocs.size();
        if (available == 0) {
          break;
        }

        int32_t limit = std::min(available, count - filled);
        int32_t used = 0;
        if (blockDocs[(size_t) limit - 1] < upTo) {
          used = limit;
        } else {
          while (used < limit && blockDocs[(size_t) used] < upTo) {
            used++;
          }
        }

        if (used == 0) {
          break;
        }
        fillScoresFromSpans(docs + filled, scores + filled, blockDocs, blockFreqs, used);
        docsEnum.consumeDocFreqBlock(used);
        filled += used;

        if (used < available) {
          break;
        }
      }
      return filled;
    }

    // Consume postings below windowEnd while retaining each posting's
    // frequency at its window-relative doc offset. Unlike fillWindowBits(),
    // this keeps the enum in frequency-capable mode so the same cursor can
    // resume ordinary scored execution after the window.
    void fillWindowBitsAndFreqs(std::span<uint64_t> windowBits,
                                std::span<int32_t> windowFreqs,
                                int32_t windowStart, int32_t windowEnd) {
      assert(windowEnd >= windowStart);
      assert(windowFreqs.size() >= (size_t) (windowEnd - windowStart));
      skipCount(SkipStats::countBulkFillCalls);
      while (windowEnd > windowStart) {
        auto [blockDocs, blockFreqs] = docsEnum.peekDocFreqBlock();
        int32_t available = (int32_t) blockDocs.size();
        if (available == 0) {
          return;
        }

        int32_t used = 0;
        while (used < available && blockDocs[(size_t) used] < windowStart) {
          used++;
        }
        int32_t firstEmit = used;
        while (used < available && blockDocs[(size_t) used] < windowEnd) {
          used++;
        }
        for (int32_t i = firstEmit; i < used; i++) {
          int32_t offset = blockDocs[(size_t) i] - windowStart;
          windowBits[(size_t) (offset >> 6)] |= 1ULL << (offset & 63);
          windowFreqs[(size_t) offset] = blockFreqs[(size_t) i];
        }
        if (used > firstEmit) {
          skipCount(SkipStats::countBulkFillBlocks);
          if (SkipStats::enabled) {
            SkipStats::countBulkFillDocs += used - firstEmit;
          }
        }
        if (used == 0) {
          return;
        }
        docsEnum.consumeDocFreqBlock(used);
        if (used < available) {
          return;
        }
      }
    }

    float scoreFreqWithNorm(int32_t freq, int64_t encodedNorm) const {
      assert(simScorer != nullptr);
      return boost * simScorer->score((float) freq, encodedNorm);
    }

    void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                        int32_t windowEnd) override {
      assert(windowEnd >= windowStart);
      skipCount(SkipStats::countBulkFillCalls);
      if (windowEnd <= windowStart) {
        return;
      }
      docsEnum.intoBitSet(windowBits, windowStart, windowEnd);
    }

    bool supportsWindowFilter() const override {
      return true;
    }

    DocsFreqEnum* windowFilterProbeDocsEnum() override {
      return &docsEnum;
    }

    void setMinCompetitiveScore(float minScore) override {
      bool rose = minScore > minCompetitiveScore;
      minCompetitiveScore = minScore;
      if (!rose || competitiveUpTo < 0) {
        return;
      }
      if (minScore > competitiveBound) {
        competitiveUpTo = -1;
        skipCount(SkipStats::impactCertificateInvalidations);
      } else {
        skipCount(SkipStats::impactCertificateSurvivedRises);
      }
    }

    int32_t parsedBlockContainingClamped(int32_t group, int32_t target) const {
      int32_t block = impacts.blockContainingInParsedGroup(group, target);
      int32_t last = impacts.groupLastBlock(group);
      return block > last ? last : block;
    }

    int32_t rangeStartDoc() const {
      return shallowGranularity == ShallowGranularity::NONE ? docsEnum.docId() : shallowTarget;
    }

    void resetShallowCursor() {
      shallowBlock = -1;
      shallowMainGroup = -1;
      shallowUpTo = -1;
      shallowImpact = 0.0f;
      shallowGranularity = ShallowGranularity::NONE;
    }

    void setShallowToParsedBlock(int32_t target, int32_t group) {
      shallowMainGroup = group;
      shallowBlock = parsedBlockContainingClamped(group, target);
      shallowUpTo = impacts.parsedBlockLastDoc(shallowBlock);
      shallowImpact = impacts.parsedBlockImpact(shallowBlock);
      shallowGranularity = ShallowGranularity::BLOCK;
    }

    void setShallowToGroup(int32_t group) {
      shallowMainGroup = group;
      shallowBlock = -1;
      shallowUpTo = impacts.groupLastDoc(group);
      shallowImpact = impacts.maxGroupImpactInRange(group, group);
      shallowGranularity = ShallowGranularity::GROUP;
      skipCount(SkipStats::impactGroupShallowAnswers);
    }

    bool refreshShallowIfParsed() {
      if (shallowGranularity == ShallowGranularity::GROUP && shallowMainGroup >= 0
          && impacts.groupParsed(shallowMainGroup)) {
        setShallowToParsedBlock(shallowTarget, shallowMainGroup);
        return true;
      }
      return false;
    }

    int32_t rangeStartBlockNoParse(int32_t group, int32_t startDoc) const {
      if (!impacts.groupParsed(group)) {
        return impacts.groupFirstBlock(group);
      }
      return parsedBlockContainingClamped(group, startDoc);
    }

    int32_t rangeEndBlockNoParse(int32_t group, int32_t upTo) const {
      if (!impacts.groupParsed(group) || upTo > impacts.groupLastDoc(group)) {
        return impacts.groupLastBlock(group);
      }
      return parsedBlockContainingClamped(group, upTo);
    }

    // Bounds scores over [shallow target, upTo] once advanceShallow() has been
    // called (the Lucene ImpactsDISI contract - callers only score docs at or
    // past their shallow target); before any advanceShallow it bounds from the
    // current doc.  Using the stale current doc to WIDEN the range here made
    // block-max conjunction hops quadratic: the enum stays behind while the
    // target hops ahead, and the widened scan walked every block in between.
    float getMaxScore(int32_t upTo) override {
      if (simScorer == nullptr) return 0.0f;
      if (!hasImpacts()) {
        return std::numeric_limits<float>::infinity();
      }
      if (upTo == PostingsReader::END) {
        return impacts.globalMaxImpact();
      }

      refreshShallowIfParsed();

      // Bound contained in the cached shallow range: answered from the cursor.
      // GROUP is a coarse frontier bound; BLOCK is the exact parsed block
      // bound. A parsed group is refreshed above before this fast path.
      if (upTo <= shallowUpTo) {
        return shallowImpact;
      }

      int32_t groupCount = impacts.numGroups();
      int32_t startDoc = rangeStartDoc();
      int32_t startGroup = shallowGranularity == ShallowGranularity::NONE
          ? impacts.groupContainingFrom(-1, startDoc)
          : shallowMainGroup;
      if (startGroup >= groupCount) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t upGroup;
      if (upTo <= impacts.groupLastDoc(startGroup)) {
        upGroup = startGroup;
      } else {
        upGroup = impacts.groupContainingFrom(startGroup, upTo);
        if (upGroup >= groupCount) {
          upGroup = groupCount - 1;
        }
      }
      if (upGroup < startGroup) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t startBlock = rangeStartBlockNoParse(startGroup, startDoc);
      int32_t upBlock = rangeEndBlockNoParse(upGroup, upTo);
      if (upBlock < startBlock) {
        return std::numeric_limits<float>::infinity();
      }
      if (startBlock == impacts.groupFirstBlock(startGroup)
          && upBlock == impacts.groupLastBlock(upGroup)) {
        if (upGroup == groupCount - 1) {
          return impacts.maxGroupImpactFrom(startGroup);
        }
        return impacts.maxGroupImpactInRange(startGroup, upGroup);
      }
      return impacts.maxImpactInRangeNoParse(startBlock, upBlock);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return ScoreBounds::nonNegative(getMaxScore(upTo));
    }

    float refineMaxScore(int32_t upTo) override {
      if (simScorer == nullptr) return 0.0f;
      if (!hasImpacts()) {
        return std::numeric_limits<float>::infinity();
      }
      skipCount(SkipStats::impactRefinesTriggered);

      int32_t startDoc = rangeStartDoc();
      int32_t startBlock = shallowGranularity == ShallowGranularity::BLOCK
          ? shallowBlock
          : impacts.blockContainingFrom(shallowBlock, startDoc);
      if (startBlock >= impacts.blockCount()) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t upBlock;
      if (upTo <= impacts.parsedBlockLastDoc(startBlock)) {
        upBlock = startBlock;
      } else {
        upBlock = impacts.blockContainingFrom(startBlock, upTo);
        if (upBlock >= impacts.blockCount()) {
          upBlock = impacts.blockCount() - 1;
        }
      }
      if (upBlock < startBlock) {
        return std::numeric_limits<float>::infinity();
      }
      float bound = impacts.maxImpactInRangeParsed(startBlock, upBlock);
      refreshShallowIfParsed();
      return bound;
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return ScoreBounds::nonNegative(refineMaxScore(upTo));
    }

    float getMaxScoreForSetup(int32_t upTo) override {
      if (simScorer == nullptr) return 0.0f;
      if (!hasImpacts()) {
        return std::numeric_limits<float>::infinity();
      }
      if (upTo == PostingsReader::END) {
        return impacts.globalMaxImpact();
      }

      int32_t startGroup = shallowGroup >= 0
          ? shallowGroup
          : impacts.groupContainingFrom(-1, docsEnum.docId());
      int32_t groupCount = impacts.numGroups();
      if (startGroup >= groupCount) {
        return std::numeric_limits<float>::infinity();
      }

      int32_t upGroup;
      if (upTo <= impacts.groupLastDoc(startGroup)) {
        upGroup = startGroup;
      } else {
        upGroup = impacts.groupContainingFrom(startGroup, upTo);
        if (upGroup >= groupCount) {
          upGroup = groupCount - 1;
        }
      }
      if (upGroup < startGroup) {
        return std::numeric_limits<float>::infinity();
      }
      if (upGroup == groupCount - 1) {
        return impacts.maxGroupImpactFrom(startGroup);
      }
      return impacts.maxGroupImpactInRange(startGroup, upGroup);
    }

    int32_t advanceShallowForSetup(int32_t target) override {
      if (!hasImpacts()) return PostingsReader::END;
      shallowGroup = impacts.groupContainingFrom(shallowGroup, target);
      return shallowGroup >= impacts.numGroups() ? PostingsReader::END
                                                 : impacts.groupLastDoc(shallowGroup);
    }

    int32_t advanceShallow(int32_t target) override {
      if (!hasImpacts()) {
        return PostingsReader::END;
      }
      // Same-range fast path.  A cached GROUP range is refreshed if another
      // caller refined it since it was cached.
      if (target >= shallowTarget && target <= shallowUpTo && !refreshShallowIfParsed()) {
        shallowTarget = target;
        skipCount(SkipStats::shallowCacheHits);
        return shallowUpTo;
      }
      skipCount(SkipStats::shallowCursorMoves);
      shallowTarget = target;
      shallowMainGroup = impacts.groupContainingFrom(shallowMainGroup, target);
      if (shallowMainGroup >= impacts.numGroups()) {
        resetShallowCursor();
        shallowTarget = target;
        return PostingsReader::END;
      }
      if (impacts.groupParsed(shallowMainGroup)) {
        setShallowToParsedBlock(target, shallowMainGroup);
      } else {
        setShallowToGroup(shallowMainGroup);
      }
      return shallowUpTo;
    }

    int64_t skippedBlocks() const {
      return skippedImpactBlocks;
    }

    /// term frequency for current doc
    int termFreq() {
      return docsEnum.termFreq();
    }

    // make a pusher / visitor for term scorer?

  };

  class TermBulkScorer final : public BulkScorer {
    static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    static constexpr int32_t kWindowWords = kWindowSize / 64;
    static_assert((kWindowSize % 64) == 0);

    TermQuery::Scorer* scorer;
    WindowFilter* windowFilter = nullptr;
    std::span<uint64_t> windowBits;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    int32_t maxDoc;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();

    void setWindowBounds(int32_t start, int32_t max) {
      windowStart = start;
      int32_t requestedEnd = windowStart + kWindowSize;
      if (requestedEnd < windowStart) {
        requestedEnd = max;
      }
      windowEnd = std::min(std::min(requestedEnd, max), maxDoc);
    }

    void clearWindowBits() {
      std::fill(windowBits.begin(), windowBits.end(), 0);
    }

    static uint64_t validMask(int32_t remaining) {
      if (remaining >= 64) return ~0ULL;
      if (remaining <= 0) return 0;
      return (1ULL << remaining) - 1ULL;
    }

    void applyDomainBits(const FixedBitSet* domainBits) {
      assert(domainBits != nullptr);
      int32_t domainWords = (int32_t) FixedBitSet::sizeInWords(domainBits->size());
      for (size_t w = 0; w < windowBits.size(); w++) {
        int32_t firstDoc = windowStart + (int32_t) (w << 6);
        int32_t remaining = windowEnd - firstDoc;
        if (remaining <= 0) {
          windowBits[w] = 0;
          continue;
        }
        uint64_t mask = validMask(remaining);
        int32_t sourceWord = firstDoc >> 6;
        int32_t shift = firstDoc & 63;
        uint64_t domainWord = 0;
        if (sourceWord < domainWords) {
          domainWord = domainBits->words[sourceWord] >> shift;
          if (shift != 0 && sourceWord + 1 < domainWords) {
            domainWord |= domainBits->words[sourceWord + 1] << (64 - shift);
          }
        }
        windowBits[w] &= domainWord & mask;
      }
    }

    void applyDocSetFilter(DocSet* filter) {
      int32_t innerSize = windowEnd - windowStart;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          int32_t doc = windowStart + index;
          if (!filter->get(doc)) {
            windowBits[(size_t) word] &= ~(1ULL << bit);
          }
          bits &= bits - 1;
        }
      }
    }

    int64_t popCountWindowBits() const {
      int64_t total = 0;
      for (uint64_t bits : windowBits) {
        total += std::popcount(bits);
      }
      return total;
    }

    bool acceptsDoc(DocSet* filter, int32_t doc) {
      if (filter != nullptr && !filter->get(doc)) {
        return false;
      }
      return windowFilter == nullptr
          || (windowFilter->probes() ? windowFilter->acceptsProbe(doc)
                                     : windowFilter->accepts(doc));
    }

    void applyWindowFilterProbe() {
      int32_t innerSize = windowEnd - windowStart;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          if (!windowFilter->acceptsProbe(windowStart + index)) {
            windowBits[(size_t) word] &= ~(1ULL << bit);
          }
          bits &= bits - 1;
        }
      }
    }

    void prepareOutputWindow(ScoreWindow& out) {
      out.min = windowStart;
      out.max = windowEnd;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
    }

  public:
    TermBulkScorer(MemPool& pool, TermQuery::Scorer* scorer, int32_t maxDoc)
        : scorer(scorer),
          windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
          outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
          maxDoc(maxDoc) {
    }

    bool attachWindowFilter(WindowFilter* filter) override {
      assert(windowFilter == nullptr);
      windowFilter = filter;
      return true;
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }

      setWindowBounds(min, max);
      if (windowFilter != nullptr && !windowFilter->probes()
          && windowFilter->prepare(windowStart, windowEnd) == 0) {
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      clearWindowBits();
      scorer->fillWindowBits(windowBits, windowStart, windowEnd);
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        applyDomainBits(&((BitDocSet*) filter)->bits());
      } else if (filter != nullptr) {
        applyDocSetFilter(filter);
      }
      if (windowFilter != nullptr) {
        if (windowFilter->probes()) {
          applyWindowFilterProbe();
        } else {
          windowFilter->intersect(windowBits);
        }
      }
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
        domainOut->addWindowWords(windowBits.data(), windowStart, windowEnd);
      }
      count += popCountWindowBits();

      if (windowEnd >= max) {
        return PostingsReader::END;
      }
      return windowEnd;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      max = std::min(max, maxDoc);
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (minCompetitiveScore > this->minCompetitiveScore) {
        this->minCompetitiveScore = minCompetitiveScore;
        scorer->setMinCompetitiveScore(minCompetitiveScore);
      }

      int32_t doc = scorer->docId();
      if (doc < min) {
        doc = scorer->advance(min);
      }
      while (doc < max) {
        windowStart = doc;
        int32_t requestedEnd = windowStart + kWindowSize;
        if (requestedEnd < windowStart) {
          requestedEnd = max;
        }
        windowEnd = std::min(std::min(requestedEnd, max), maxDoc);

        int32_t upTo = scorer->advanceShallow(doc);
        if (upTo != PostingsReader::END) {
          if (upTo < doc) {
            upTo = doc;
          }
          int32_t shallowEnd = upTo >= PostingsReader::END - 1 ? max : upTo + 1;
          windowEnd = std::min(windowEnd, shallowEnd);
        }

        float maxScore = scorer->getMaxScore(windowEnd - 1);
        if (std::isfinite(maxScore)
            && (double) maxScore < (double) this->minCompetitiveScore) {
          if (windowEnd >= max) {
            out.max = max;
            return PostingsReader::END;
          }
          doc = scorer->advance(windowEnd);
          continue;
        }

        prepareOutputWindow(out);
        if (windowFilter != nullptr && !windowFilter->probes()
            && windowFilter->prepare(windowStart, windowEnd) == 0) {
          return windowEnd >= max ? PostingsReader::END : windowEnd;
        }
        int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
        float blockScores[Postings::DOCS_BLOCK_SIZE];
        int32_t n = 0;
        while ((n = scorer->fillScoreBlock(blockDocs, blockScores,
                                           Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
          for (int32_t i = 0; i < n; i++) {
            int32_t matchedDoc = blockDocs[i];
            float score = blockScores[i];
            if (acceptsDoc(filter, matchedDoc)
                && score >= this->minCompetitiveScore) {
              assert(out.size < kWindowSize);
              out.docs[(size_t) out.size] = matchedDoc;
              out.scores[(size_t) out.size] = score;
              out.size++;
            }
          }
        }
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      out.max = max;
      return PostingsReader::END;
    }
  };

};

inline BulkScorer* TermQuery::Weight::Supplier::bulkScorer(MemPool& targetPool) {
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(weight.createScorer(targetPool, segment));
  if (scorer == nullptr) {
    return nullptr;
  }
  return targetPool.make<TermQuery::TermBulkScorer>(targetPool, scorer, segment.maxDoc());
}

} // namespace solux
