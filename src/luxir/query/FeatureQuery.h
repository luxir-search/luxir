// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include "ImpactsIndex.h"
#include "Query.h"
#include "luxir/search/Similarity.h"

namespace luxir {

// Scores the documents that have one feature (term) of a feature field
// (FeatureFieldType) by FeatureScorer: weight * f(stored value), with the
// stored value decoded from the term frequency. A sparse_vector query is a
// disjunction of these with LINEAR f, so its score is the dot product of the
// query and document weights; rank_feature picks the other functions.
//
// Feature postings have no norms, so each block's impact is its max tf and
// the block bound is f(decode(maxTf)) (f only grows). The scorer skips
// blocks below the collector's floor and answers block-max bounds, so
// MaxScore/WAND disjunctions prune feature clauses like BM25 term clauses.
class FeatureQuery final : public luxir::Query {
  std::string_view field;
  std::string_view term;
  // weight and the function; a SATURATION pivot <= 0 is computed per request
  // from the feature's index statistics (see Weight).
  FeatureScorer scorer;

public:
  FeatureQuery(std::string_view field, std::string_view term, const FeatureScorer& scorer)
      : Query(QueryKind::FEATURE), field(field), term(term), scorer(scorer) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const FeatureQuery&>(other);
    return field == rhs.field && term == rhs.term && scorer.function == rhs.scorer.function
        && scorer.weight == rhs.scorer.weight && scorer.pivot == rhs.scorer.pivot
        && scorer.scalingFactor == rhs.scorer.scalingFactor
        && scorer.exponent == rhs.scorer.exponent;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, term);
    value = mixHash(value, scorer.function);
    value = mixHash(value, scorer.weight);
    value = mixHash(value, scorer.pivot);
    value = mixHash(value, scorer.scalingFactor);
    return mixHash(value, scorer.exponent);
  }

  std::string_view getField() const { return field; }
  std::string_view getTerm() const { return term; }
  const FeatureScorer& getScorer() const { return scorer; }

  void validateLogicalImpl(PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, scorer.weight);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    // Membership is the docs that have the feature; the function only
    // shapes scores.
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendTerm(term);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Weight;

  Weight* createWeight(Context& context, int32_t flags, float multiplier = 1.0f) override;

  class Scorer final : public Query::Scorer {
    DocsFreqEnum& docsEnum;
    const FeatureScorer* featureScorer;  // null when scores are not needed
    ImpactsIndex impacts;
    int32_t doc = -1;
    float minCompetitiveScore = 0.0f;
    // Every doc up to here lies in a block that can reach the floor.
    int32_t competitiveUpTo = -1;
    int32_t competitiveGroup = -1;
    int32_t shallowTarget = -1;
    int64_t skippedBlocks = 0;

    bool pruning() const {
      return featureScorer != nullptr && !impacts.empty() && minCompetitiveScore > 0.0f;
    }

    // First doc at or after doc (the enum's position) in a block that can
    // reach the floor.
    int32_t competitive(int32_t current) {
      while (current != PostingsReader::END && current > competitiveUpTo) {
        auto landing = impacts.firstCompetitiveTarget(current, minCompetitiveScore,
                                                      skippedBlocks, competitiveGroup);
        competitiveGroup = landing.group;
        if (landing.doc == PostingsReader::END) return PostingsReader::END;
        competitiveUpTo = landing.lastDoc;
        if (landing.doc > current) current = docsEnum.advance(landing.doc);
      }
      return current;
    }

  public:
    Scorer(MemPool& pool, DocsFreqEnum& docsEnum, const FeatureScorer* featureScorer)
        : docsEnum(docsEnum), featureScorer(featureScorer) {
      if (featureScorer != nullptr) impacts.build(pool, docsEnum, *featureScorer);
    }

    int32_t next() override {
      doc = docsEnum.nextDoc();
      if (pruning()) doc = competitive(doc);
      return doc;
    }

    int32_t advance(int32_t target) override {
      return doc = docsEnum.advance(target);
    }

    int32_t docId() override { return doc; }

    float score() override {
      return featureScorer == nullptr ? 0.0f : featureScorer->score(docsEnum.termFreq());
    }

    void setMinCompetitiveScore(float minScore) override {
      if (minScore > minCompetitiveScore) {
        minCompetitiveScore = minScore;
        competitiveUpTo = -1;
      }
    }

    float getMaxScore(int32_t upTo) override {
      if (featureScorer == nullptr) return 0.0f;
      if (impacts.empty()) return featureScorer->unboundedMax();
      if (upTo == PostingsReader::END) return impacts.globalMaxImpact();
      // Bound [shallow target, upTo], or from the current doc before any
      // advanceShallow (the TermQuery::Scorer contract).
      int32_t start = std::max(shallowTarget >= 0 ? shallowTarget : doc, 0);
      int32_t startBlock = impacts.blockContaining(start);
      if (startBlock >= impacts.blockCount()) return std::numeric_limits<float>::infinity();
      int32_t upBlock = std::min(impacts.blockContainingFrom(startBlock, upTo),
                                 impacts.blockCount() - 1);
      if (upBlock < startBlock) return std::numeric_limits<float>::infinity();
      return impacts.maxImpactInRange(startBlock, upBlock);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return ScoreBounds::nonNegative(getMaxScore(upTo));
    }

    int32_t advanceShallow(int32_t target) override {
      if (impacts.empty()) return PostingsReader::END;
      shallowTarget = target;
      int32_t block = impacts.blockContaining(target);
      return block >= impacts.blockCount() ? PostingsReader::END : impacts.lastDoc(block);
    }
  };

  class Weight final : public Query::Weight {
    CachedTermInfo* cachedTermInfo = nullptr;
    FeatureScorer scorer;

  public:
    Weight(Context& context, FeatureQuery& query, int32_t flags, float multiplier)
        : Query::Weight(context, query, flags), scorer(query.getScorer()) {
      if (!needsScores()) traits |= IS_CONSTANT_SCORING;
      if (auto* fieldInfo = context.getCachedFieldInfo(query.getField())) {
        cachedTermInfo = context.getCachedTerminfo(*fieldInfo, query.getTerm());
      }
      scorer.weight = checkedBoostProduct(multiplier, scorer.weight);
      if (scorer.function == FeatureScorer::SATURATION && !(scorer.pivot > 0.0f)) {
        // Lucene's computePivotFeatureValue: the mean encoded value is the
        // log-space mean of the weights, an approximate geometric mean.
        const auto& stats = cachedTermInfo ? cachedTermInfo->termStats : Similarity::TermStats{};
        scorer.pivot = stats.docFreq > 0
            ? FeatureScorer::decodeTf((int32_t) ((double) stats.totalTermFreq / (double) stats.docFreq))
            : 1.0f;
      }
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      if (cachedTermInfo == nullptr) return nullptr;
      auto* docsEnum = cachedTermInfo->useDocsEnum<DocsEnumTier::FREQS>(targetPool, segment);
      if (docsEnum == nullptr) return nullptr;
      return targetPool.make<Scorer>(targetPool, *docsEnum, needsScores() ? &scorer : nullptr);
    }

    CountShortcut countShortcut(IndexReader::Segment& segment, DocSet* domain) override {
      int64_t docs = cachedTermInfo ? cachedTermInfo->docFreq(segment.ord) : 0;
      return countBound(segment, domain, docs, CountHow::METADATA);
    }

    class Supplier final : public Query::ScorerSupplier {
      FeatureQuery::Weight& weight;
      IndexReader::Segment& segment;

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          return supplier.weight.createScorer(targetPool, supplier.segment);
        }

      public:
        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost)
          : Query::ScorerPlan(planContext, shape, cost), supplier(supplier) {}
      };

    public:
      Supplier(FeatureQuery::Weight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override {
        return weight.cachedTermInfo ? weight.cachedTermInfo->docFreq(segment.ord) : 0;
      }

      MembershipWork membershipWork(int32_t maxDoc) override {
        unused(maxDoc);
        // Priced like a term's postings (TermQuery::Weight::Supplier).
        double df = (double) cost();
        return {df, 16 * df, std::ceil(df / DocsEnumMeta::L1_DOCS), 85, 1024, 8 * df};
      }

      Query::ScorerShape describeScorer(const Query::PlanContext& buildContext) const override {
        unused(buildContext);
        return {
          .matchState = weight.cachedTermInfo == nullptr
                  || weight.cachedTermInfo->docFreq(segment.ord) == 0
              ? Query::MatchState::EMPTY
              : Query::MatchState::NONEMPTY,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = Query::ReportedTwoPhase::NO,
          .windowFillClause = Query::ClauseShape::NONE,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .termConjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
      }

      Query::ScorerPlan* resolve(MemPool& planPool,
                                 const Query::PlanContext& planContext) override {
        return planPool.make<Plan>(*this, planContext, describeScorer(planContext), cost());
      }
    };

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      if (cachedTermInfo == nullptr || cachedTermInfo->docFreq(segment.ord) == 0) return nullptr;
      return targetPool.make<Supplier>(*this, segment);
    }
  };
};

inline FeatureQuery::Weight* FeatureQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  return context.pool.make<FeatureQuery::Weight>(context, *this, flags, multiplier);
}

static_assert(std::is_trivially_destructible_v<FeatureQuery::Weight>);
static_assert(std::is_trivially_destructible_v<FeatureQuery::Scorer>);

} // namespace luxir
