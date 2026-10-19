// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "Query.h"
#include "TermQuery.h"
#include "luxir/reader/NormsReader.h"

namespace luxir {

// BM25F over one term in several TEXT fields, scored as if the fields were a
// single combined field (Lucene's CombinedFieldQuery, the engine behind
// Elasticsearch combined_fields). Each field's weight multiplies its term
// frequency and its document length:
//
//   freq(d)   = sum_f w_f * tf_f(d)
//   length(d) = sum_f w_f * length_f(d), rounded and re-encoded like a norm
//   avgdl     = sum_f w_f * sumTotalTermFreq_f / max_f docsWithField_f
//   idf       = BM25 idf from max_f docFreq_f and max_f docsWithField_f
//
// Every listed field contributes its length to a document even when the
// term is absent from that field, so a long body still dampens a title hit.
// Weights below 1 are legal but break BM25's saturation guarantee; the
// combined_fields arm requires >= 1 like Elasticsearch.
//
// Membership is the union of the per-field postings, so an unscored use
// rewrites to a disjunction of TermQuerys and shares their filter keys.
class CombinedFieldQuery final : public Query {
  std::span<const std::string_view> fields;
  std::span<const float> weights;
  std::string_view term;

public:
  CombinedFieldQuery(std::span<const std::string_view> fields,
                     std::span<const float> weights, std::string_view term)
    : Query(QueryKind::COMBINED_FIELDS), fields(fields), weights(weights), term(term) {
    assert(!fields.empty() && fields.size() == weights.size());
  }

