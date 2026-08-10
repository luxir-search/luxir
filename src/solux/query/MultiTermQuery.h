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
    int32_t docid = -1;

    // First set bit at or after `from`, or END when none remain.
    int32_t advanceTo(int32_t from) {
      if (from >= maxDoc) return docid = PostingsReader::END;
      return docid = bits.nextSetBit(from);  // MAX_INDEX == PostingsReader::END when none
    }

  public:
    Scorer(FixedBitSet bits, int32_t maxDoc, float constantScore)
      : Query::ConstantScorer(constantScore), bits(bits), maxDoc(maxDoc) {}

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
      return !disableDenseFillForTests;
    }

    void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                        int32_t windowEnd) override {
      assert(windowStart >= 0 && windowEnd >= windowStart && windowEnd <= maxDoc);
      if (windowEnd <= windowStart) return;

      int32_t bitCount = windowEnd - windowStart;
      int32_t words = (bitCount + 63) >> 6;
      int32_t sourceWord = windowStart >> 6;
      int32_t shift = windowStart & 63;
      int32_t sourceWords = (int32_t) FixedBitSet::sizeInWords(maxDoc);
      for (int32_t i = 0; i < words; i++) {
        uint64_t sourceBits = bits.words[sourceWord + i] >> shift;
        if (shift != 0 && sourceWord + i + 1 < sourceWords) {
          sourceBits |= bits.words[sourceWord + i + 1] << (64 - shift);
        }
        if (i + 1 == words && (bitCount & 63) != 0) {
          sourceBits &= (1ULL << (bitCount & 63)) - 1ULL;
        }
        windowBits[(size_t) i] |= sourceBits;
      }
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

    struct ExpansionMemo {
      using States = std::vector<TermsEnum::PostingsState>;
      std::variant<States, BitsetPayload> payload;
      int64_t sumDocFreq;
      size_t termCount;
      ExpansionMode autoMode;
      Query::MatchState matchState;

      ExpansionMemo(States&& states,
                    int64_t sumDocFreq, ExpansionMode autoMode)
        : payload(std::in_place_type<States>, std::move(states)),
          sumDocFreq(sumDocFreq),
          termCount(std::get<States>(payload).size()), autoMode(autoMode),
          matchState(termCount == 0 ? Query::MatchState::EMPTY
                                   : Query::MatchState::NONEMPTY) {}

      ExpansionMemo(BitsetPayload bitset, int64_t sumDocFreq,
                    size_t termCount)
        : payload(bitset), sumDocFreq(sumDocFreq), termCount(termCount),
          autoMode(ExpansionMode::EAGER),
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

    struct ExpansionSlot {
      ExpansionMemo* memo = nullptr;
    };

    MultiTermQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    float boost;
    bool canUseLazy;
    google::protobuf::Arena& memoArena;
    std::span<ExpansionSlot> expansionSlots;

  public:
    // Test/bench force switch over the constant-score union scorers. AUTO is
    // the production policy; the forced lazy modes bind only where the lazy
    // preconditions hold and the retained-state budget is not exceeded.
    // Everything else stays eager.
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
                   && scorerModeForTests != ScorerMode::FORCE_EAGER),
        memoArena(context.arena()),
        expansionSlots(
            context.pool.make_span<ExpansionSlot>(context.numSegments())) {
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
        bool canUseLazy, const Query::ScorerBuildContext& buildContext,
        const ExpansionMemo& memo) {
      if (memo.hasBitset()) {
        assert(memo.autoMode == ExpansionMode::EAGER);
        return ExpansionMode::EAGER;
      }
      if (!canUseLazy) return ExpansionMode::EAGER;
      switch (buildContext.multiTermScorerModeForTests) {
        case ScorerMode::AUTO:
          return memo.autoMode;
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

    bool fillExpansionMemo(
        MemPool& scratchPool, IndexReader::Segment& segment,
        const Query::ScorerBuildContext& buildContext) {
      assert(segment.ord >= 0
             && (size_t) segment.ord < expansionSlots.size());
      ExpansionSlot& slot = expansionSlots[(size_t) segment.ord];
      if (slot.memo != nullptr) return false;

      ExpansionMemo::States states;
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
            ExpansionMemo::States().swap(states);
          }
        }
      }

      // The request arena survives every segment-local pool rewind. Each
      // segment task publishes only its preallocated ordinal slot.
      ExpansionMemo* memo;
      if (bitWords != nullptr) {
        memo = solux::arenaCreate<ExpansionMemo>(
            memoArena, BitsetPayload{bitWords}, sumDocFreq, termCount);
      } else {
        ExpansionMode autoMode = chooseAutoMode(
            termCount, buildContext.multiTermMaxLazyStateBytes);
        memo = solux::arenaCreate<ExpansionMemo>(
            memoArena, std::move(states), sumDocFreq, autoMode);
      }
      slot.memo = memo;
      return true;
    }

    const ExpansionMemo& expansionMemo(
        MemPool& scratchPool, IndexReader::Segment& segment,
        const Query::ScorerBuildContext& buildContext) {
      fillExpansionMemo(scratchPool, segment, buildContext);
      return *expansionSlots[(size_t) segment.ord].memo;
    }

    Query::Scorer* createEagerScorer(
        MemPool& targetPool,
        std::span<const TermsEnum::PostingsState> states,
        int32_t maxDoc) {
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(
          nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);
      for (const auto& state : states) {
        addPostingsToBitset(bits, state);
      }
      return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
    }

    Query::Scorer* createScorerForMode(
        MemPool& targetPool, IndexReader::Segment& segment,
        const Query::ScorerBuildContext& buildContext) {
      const ExpansionMemo& memo = expansionMemo(
          targetPool, segment, buildContext);
      unused(memo.sumDocFreq);
      if (memo.matchState == Query::MatchState::EMPTY) return nullptr;
      int32_t maxDoc = segment.postingsReader().maxDoc();
      ExpansionMode mode = chooseMode(canUseLazy, buildContext, memo);
      if (memo.hasBitset()) {
        assert(mode == ExpansionMode::EAGER);
        return targetPool.make<MultiTermQuery::Scorer>(
            FixedBitSet(memo.bitWords(), maxDoc), maxDoc, boost);
      }
      const ExpansionMemo::States& states = memo.states();
      assert(memo.termCount == states.size());
      if (mode == ExpansionMode::HEAP) {
        auto enums = targetPool.make_span<DocsOnlyEnum*>(memo.termCount);
        std::fill(enums.begin(), enums.end(), nullptr);
        auto heap = targetPool.make_span<uint64_t>(memo.termCount);
        auto windowBits = targetPool.make_span<uint64_t>(
            (size_t) UnionHeapScorer::WINDOW_WORDS);
        return targetPool.make<UnionHeapScorer>(
            std::span<const TermsEnum::PostingsState>(states),
            enums, heap, windowBits, targetPool, maxDoc, boost);
      }
      if (mode == ExpansionMode::WINDOWED) {
        static_assert(std::is_trivially_destructible_v<DocsOnlyEnum>);
        auto* docsEnums = (DocsOnlyEnum*) targetPool.alloc(
            memo.termCount * sizeof(DocsOnlyEnum), alignof(DocsOnlyEnum));
        for (size_t i = 0; i < memo.termCount; i++) {
          new (&docsEnums[i]) DocsOnlyEnum(states[i]);
        }
        auto windowBits =
            targetPool.make_span<uint64_t>((size_t) UnionLazyScorer::WINDOW_WORDS);
        return targetPool.make<UnionLazyScorer>(
            std::span(docsEnums, memo.termCount), windowBits, maxDoc, boost);
      }
      return createEagerScorer(targetPool, states, maxDoc);
    }

    class Supplier final : public Query::ScorerSupplier {
      Weight& weight;
      IndexReader::Segment& segment;
      MemPool& scratchPool;

    public:
      Supplier(Weight& weight, IndexReader::Segment& segment,
               MemPool& scratchPool)
        : weight(weight), segment(segment), scratchPool(scratchPool) {}

      int64_t cost() override {
        ExpansionMemo* memo =
            weight.expansionSlots[(size_t) segment.ord].memo;
        if (disableTruthfulCostForTests || memo == nullptr) {
          return segment.maxDoc();
        }
        return std::min<int64_t>(memo->sumDocFreq, segment.maxDoc());
      }

      Query::ScorerShape describeScorer(
          const Query::ScorerBuildContext& buildContext) const override {
        Query::MatchState matchState = Query::MatchState::UNKNOWN;
        if (weight.cachedFieldInfo == nullptr
            || weight.cachedFieldInfo->segInfos[segment.ord] == nullptr) {
          matchState = Query::MatchState::EMPTY;
        } else {
          ExpansionMemo* memo =
              weight.expansionSlots[(size_t) segment.ord].memo;
          if (memo != nullptr) matchState = memo->matchState;
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
        ExpansionMemo* memo =
            weight.expansionSlots[(size_t) segment.ord].memo;
        if (memo != nullptr) {
          ExpansionMode mode = chooseMode(
              weight.canUseLazy, buildContext, *memo);
          if (mode == ExpansionMode::EAGER
              && !disableDenseFillForTests) {
            shape.windowFillClause = Query::ClauseShape::DIRECT;
          }
          return shape;
        }
        bool eagerGuaranteed = !weight.canUseLazy
            || buildContext.multiTermScorerModeForTests
                == ScorerMode::FORCE_EAGER;
        if (eagerGuaranteed && !disableDenseFillForTests) {
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
            // memo records the actual branch.
            unused(buildContext.multiTermMaxLazyStateBytes);
            if (!disableDenseFillForTests) {
              shape.windowFillClause = Query::ClauseShape::UNKNOWN;
            }
            return shape;
        }
        std::unreachable();
      }

      bool fillExpansionMemo(
          const Query::ScorerBuildContext& buildContext) override {
        return weight.fillExpansionMemo(
            scratchPool, segment, buildContext);
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        // Driven consumption keeps the lazy union: windows fill only at
        // probed docids, and the returned next-union doc is a skip fence for
        // the driver's probe loop, so a sparse or pruning lead never pays
        // for the unvisited remainder the eager build materializes up front.
        return weight.createScorerForMode(
            targetPool, segment, Weight::scorerBuildContext(leadCost));
      }
    };

  public:
    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment, targetPool);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return createScorerForMode(
          targetPool, segment,
          scorerBuildContext(std::numeric_limits<int64_t>::max()));
    }

    bool expansionMemoUsesBitsetForTests(
        const IndexReader::Segment& segment) const {
      ExpansionMemo* memo = expansionSlots[(size_t) segment.ord].memo;
      return memo != nullptr && memo->hasBitset();
    }

    size_t expansionMemoRetainedStatesForTests(
        const IndexReader::Segment& segment) const {
      ExpansionMemo* memo = expansionSlots[(size_t) segment.ord].memo;
      assert(memo != nullptr);
      return memo->hasBitset() ? 0 : memo->states().size();
    }
  };
};

} // namespace solux
