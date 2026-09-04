#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

#include "ImpactsIndex.h"
#include "Query.h"
#include "luxir/reader/NormsReader.h"
#include "luxir/reader/PosEnum.h"

namespace luxir {

class PhraseQuery final : public Query {
  std::string_view field;
  std::span<std::string_view> terms;
  std::span<const int32_t> positions;
  int32_t slop;

public:
  static inline bool disableShapesForTests = false;

  struct ScorerControls {
    static inline bool countMatchesForTests = false;
    static inline int64_t matchCallsForTests = 0;
    static inline bool disableDocBoundForTests = false;
    static inline bool disableSortForTests = false;
    static inline bool disableRepeatDedupForTests = false;
    static inline bool disableRawBoundsForTests = false;
    static inline bool disableCompetitiveBlocksForTests = false;
    static inline bool disableSloppyGallopForTests = false;
  };

  struct RepeatGroup {
    DocsPosEnum* docsEnum = nullptr;
    PosEnum* posEnum = nullptr;
    int32_t* buf = nullptr;
    int32_t cap = 0;
    int32_t count = 0;
    int32_t filled = 0;
    std::span<const int32_t> slots;
  };

  class ExactMatcher;
  class SloppyMatcher;
  template<class MatcherPolicy> class PhraseScorer;
  using Scorer = PhraseScorer<ExactMatcher>;
  using SloppyScorer = PhraseScorer<SloppyMatcher>;

private:
  struct EstimateCalculation {
    bool nonempty = false;
    std::vector<int32_t> querySlotSources;
    std::vector<int32_t> conjunctionOrder;
    std::vector<int32_t> slotOrder;
    std::vector<int32_t> slotGroup;
    std::vector<std::vector<int32_t>> groupSlots;
    int64_t approximationCost = 0;
    float matchCost = 0.0f;
  };

  struct EstimateGroup {
    std::span<const int32_t> slots;
  };

  // Demand-independent estimator facts. The memo is segment-pool-owned and
  // intentionally contains only POD spans; the demand-specific window-fill
  // arm belongs to the affine ScorerPlan.
  struct EstimateMemo {
    bool nonempty = false;
    std::span<const int32_t> querySlotSources;
    std::span<const int32_t> conjunctionOrder;
    std::span<const int32_t> slotOrder;
    std::span<const int32_t> slotGroup;
    std::span<const EstimateGroup> groupSlots;
    int64_t approximationCost = 0;
    float matchCost = 0.0f;
  };

  static int32_t narrowedTotalTermFreq(
      const TermsEnum::PostingsState& state) {
    // Match DocsEnumMeta::totalTermFreq(), including its current int32_t
    // narrowing of the int64_t postings-state value.
    return (int32_t) state.totalTermFreq;
  }

  static EstimateCalculation estimate(
      std::span<const TermsEnum::PostingsState* const> states,
      std::span<const std::string_view> queryTerms,
      std::span<const int32_t> queryPositions, int32_t slop,
      bool disableSort, bool disableRepeatDedup) {
    assert(states.size() == queryTerms.size());
    assert(states.size() == queryPositions.size());
    EstimateCalculation result;
    for (const auto* state : states) {
      if (state == nullptr) return result;
    }
    result.nonempty = true;

    auto byCost = [&](int32_t a, int32_t b) {
      const auto& sa = *states[(size_t) a];
      const auto& sb = *states[(size_t) b];
      if (sa.docFreq != sb.docFreq) return sa.docFreq < sb.docFreq;
      int32_t ta = narrowedTotalTermFreq(sa);
      int32_t tb = narrowedTotalTermFreq(sb);
      if (ta != tb) return ta < tb;
      return queryTerms[(size_t) a] < queryTerms[(size_t) b];
    };
    result.slotOrder.resize(states.size());
    for (size_t i = 0; i < result.slotOrder.size(); i++) {
      result.slotOrder[i] = (int32_t) i;
    }
    if (!disableSort) {
      std::stable_sort(
          result.slotOrder.begin(), result.slotOrder.end(), byCost);
    }

    result.querySlotSources.resize(states.size());
    bool dedupRepeats = !disableRepeatDedup || slop > 0;
    if (disableSort) {
      // Preserve the test-only text-order plan. Repeat dedup cannot ride the
      // disabled cost sort, so retain the old first-occurrence fallback.
      for (int32_t i = 0; i < (int32_t) states.size(); i++) {
        int32_t source = i;
        if (dedupRepeats) {
          for (int32_t j = 0; j < i; j++) {
            if (queryTerms[(size_t) j] == queryTerms[(size_t) i]) {
              source = j;
              break;
            }
          }
        }
        result.querySlotSources[(size_t) i] = source;
        if (source == i) result.conjunctionOrder.push_back(i);
      }
    } else {
      // Equal terms share stats and are adjacent in slotOrder. Collapse each
      // run while preserving the minimum query ordinal as its enum source.
      for (size_t begin = 0; begin < result.slotOrder.size();) {
        size_t end = begin + 1;
        if (dedupRepeats) {
          std::string_view term = queryTerms[
              (size_t) result.slotOrder[begin]];
          while (end < result.slotOrder.size()
                 && queryTerms[(size_t) result.slotOrder[end]] == term) {
            end++;
          }
        }
        int32_t source = result.slotOrder[begin];
        for (size_t i = begin + 1; i < end; i++) {
          source = std::min(source, result.slotOrder[i]);
        }
        for (size_t i = begin; i < end; i++) {
          result.querySlotSources[
              (size_t) result.slotOrder[i]] = source;
        }
        result.conjunctionOrder.push_back(source);
        begin = end;
      }
    }

    result.slotGroup.assign(states.size(), -1);
    auto addRepeatGroup = [&](std::vector<int32_t> members) {
      if (members.size() < 2) return;
      std::sort(members.begin(), members.end(), [&](int32_t a, int32_t b) {
        int32_t aOrdinal = result.slotOrder[(size_t) a];
        int32_t bOrdinal = result.slotOrder[(size_t) b];
        if (queryPositions[(size_t) aOrdinal]
            != queryPositions[(size_t) bOrdinal]) {
          return queryPositions[(size_t) aOrdinal]
              < queryPositions[(size_t) bOrdinal];
        }
        return aOrdinal < bOrdinal;
      });
      int32_t group = (int32_t) result.groupSlots.size();
      for (int32_t slot : members) {
        result.slotGroup[(size_t) slot] = group;
      }
      result.groupSlots.push_back(std::move(members));
    };
    if (disableSort) {
      // Sources are not adjacent in the text-order test plan.
      for (size_t i = 0; i < result.slotOrder.size(); i++) {
        if (result.slotGroup[i] >= 0) continue;
        int32_t source = result.querySlotSources[
            (size_t) result.slotOrder[i]];
        std::vector<int32_t> members;
        for (size_t j = i; j < result.slotOrder.size(); j++) {
          int32_t candidateSource = result.querySlotSources[
              (size_t) result.slotOrder[j]];
          if (candidateSource == source) members.push_back((int32_t) j);
        }
        addRepeatGroup(std::move(members));
      }
    } else {
      for (size_t begin = 0; begin < result.slotOrder.size();) {
        int32_t source = result.querySlotSources[
            (size_t) result.slotOrder[begin]];
        size_t end = begin + 1;
        while (end < result.slotOrder.size()
               && result.querySlotSources[(size_t) result.slotOrder[end]]
                   == source) {
          end++;
        }
        std::vector<int32_t> members;
        members.reserve(end - begin);
        for (size_t i = begin; i < end; i++) {
          members.push_back((int32_t) i);
        }
        addRepeatGroup(std::move(members));
        begin = end;
      }
    }

    assert(!result.conjunctionOrder.empty());
    const auto* approximationState = states[(size_t)
        result.querySlotSources[(size_t) result.conjunctionOrder[0]]];
    result.approximationCost = approximationState->docFreq;
    if (slop > 0) {
      double averageTf = 0.0;
      for (int32_t ordinal : result.slotOrder) {
        const auto& state = *states[(size_t)
            result.querySlotSources[(size_t) ordinal]];
        averageTf += state.docFreq > 0
            ? (double) narrowedTotalTermFreq(state) / (double) state.docFreq
            : 1.0;
      }
      double slotCount = (double) result.slotOrder.size();
      double estimate = slotCount * averageTf
          + (double) result.groupSlots.size() * slotCount;
      result.matchCost = (float) std::min(
          estimate, (double) std::numeric_limits<float>::max());
    } else {
      for (int32_t ordinal : result.slotOrder) {
        const auto& state = *states[(size_t)
            result.querySlotSources[(size_t) ordinal]];
        if (state.docFreq > 0) {
          result.matchCost += (float) narrowedTotalTermFreq(state)
              / (float) state.docFreq;
        } else {
          result.matchCost += 1.0f;
        }
      }
    }
    return result;
  }

