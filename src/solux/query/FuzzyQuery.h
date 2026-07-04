#pragma once

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "MultiTermQuery.h"
#include "solux/reader/FuzzySeekEnum.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/NormsReader.h"
#include "solux/util/MemPool.h"

namespace solux {

// Fuzzy term query over byte-wise Levenshtein distance. Filter use cases take
// the constant-score MultiTermQuery path; scoring use cases blend expansion
// stats and score each doc once using the expansion's damped term mass.
class FuzzyQuery final : public MultiTermQuery {
  std::string_view term;
  int maxEdits;
  int prefixLength;   // clamped to <= term length
  int maxExpansions;  // 0 means complete, subject only to the operator limit

  struct ExpansionCandidate {
    std::string_view term;
    float damp = 1.0f;
    Similarity::TermStats termStats = {};
  };

  struct Expansion {
    std::string_view term;
    float damp = 1.0f;
    CachedTermInfo* termInfo = nullptr;
  };

  // Copy transient term bytes (FuzzySeekEnum reuses its buffer across terms)
  // into `dst` so a string_view can outlive the enum's next advance.
  static std::string_view copyTerm(MemPool& dst, std::string_view t) {
    if (t.empty()) return {};
    char* d = dst.alloc(t.size());
    memcpy(d, t.data(), t.size());
    return {d, t.size()};
  }

  static bool betterExpansion(const ExpansionCandidate& a, const ExpansionCandidate& b) {
    if (a.damp != b.damp) return a.damp > b.damp;
    return a.term < b.term;
  }

  static int effectiveLimit(int requested, int operatorLimit) {
    int userLimit = requested > 0 ? requested : std::numeric_limits<int>::max();
    int opLimit = operatorLimit > 0 ? operatorLimit : std::numeric_limits<int>::max();
    return std::min(userLimit, opLimit);
  }

public:
  FuzzyQuery(std::string_view field, std::string_view term, int maxEdits,
             int prefixLength = 0, int maxExpansions = 0, float boost = 1.0f)
    : MultiTermQuery(field, boost), term(term), maxEdits(maxEdits),
      prefixLength(std::min(prefixLength, (int)term.size())), maxExpansions(maxExpansions) {}

  std::string_view getTerm() const { return term; }
  int getMaxEdits() const { return maxEdits; }
  int getPrefixLength() const { return prefixLength; }
  int getMaxExpansions() const { return maxExpansions; }

  // Filter path: MultiTermQuery builds the constant-score bitset from this.
  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    std::string_view prefix = term.substr(0, prefixLength);
    std::string_view suffix = term.substr(prefixLength);
    return pool.make<FuzzySeekEnum>(pool, te, prefix, suffix, maxEdits);
  }

  class BlendedScorer final : public Query::Scorer {
  public:
    struct Sub {
      DocsEnum* docsEnum = nullptr;
      float damp = 1.0f;
      int32_t doc = -1;
    };

  private:
    std::span<Sub> subs;
    std::span<int32_t> heap;
    int32_t heapSize = 0;
    std::optional<NormsReader::Iterator> normsIter;
    std::optional<IntColReader::Iterator> valueIter;
    Similarity::BM25Scorer* simScorer = nullptr;
    float boost = 1.0f;
    int32_t currentDoc = -1;
    float currentFreq = 0.0f;
    float currentRawFreq = 0.0f;

    struct HeapComp {
      Sub* subs = nullptr;
      bool operator()(int32_t lhs, int32_t rhs) const {
        int32_t lhsDoc = subs[lhs].doc;
        int32_t rhsDoc = subs[rhs].doc;
        if (lhsDoc != rhsDoc) return lhsDoc > rhsDoc;
        return lhs > rhs;
      }
    };

    HeapComp comp() {
      return HeapComp{subs.data()};
    }

    int32_t popHeap() {
      std::pop_heap(heap.begin(), heap.begin() + heapSize, comp());
      return heap[(size_t)--heapSize];
    }

    void pushHeap(int32_t idx) {
      heap[(size_t)heapSize++] = idx;
      std::push_heap(heap.begin(), heap.begin() + heapSize, comp());
    }

    void advanceLeadTerms(int32_t target) {
      while (heapSize > 0) {
        int32_t idx = heap[0];
        if (subs[(size_t)idx].doc >= target) return;
        idx = popHeap();
        Sub& sub = subs[(size_t)idx];
        int32_t doc = sub.docsEnum->advance(target);
        if (doc != PostingsReader::END) {
          sub.doc = doc;
          pushHeap(idx);
        }
      }
    }

