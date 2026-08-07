#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

#include "ImpactsIndex.h"
#include "Query.h"
#include "solux/reader/NormsReader.h"
#include "solux/reader/PosEnum.h"

namespace solux {

class PhraseQuery final : public Query {
  std::string_view field;
  std::span<std::string_view> terms;
  std::span<const int32_t> positions;
  int32_t slop;

public:
  struct ScorerControls {
    static inline bool countMatchesForTests = false;
    static inline int64_t matchCallsForTests = 0;
    static inline bool disableDocBoundForTests = false;
    static inline bool disableSortForTests = false;
    static inline bool disableRepeatDedupForTests = false;
    static inline bool disableRawBoundsForTests = false;
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

  PhraseQuery(std::string_view field, std::span<std::string_view> terms,
              std::span<const int32_t> positions, int32_t slop = 0)
      : field(field), terms(terms), positions(positions), slop(slop) {
    assert(terms.size() == positions.size());
    assert(terms.size() >= 2);
    assert(slop >= 0);
  }

  [[nodiscard]] std::string_view getField() const { return field; }
  [[nodiscard]] std::span<std::string_view> getTerms() const { return terms; }
  [[nodiscard]] std::span<const int32_t> getPositions() const { return positions; }
  [[nodiscard]] int32_t getSlop() const { return slop; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendTag(FilterKeyTag::PHRASE);
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

  public:
    Weight(Query::Context& context, PhraseQuery& query, int32_t flags, float multiplier)
        : Query::Weight(context, flags), query(query) {
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

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      if (cachedFieldInfo == nullptr) return nullptr;
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) return nullptr;

      auto querySlotEnums = targetPool.make_span<DocsPosEnum*>(cachedTermInfos.size());
      auto querySlotPosEnums = targetPool.make_span<PosEnum*>(cachedTermInfos.size());
      auto queryTerms = query.getTerms();
      struct TermEnums {
        DocsPosEnum* docs;
        PosEnum* positions;
      };
      std::vector<TermEnums> distinct;
      distinct.reserve(cachedTermInfos.size());
      bool dedupRepeats = !ScorerControls::disableRepeatDedupForTests || query.getSlop() > 0;
      for (int32_t i = 0; i < (int32_t) cachedTermInfos.size(); i++) {
        int32_t first = i;
        if (dedupRepeats) {
          for (int32_t j = 0; j < i; j++) {
            if (queryTerms[(size_t) j] == queryTerms[(size_t) i]) {
              first = j;
              break;
            }
          }
        }
        if (first != i) {
          querySlotEnums[(size_t) i] = querySlotEnums[(size_t) first];
          querySlotPosEnums[(size_t) i] = querySlotPosEnums[(size_t) first];
          continue;
        }
        DocsPosEnum* docsEnum = cachedTermInfos[(size_t) i]
            ->useDocsEnum<DocsEnumTier::POSITIONS>(targetPool, segment);
        if (docsEnum == nullptr) return nullptr;
        PosEnum* posEnum = targetPool.make<PosEnum>(*docsEnum);
        querySlotEnums[(size_t) i] = docsEnum;
        querySlotPosEnums[(size_t) i] = posEnum;
        distinct.push_back({docsEnum, posEnum});
      }

      auto byCost = [](DocsPosEnum* a, DocsPosEnum* b) {
        if (a->numDocs() != b->numDocs()) return a->numDocs() < b->numDocs();
        if (a->totalTermFreq() != b->totalTermFreq()) {
          return a->totalTermFreq() < b->totalTermFreq();
        }
        return false;
      };
      if (!ScorerControls::disableSortForTests) {
        std::stable_sort(distinct.begin(), distinct.end(), [&](const TermEnums& a,
                                                               const TermEnums& b) {
          return byCost(a.docs, b.docs);
        });
      }
      auto conjunctionEnums = targetPool.make_span<DocsPosEnum*>(distinct.size());
      auto conjunctionPosEnums = targetPool.make_span<PosEnum*>(distinct.size());
      for (size_t i = 0; i < distinct.size(); i++) {
        conjunctionEnums[i] = distinct[i].docs;
        conjunctionPosEnums[i] = distinct[i].positions;
      }

      std::vector<int32_t> order(querySlotEnums.size());
      for (size_t i = 0; i < order.size(); i++) order[i] = (int32_t) i;
      if (!ScorerControls::disableSortForTests) {
        std::stable_sort(order.begin(), order.end(), [&](int32_t a, int32_t b) {
          DocsPosEnum* ea = querySlotEnums[(size_t) a];
          DocsPosEnum* eb = querySlotEnums[(size_t) b];
          if (byCost(ea, eb)) return true;
          if (byCost(eb, ea)) return false;
          return a < b;
        });
      }
      auto slotEnums = targetPool.make_span<DocsPosEnum*>(order.size());
      auto slotPosEnums = targetPool.make_span<PosEnum*>(order.size());
      auto positions = targetPool.make_span<int32_t>(order.size());
      auto ordinals = targetPool.make_span<int32_t>(order.size());
      for (size_t k = 0; k < order.size(); k++) {
        int32_t ord = order[k];
        slotEnums[k] = querySlotEnums[(size_t) ord];
        slotPosEnums[k] = querySlotPosEnums[(size_t) ord];
        positions[k] = query.getPositions()[(size_t) ord];
        ordinals[k] = ord;
      }

      auto slotGroup = targetPool.make_span<int32_t>(slotEnums.size());
      std::fill(slotGroup.begin(), slotGroup.end(), -1);
      std::vector<std::vector<int32_t>> groupSlots;
      for (size_t i = 0; i < slotEnums.size(); i++) {
        if (slotGroup[i] >= 0) continue;
        std::vector<int32_t> members;
        for (size_t j = i; j < slotEnums.size(); j++) {
          if (slotEnums[j] == slotEnums[i]) members.push_back((int32_t) j);
        }
        if (members.size() < 2) continue;
        std::sort(members.begin(), members.end(), [&](int32_t a, int32_t b) {
          if (positions[(size_t) a] != positions[(size_t) b]) {
            return positions[(size_t) a] < positions[(size_t) b];
          }
          return ordinals[(size_t) a] < ordinals[(size_t) b];
        });
        int32_t gid = (int32_t) groupSlots.size();
        for (int32_t slot : members) slotGroup[(size_t) slot] = gid;
        groupSlots.push_back(std::move(members));
      }
      auto groups = targetPool.make_span<RepeatGroup>(groupSlots.size());
      for (size_t g = 0; g < groupSlots.size(); g++) {
        groups[g].docsEnum = slotEnums[(size_t) groupSlots[g][0]];
        groups[g].posEnum = slotPosEnums[(size_t) groupSlots[g][0]];
        groups[g].slots = targetPool.copy_span(
            std::span<int32_t>(groupSlots[g].data(), groupSlots[g].size()));
      }

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
            query.getSlop(), (inputFlags & EXCLUSION_WINDOW_FILL) != 0);
      }
      return targetPool.make<Scorer>(
          targetPool, slotEnums, slotPosEnums, positions, ordinals,
          conjunctionEnums, conjunctionPosEnums, normsReader,
          simScorer, impacts, impactMultiplicities, slotGroup, groups, 0,
          (inputFlags & EXCLUSION_WINDOW_FILL) != 0);
    }