  static EstimateMemo* memoizeEstimate(
      MemPool& pool, EstimateCalculation& estimate) {
    auto* memo = pool.make<EstimateMemo>();
    memo->nonempty = estimate.nonempty;
    memo->querySlotSources = pool.copy_span(
        std::span<int32_t>(estimate.querySlotSources));
    memo->conjunctionOrder = pool.copy_span(
        std::span<int32_t>(estimate.conjunctionOrder));
    memo->slotOrder = pool.copy_span(
        std::span<int32_t>(estimate.slotOrder));
    memo->slotGroup = pool.copy_span(
        std::span<int32_t>(estimate.slotGroup));
    auto groups = pool.make_span<EstimateGroup>(estimate.groupSlots.size());
    for (size_t i = 0; i < groups.size(); i++) {
      groups[i].slots = pool.copy_span(
          std::span<int32_t>(estimate.groupSlots[i]));
    }
    memo->groupSlots = groups;
    memo->approximationCost = estimate.approximationCost;
    memo->matchCost = estimate.matchCost;
    return memo;
  }

  // Position-verification price for a windowed exclusion fill, relative to
  // the positive side's cost. A windowed fill verifies positions for every
  // approximation hit in the window; the pull path verifies only the docs
  // that survive the positive leapfrog, so admitting too eagerly loses.
  // MEASURED on the 70-query neg_phrase population (5M corpus): weight 4.0
  // admits 8 and gives class 0.90; 1.0 admits 22 and gives 0.77 with one
  // query at 1.12; 0.25 admits 41 and gives 0.68 but grows a loss tail of
  // seven queries up to 1.53. 1.0 takes most of the win without the tail.
  static constexpr double kExclusionWindowFillPositionWeight = 1.0;

  static bool estimateSupportsWindowFill(
      int64_t approximationCost, float matchCost, int64_t positiveCost) {
    double fillWork = (double) approximationCost
        * (double) std::max(matchCost, 1.0f)
        * kExclusionWindowFillPositionWeight;
    return fillWork <= (double) positiveCost;
  }

public:

  PhraseQuery(std::string_view field, std::span<std::string_view> terms,
              std::span<const int32_t> positions, int32_t slop = 0)
      : Query(QueryKind::PHRASE), field(field), terms(terms),
        positions(positions), slop(slop) {
    assert(terms.size() == positions.size());
    assert(terms.size() >= 2);
    assert(slop >= 0);
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const PhraseQuery&>(other);
    return field == rhs.field && slop == rhs.slop
        && terms.size() == rhs.terms.size()
        && positions.size() == rhs.positions.size()
        && std::equal(terms.begin(), terms.end(), rhs.terms.begin())
        && std::equal(positions.begin(), positions.end(),
                      rhs.positions.begin());
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, slop);
    value = mixSampledSequence(
        value, terms,
        [](uint64_t seed, std::string_view term) {
          return mixHash(seed, term);
        });
    return mixSampledSequence(
        value, positions,
        [](uint64_t seed, int32_t position) {
          return mixHash(seed, position);
        });
  }

  [[nodiscard]] std::string_view getField() const { return field; }
  [[nodiscard]] std::span<std::string_view> getTerms() const { return terms; }
  [[nodiscard]] std::span<const int32_t> getPositions() const { return positions; }
  [[nodiscard]] int32_t getSlop() const { return slop; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendSize(terms.size());
    for (std::string_view term : terms) out.appendTerm(term);
    out.appendSize(positions.size());
    for (int32_t position : positions) out.appendInt32(position);
    out.appendInt32(slop);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<PhraseQuery::Weight>(context, *this, flags, multiplier);
  }

  class Weight final : public Query::Weight {
    PhraseQuery& query;
    CachedFieldInfo* cachedFieldInfo;
    std::span<CachedTermInfo*> cachedTermInfos;
    Similarity::BM25Scorer* simScorer = nullptr;

    EstimateCalculation estimateScorer(
        IndexReader::Segment& segment, bool disableSort,
        bool disableRepeatDedup) const {
      if (cachedFieldInfo == nullptr
          || cachedFieldInfo->segInfos[segment.ord] == nullptr) {
        return {};
      }
      std::vector<const TermsEnum::PostingsState*> states;
      states.reserve(cachedTermInfos.size());
      for (const auto* termInfo : cachedTermInfos) {
        states.push_back(termInfo->postingsStates[segment.ord]);
      }
      return PhraseQuery::estimate(
          states, query.getTerms(), query.getPositions(), query.getSlop(),
          disableSort, disableRepeatDedup);
    }

#ifndef NDEBUG
    void assertConstructionMatchesEstimate(
        IndexReader::Segment& segment, const EstimateMemo& estimate,
        std::span<DocsPosEnum*> querySlotEnums,
        std::span<DocsPosEnum*> slotEnums,
        std::span<const int32_t> positions,
        std::span<const int32_t> ordinals,
        std::span<DocsPosEnum*> conjunctionEnums,
        std::span<const int32_t> slotGroup,
        std::span<RepeatGroup> groups) const {
      assert(estimate.nonempty);
      assert(querySlotEnums.size() == estimate.querySlotSources.size());
      for (size_t i = 0; i < querySlotEnums.size(); i++) {
        int32_t source = estimate.querySlotSources[i];
        assert(querySlotEnums[i] == querySlotEnums[(size_t) source]);
      }
      assert(conjunctionEnums.size() == estimate.conjunctionOrder.size());
      for (size_t i = 0; i < conjunctionEnums.size(); i++) {
        int32_t ordinal = estimate.conjunctionOrder[i];
        assert(conjunctionEnums[i] == querySlotEnums[(size_t) ordinal]);
      }
      assert(conjunctionEnums[0]->numDocs() == estimate.approximationCost);
      assert(slotEnums.size() == estimate.slotOrder.size());
      for (size_t i = 0; i < slotEnums.size(); i++) {
        int32_t ordinal = estimate.slotOrder[i];
        int32_t source = estimate.querySlotSources[(size_t) ordinal];
        const auto* state = cachedTermInfos[(size_t) source]
            ->postingsStates[segment.ord];
        assert(state != nullptr);
        assert(slotEnums[i] == querySlotEnums[(size_t) ordinal]);
        assert(slotEnums[i]->numDocs() == state->docFreq);
        assert(slotEnums[i]->totalTermFreq()
               == narrowedTotalTermFreq(*state));
        assert(positions[i] == query.getPositions()[(size_t) ordinal]);
        assert(ordinals[i] == ordinal);
      }
      assert(std::equal(slotGroup.begin(), slotGroup.end(),
                        estimate.slotGroup.begin(),
                        estimate.slotGroup.end()));
      assert(groups.size() == estimate.groupSlots.size());
      for (size_t i = 0; i < groups.size(); i++) {
        std::span<const int32_t> expected = estimate.groupSlots[i].slots;
        assert(groups[i].docsEnum == slotEnums[(size_t) expected[0]]);
        assert(groups[i].slots.size() == expected.size());
        assert(std::equal(groups[i].slots.begin(), groups[i].slots.end(),
                          expected.begin(), expected.end()));
      }
    }
#endif