    int32_t collectCurrentDoc() {
      if (heapSize == 0) {
        currentDoc = PostingsReader::END;
        currentFreq = 0.0f;
        currentRawFreq = 0.0f;
        return currentDoc;
      }

      currentDoc = subs[(size_t)heap[0]].doc;
      currentFreq = 0.0f;
      currentRawFreq = 0.0f;
      do {
        int32_t idx = popHeap();
        Sub& sub = subs[(size_t)idx];
        float tf = (float)sub.docsEnum->termFreq();
        currentFreq += sub.damp * tf;
        currentRawFreq += tf;
        int32_t doc = sub.docsEnum->nextDoc();
        if (doc != PostingsReader::END) {
          sub.doc = doc;
          pushHeap(idx);
        }
      } while (heapSize > 0 && subs[(size_t)heap[0]].doc == currentDoc);
      return currentDoc;
    }

    int64_t lookupNorm(int32_t doc) {
      if (normsIter) {
        int32_t normDoc = normsIter->advance(doc);
        assert(normDoc == doc);
        return normsIter->value();
      }
      int32_t normDoc = valueIter->advance(doc);
      assert(normDoc == doc);
      return valueIter->value();
    }

  public:
    BlendedScorer(MemPool& pool, std::span<Sub> subs, NormsReader* normsReader,
                  IntColReader* valueReader, Similarity::BM25Scorer* simScorer,
                  float boost)
      : subs(subs), heap(pool.make_span<int32_t>(subs.size())), simScorer(simScorer),
        boost(boost) {
      assert(simScorer != nullptr);
      assert(normsReader == nullptr || valueReader == nullptr);
      assert(normsReader != nullptr || valueReader != nullptr);
      if (normsReader != nullptr) normsIter.emplace(*normsReader);
      if (valueReader != nullptr) valueIter.emplace(*valueReader);

      for (int32_t i = 0; i < (int32_t)subs.size(); i++) {
        int32_t doc = subs[(size_t)i].docsEnum->nextDoc();
        if (doc != PostingsReader::END) {
          subs[(size_t)i].doc = doc;
          heap[(size_t)heapSize++] = i;
        }
      }
      std::make_heap(heap.begin(), heap.begin() + heapSize, comp());
    }

    int32_t next() override {
      return collectCurrentDoc();
    }

    int32_t advance(int32_t target) override {
      assert(currentDoc < target);
      advanceLeadTerms(target);
      return collectCurrentDoc();
    }

    int32_t docId() override {
      return currentDoc;
    }

    // The damp multiplier is applied OUTSIDE the similarity, to the raw
    // summed freq, rather than feeding the damped freq into BM25: tf
    // saturation would compress damping passed through the tf argument
    // (one edit would cost far less than its intended share), and the
    // outside multiplier reduces exactly to dampedTermScore for the
    // common one-term-per-doc case. currentFreq / currentRawFreq is the
    // freq-weighted average damp of the expansion terms in this doc.
    float score() override {
      float damp = currentRawFreq > 0.0f ? currentFreq / currentRawFreq : 0.0f;
      return boost * damp * simScorer->score(currentRawFreq, lookupNorm(currentDoc));
    }
  };

  class BlendedTermWeight final : public Query::Weight {
    FuzzyQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    std::span<Expansion> expansions;
    Similarity::TermStats blendedStats = {};
    Similarity::BM25Scorer* simScorer = nullptr;

    std::vector<ExpansionCandidate> collectCandidates(Context& context, MemPool& scratch,
                                                       CachedFieldInfo& fieldInfo) {
      boost::unordered_flat_map<std::string_view, int32_t,
                                PackedTermHash, PackedTermEqual> termToIndex;
      std::vector<ExpansionCandidate> candidates;
      std::string_view prefix = query.term.substr(0, query.prefixLength);
      std::string_view suffix = query.term.substr(query.prefixLength);

      size_t nSegs = context.numSegments();
      for (size_t s = 0; s < nSegs; s++) {
        auto* segFieldInfo = fieldInfo.segInfos[s];
        if (segFieldInfo == nullptr) continue;
        auto& postingsReader = context.topReader.segments()[s].postingsReader();
        TermsEnum te(scratch, postingsReader, *segFieldInfo);
        FuzzySeekEnum fte(scratch, te, prefix, suffix, query.maxEdits);
        while (fte.next()) {
          std::string_view t = fte.termView();
          auto iter = termToIndex.find(t);
          if (iter == termToIndex.end()) {
            std::string_view termCopy = copyTerm(scratch, t);
            int32_t idx = (int32_t)candidates.size();
            termToIndex.emplace(termCopy, idx);
            candidates.push_back({termCopy, fte.currentScore(), {}});
            iter = termToIndex.find(termCopy);
          }
          ExpansionCandidate& candidate = candidates[(size_t)iter->second];
          candidate.termStats.docFreq += te.docFreq();
          candidate.termStats.totalTermFreq += te.totalTermFreq();
        }
      }
      return candidates;
    }