    class Supplier final : public Query::ScorerSupplier {
      PhraseQuery::Weight& weight;
      IndexReader::Segment& segment;

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

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        Query::Scorer* scorer = weight.createScorer(targetPool, segment);
        if (scorer == nullptr) return nullptr;
        if (weight.query.getSlop() > 0) {
          static_cast<PhraseQuery::SloppyScorer*>(scorer)
              ->setWindowFillPositiveCost(leadCost);
        } else {
          static_cast<PhraseQuery::Scorer*>(scorer)
              ->setWindowFillPositiveCost(leadCost);
        }
        return scorer;
      }
    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool,
                                           IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
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
      while (advanceSlot(scorer, active)) {
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

    // Position-verification price for a windowed exclusion fill, relative to
    // the positive side's cost. A windowed fill verifies positions for every
    // approximation hit in the window; the pull path verifies only the docs
    // that survive the positive leapfrog, so admitting too eagerly loses.
    // MEASURED on the 70-query neg_phrase population (5M corpus): weight 4.0
    // admits 8 and gives class 0.90; 1.0 admits 22 and gives 0.77 with one
    // query at 1.12; 0.25 admits 41 and gives 0.68 but grows a loss tail of
    // seven queries up to 1.53. 1.0 takes most of the win without the tail.
    static constexpr double kExclusionWindowFillPositionWeight = 1.0;

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
    int32_t shallowTarget = -1;
    int32_t docid = -1;
    int32_t checkedDocid = -1;
    bool checkedMatch = false;
    float matchCostEstimate = 0.0f;
    int64_t windowFillPositiveCost = 0;
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

    int32_t doApproximationNext() { return doNext(conjunctionEnums[0]->next()); }
    int32_t doApproximationAdvance(int32_t target) {
      return doNext(conjunctionEnums[0]->advance(target));
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
                 int32_t slop, bool exclusionWindowFill = false)
        : slotEnums(slotEnums), slotPosEnums(slotPosEnums), positions(positions),
          ordinals(ordinals), conjunctionEnums(conjunctionEnums),
          conjunctionPosEnums(conjunctionPosEnums), pool(&targetPool), slotGroup(slotGroup),
          groups(groups), simScorer(simScorer), impacts(impacts),
          impactMultiplicities(impactMultiplicities), matcher(slop),
          approximationCost(conjunctionEnums[0]->numDocs()),
          exclusionWindowFill(exclusionWindowFill) {
      if (!groups.empty()) slotCursor = targetPool.make_span<int32_t>(slotEnums.size());
      assert((simScorer == nullptr) == (normsReader == nullptr));
      if (normsReader != nullptr) {
        normsIter.emplace(*normsReader);
        flatNormsBase = normsReader->flatBase();
      }
      if constexpr (MatcherPolicy::IS_SLOPPY) {
        double averageTf = 0.0;
        for (DocsPosEnum* docsEnum : slotEnums) {
          int32_t numDocs = docsEnum->numDocs();
          averageTf += numDocs > 0
              ? (double) docsEnum->totalTermFreq() / (double) numDocs : 1.0;
        }
        double estimate = (double) slotEnums.size() * averageTf
            + (double) groups.size() * (double) slotEnums.size();
        matchCostEstimate = (float) std::min(
            estimate, (double) std::numeric_limits<float>::max());
      } else {
        for (auto* docsEnum : slotEnums) {
          int32_t numDocs = docsEnum->numDocs();
          if (numDocs > 0) {
            matchCostEstimate += (float) docsEnum->totalTermFreq() / (float) numDocs;
          } else {
            matchCostEstimate += 1.0f;
          }
        }
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
    bool hasTwoPhase() const override { return true; }
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

    void setWindowFillPositiveCost(int64_t cost) {
      windowFillPositiveCost = cost;
    }

    bool supportsWindowFilter() const override {
      if (!exclusionWindowFill) {
        return false;
      }
      double fillWork = (double) approximationCost
          * (double) std::max(matchCostEstimate, 1.0f)
          * kExclusionWindowFillPositionWeight;
      bool admitted = fillWork <= (double) windowFillPositiveCost;
      skipCount(admitted ? SkipStats::phraseExclusionWindowAdmits
                         : SkipStats::phraseExclusionWindowRejects);
      return admitted;
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
      if (docid < windowStart) {
        doApproximationAdvance(windowStart);
      }
      while (docid < windowEnd) {
        if (doMatches()) {
          int32_t index = docid - windowStart;
          windowBits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
        }
        doApproximationNext();
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
      minCompetitiveScore = minScore;
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

} // namespace solux