  public:
    Weight(Query::Context& context, PhraseQuery& query, int32_t flags, float multiplier)
        : Query::Weight(context, query, flags), query(query) {
      bool needScores = (flags & NEED_SCORES) != 0;
      if (!needScores) traits |= IS_CONSTANT_SCORING;
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
      if (cachedFieldInfo == nullptr) return;

      cachedTermInfos = context.pool.make_span<CachedTermInfo*>(query.getTerms().size());
      Similarity similarity;
      double idf = 0.0;
      for (int i = 0; i < cachedTermInfos.size(); i++) {
        cachedTermInfos[i] = context.getCachedTerminfo(*cachedFieldInfo, query.getTerms()[i]);
        if (cachedTermInfos[i] == nullptr) {
          cachedFieldInfo = nullptr;
          return;
        }
        idf += similarity.idf(cachedFieldInfo->fieldStats, cachedTermInfos[i]->termStats);
      }
      if (needScores) {
        simScorer = context.pool.make<Similarity::BM25Scorer>(
            similarity.getScorer(multiplier, cachedFieldInfo->fieldStats, (float) idf));
      }
    }

    std::vector<int32_t> querySlotSourcesForTests(
        IndexReader::Segment& segment, bool disableSort = false,
        bool disableRepeatDedup = false) const {
      return estimateScorer(
          segment, disableSort, disableRepeatDedup).querySlotSources;
    }

    std::vector<int32_t> conjunctionOrderForTests(
        IndexReader::Segment& segment, bool disableSort = false,
        bool disableRepeatDedup = false) const {
      return estimateScorer(
          segment, disableSort, disableRepeatDedup).conjunctionOrder;
    }

    Query::Scorer* buildScorer(
        MemPool& targetPool, IndexReader::Segment& segment,
        const EstimateMemo& estimate) {
      if (!estimate.nonempty) return nullptr;
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      assert(segFieldInfo != nullptr);

      auto querySlotEnums = targetPool.make_span<DocsPosEnum*>(cachedTermInfos.size());
      auto querySlotPosEnums = targetPool.make_span<PosEnum*>(cachedTermInfos.size());
      for (int32_t i = 0; i < (int32_t) cachedTermInfos.size(); i++) {
        int32_t source = estimate.querySlotSources[(size_t) i];
        if (source != i) {
          querySlotEnums[(size_t) i] = querySlotEnums[(size_t) source];
          querySlotPosEnums[(size_t) i] =
              querySlotPosEnums[(size_t) source];
          continue;
        }
        DocsPosEnum* docsEnum = cachedTermInfos[(size_t) i]
            ->useDocsEnum<DocsEnumTier::POSITIONS>(targetPool, segment);
        assert(docsEnum != nullptr);
        if (docsEnum == nullptr) return nullptr;
        PosEnum* posEnum = targetPool.make<PosEnum>(*docsEnum);
        querySlotEnums[(size_t) i] = docsEnum;
        querySlotPosEnums[(size_t) i] = posEnum;
      }

      auto conjunctionEnums = targetPool.make_span<DocsPosEnum*>(
          estimate.conjunctionOrder.size());
      auto conjunctionPosEnums = targetPool.make_span<PosEnum*>(
          estimate.conjunctionOrder.size());
      for (size_t i = 0; i < estimate.conjunctionOrder.size(); i++) {
        int32_t ordinal = estimate.conjunctionOrder[i];
        conjunctionEnums[i] = querySlotEnums[(size_t) ordinal];
        conjunctionPosEnums[i] = querySlotPosEnums[(size_t) ordinal];
      }

      auto slotEnums = targetPool.make_span<DocsPosEnum*>(
          estimate.slotOrder.size());
      auto slotPosEnums = targetPool.make_span<PosEnum*>(
          estimate.slotOrder.size());
      auto positions = targetPool.make_span<int32_t>(
          estimate.slotOrder.size());
      auto ordinals = targetPool.make_span<int32_t>(
          estimate.slotOrder.size());
      for (size_t k = 0; k < estimate.slotOrder.size(); k++) {
        int32_t ord = estimate.slotOrder[k];
        slotEnums[k] = querySlotEnums[(size_t) ord];
        slotPosEnums[k] = querySlotPosEnums[(size_t) ord];
        positions[k] = query.getPositions()[(size_t) ord];
        ordinals[k] = ord;
      }

      std::span<const int32_t> slotGroup = estimate.slotGroup;
      auto groups = targetPool.make_span<RepeatGroup>(
          estimate.groupSlots.size());
      for (size_t g = 0; g < estimate.groupSlots.size(); g++) {
        std::span<const int32_t> groupSlots =
            estimate.groupSlots[g].slots;
        groups[g].docsEnum = slotEnums[(size_t) groupSlots[0]];
        groups[g].posEnum = slotPosEnums[(size_t) groupSlots[0]];
        groups[g].slots = groupSlots;
      }

#ifndef NDEBUG
      assertConstructionMatchesEstimate(
          segment, estimate, querySlotEnums, slotEnums, positions, ordinals,
          conjunctionEnums, slotGroup, groups);
#endif

      NormsReader* normsReader = (inputFlags & NEED_SCORES) != 0
          ? targetPool.make<NormsReader>(segment.postingsReader(), *segFieldInfo)
          : nullptr;

      std::span<ImpactsIndex> impacts;
      std::span<int32_t> impactMultiplicities;
      if (simScorer != nullptr && normsReader != nullptr) {
        auto built = targetPool.make_span<ImpactsIndex>(conjunctionEnums.size());
        auto multiplicities = targetPool.make_span<int32_t>(conjunctionEnums.size());
        bool allBuilt = true;
        for (size_t i = 0; i < conjunctionEnums.size(); i++) {
          DocsPosEnum* docsEnum = conjunctionEnums[i];
          int32_t multiplicity = 0;
          for (DocsPosEnum* slotEnum : slotEnums) {
            if (slotEnum == docsEnum) multiplicity++;
          }
          multiplicities[i] = multiplicity;
          built[i].build(targetPool, *docsEnum, *simScorer, 1.0f, true);
          allBuilt &= !built[i].empty();
        }
        impactMultiplicities = multiplicities;
        if (allBuilt) impacts = built;
      }

      if (query.getSlop() > 0) {
        return targetPool.make<SloppyScorer>(
            targetPool, slotEnums, slotPosEnums, positions, ordinals,
            conjunctionEnums, conjunctionPosEnums, normsReader,
            simScorer, impacts, impactMultiplicities, slotGroup, groups,
            estimate.approximationCost, estimate.matchCost, query.getSlop(),
            (inputFlags & EXCLUSION_WINDOW_FILL) != 0);
      }
      return targetPool.make<Scorer>(
          targetPool, slotEnums, slotPosEnums, positions, ordinals,
          conjunctionEnums, conjunctionPosEnums, normsReader,
          simScorer, impacts, impactMultiplicities, slotGroup, groups,
          estimate.approximationCost, estimate.matchCost, 0,
          (inputFlags & EXCLUSION_WINDOW_FILL) != 0);
    }

