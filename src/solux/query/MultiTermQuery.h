#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#include "PostingsUnion.h"
#include "Query.h"
#include "solux/reader/FilteredTermsEnum.h"
#include "solux/util/screaming.h"

namespace solux {

// Base for constant-score queries that union postings from multiple terms in
// one field. Subclasses provide the filtered term iterator.
class MultiTermQuery : public Query {
protected:
  std::string_view field;

public:
  explicit MultiTermQuery(std::string_view field) : field(field) {}

  std::string_view getField() const { return field; }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  // Build the per-segment filtered term iterator.
  virtual FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) = 0;

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<MultiTermQuery::Weight>(context, *this, flags, multiplier);
  }

  // Iterates set bits of the membership bitset under a constant score.
  class Scorer final : public Query::ConstantScorer {
    FixedBitSet bits;
    int32_t maxDoc;
    int32_t docid = -1;

    // First set bit at or after `from`, or END when none remain.
    int32_t advanceTo(int32_t from) {
      if (from >= maxDoc) return docid = PostingsReader::END;
      return docid = bits.nextSetBit(from);  // MAX_INDEX == PostingsReader::END when none
    }

  public:
    Scorer(FixedBitSet bits, int32_t maxDoc, float constantScore)
      : Query::ConstantScorer(constantScore), bits(bits), maxDoc(maxDoc) {}

    int32_t next() override {
      if (docid == PostingsReader::END) return docid = PostingsReader::END;
      return advanceTo(docid + 1);
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);  // strict advance
      return advanceTo(target);
    }
    int32_t docId() override { return docid; }

  protected:
    void exhaust() override { maxDoc = 0; }
  };

  class Weight final : public Query::Weight {
    MultiTermQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    float boost;
    bool canUseLazy;

  public:
    // Test/bench force switch over the constant-score union scorers. AUTO is
    // the production policy; the forced modes bind only where the lazy
    // preconditions hold (scored + pruning, self-driven or conjunction
    // driven), everything else stays eager.
    using ScorerMode = Query::ScorerBuildContext::MultiTermScorerMode;
    static inline ScorerMode scorerModeForTests = ScorerMode::AUTO;

    // Past this many terms AUTO abandons the all-live-cursors windowed scorer
    // for the heap scorer, whose per-term cost is one retained state instead
    // of a full cursor.
    static constexpr size_t MAX_LAZY_TERMS = 4096;
    // Retained-state budget for the heap scorer; expansions past it (term
    // ranges approaching the whole dictionary) fall back to the eager union.
    // Initial value, not yet measured against a real budget tradeoff.
    // Non-const so tests can pin the spill path without a quarter-million
    // term index.
    static inline size_t maxLazyStateBytes = 32u << 20;

    static Query::ScorerBuildContext scorerBuildContext(int64_t leadCost) {
      return {
        .leadCost = leadCost,
        .multiTermScorerModeForTests = scorerModeForTests,
        .multiTermMaxLazyStateBytes = maxLazyStateBytes,
      };
    }

    Weight(Context& context, MultiTermQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags), query(query),
        boost(constantWhenScored(flags, multiplier)),
        canUseLazy((flags & (NEED_SCORES | ALLOW_PRUNING))
                     == (NEED_SCORES | ALLOW_PRUNING)
                   && scorerModeForTests != ScorerMode::FORCE_EAGER) {
      traits |= IS_CONSTANT_SCORING;  // every match scores the same
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
    }

  private:
    // Retained-state budget exceeded: union the collected states plus the
    // rest of the term stream into a full bitset.
    Query::Scorer* eagerRemainder(
        MemPool& targetPool, FilteredTermsEnum& fenum,
        const std::vector<TermsEnum::PostingsState>& states, int32_t maxDoc) {
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(
          nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);
      std::span<uint64_t> bitWords(words, nWords);

      for (const auto& state : states) {
        DocsOnlyEnum docsEnum(state);
        docsEnum.intoBitSet(bitWords, 0, maxDoc);
      }
      while (fenum.next()) {
        DocsOnlyEnum docsEnum(fenum.terms());
        docsEnum.intoBitSet(bitWords, 0, maxDoc);
      }
      return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
    }

    Query::Scorer* createScorerForMode(
        MemPool& targetPool, IndexReader::Segment& segment, bool useLazy) {
      if (cachedFieldInfo == nullptr) return nullptr;       // field absent everywhere
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) return nullptr;          // field absent in this segment

      // Fresh per-segment cursor because scorers can be built in parallel.
      auto* termsEnum = targetPool.make<TermsEnum>(targetPool, segment.postingsReader(), *segFieldInfo);
      FilteredTermsEnum* fenum = query.createFilteredEnum(targetPool, *termsEnum);

      int32_t maxDoc = segment.postingsReader().maxDoc();
      if (useLazy) {
        const ScorerMode mode = scorerModeForTests;
        // AUTO bounds retained states by bytes; the forced modes are test
        // hooks and unbounded so the A/B matrix can probe past the budget.
        const size_t maxStates = mode == ScorerMode::AUTO
            ? maxLazyStateBytes / sizeof(TermsEnum::PostingsState)
            : std::numeric_limits<size_t>::max();
        std::vector<TermsEnum::PostingsState> states;
        while (fenum->next()) {
          states.push_back(fenum->terms().postingsState());
          if (states.size() > maxStates) {
            return eagerRemainder(targetPool, *fenum, states, maxDoc);
          }
        }
        if (states.empty()) return nullptr;

        if (mode == ScorerMode::FORCE_HEAP
            || (mode == ScorerMode::AUTO && states.size() > MAX_LAZY_TERMS)) {
          auto statesArr = targetPool.make_span<TermsEnum::PostingsState>(states.size());
          std::copy(states.begin(), states.end(), statesArr.begin());
          auto enums = targetPool.make_span<DocsOnlyEnum*>(states.size());
          std::fill(enums.begin(), enums.end(), nullptr);
          auto heap = targetPool.make_span<uint64_t>(states.size());
          auto windowBits =
              targetPool.make_span<uint64_t>((size_t) UnionHeapScorer::WINDOW_WORDS);
          return targetPool.make<UnionHeapScorer>(
              std::span<const TermsEnum::PostingsState>(statesArr), enums, heap,
              windowBits, targetPool, maxDoc, boost);
        }

        static_assert(std::is_trivially_destructible_v<DocsOnlyEnum>);
        auto* docsEnums = (DocsOnlyEnum*) targetPool.alloc(
            states.size() * sizeof(DocsOnlyEnum), alignof(DocsOnlyEnum));
        for (size_t i = 0; i < states.size(); i++) {
          new (&docsEnums[i]) DocsOnlyEnum(states[i]);
        }
        auto windowBits =
            targetPool.make_span<uint64_t>((size_t) UnionLazyScorer::WINDOW_WORDS);
        return targetPool.make<UnionLazyScorer>(
            std::span(docsEnums, states.size()), windowBits, maxDoc, boost);
      }

      // Bitset words live in targetPool so the scorer stays trivially destructible.
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);

      bool anyTerm = false;
      // Reclaim each term's postings-enum allocations before scanning the next term.
      auto savepoint = targetPool.getSavePoint();
      while (fenum->next()) {
        anyTerm = true;
        {
          DocsOnlyEnum docsEnum(fenum->terms());
          for (int32_t doc = docsEnum.nextDoc(); doc != PostingsReader::END; doc = docsEnum.nextDoc()) {
            bits.set(doc);
          }
        }
        targetPool.rewind(savepoint);
      }

      if (!anyTerm) return nullptr;  // the field exists but no term matched
      return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
    }

    class Supplier final : public Query::ScorerSupplier {
      Weight& weight;
      IndexReader::Segment& segment;

    public:
      Supplier(Weight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override { return segment.maxDoc(); }

      Query::ScorerShape describeScorer(
          const Query::ScorerBuildContext& buildContext) const override {
        Query::MatchState matchState = Query::MatchState::UNKNOWN;
        if (weight.cachedFieldInfo == nullptr
            || weight.cachedFieldInfo->segInfos[segment.ord] == nullptr) {
          matchState = Query::MatchState::EMPTY;
        }
        Query::ScorerShape shape{
          .matchState = matchState,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = Query::ReportedTwoPhase::NO,
          .windowFillClause = Query::ClauseShape::NONE,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
        if (!weight.canUseLazy) {
          return shape;
        }
        switch (buildContext.multiTermScorerModeForTests) {
          case ScorerMode::FORCE_WINDOWED:
          case ScorerMode::FORCE_HEAP:
          case ScorerMode::FORCE_EAGER:
            return shape;
          case ScorerMode::AUTO:
            // Retained-state overflow may switch AUTO to the eager scorer,
            // but all current alternatives expose the same declared shape.
            unused(buildContext.multiTermMaxLazyStateBytes);
            return shape;
        }
        std::unreachable();
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        // Driven consumption keeps the lazy union: windows fill only at
        // probed docids, and the returned next-union doc is a skip fence for
        // the driver's probe loop, so a sparse or pruning lead never pays
        // for the unvisited remainder the eager build materializes up front.
        unused(leadCost);
        return weight.createScorerForMode(targetPool, segment, weight.canUseLazy);
      }
    };

  public:
    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return createScorerForMode(targetPool, segment, canUseLazy);
    }
  };
};

} // namespace solux