  std::span<const std::string_view> getFields() const { return fields; }
  std::span<const float> getWeights() const { return weights; }
  std::string_view getTerm() const { return term; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const CombinedFieldQuery&>(other);
    if (term != rhs.term || fields.size() != rhs.fields.size()) return false;
    for (size_t i = 0; i < fields.size(); i++) {
      if (fields[i] != rhs.fields[i]
          || std::bit_cast<uint32_t>(weights[i]) != std::bit_cast<uint32_t>(rhs.weights[i])) {
        return false;
      }
    }
    return true;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), term);
    for (size_t i = 0; i < fields.size(); i++) {
      value = mixHash(value, fields[i]);
      value = mixHash(value, std::bit_cast<uint32_t>(weights[i]));
    }
    return value;
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, 1.0f);
  }

  // Membership only: weights shape scores, never which docs contain the term.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendKind(kind);
    out.appendInt32((int32_t)fields.size());
    for (auto field : fields) out.appendString(field);
    out.appendTerm(term);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  class Scorer final : public Query::Scorer {
  public:
    struct Postings {
      DocsFreqEnum* docs;
      float weight;
    };
    struct Norms {
      const uint8_t* flat;
      NormsReader::Iterator* sparse;
      float weight;
    };

  private:
    std::span<Postings> postings;
    std::span<Norms> norms;
    Similarity::BM25Scorer* simScorer;
    int32_t doc = -1;
    float freq = 0.0f;

    int32_t settle() {
      int32_t minDoc = PostingsReader::END;
      for (auto& p : postings) minDoc = std::min(minDoc, p.docs->docId());
      doc = minDoc;
      if (doc == PostingsReader::END) return doc;
      freq = 0.0f;
      for (auto& p : postings) {
        if (p.docs->docId() == doc) freq += p.weight * (float)p.docs->termFreq();
      }
      return doc;
    }

    int64_t combinedNorm() {
      double length = 0.0;
      for (auto& n : norms) {
        uint8_t code;
        if (n.flat != nullptr) {
          code = n.flat[doc];
        } else {
          if (n.sparse->docId() < doc) n.sparse->advance(doc);
          if (n.sparse->docId() != doc) continue;
          code = n.sparse->value();
        }
        length += (double)n.weight * (double)SmallFloat::decodeLengthByte(code);
      }
      double rounded = std::min(std::round(length), (double)std::numeric_limits<int32_t>::max());
      return (int64_t)SmallFloat::intToByte4((int32_t)rounded);
    }

  public:
    Scorer(std::span<Postings> postings, std::span<Norms> norms,
           Similarity::BM25Scorer* simScorer)
      : postings(postings), norms(norms), simScorer(simScorer) {}

    int32_t docId() override { return doc; }

    int32_t next() override {
      for (auto& p : postings) {
        if (p.docs->docId() <= doc) p.docs->next();
      }
      return settle();
    }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      for (auto& p : postings) {
        if (p.docs->docId() < target) p.docs->advance(target);
      }
      return settle();
    }

    float score() override {
      return simScorer->score(freq, combinedNorm());
    }

    // BM25 never reaches its idf weight, so it bounds every document.
    float getMaxScore(int32_t upTo) override {
      unused(upTo);
      return simScorer->internalWeight();
    }

    float getMaxScoreForSetup(int32_t upTo) override {
      return getMaxScore(upTo);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return ScoreBounds::nonNegative(getMaxScore(upTo));
    }
  };

  class Weight final : public Query::Weight {
    CombinedFieldQuery& query;
    std::span<CachedFieldInfo*> fieldInfos;
    std::span<CachedTermInfo*> termInfos;
    Similarity::BM25Scorer* simScorer = nullptr;

    int64_t segmentCost(IndexReader::Segment& segment) const {
      int64_t cost = 0;
      for (auto* info : termInfos) {
        if (info != nullptr) cost += info->docFreq(segment.ord);
      }
      return cost;
    }

  public:
    Weight(Context& context, CombinedFieldQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, query, flags), query(query) {
      size_t n = query.fields.size();
      fieldInfos = context.pool.make_span<CachedFieldInfo*>(n);
      termInfos = context.pool.make_span<CachedTermInfo*>(n);
      int64_t docCount = 0;
      int64_t docFreq = 0;
      double sumTotalTermFreq = 0.0;
      bool anyTerm = false;
      for (size_t i = 0; i < n; i++) {
        fieldInfos[i] = context.getCachedFieldInfo(query.fields[i]);
        termInfos[i] = nullptr;
        if (fieldInfos[i] == nullptr) continue;
        const auto& stats = fieldInfos[i]->fieldStats;
        docCount = std::max(docCount, stats.docsWithField);
        sumTotalTermFreq += (double)query.weights[i] * (double)stats.sumTotalTermFreq;
        termInfos[i] = context.getCachedTerminfo(*fieldInfos[i], query.term);
        if (termInfos[i] != nullptr) {
          anyTerm = true;
          docFreq = std::max(docFreq, termInfos[i]->termStats.docFreq);
        }
      }
      if (anyTerm && docCount > 0) {
        Similarity similarity;
        float idf = similarity.idf(docFreq, docCount);
        float avgdl = (float)(sumTotalTermFreq / (double)docCount);
        simScorer = context.pool.make<Similarity::BM25Scorer>(
            multiplier, similarity.k1, similarity.b, idf, avgdl);
      }
    }

    class Supplier final : public Query::ScorerSupplier {
      Weight& weight;
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
      Supplier(Weight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override { return weight.segmentCost(segment); }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        unused(buildContext);
        return {
          .matchState = Query::MatchState::NONEMPTY,
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

      Query::ScorerPlan* resolve(
          MemPool& planPool, const Query::PlanContext& planContext) override {
        return planPool.make<Plan>(*this, planContext, describeScorer(planContext), cost());
      }
    };

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      if (simScorer == nullptr || segmentCost(segment) == 0) return nullptr;
      return targetPool.make<Supplier>(*this, segment);
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      if (simScorer == nullptr) return nullptr;
      size_t n = query.fields.size();
      size_t numPostings = 0;
      size_t numNorms = 0;
      auto postings = targetPool.make_span<Scorer::Postings>(n);
      auto norms = targetPool.make_span<Scorer::Norms>(n);
      for (size_t i = 0; i < n; i++) {
        if (fieldInfos[i] == nullptr) continue;
        const SegFieldInfo* segInfo = fieldInfos[i]->segInfos.at(segment);
        if (segInfo == nullptr) continue;
        float weight = query.weights[i];
        if (termInfos[i] != nullptr) {
          auto* docs = termInfos[i]->useDocsEnum<DocsEnumTier::FREQS>(targetPool, segment);
          if (docs != nullptr) postings[numPostings++] = {docs, weight};
        }
        if (segInfo->type != FieldType::TEXT
            || segInfo->normsFormat == SegFieldInfo::NORMS_NONE) {
          continue;
        }
        auto* reader = targetPool.make<NormsReader>(segment.postingsReader(), *segInfo);
        if (reader->numBytes() == 0) continue;
        if (reader->isFlat()) {
          norms[numNorms++] = {reader->flatBase(), nullptr, weight};
        } else {
          norms[numNorms++] = {nullptr, targetPool.make<NormsReader::Iterator>(*reader), weight};
        }
      }
      if (numPostings == 0) return nullptr;
      return targetPool.make<Scorer>(postings.first(numPostings), norms.first(numNorms), simScorer);
    }
  };

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

static_assert(std::is_trivially_destructible_v<CombinedFieldQuery::Scorer>);

} // namespace luxir

#include "BooleanQuery.h"

namespace luxir {

inline Query::Weight* CombinedFieldQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  if ((flags & NEED_SCORES) != 0) {
    return context.pool.make<CombinedFieldQuery::Weight>(context, *this, flags, multiplier);
  }
  // Unscored: the same match set as a disjunction of the per-field terms,
  // which keeps the term planner's docs-only and window-fill routes.
  Query* rewritten;
  if (fields.size() == 1) {
    rewritten = context.pool.make<TermQuery>(fields[0], term);
  } else {
    auto clauses = context.pool.make_span<Query*>(fields.size());
    for (size_t i = 0; i < fields.size(); i++) {
      clauses[i] = context.pool.make<TermQuery>(fields[i], term);
    }
    rewritten = context.pool.make<BooleanQuery>(
        std::span<Query*>{}, clauses, std::span<Query*>{}, std::span<Query*>{});
  }
#ifndef NDEBUG
  if (context.logicalValidationActive()) {
    rewritten->validateLogical(context.planningContext(), multiplier);
  }
#endif
  return rewritten->createWeight(context, flags, multiplier);
}

} // namespace luxir