    class Supplier final : public Query::ScorerSupplier {
      enum class ScorerArm : uint8_t {
        EMPTY,
        PULL,
        WINDOW_FILL,
      };

      PhraseQuery::Weight& weight;
      IndexReader::Segment& segment;
      std::array<EstimateMemo*, 4> estimateMemos{};

      template<class EstimateType>
      ScorerArm selectArm(
          const EstimateType& estimate,
          const Query::PlanContext& planContext) const {
        if (!estimate.nonempty) return ScorerArm::EMPTY;
        if ((weight.inputFlags & EXCLUSION_WINDOW_FILL) != 0
            && estimateSupportsWindowFill(
                estimate.approximationCost, estimate.matchCost,
                planContext.demand.candidates)) {
          return ScorerArm::WINDOW_FILL;
        }
        return ScorerArm::PULL;
      }

      static Query::ScorerShape shapeFor(ScorerArm arm) {
        return {
          .matchState = arm == ScorerArm::EMPTY
              ? Query::MatchState::EMPTY
              : Query::MatchState::NONEMPTY,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = Query::ReportedTwoPhase::YES,
          .windowFillClause = arm == ScorerArm::WINDOW_FILL
              ? Query::ClauseShape::DIRECT
              : Query::ClauseShape::NONE,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .termConjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
      }

      const EstimateMemo& estimateMemo(
          MemPool& planPool, const Query::PlanContext& planContext) {
        size_t key = (planContext.phraseDisableSortForTests ? 2u : 0u)
            | (planContext.phraseDisableRepeatDedupForTests ? 1u : 0u);
        EstimateMemo*& memo = estimateMemos[key];
        if (memo == nullptr) {
          EstimateCalculation estimate = weight.estimateScorer(
              segment, planContext.phraseDisableSortForTests,
              planContext.phraseDisableRepeatDedupForTests);
          memo = memoizeEstimate(planPool, estimate);
        }
        return *memo;
      }

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;
        const EstimateMemo& estimate;
        ScorerArm arm;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          if (arm == ScorerArm::EMPTY) return nullptr;
          Query::Scorer* scorer = supplier.weight.buildScorer(
              targetPool, supplier.segment, estimate);
          assert(scorer != nullptr);
          return scorer;
        }

      public:
        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost,
             const EstimateMemo& estimate, ScorerArm arm)
          : Query::ScorerPlan(
                planContext, shape, cost),
            supplier(supplier), estimate(estimate), arm(arm) {}
      };

    public:
      Supplier(PhraseQuery::Weight& weight, IndexReader::Segment& segment)
          : weight(weight), segment(segment) {}