    void truncateCandidates(Context& context, std::vector<ExpansionCandidate>& candidates) {
      int limit = effectiveLimit(query.maxExpansions, context.limits.fuzzyMaxExpansions);
      int matched = (int)candidates.size();
      int userLimit = query.maxExpansions > 0 ? query.maxExpansions : std::numeric_limits<int>::max();
      int operatorLimit = context.limits.fuzzyMaxExpansions > 0
          ? context.limits.fuzzyMaxExpansions
          : std::numeric_limits<int>::max();

      if (matched > limit) {
        std::partial_sort(candidates.begin(), candidates.begin() + limit, candidates.end(),
                          betterExpansion);
        candidates.resize((size_t)limit);
      }

      if (matched > operatorLimit && operatorLimit < userLimit) {
        context.warn("fuzzy_clamped",
                     std::format("fuzzy expansion matched {} terms for field '{}' term '{}'; "
                                 "operator limit {} kept closest terms",
                                 matched, query.getField(), query.getTerm(), operatorLimit));
      }
    }

    // The whole expansion scores as ONE pseudo-term with a single blended
    // TermStats: docFreq/ttf = the max across the expanded terms (each
    // already summed across segments during collection). Per-term IDF
    // would invert relevance for an on-by-default typo feature - a
    // misspelling is RARE in the corpus, so its huge IDF can outrank the
    // exact match. Blending to the max df gives every variant the exact
    // term's (or the commonest variant's) IDF, and the edit-distance damp
    // is then the only thing separating exact from typo. Same technique
    // as Lucene's blended/synonym term queries.
    void populateKeptTerms(Context& context, std::vector<ExpansionCandidate>& candidates) {
      if (candidates.empty()) return;

      expansions = context.pool.make_span<Expansion>(candidates.size());
      for (size_t i = 0; i < candidates.size(); i++) {
        ExpansionCandidate& candidate = candidates[i];
        std::string_view termCopy = copyTerm(context.pool, candidate.term);
        CachedTermInfo* termInfo = context.getCachedTerminfo(*cachedFieldInfo, termCopy);
        assert(termInfo != nullptr);
        expansions[i] = {termCopy, candidate.damp, termInfo};
        blendedStats.docFreq = std::max(blendedStats.docFreq, candidate.termStats.docFreq);
        blendedStats.totalTermFreq = std::max(blendedStats.totalTermFreq,
                                              candidate.termStats.totalTermFreq);
      }

      simScorer = context.pool.make<Similarity::BM25Scorer>(
          Similarity().getScorer(1.0f, cachedFieldInfo->fieldStats, blendedStats));
    }

  public:
    BlendedTermWeight(Context& context, FuzzyQuery& query, int32_t flags)
      : Query::Weight(context, flags), query(query) {
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
      if (cachedFieldInfo == nullptr) return;

      MemPool scratch;
      auto candidates = collectCandidates(context, scratch, *cachedFieldInfo);
      truncateCandidates(context, candidates);
      populateKeptTerms(context, candidates);
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      if (cachedFieldInfo == nullptr || expansions.empty()) return nullptr;
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) return nullptr;

      auto* subs = targetPool.make_arr<BlendedScorer::Sub>(expansions.size());
      int32_t subCount = 0;
      for (const Expansion& expansion : expansions) {
        DocsEnum* docsEnum = expansion.termInfo->useDocsEnum(targetPool, segment);
        if (docsEnum == nullptr) continue;
        subs[(size_t)subCount++] = {docsEnum, expansion.damp, -1};
      }
      if (subCount == 0) return nullptr;

      NormsReader* normsReader = nullptr;
      IntColReader* valueReader = nullptr;
      if (segFieldInfo->type == FieldType::TEXT) {
        normsReader = targetPool.make<NormsReader>(segment.postingsReader(), *segFieldInfo);
      } else if (segFieldInfo->columnLoc.offset() > 0) {
        valueReader = targetPool.make<IntColReader>(segment.postingsReader(), *segFieldInfo);
      }
      return targetPool.make<BlendedScorer>(
          targetPool, std::span<BlendedScorer::Sub>(subs, (size_t)subCount),
          normsReader, valueReader, simScorer, query.getBoost());
    }

    class Supplier final : public Query::ScorerSupplier {
      BlendedTermWeight& weight;
      IndexReader::Segment& segment;

    public:
      Supplier(BlendedTermWeight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override {
        int64_t total = 0;
        for (const Expansion& expansion : weight.expansions) {
          auto* docsEnum = expansion.termInfo->docsEnums[segment.ord];
          if (docsEnum != nullptr) total += docsEnum->numDocs();
        }
        return total;
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return weight.createScorer(targetPool, segment);
      }
    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool,
                                          IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }
  };

  Query::Weight* createWeight(Context& context, int32_t flags) override {
    if ((flags & NEED_SCORES) == 0) {
      return MultiTermQuery::createWeight(context, flags);
    }
    return context.pool.make<BlendedTermWeight>(context, *this, flags);
  }
};

} // namespace solux
