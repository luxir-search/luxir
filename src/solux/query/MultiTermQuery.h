#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <string_view>
#include <variant>
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
  static inline bool disableDenseFillForTests = false;
  static inline bool disableTruthfulCostForTests = false;

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
    bool denseFillDisabled;
    int32_t docid = -1;

    // First set bit at or after `from`, or END when none remain.
    int32_t advanceTo(int32_t from) {
      if (from >= maxDoc) return docid = PostingsReader::END;
      return docid = bits.nextSetBit(from);  // MAX_INDEX == PostingsReader::END when none
    }

  public:
    Scorer(FixedBitSet bits, int32_t maxDoc, float constantScore)
      : Query::ConstantScorer(constantScore), bits(bits), maxDoc(maxDoc),
        denseFillDisabled(disableDenseFillForTests) {}

    Scorer(FixedBitSet bits, int32_t maxDoc, float constantScore,
           bool denseFillDisabled)
      : Query::ConstantScorer(constantScore), bits(bits), maxDoc(maxDoc),
        denseFillDisabled(denseFillDisabled) {}

    const uint64_t* bitWordsForTests() const { return bits.words; }

    int32_t next() override {
      if (docid == PostingsReader::END) return docid = PostingsReader::END;
      return advanceTo(docid + 1);
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);  // strict advance
      return advanceTo(target);
    }
    int32_t docId() override { return docid; }

    bool supportsWindowFilter() const override {
      return !denseFillDisabled;
    }

    void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                        int32_t windowEnd) override {
      assert(windowStart >= 0 && windowEnd >= windowStart && windowEnd <= maxDoc);
      bits.orRange(windowBits, windowStart, windowEnd);
    }

    DocsFreqEnum* windowFilterProbeDocsEnum() override { return nullptr; }

  protected:
    void exhaust() override { maxDoc = 0; }
  };

  class Weight final : public Query::Weight {
    enum class ExpansionMode : uint8_t {
      EAGER,
      WINDOWED,
      HEAP,
    };

    struct BitsetPayload {
      uint64_t* words;
    };

    // Dictionary-derived facts only. The retained-state budget changes the
    // facts representation (states versus an already-built bitset), so the
    // named segment memo keeps an exact-budget entry rather than letting the
    // first test context win. EAGER/WINDOWED/HEAP is selected per resolve and
    // exists only in ScorerPlan.
    struct ExpansionFacts {
      using States = std::vector<TermsEnum::PostingsState>;
      std::variant<States, BitsetPayload> payload;
      int64_t sumDocFreq;
      size_t termCount;
      size_t maxLazyStateBytes;
      Query::MatchState matchState;
      ExpansionFacts* next = nullptr;

      ExpansionFacts(States&& states, int64_t sumDocFreq,
                     size_t maxLazyStateBytes)
        : payload(std::in_place_type<States>, std::move(states)),
          sumDocFreq(sumDocFreq),
          termCount(std::get<States>(payload).size()),
          maxLazyStateBytes(maxLazyStateBytes),
          matchState(termCount == 0 ? Query::MatchState::EMPTY
                                   : Query::MatchState::NONEMPTY) {}

      ExpansionFacts(BitsetPayload bitset, int64_t sumDocFreq,
                     size_t termCount, size_t maxLazyStateBytes)
        : payload(bitset), sumDocFreq(sumDocFreq), termCount(termCount),
          maxLazyStateBytes(maxLazyStateBytes),
          matchState(termCount == 0 ? Query::MatchState::EMPTY
                                   : Query::MatchState::NONEMPTY) {
        assert(termCount != 0);
        assert(bitset.words != nullptr);
      }

      bool hasBitset() const {
        return std::holds_alternative<BitsetPayload>(payload);
      }

      const States& states() const {
        return std::get<States>(payload);
      }

      uint64_t* bitWords() const {
        return std::get<BitsetPayload>(payload).words;
      }
    };

    struct ExpansionMemo {
      ExpansionFacts* facts = nullptr;
    };

    MultiTermQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    float boost;
    bool canUseLazy;
    google::protobuf::Arena& memoArena;
    std::span<ExpansionMemo> expansionMemos;

  public:
    // Test/bench force switch over the constant-score union scorers. AUTO is
    // the production policy. FORCE_EAGER always selects the eager scorer;
    // forced lazy modes require the lazy preconditions and retained-state
    // budget. Everything else stays eager.
    using ScorerMode = Query::PlanContext::MultiTermScorerMode;
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

    static Query::PlanContext scorerBuildContext(
        const Query::Demand& demand,
        bool phraseDisableSortForTests = false,
        bool phraseDisableRepeatDedupForTests = false,
        bool phraseDisableShapesForTests = false) {
      return {
        .demand = demand,
        .multiTermScorerModeForTests = scorerModeForTests,
        .multiTermMaxLazyStateBytes = maxLazyStateBytes,
        .multiTermDisableDenseFillForTests = disableDenseFillForTests,
        .phraseDisableSortForTests = phraseDisableSortForTests,
        .phraseDisableRepeatDedupForTests =
            phraseDisableRepeatDedupForTests,
        .phraseDisableShapesForTests = phraseDisableShapesForTests,
      };
    }

    static Query::PlanContext scorerBuildContext(
        int64_t leadCost, bool phraseDisableSortForTests = false,
        bool phraseDisableRepeatDedupForTests = false,
        bool phraseDisableShapesForTests = false) {
      return scorerBuildContext(
          Query::Demand::fromLeadCost(leadCost),
          phraseDisableSortForTests,
          phraseDisableRepeatDedupForTests,
          phraseDisableShapesForTests);
    }

    Weight(Context& context, MultiTermQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags), query(query),
        boost(constantWhenScored(flags, multiplier)),
        canUseLazy((flags & (NEED_SCORES | ALLOW_PRUNING))
                     == (NEED_SCORES | ALLOW_PRUNING)
                   && scorerModeForTests != ScorerMode::FORCE_EAGER),
        memoArena(context.arena()),
        expansionMemos(
            context.pool.make_span<ExpansionMemo>(context.numSegments())) {
      traits |= IS_CONSTANT_SCORING;  // every match scores the same
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
    }

  private:
    static ExpansionMode chooseAutoMode(
        size_t termCount, size_t maxStateBytes) {
      size_t maxStates =
          maxStateBytes / sizeof(TermsEnum::PostingsState);
      if (termCount > maxStates) return ExpansionMode::EAGER;
      if (termCount > MAX_LAZY_TERMS) return ExpansionMode::HEAP;
      return ExpansionMode::WINDOWED;
    }

    static ExpansionMode chooseMode(
        bool canUseLazy, const Query::PlanContext& buildContext,
        const ExpansionFacts& facts) {
      if (facts.hasBitset()) {
        return ExpansionMode::EAGER;
      }
      if (!canUseLazy) return ExpansionMode::EAGER;
      switch (buildContext.multiTermScorerModeForTests) {
        case ScorerMode::AUTO:
          return chooseAutoMode(
              facts.termCount,
              buildContext.multiTermMaxLazyStateBytes);
        case ScorerMode::FORCE_EAGER:
          return ExpansionMode::EAGER;
        case ScorerMode::FORCE_WINDOWED:
          return ExpansionMode::WINDOWED;
        case ScorerMode::FORCE_HEAP:
          return ExpansionMode::HEAP;
      }
      std::unreachable();
    }

    static void addPostingsToBitset(
        FixedBitSet& bits, const TermsEnum::PostingsState& state) {
      DocsOnlyEnum docsEnum(state);
      for (int32_t doc = docsEnum.nextDoc();
           doc != PostingsReader::END; doc = docsEnum.nextDoc()) {
        bits.set(doc);
      }
    }

    static ExpansionFacts* findExpansionFacts(
        const ExpansionMemo& memo, size_t maxLazyStateBytes) {
      for (ExpansionFacts* facts = memo.facts;
           facts != nullptr; facts = facts->next) {
        if (facts->maxLazyStateBytes == maxLazyStateBytes) return facts;
      }
      return nullptr;
    }

    bool fillExpansionMemo(
        MemPool& scratchPool, IndexReader::Segment& segment,
        const Query::PlanContext& buildContext) {
      assert(segment.ord >= 0
             && (size_t) segment.ord < expansionMemos.size());
      ExpansionMemo& memo = expansionMemos[(size_t) segment.ord];
      if (findExpansionFacts(
              memo, buildContext.multiTermMaxLazyStateBytes) != nullptr) {
        return false;
      }

      ExpansionFacts::States states;
      size_t maxStates = buildContext.multiTermMaxLazyStateBytes
          / sizeof(TermsEnum::PostingsState);
      uint64_t* bitWords = nullptr;
      size_t termCount = 0;
      int64_t sumDocFreq = 0;
      if (cachedFieldInfo != nullptr) {
        auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
        if (segFieldInfo != nullptr) {
          auto scratchGuard = scratchPool.rewindScopeGuard();
          auto* termsEnum = scratchPool.make<TermsEnum>(
              scratchPool, segment.postingsReader(), *segFieldInfo);
          FilteredTermsEnum* fenum =
              query.createFilteredEnum(scratchPool, *termsEnum);
          skipCount(SkipStats::multitermExpansions);
          while (fenum->next()) {
            TermsEnum::PostingsState state =
                fenum->terms().postingsState();
            termCount++;
            sumDocFreq += state.docFreq;
            if (bitWords != nullptr) {
              FixedBitSet bits(bitWords, segment.maxDoc());
              addPostingsToBitset(bits, state);
              continue;
            }
            if (states.size() < maxStates) {
              if (states.size() == states.capacity()) {
                size_t nextCapacity = states.empty()
                    ? std::min<size_t>(8, maxStates)
                    : states.capacity() > maxStates / 2
                        ? maxStates
                        : states.capacity() * 2;
                states.reserve(nextCapacity);
              }
              states.push_back(state);
              continue;
            }

            size_t nWords = FixedBitSet::sizeInWords(segment.maxDoc());
            bitWords = google::protobuf::Arena::CreateArray<uint64_t>(
                &memoArena, nWords);
            memset(bitWords, 0, nWords * sizeof(uint64_t));
            FixedBitSet bits(bitWords, segment.maxDoc());
            for (const auto& retained : states) {
              addPostingsToBitset(bits, retained);
            }
            addPostingsToBitset(bits, state);
            ExpansionFacts::States().swap(states);
          }
        }
      }

      // The request arena survives every segment-local pool rewind. Each
      // segment publishes into its preallocated ordinal memo.
      ExpansionFacts* facts;
      if (bitWords != nullptr) {
        facts = solux::arenaCreate<ExpansionFacts>(
            memoArena, BitsetPayload{bitWords}, sumDocFreq, termCount,
            buildContext.multiTermMaxLazyStateBytes);
      } else {
        facts = solux::arenaCreate<ExpansionFacts>(
            memoArena, std::move(states), sumDocFreq,
            buildContext.multiTermMaxLazyStateBytes);
      }
      facts->next = memo.facts;
      memo.facts = facts;
      return true;
    }

    const ExpansionFacts& expansionFacts(
        MemPool& scratchPool, IndexReader::Segment& segment,
        const Query::PlanContext& buildContext) {
      fillExpansionMemo(scratchPool, segment, buildContext);
      ExpansionFacts* facts = findExpansionFacts(
          expansionMemos[(size_t) segment.ord],
          buildContext.multiTermMaxLazyStateBytes);
      assert(facts != nullptr);
      return *facts;
    }

    Query::Scorer* createEagerScorer(
        MemPool& targetPool,
        std::span<const TermsEnum::PostingsState> states,
        int32_t maxDoc, bool denseFillDisabled) {
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(
          nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);
      for (const auto& state : states) {
        addPostingsToBitset(bits, state);
      }
      return targetPool.make<MultiTermQuery::Scorer>(
          bits, maxDoc, boost, denseFillDisabled);
    }

    Query::Scorer* createScorerFromPlan(
        MemPool& targetPool, IndexReader::Segment& segment,
        const ExpansionFacts& facts, ExpansionMode mode,
        bool denseFillDisabled) {
      unused(facts.sumDocFreq);
      if (facts.matchState == Query::MatchState::EMPTY) return nullptr;
      int32_t maxDoc = segment.postingsReader().maxDoc();
      if (facts.hasBitset()) {
        assert(mode == ExpansionMode::EAGER);
        return targetPool.make<MultiTermQuery::Scorer>(
            FixedBitSet(facts.bitWords(), maxDoc), maxDoc, boost,
            denseFillDisabled);
      }
      const ExpansionFacts::States& states = facts.states();
      assert(facts.termCount == states.size());
      if (mode == ExpansionMode::HEAP) {
        auto enums = targetPool.make_span<DocsOnlyEnum*>(facts.termCount);
        std::fill(enums.begin(), enums.end(), nullptr);
        auto heap = targetPool.make_span<uint64_t>(facts.termCount);
        auto windowBits = targetPool.make_span<uint64_t>(
            (size_t) UnionHeapScorer::WINDOW_WORDS);
        return targetPool.make<UnionHeapScorer>(
            std::span<const TermsEnum::PostingsState>(states),
            enums, heap, windowBits, targetPool, maxDoc, boost);
      }
      if (mode == ExpansionMode::WINDOWED) {
        static_assert(std::is_trivially_destructible_v<DocsOnlyEnum>);
        auto* docsEnums = (DocsOnlyEnum*) targetPool.alloc(
            facts.termCount * sizeof(DocsOnlyEnum), alignof(DocsOnlyEnum));
        for (size_t i = 0; i < facts.termCount; i++) {
          new (&docsEnums[i]) DocsOnlyEnum(states[i]);
        }
        auto windowBits =
            targetPool.make_span<uint64_t>((size_t) UnionLazyScorer::WINDOW_WORDS);
        return targetPool.make<UnionLazyScorer>(
            std::span(docsEnums, facts.termCount), windowBits, maxDoc, boost);
      }
      return createEagerScorer(
          targetPool, states, maxDoc, denseFillDisabled);
    }

    class Supplier final : public Query::ScorerSupplier {
      Weight& weight;
      IndexReader::Segment& segment;
      MemPool& scratchPool;

      static Query::ScorerShape shapeFor(
          Query::MatchState matchState, ExpansionMode mode,
          const Query::PlanContext& planContext) {
        return {
          .matchState = matchState,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = Query::ReportedTwoPhase::NO,
          .windowFillClause = mode == ExpansionMode::EAGER
                  && !planContext.multiTermDisableDenseFillForTests
              ? Query::ClauseShape::DIRECT
              : Query::ClauseShape::NONE,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
      }

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;
        const ExpansionFacts& facts;
        ExpansionMode mode;
        bool denseFillDisabled;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          return supplier.weight.createScorerFromPlan(
              targetPool, supplier.segment, facts, mode,
              denseFillDisabled);
        }

      public:
        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost,
             const ExpansionFacts& facts, ExpansionMode mode)
          : Query::ScorerPlan(supplier, planContext, shape, cost),
            supplier(supplier), facts(facts), mode(mode),
            denseFillDisabled(
                planContext.multiTermDisableDenseFillForTests) {}
      };

    public:
      Supplier(Weight& weight, IndexReader::Segment& segment,
               MemPool& scratchPool)
        : weight(weight), segment(segment), scratchPool(scratchPool) {}

      int64_t cost() override {
        ExpansionFacts* facts =
            weight.expansionMemos[(size_t) segment.ord].facts;
        if (disableTruthfulCostForTests || facts == nullptr) {
          return segment.maxDoc();
        }
        return std::min<int64_t>(facts->sumDocFreq, segment.maxDoc());
      }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        Query::MatchState matchState = Query::MatchState::UNKNOWN;
        ExpansionFacts* facts = findExpansionFacts(
            weight.expansionMemos[(size_t) segment.ord],
            buildContext.multiTermMaxLazyStateBytes);
        if (weight.cachedFieldInfo == nullptr
            || weight.cachedFieldInfo->segInfos[segment.ord] == nullptr) {
          matchState = Query::MatchState::EMPTY;
        } else if (facts != nullptr) {
          matchState = facts->matchState;
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
        if (facts != nullptr) {
          ExpansionMode mode = chooseMode(
              weight.canUseLazy, buildContext, *facts);
          if (mode == ExpansionMode::EAGER
              && !buildContext.multiTermDisableDenseFillForTests) {
            shape.windowFillClause = Query::ClauseShape::DIRECT;
          }
          return shape;
        }
        bool eagerGuaranteed = !weight.canUseLazy
            || buildContext.multiTermScorerModeForTests
                == ScorerMode::FORCE_EAGER;
        if (eagerGuaranteed
            && !buildContext.multiTermDisableDenseFillForTests) {
          shape.windowFillClause = Query::ClauseShape::DIRECT;
        }
        if (!weight.canUseLazy) return shape;
        switch (buildContext.multiTermScorerModeForTests) {
          case ScorerMode::FORCE_WINDOWED:
          case ScorerMode::FORCE_HEAP:
          case ScorerMode::FORCE_EAGER:
            return shape;
          case ScorerMode::AUTO:
            // Retained-state overflow may switch AUTO to the eager scorer,
            // which fills windows; keep the answer open until the expansion
            // facts record the actual representation.
            unused(buildContext.multiTermMaxLazyStateBytes);
            if (!buildContext.multiTermDisableDenseFillForTests) {
              shape.windowFillClause = Query::ClauseShape::UNKNOWN;
            }
            return shape;
        }
        std::unreachable();
      }

      Query::UnresolvedSupplierCause unresolvedScorerCause(
          const Query::PlanContext& buildContext) const override {
        return describeScorer(buildContext).hasUnknown()
            ? Query::UnresolvedSupplierCause::MULTITERM
            : Query::UnresolvedSupplierCause::NONE;
      }

      bool fillExpansionMemo(
          const Query::PlanContext& buildContext) override {
        return weight.fillExpansionMemo(
            scratchPool, segment, buildContext);
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        const ExpansionFacts& facts = weight.expansionFacts(
            planPool, segment, planContext);
        ExpansionMode mode = chooseMode(
            weight.canUseLazy, planContext, facts);
        return planPool.make<Plan>(
            *this, planContext,
            shapeFor(facts.matchState, mode, planContext), cost(),
            facts, mode);
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        // Driven consumption keeps the lazy union: windows fill only at
        // probed docids, and the returned next-union doc is a skip fence for
        // the driver's probe loop, so a sparse or pruning lead never pays
        // for the unvisited remainder the eager build materializes up front.
        Query::PlanContext planContext =
            Weight::scorerBuildContext(leadCost);
        return resolve(targetPool, planContext)->build(targetPool);
      }
    };

  public:
    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment, targetPool);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      auto* supplier = scorerSupplier(targetPool, segment);
      Query::PlanContext planContext = scorerBuildContext(
          std::numeric_limits<int64_t>::max());
      return supplier->resolve(targetPool, planContext)->build(targetPool);
    }

    bool expansionMemoUsesBitsetForTests(
        const IndexReader::Segment& segment) const {
      ExpansionFacts* facts =
          expansionMemos[(size_t) segment.ord].facts;
      return facts != nullptr && facts->hasBitset();
    }

    size_t expansionMemoRetainedStatesForTests(
        const IndexReader::Segment& segment) const {
      ExpansionFacts* facts =
          expansionMemos[(size_t) segment.ord].facts;
      assert(facts != nullptr);
      return facts->hasBitset() ? 0 : facts->states().size();
    }
  };
};

} // namespace solux