      int64_t cost() override {
        if (weight.cachedFieldInfo == nullptr) return 0;
        int64_t minCost = -1;
        for (auto* termInfo : weight.cachedTermInfos) {
          int64_t c = termInfo->docFreq(segment.ord);
          if (c == 0) return 0;
          if (minCost < 0 || c < minCost) minCost = c;
        }
        return minCost < 0 ? 0 : minCost;
      }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        if (buildContext.phraseDisableShapesForTests) return {};
        EstimateCalculation estimate = weight.estimateScorer(
            segment, buildContext.phraseDisableSortForTests,
            buildContext.phraseDisableRepeatDedupForTests);
        return shapeFor(selectArm(estimate, buildContext));
      }

      Query::UnresolvedSupplierCause unresolvedScorerCause(
          const Query::PlanContext& buildContext) const override {
        return buildContext.phraseDisableShapesForTests
            ? Query::UnresolvedSupplierCause::PHRASE
            : Query::UnresolvedSupplierCause::NONE;
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        const EstimateMemo& estimate = estimateMemo(planPool, planContext);
        ScorerArm arm = selectArm(estimate, planContext);
        return planPool.make<Plan>(
            *this, planContext, shapeFor(arm), cost(),
            estimate, arm);
      }

      Query::PlanContext makePlanContext(
          const Query::Demand& demand) const override {
        Query::PlanContext context = scorerBuildContext(demand.candidates);
        context.demand = demand;
        return context;
      }

    };

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      return targetPool.make<Supplier>(*this, segment);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      auto* supplier = scorerSupplier(targetPool, segment);
      Query::PlanContext planContext = scorerBuildContext(
          std::numeric_limits<int64_t>::max());
      return supplier->resolve(targetPool, planContext)->build(targetPool);
    }

    static Query::PlanContext scorerBuildContext(int64_t leadCost) {
      Query::PlanContext planContext =
          Query::PlanContext::fromLeadCost(leadCost);
      planContext.phraseDisableSortForTests =
          ScorerControls::disableSortForTests;
      planContext.phraseDisableRepeatDedupForTests =
          ScorerControls::disableRepeatDedupForTests;
      planContext.phraseDisableShapesForTests =
          PhraseQuery::disableShapesForTests;
      return planContext;
    }
  };

  class ExactMatcher {
    int32_t pos = -1;
    int32_t freq = 0;
    bool freqComplete = true;
    int32_t largestPossiblePos = 0;

    template<class S>
    int32_t doNextPosition(S& scorer, int32_t target) {
      auto posEnums = scorer.slotPosEnums;
      auto positions = scorer.positions;
      outer:
      for (;;) {
        if (target > largestPossiblePos) {
          pos = PostingsReader::END;
          return PostingsReader::END;
        }

        for (int j = 1; j < posEnums.size(); j++) {
          int32_t adjustedTarget = target + positions[j];
          int32_t p = posEnums[j]->advancePosition((int32_t) adjustedTarget);
          assert(p >= adjustedTarget);
          if (p > adjustedTarget) {
            target = p - positions[j];
            if (target > largestPossiblePos) {
              pos = PostingsReader::END;
              return PostingsReader::END;
            }
            adjustedTarget = target + positions[0];
            p = posEnums[0]->advancePosition(adjustedTarget);
            target = p - positions[0];
            goto outer;
          }
        }
        pos = target;
        freq++;
        return pos;
      }
    }

    template<class S>
    int32_t doNextPositionRepeats(S& scorer, int32_t target) {
      auto posEnums = scorer.slotPosEnums;
      auto positions = scorer.positions;
      outer:
      for (;;) {
        if (target > largestPossiblePos) {
          pos = PostingsReader::END;
          return PostingsReader::END;
        }

        for (int j = 1; j < posEnums.size(); j++) {
          int32_t adjustedTarget = target + positions[j];
          int32_t p = scorer.repeatSlotAdvance(j, adjustedTarget);
          assert(p >= adjustedTarget);
          if (p > adjustedTarget) {
            target = p - positions[j];
            if (target > largestPossiblePos) {
              pos = PostingsReader::END;
              return PostingsReader::END;
            }
            adjustedTarget = target + positions[0];
            p = scorer.repeatSlotAdvance(0, adjustedTarget);
            target = p - positions[0];
            goto outer;
          }
        }
        pos = target;
        freq++;
        return pos;
      }
    }

  public:
    static constexpr bool IS_SLOPPY = false;

    explicit ExactMatcher(int32_t slop) { unused(slop); }

    template<class S>
    void init(S& scorer) {
      int32_t maxOff = 0;
      for (auto position : scorer.positions) maxOff = std::max(maxOff, position);
      largestPossiblePos = PostingsReader::END - 1 - maxOff;
    }

    template<class S>
    bool matches(S& scorer) {
      freq = 0;
      if (scorer.docid == PostingsReader::END) {
        pos = PostingsReader::END;
        return false;
      }
      if (scorer.minCompetitiveScore > 0.0f && scorer.simScorer != nullptr
          && !ScorerControls::disableDocBoundForTests) {
        int32_t maxFreq = scorer.slotEnums[0]->termFreq();
        for (size_t j = 1; j < scorer.slotEnums.size() && maxFreq > 1; j++) {
          maxFreq = std::min(maxFreq, scorer.slotEnums[j]->termFreq());
        }
        if (scorer.simScorer->score((float) maxFreq, scorer.lookupNorm(scorer.docid))
            < scorer.minCompetitiveScore) {
          skipCount(SkipStats::phraseBoundRejects);
          pos = PostingsReader::END;
          return false;
        }
      }
      skipCount(SkipStats::phraseVerifies);
      for (auto* posEnum : scorer.conjunctionPosEnums) posEnum->startPositions();
      bool matched;
      if (scorer.groups.empty()) {
        matched = doNextPosition(
            scorer, scorer.slotPosEnums[0]->advancePosition(scorer.positions[0])
                        - scorer.positions[0]) != PostingsReader::END;
      } else {
        scorer.resetRepeatGroups();
        matched = doNextPositionRepeats(
            scorer, scorer.repeatSlotAdvance(0, scorer.positions[0])
                        - scorer.positions[0]) != PostingsReader::END;
      }
      freqComplete = !matched;
      return matched;
    }

    template<class S>
    int32_t numMatches(S& scorer) {
      while (pos != PostingsReader::END) {
        if (scorer.groups.empty()) {
          doNextPosition(scorer, scorer.slotPosEnums[0]->nextPosition() - scorer.positions[0]);
        } else {
          doNextPositionRepeats(
              scorer, scorer.repeatSlotNext(0) - scorer.positions[0]);
        }
      }
      freqComplete = true;
      return freq;
    }

    template<class S>
    float scoreFreq(S& scorer) {
      if (!freqComplete) numMatches(scorer);
      return (float) freq;
    }

    template<class S>
    float getMaxScore(S& scorer, int32_t upTo) {
      if (scorer.impacts.empty()) return std::numeric_limits<float>::infinity();
      int32_t startDoc = scorer.shallowTarget >= 0 ? scorer.shallowTarget : scorer.docid;
      float maxScore = std::numeric_limits<float>::infinity();
      for (const auto& termImpacts : scorer.impacts) {
        int32_t startBlock = termImpacts.blockContaining(startDoc);
        if (startBlock >= termImpacts.blockCount()) continue;
        int32_t upBlock = termImpacts.blockContaining(upTo);
        if (upBlock >= termImpacts.blockCount()) upBlock = termImpacts.blockCount() - 1;
        if (upBlock < startBlock) continue;
        float termMax = upBlock == termImpacts.blockCount() - 1
            ? termImpacts.maxImpactFrom(startBlock)
            : termImpacts.maxImpactInRange(startBlock, upBlock);
        maxScore = std::min(maxScore, termMax);
      }
      return maxScore;
    }
  };

  class SloppyMatcher {
    int32_t slop;
    std::span<int32_t> actual;
    std::span<int64_t> rebased;
    int64_t endPosition = std::numeric_limits<int64_t>::min();
    int64_t matchLength = std::numeric_limits<int64_t>::max();
    float freq = 0.0f;
    bool positioned = false;
    bool freqComplete = true;

    template<class S>
    bool less(const S& scorer, int32_t a, int32_t b) const {
      if (rebased[(size_t) a] != rebased[(size_t) b]) {
        return rebased[(size_t) a] < rebased[(size_t) b];
      }
      if (scorer.positions[(size_t) a] != scorer.positions[(size_t) b]) {
        return scorer.positions[(size_t) a] < scorer.positions[(size_t) b];
      }
      return scorer.ordinals[(size_t) a] < scorer.ordinals[(size_t) b];
    }

    template<class S>
    std::pair<int32_t, int64_t> minAndCapturedSecond(const S& scorer) const {
      int32_t minSlot = 0;
      for (int32_t i = 1; i < (int32_t) actual.size(); i++) {
        if (less(scorer, i, minSlot)) minSlot = i;
      }
      int32_t second = minSlot == 0 ? 1 : 0;
      for (int32_t i = 0; i < (int32_t) actual.size(); i++) {
        if (i != minSlot && less(scorer, i, second)) second = i;
      }
      return {minSlot, rebased[(size_t) second]};
    }

    template<class S>
    void setPosition(const S& scorer, int32_t slot, int32_t position) {
      actual[(size_t) slot] = position;
      int64_t value = (int64_t) position - (int64_t) scorer.positions[(size_t) slot];
      rebased[(size_t) slot] = value;
      endPosition = std::max(endPosition, value);
    }

    template<class S>
    bool advanceSlot(S& scorer, int32_t slot) {
      int32_t position = scorer.slotGroup[(size_t) slot] < 0
          ? scorer.slotPosEnums[(size_t) slot]->nextPosition()
          : scorer.repeatSlotNext(slot);
      if (position == PostingsReader::END) return false;
      setPosition(scorer, slot, position);
      return true;
    }

    // advanceSlot for nextMatch's walk: hop the active slot over positions
    // that can neither cross capturedSecond nor land within slop of
    // endPosition. Candidate lengths shrink monotonically as the active slot
    // approaches endPosition, so skipped positions all carry lengths above
    // slop - the emission test ignores them and they can never set an
    // emitted matchLength. The capturedSecond+1 cap keeps every crossing
    // position visited. Repeat-group slots pull from a shared buffer and
    // keep the stepwise walk.
    template<class S>
    bool advanceSlotGallop(S& scorer, int32_t slot, int64_t capturedSecond) {
      if (scorer.slotGroup[(size_t) slot] >= 0
          || ScorerControls::disableSloppyGallopForTests) {
        return advanceSlot(scorer, slot);
      }
      int64_t targetRebased = std::min(
          endPosition - (int64_t) slop, capturedSecond + 1);
      int64_t targetActual =
          targetRebased + (int64_t) scorer.positions[(size_t) slot];
      if (targetActual <= (int64_t) actual[(size_t) slot] + 1) {
        return advanceSlot(scorer, slot);
      }
      int32_t position = scorer.slotPosEnums[(size_t) slot]->advancePosition(
          (int32_t) std::min(targetActual,
                             (int64_t) PostingsReader::END - 1));
      if (position == PostingsReader::END) return false;
      setPosition(scorer, slot, position);
      return true;
    }

    template<class S>
    int32_t collidingSlot(const S& scorer, int32_t slot) const {
      int32_t group = scorer.slotGroup[(size_t) slot];
      if (group < 0) return -1;
      for (int32_t other : scorer.groups[(size_t) group].slots) {
        if (other != slot && actual[(size_t) other] == actual[(size_t) slot]) return other;
      }
      return -1;
    }

    template<class S>
    int32_t lesserCollision(const S& scorer, int32_t a, int32_t b) const {
      if (rebased[(size_t) a] != rebased[(size_t) b]) {
        return rebased[(size_t) a] < rebased[(size_t) b] ? a : b;
      }
      if (scorer.positions[(size_t) a] != scorer.positions[(size_t) b]) {
        return scorer.positions[(size_t) a] < scorer.positions[(size_t) b] ? a : b;
      }
      return scorer.ordinals[(size_t) a] < scorer.ordinals[(size_t) b] ? a : b;
    }

    template<class S>
    bool resolveCollisions(S& scorer, int32_t& active) {
      for (;;) {
        int32_t collision = collidingSlot(scorer, active);
        if (collision < 0) return true;
        active = lesserCollision(scorer, active, collision);
        if (!advanceSlot(scorer, active)) return false;
      }
    }

    template<class S>
    bool initPositions(S& scorer) {
      for (const RepeatGroup& group : scorer.groups) {
        if (group.docsEnum->termFreq() < (int32_t) group.slots.size()) return false;
      }
      for (auto* posEnum : scorer.conjunctionPosEnums) posEnum->startPositions();
      scorer.resetRepeatGroups();
      endPosition = std::numeric_limits<int64_t>::min();

      for (int32_t slot = 0; slot < (int32_t) scorer.slotEnums.size(); slot++) {
        if (scorer.slotGroup[(size_t) slot] >= 0) continue;
        int32_t position = scorer.slotPosEnums[(size_t) slot]->nextPosition();
        if (position == PostingsReader::END) return false;
        setPosition(scorer, slot, position);
      }
      for (const RepeatGroup& group : scorer.groups) {
        for (int32_t slot : group.slots) {
          int32_t position = scorer.repeatSlotFirst(slot);
          if (position == PostingsReader::END) return false;
          setPosition(scorer, slot, position);
        }
        for (int32_t j = 1; j < (int32_t) group.slots.size(); j++) {
          int32_t slot = group.slots[(size_t) j];
          for (int32_t k = 0; k < j; k++) {
            if (!advanceSlot(scorer, slot)) return false;
          }
        }
      }
      positioned = true;
      return true;
    }

    // The captured second-min is the cycle's fixed threshold (Lucene's
    // `next`): refreshing it after collision advances coalesces adjacent
    // matches ("a a" over positions 0,1,2 must yield freq 2, not 1).
    //
    // Control-flow note vs Lucene: Lucene keeps driving the originally
    // popped slot after collision resolution (advanceRpts rebinds only
    // locally); this walk continues with the last slot the resolution
    // advanced. Equivalent, given the strictly-increasing per-term
    // position invariant: if resolution ends on a different slot than was
    // popped, that slot pre-advance sat at or past the captured second-min
    // (non-popped slots start at their scan-time positions), so its
    // advance lands strictly past it; and the popped slot stopped being
    // the collision-lesser only by landing past a member itself at or
    // past the captured second-min. Both therefore cross the threshold
    // and take the same done-minimizing branch below.
    template<class S>
    bool nextMatch(S& scorer) {
      if (!positioned) return false;
      auto [activeStart, capturedSecond] = minAndCapturedSecond(scorer);
      int32_t active = activeStart;
      matchLength = endPosition - rebased[(size_t) active];
      for (;;) {
        // Skipping is only possible while the walk is outside the slop zone,
        // so the in-zone steady state pays one comparison, not a gallop probe.
        bool advanced = matchLength > (int64_t) slop
            ? advanceSlotGallop(scorer, active, capturedSecond)
            : advanceSlot(scorer, active);
        if (!advanced) break;
        if (!resolveCollisions(scorer, active)) break;
        if (rebased[(size_t) active] > capturedSecond) {
          if (matchLength <= (int64_t) slop) return true;
          auto next = minAndCapturedSecond(scorer);
          active = next.first;
          capturedSecond = next.second;
          matchLength = endPosition - rebased[(size_t) active];
        } else {
          int64_t candidateLength = endPosition - rebased[(size_t) active];
          if (candidateLength < matchLength) matchLength = candidateLength;
        }
      }
      positioned = false;
      return matchLength <= (int64_t) slop;
    }

    float matchWeight() const {
      return 1.0f / (float) (int64_t(1) + matchLength);
    }

  public:
    static constexpr bool IS_SLOPPY = true;

    explicit SloppyMatcher(int32_t slop) : slop(slop) {}

    template<class S>
    void init(S& scorer) {
      actual = scorer.pool->template make_span<int32_t>(scorer.slotEnums.size());
      rebased = scorer.pool->template make_span<int64_t>(scorer.slotEnums.size());
    }

    template<class S>
    bool matches(S& scorer) {
      freq = 0.0f;
      freqComplete = true;
      positioned = false;
      if (scorer.docid == PostingsReader::END) return false;

      for (const RepeatGroup& group : scorer.groups) {
        if (group.docsEnum->termFreq() < (int32_t) group.slots.size()) return false;
      }
      if (scorer.minCompetitiveScore > 0.0f && scorer.simScorer != nullptr
          && !ScorerControls::disableDocBoundForTests) {
        int64_t maxFreq = 1;
        for (DocsPosEnum* docsEnum : scorer.slotEnums) {
          maxFreq += (int64_t) docsEnum->termFreq() - 1;
        }
        float boundFreq = S::roundUpToFloat(maxFreq);
        if (scorer.simScorer->score(boundFreq, scorer.lookupNorm(scorer.docid))
            < scorer.minCompetitiveScore) {
          skipCount(SkipStats::phraseBoundRejects);
          return false;
        }
      }
      skipCount(SkipStats::phraseVerifies);
      if (!initPositions(scorer) || !nextMatch(scorer)) return false;
      freq = matchWeight();
      freqComplete = false;
      return true;
    }

    template<class S>
    float scoreFreq(S& scorer) {
      if (!freqComplete) {
        while (nextMatch(scorer)) freq += matchWeight();
        freqComplete = true;
      }
      return freq;
    }

    template<class S>
    float getMaxScore(S& scorer, int32_t upTo) {
      if (scorer.simScorer == nullptr) return 0.0f;
      float tier1 = scorer.simScorer->internalWeight();
      if (scorer.impacts.empty() || ScorerControls::disableRawBoundsForTests) return tier1;
      int32_t startDoc = scorer.shallowTarget >= 0 ? scorer.shallowTarget : scorer.docid;
      int64_t freqBound = 1;
      int32_t normLower = 0;
      for (size_t i = 0; i < scorer.impacts.size(); i++) {
        const ImpactsIndex& termImpacts = scorer.impacts[i];
        int32_t startBlock = termImpacts.blockContaining(startDoc);
        if (startBlock >= termImpacts.blockCount()) return tier1;
        int32_t upBlock = termImpacts.blockContaining(upTo);
        if (upBlock >= termImpacts.blockCount()) upBlock = termImpacts.blockCount() - 1;
        if (upBlock < startBlock) return tier1;
        ImpactsIndex::RawRange raw = termImpacts.rawRangeInRange(startBlock, upBlock);
        if (raw.maxTf <= 0 || raw.minNorm == std::numeric_limits<int32_t>::max()) return tier1;
        freqBound += (int64_t) scorer.impactMultiplicities[i]
            * ((int64_t) raw.maxTf - 1);
        normLower = std::max(normLower, raw.minNorm);
      }
      return scorer.simScorer->score(S::roundUpToFloat(freqBound), normLower);
    }
  };

  template<class MatcherPolicy>
  class PhraseScorer final : public Query::Scorer {
    friend MatcherPolicy;

    std::span<DocsPosEnum*> slotEnums;
    std::span<PosEnum*> slotPosEnums;
    std::span<const int32_t> positions;
    std::span<const int32_t> ordinals;
    std::span<DocsPosEnum*> conjunctionEnums;
    std::span<PosEnum*> conjunctionPosEnums;
    MemPool* pool;
    std::span<const int32_t> slotGroup;
    std::span<RepeatGroup> groups;
    std::span<int32_t> slotCursor;
    std::optional<NormsReader::Iterator> normsIter;
    Similarity::BM25Scorer* simScorer;
    std::span<ImpactsIndex> impacts;
    std::span<const int32_t> impactMultiplicities;
    MatcherPolicy matcher;
    float minCompetitiveScore = 0.0f;
    // Competitive-block gate certificate (TermQuery shape): lead postings at
    // or below competitiveUpTo may compete; competitiveBound is the certified
    // composed bound, so threshold rises below it keep the certificate.
    int32_t competitiveUpTo = PostingsReader::END;
    float competitiveBound = std::numeric_limits<float>::infinity();
    // Monotone impact-group resume hints, one per conjunction term.
    std::span<int32_t> impactCursors;
    int64_t skippedImpactBlocks = 0;
    int32_t shallowTarget = -1;
    int32_t docid = -1;
    int32_t checkedDocid = -1;
    bool checkedMatch = false;
    float matchCostEstimate = 0.0f;
    int64_t approximationCost;
    bool exclusionWindowFill;
    const uint8_t* flatNormsBase = nullptr;
    int32_t cachedNormDoc = -1;
    uint8_t cachedNorm = 0;
#ifndef NDEBUG
    int32_t protocol = 0;
#endif

    int32_t doNext(int32_t target) {
      auto* firstEnum = conjunctionEnums[0];
      outer:
      for (;;) {
        for (int j = 1; j < conjunctionEnums.size(); j++) {
          if (conjunctionEnums[j]->docId() < target) {
            int32_t id = conjunctionEnums[j]->advance(target);
            assert(id >= target);
            if (id > target) {
              target = firstEnum->advance(id);
              goto outer;
            }
          }
        }
        docid = target;
        return docid;
      }
    }

    // Cold path of the competitive-block gate. Windows are keyed to the LEAD
    // (rarest) term's impact blocks, Lucene mergeImpacts-style: the lead's
    // firstCompetitiveTarget group-hops its dead blocks, and each surviving
    // lead block [doc, windowEnd] is then bounded by the other terms' parse-
    // free range bounds over the same window. The composed exact-phrase bound
    // over a range is min over terms of that term's max impact there (phrase
    // freq <= each term's freq, and the per-term impacts are scored with the
    // phrase's own scorer), so a window whose min falls below the threshold
    // holds no competitive doc. Keying to the lead keeps certificates wide
    // (one cold call per ~128 lead postings), where a min-over-terms window
    // would collapse to the densest term's ~128-posting doc span and put the
    // cold path on every lead posting. Certifies [returned doc, competitiveUpTo].
    LUXIR_NOINLINE int32_t skipNonCompetitiveBlocks(int32_t doc) {
      if (impacts.empty() || !(minCompetitiveScore > 0.0f)) {
        competitiveUpTo = PostingsReader::END;
        competitiveBound = impacts.empty()
            ? std::numeric_limits<float>::infinity() : 0.0f;
        return doc;
      }
      int64_t skippedBefore = skippedImpactBlocks;
      while (doc != PostingsReader::END) {
        skipCount(SkipStats::phraseCompetitiveColdLookups);
        auto landing = impacts[0].firstCompetitiveTarget(
            doc, minCompetitiveScore, skippedImpactBlocks, impactCursors[0]);
        impactCursors[0] = landing.group;
        if (landing.doc == PostingsReader::END) {
          doc = PostingsReader::END;
          break;
        }
        if (landing.doc > doc) {
          doc = conjunctionEnums[0]->advance(landing.doc);
          if (doc > landing.lastDoc) continue;
        }
        int32_t windowEnd = landing.lastDoc;
        float bound = landing.impact;
        for (size_t i = 1; i < impacts.size() && bound >= minCompetitiveScore;
             i++) {
          // Non-lead terms are bounded at GROUP granularity only: group-table
          // reads, never an L0 group parse. A dense term's parse would cost
          // more than the fine bound is worth; the lead's own block impacts
          // carry the fine-grained decisions.
          const ImpactsIndex& termImpacts = impacts[i];
          if (termImpacts.numGroups() == 0) {
            bound = std::min(bound, termImpacts.globalMaxImpact());
            continue;
          }
          int32_t gFrom =
              termImpacts.groupContainingFrom(impactCursors[i], doc);
          impactCursors[i] = gFrom;
          if (gFrom >= termImpacts.numGroups()) {
            continue;  // past this term's impact data: unbounded
          }
          int32_t gTo = termImpacts.groupContainingFrom(gFrom, windowEnd);
          if (gTo >= termImpacts.numGroups()) continue;  // tail unbounded
          bound = std::min(
              bound, termImpacts.maxGroupImpactInRange(gFrom, gTo));
        }
        if (bound >= minCompetitiveScore) {
          competitiveUpTo = windowEnd;
          competitiveBound = bound;
          break;
        }
        skippedImpactBlocks++;
        doc = conjunctionEnums[0]->advance(windowEnd + 1);
      }
      if (SkipStats::enabled) {
        SkipStats::phraseImpactBlocksSkipped +=
            skippedImpactBlocks - skippedBefore;
      }
      return doc;
    }

    int32_t competitiveLead(int32_t doc) {
      if constexpr (MatcherPolicy::IS_SLOPPY) return doc;
      if (ScorerControls::disableCompetitiveBlocksForTests) return doc;
      return doc <= competitiveUpTo ? doc : skipNonCompetitiveBlocks(doc);
    }

    int32_t doApproximationNext() {
      return doNext(competitiveLead(conjunctionEnums[0]->next()));
    }
    int32_t doApproximationAdvance(int32_t target) {
      return doNext(competitiveLead(conjunctionEnums[0]->advance(target)));
    }

    void resetRepeatGroups() {
      for (auto& group : groups) {
        group.count = group.docsEnum->termFreq();
        if (group.count >= group.cap) {
          group.cap = std::max(group.count + 1, group.cap * 2);
          group.buf = pool->make_arr<int32_t>((size_t) group.cap);
        }
        group.filled = 0;
        group.buf[group.count] = PostingsReader::END;
      }
      std::fill(slotCursor.begin(), slotCursor.end(), 0);
    }

    int32_t repeatSlotFirst(int32_t slot) {
      int32_t groupId = slotGroup[(size_t) slot];
      assert(groupId >= 0);
      RepeatGroup& group = groups[(size_t) groupId];
      while (group.filled == 0 && group.filled < group.count) {
        group.buf[group.filled++] = group.posEnum->nextPosition();
      }
      slotCursor[(size_t) slot] = 0;
      return group.buf[0];
    }

    int32_t repeatSlotAdvance(int32_t slot, int32_t target) {
      int32_t groupId = slotGroup[(size_t) slot];
      if (groupId < 0) return slotPosEnums[(size_t) slot]->advancePosition(target);
      RepeatGroup& group = groups[(size_t) groupId];
      int32_t idx = slotCursor[(size_t) slot];
      for (;;) {
        while (idx >= group.filled && group.filled < group.count) {
          group.buf[group.filled++] = group.posEnum->nextPosition();
        }
        if (group.buf[idx] >= target) break;
        idx++;
      }
      slotCursor[(size_t) slot] = idx;
      return group.buf[idx];
    }

    int32_t repeatSlotNext(int32_t slot) {
      int32_t groupId = slotGroup[(size_t) slot];
      if (groupId < 0) return slotPosEnums[(size_t) slot]->nextPosition();
      RepeatGroup& group = groups[(size_t) groupId];
      int32_t idx = slotCursor[(size_t) slot];
      if (idx >= group.count) return PostingsReader::END;
      idx++;
      while (idx >= group.filled && group.filled < group.count) {
        group.buf[group.filled++] = group.posEnum->nextPosition();
      }
      slotCursor[(size_t) slot] = idx;
      return group.buf[idx];
    }

    int64_t lookupNorm(int32_t doc) {
      if (flatNormsBase != nullptr) return flatNormsBase[doc];
      if (doc != cachedNormDoc) {
        int32_t normDoc = normsIter->advance(doc);
        assert(normDoc == doc);
        unused(normDoc);
        cachedNormDoc = doc;
        cachedNorm = normsIter->value();
      }
      return cachedNorm;
    }

    bool doMatches() {
      if (docid == checkedDocid) return checkedMatch;
      checkedDocid = docid;
      checkedMatch = false;
      if (ScorerControls::countMatchesForTests && docid != PostingsReader::END) {
        ScorerControls::matchCallsForTests++;
      }
      checkedMatch = matcher.matches(*this);
      return checkedMatch;
    }

#ifndef NDEBUG
    void markSinglePhase() {
      assert(protocol != 2 && protocol != 3);
      protocol = 1;
    }
    void markTwoPhase() {
      assert(protocol != 1 && protocol != 3);
      protocol = 2;
    }
    void markExternal() {
      assert(protocol != 1 && protocol != 2);
      protocol = 3;
    }
#endif

  public:
    static inline bool& countMatchesForTests = ScorerControls::countMatchesForTests;
    static inline int64_t& matchCallsForTests = ScorerControls::matchCallsForTests;
    static inline bool& disableDocBoundForTests = ScorerControls::disableDocBoundForTests;
    static inline bool& disableSortForTests = ScorerControls::disableSortForTests;
    static inline bool& disableRepeatDedupForTests = ScorerControls::disableRepeatDedupForTests;
    static inline bool& disableRawBoundsForTests = ScorerControls::disableRawBoundsForTests;
    static inline bool& disableCompetitiveBlocksForTests =
        ScorerControls::disableCompetitiveBlocksForTests;
    static inline bool& disableSloppyGallopForTests =
        ScorerControls::disableSloppyGallopForTests;

    static float roundUpToFloat(int64_t value) {
      float rounded = (float) value;
      if ((double) rounded < (double) value) {
        rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
      }
      return rounded;
    }

    PhraseScorer(MemPool& targetPool, std::span<DocsPosEnum*> slotEnums,
                 std::span<PosEnum*> slotPosEnums,
                 std::span<const int32_t> positions, std::span<const int32_t> ordinals,
                 std::span<DocsPosEnum*> conjunctionEnums,
                 std::span<PosEnum*> conjunctionPosEnums, NormsReader* normsReader,
                 Similarity::BM25Scorer* simScorer, std::span<ImpactsIndex> impacts,
                 std::span<const int32_t> impactMultiplicities,
                 std::span<const int32_t> slotGroup, std::span<RepeatGroup> groups,
                 int64_t approximationCost, float matchCostEstimate,
                 int32_t slop, bool exclusionWindowFill = false)
        : slotEnums(slotEnums), slotPosEnums(slotPosEnums), positions(positions),
          ordinals(ordinals), conjunctionEnums(conjunctionEnums),
          conjunctionPosEnums(conjunctionPosEnums), pool(&targetPool), slotGroup(slotGroup),
          groups(groups), simScorer(simScorer), impacts(impacts),
          impactMultiplicities(impactMultiplicities), matcher(slop),
          matchCostEstimate(matchCostEstimate),
          approximationCost(approximationCost),
          exclusionWindowFill(exclusionWindowFill) {
      if (!groups.empty()) slotCursor = targetPool.make_span<int32_t>(slotEnums.size());
      assert(approximationCost == conjunctionEnums[0]->numDocs());
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) {
        normsIter.emplace(*normsReader);
        flatNormsBase = normsReader->flatBase();
      }
      if (!MatcherPolicy::IS_SLOPPY && !impacts.empty()) {
        competitiveBound = 0.0f;
        impactCursors = targetPool.make_span<int32_t>(impacts.size());
        std::fill(impactCursors.begin(), impactCursors.end(), -1);
      }
      matcher.init(*this);
    }

    int32_t nextApprox() {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationNext();
    }
    int32_t advanceApprox(int32_t target) {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationAdvance(target);
    }
    bool confirmMatch() {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doMatches();
    }
    int32_t approximationNext() override {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationNext();
    }
    int32_t approximationAdvance(int32_t target) override {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doApproximationAdvance(target);
    }
    int32_t approximationDocId() override { return docid; }
    std::span<DocsPosEnum*> approximationEnums() override { return conjunctionEnums; }
    bool matches() override {
#ifndef NDEBUG
      markTwoPhase();
#endif
      return doMatches();
    }
    bool matchesAt(int32_t doc) override {
#ifndef NDEBUG
      markExternal();
      for (DocsPosEnum* docsEnum : conjunctionEnums) assert(docsEnum->docId() == doc);
#endif
      docid = doc;
      return doMatches();
    }
    float matchCost() override { return matchCostEstimate; }

    void recordWindowFilterCommit(bool supported) const override {
      if (exclusionWindowFill) {
        skipCount(supported ? SkipStats::phraseExclusionWindowAdmits
                            : SkipStats::phraseExclusionWindowRejects);
      }
    }

    void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                        int32_t windowEnd) override {
#ifndef NDEBUG
      markSinglePhase();
#endif
      skipCount(SkipStats::countBulkFillCalls);
      if (windowEnd <= windowStart || docid == PostingsReader::END) {
        return;
      }
      // Membership fill: never route through the competitive gate, which is
      // licensed to drop sub-threshold docs.
      if (docid < windowStart) {
        doNext(conjunctionEnums[0]->advance(windowStart));
      }
      while (docid < windowEnd) {
        if (doMatches()) {
          int32_t index = docid - windowStart;
          windowBits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
        }
        doNext(conjunctionEnums[0]->next());
      }
    }

    int32_t next() override {
#ifndef NDEBUG
      markSinglePhase();
#endif
      if (docid == PostingsReader::END) return PostingsReader::END;
      doApproximationNext();
      for (;;) {
        if (docid == PostingsReader::END) return PostingsReader::END;
        if (doMatches()) return docid;
        doApproximationNext();
      }
    }

    int32_t advance(int32_t target) override {
#ifndef NDEBUG
      markSinglePhase();
#endif
      assert(docid < target);
      doApproximationAdvance(target);
      for (;;) {
        if (docid == PostingsReader::END) return PostingsReader::END;
        if (doMatches()) return docid;
        doApproximationNext();
      }
    }

    int32_t docId() override { return docid; }

    int32_t numMatches() requires (!MatcherPolicy::IS_SLOPPY) {
      return matcher.numMatches(*this);
    }

    float phraseFreqForTests() { return matcher.scoreFreq(*this); }

    void setMinCompetitiveScore(float minScore) override {
      bool rose = minScore > minCompetitiveScore;
      minCompetitiveScore = minScore;
      if constexpr (!MatcherPolicy::IS_SLOPPY) {
        if (!rose || competitiveUpTo < 0) return;
        if (minScore > competitiveBound) {
          competitiveUpTo = -1;
          skipCount(SkipStats::impactCertificateInvalidations);
        } else {
          skipCount(SkipStats::impactCertificateSurvivedRises);
        }
      }
    }

    float getMaxScore(int32_t upTo) override {
      if (simScorer == nullptr) return 0.0f;
      return matcher.getMaxScore(*this, upTo);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return ScoreBounds::nonNegative(getMaxScore(upTo));
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return ScoreBounds::nonNegative(getMaxScore(upTo));
    }

    int32_t advanceShallow(int32_t target) override {
      if (impacts.empty()) return PostingsReader::END;
      shallowTarget = target;
      int32_t upTo = PostingsReader::END;
      for (const auto& termImpacts : impacts) {
        int32_t block = termImpacts.blockContaining(target);
        if (block >= termImpacts.blockCount()) return PostingsReader::END;
        upTo = std::min(upTo, termImpacts.lastDoc(block));
      }
      return upTo;
    }

    float score() override {
      if (simScorer == nullptr) return 0.0f;
      return simScorer->score(matcher.scoreFreq(*this), lookupNorm(docid));
    }
  };
};

} // namespace luxir
