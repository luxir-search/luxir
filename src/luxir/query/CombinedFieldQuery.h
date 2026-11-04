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

// Norm tables for one field weight vector, shared by the request's
// CombinedFieldQuery weights. Per field, 256 entries each: weighted decoded
// lengths, and their encoded norms for a segment where that field alone has
// norms. A two-field vector adds the pair table of
// CombinedFieldQuery::fillPairNorms once its weights' estimated pair
// evaluations reach the build threshold.
struct CombinedNormTables {
  std::span<const float> weights;
  const double* lengths;
  const uint8_t* singleNorms;
  const uint8_t* pairNorms;
  int64_t pairEvaluations;
  CombinedNormTables* next;
};

// BM25F over (field, term) entries, scored as if the fields were a
// single combined field. One field with several terms is a synonym query.
// Each field's weight multiplies its term frequency and its document length,
// as in Lucene's CombinedFieldQuery. Term weights affect only frequency:
//
//   freq(d)   = sum_entries w_f * w_term * tf_entry(d)
//   length(d) = sum_f w_f * length_f(d), rounded and re-encoded like a norm
//   avgdl     = sum_f w_f * sumTotalTermFreq_f / max_f docsWithField_f
//   idf       = BM25 idf from max_entries docFreq and max_f docsWithField_f
//   term stats: docFreq = max_entries docFreq, totalTermFreq = sum_entries ttf
//
// Each distinct field is listed once, independently of the term entries.
// Fields must all be TEXT or all be non-TEXT: combined multi_match groups
// contain only TEXT, and case-insensitive synonym queries contain one field.
// Non-TEXT queries use neutral length and never read document norms.
// Every listed field contributes its length to a document even when the
// term is absent from that field, so a long body still dampens a title hit.
// Positive field weights and term weights in (0, 1] retain the BM25 score bound.
//
// Membership is the union of the per-field postings, so an unscored use
// rewrites to a disjunction of TermQuerys. Its own filter key identifies the
// field/term entries, independently of the scoring weights.
class CombinedFieldQuery final : public Query {
public:
  struct Term {
    size_t field;
    std::string_view text;
    float weight = 1.0f;

    bool operator==(const Term& rhs) const {
      return field == rhs.field && text == rhs.text
          && std::bit_cast<uint32_t>(weight) == std::bit_cast<uint32_t>(rhs.weight);
    }
  };

private:
  std::span<const std::string_view> fields;
  std::span<const float> weights;
  std::span<const Term> terms;

public:
  CombinedFieldQuery(std::span<const std::string_view> fields,
                     std::span<const float> weights, std::span<const Term> terms)
    : Query(QueryKind::COMBINED_FIELDS), fields(fields), weights(weights), terms(terms) {
    assert(!fields.empty() && fields.size() == weights.size() && !terms.empty());
    for (const auto& term : terms) {
      assert(term.field < fields.size() && term.weight > 0.0f && term.weight <= 1.0f);
    }
    for (float weight : weights) assert(weight > 0.0f && std::isfinite(weight));
  }

  // Estimated pair-table evaluations that amortize building it: the 64K
  // encodings cost about as much as this many summed-length evaluations.
  static constexpr int64_t PAIR_TABLE_MIN_EVALUATIONS = 50000;
  static inline int64_t pairTableMinEvaluationsForTests = PAIR_TABLE_MIN_EVALUATIONS;

  std::span<const std::string_view> getFields() const { return fields; }
  std::span<const float> getWeights() const { return weights; }
  std::span<const Term> getTerms() const { return terms; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const CombinedFieldQuery&>(other);
    if (fields.size() != rhs.fields.size() || !std::ranges::equal(terms, rhs.terms)) return false;
    for (size_t i = 0; i < fields.size(); i++) {
      if (fields[i] != rhs.fields[i]
          || std::bit_cast<uint32_t>(weights[i]) != std::bit_cast<uint32_t>(rhs.weights[i])) {
        return false;
      }
    }
    return true;
  }

  uint64_t hashImpl() const override {
    uint64_t value = Query::hashImpl();
    for (size_t i = 0; i < fields.size(); i++) {
      value = mixHash(value, fields[i]);
      value = mixHash(value, std::bit_cast<uint32_t>(weights[i]));
    }
    for (const auto& term : terms) {
      value = mixHash(value, term.field);
      value = mixHash(value, term.text);
      value = mixHash(value, std::bit_cast<uint32_t>(term.weight));
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
    out.appendInt32((int32_t)terms.size());
    for (const auto& term : terms) {
      out.appendInt32((int32_t)term.field);
      out.appendTerm(term.text);
    }
    return FilterKeyScope::SEGMENT_STABLE;
  }

  // The combined length rounded half away from zero, clamped to int32 and
  // encoded like a norm. Bit-identical to std::round for the non-negative,
  // finite lengths a scorer produces, without the libm call.
  static uint8_t encodeLength(double length) {
    constexpr int32_t maxLength = std::numeric_limits<int32_t>::max();
    if (!(length < (double)maxLength)) return SmallFloat::intToByte4(maxLength);
    auto whole = (int64_t)length;
    if (length - (double)whole >= 0.5) whole++;
    return SmallFloat::intToByte4((int32_t)whole);
  }

  // Final norms of a two-field document, indexed by code0 << 8 | code1.
  static void fillPairNorms(std::span<uint8_t> out, float weight0, float weight1) {
    assert(out.size() == 256 * 256);
    double lengths0[256], lengths1[256];
    for (size_t code = 0; code < 256; code++) {
      lengths0[code] = (double)weight0 * (double)SmallFloat::decodeLengthByte((uint8_t)code);
      lengths1[code] = (double)weight1 * (double)SmallFloat::decodeLengthByte((uint8_t)code);
    }
    for (size_t code0 = 0; code0 < 256; code0++) {
      for (size_t code1 = 0; code1 < 256; code1++) {
        out[code0 << 8 | code1] = encodeLength(lengths0[code0] + lengths1[code1]);
      }
    }
  }

  class Scorer final : public Query::Scorer {
  public:
    struct Postings {
      DocsFreqEnum* docs;
      float weight;
    };
    // lengths[code] is the field weight times the decoded field length.
    struct Norms {
      const double* lengths;
      const uint8_t* flat = nullptr;
      NormsReader::Iterator* sparse = nullptr;
    };

  private:
    std::span<Postings> postings;
    std::span<Norms> norms;
    Similarity::BM25Scorer* simScorer;
    // Set when one field has norms in this segment: maps its code to the final norm.
    const uint8_t* singleNorms;
    // Set when both fields of a two-field query have norms; see fillPairNorms.
    const uint8_t* pairNorms;
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

    // A document without the field reads as code 0, which decodes to length 0.
    uint8_t code(Norms& n) {
      if (n.flat != nullptr) return n.flat[doc];
      if (n.sparse->docId() < doc) n.sparse->advance(doc);
      return n.sparse->docId() == doc ? n.sparse->value() : 0;
    }

    // Sums in field order, as the reference sum_f w_f * length_f(d) does. A
    // missing field adds +0.0, which leaves the non-negative sum unchanged.
    int64_t combinedNorm() {
      if (pairNorms != nullptr) return pairNorms[(size_t)code(norms[0]) << 8 | code(norms[1])];
      if (singleNorms != nullptr) return singleNorms[code(norms[0])];
      double length = 0.0;
      for (auto& n : norms) length += n.lengths[code(n)];
      return encodeLength(length);
    }

  public:
    Scorer(std::span<Postings> postings, std::span<Norms> norms,
           Similarity::BM25Scorer* simScorer, const uint8_t* singleNorms,
           const uint8_t* pairNorms)
      : postings(postings), norms(norms), simScorer(simScorer),
        singleNorms(singleNorms), pairNorms(pairNorms) {
      assert(singleNorms == nullptr || norms.size() == 1);
      assert(pairNorms == nullptr || norms.size() == 2);
    }

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
    Similarity::TermStats termStats;
    Similarity::BM25Scorer* simScorer = nullptr;
    CombinedNormTables* normTables = nullptr;
    bool hasNorms = false;

    // Finds or builds the tables for this field weight vector. A two-field
    // vector builds the 64K-entry pair table once the estimated evaluations of
    // all its weights amortize it. Construction is serial, so every weight,
    // including earlier ones, reads the table when it creates scorers.
    void useNormTables(Context& context) {
      auto bits = [](float weight) { return std::bit_cast<uint32_t>(weight); };
      auto sameWeights = [&](std::span<const float> weights) {
        return std::ranges::equal(weights, query.weights, {}, bits, bits);
      };
      for (auto* tables = context.combinedNormTables; tables != nullptr; tables = tables->next) {
        if (sameWeights(tables->weights)) {
          normTables = tables;
          break;
        }
      }
      if (normTables == nullptr) {
        size_t n = query.weights.size();
        auto lengths = context.pool.make_span<double>(n * 256);
        auto singleNorms = context.pool.make_span<uint8_t>(n * 256);
        for (size_t i = 0; i < n; i++) {
          for (size_t code = 0; code < 256; code++) {
            double length = (double)query.weights[i] * (double)SmallFloat::decodeLengthByte((uint8_t)code);
            lengths[i * 256 + code] = length;
            singleNorms[i * 256 + code] = encodeLength(length);
          }
        }
        normTables = context.pool.make<CombinedNormTables>(
            query.weights, lengths.data(), singleNorms.data(), nullptr, 0, context.combinedNormTables);
        context.combinedNormTables = normTables;
      }
      if (query.weights.size() != 2 || normTables->pairNorms != nullptr) return;
      int64_t threshold = pairTableMinEvaluationsForTests;
      normTables->pairEvaluations += pairEvaluations(context, threshold - normTables->pairEvaluations);
      if (normTables->pairEvaluations >= threshold) {
        auto pairNorms = context.pool.make_span<uint8_t>(256 * 256);
        fillPairNorms(pairNorms, query.weights[0], query.weights[1]);
        normTables->pairNorms = pairNorms.data();
      }
    }

    // Norm evaluations this weight may make through a pair table: in each
    // segment where both fields have norms, its matches capped at maxDoc.
    // Counts no further than limit.
    int64_t pairEvaluations(Context& context, int64_t limit) const {
      auto fieldHasNorms = [&](size_t field, const IndexReader::Segment& segment) {
        if (fieldInfos[field] == nullptr) return false;
        const SegFieldInfo* segInfo = fieldInfos[field]->segInfos.at(segment);
        return segInfo != nullptr && segInfo->normsFormat != SegFieldInfo::NORMS_NONE
            && segInfo->normsLen > 0;
      };
      int64_t evaluations = 0;
      for (auto& segment : context.topReader.segments()) {
        if (evaluations >= limit) break;
        if (fieldHasNorms(0, segment) && fieldHasNorms(1, segment)) {
          evaluations += std::min<int64_t>(segment.maxDoc(), segmentCost(segment));
        }
      }
      return evaluations;
    }

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
      termInfos = context.pool.make_span<CachedTermInfo*>(query.terms.size());
      int64_t docCount = 0;
      double sumTotalTermFreq = 0.0;
      bool anyTerm = false;
      std::optional<bool> fieldHasNorms;
      for (size_t i = 0; i < n; i++) {
        fieldInfos[i] = context.getCachedFieldInfo(query.fields[i]);
        if (fieldInfos[i] == nullptr) continue;
        // Use segment metadata, as in TermQuery; older fields need not be
        // listed in the current schema. Query builders keep these modes apart.
        const auto& segInfos = fieldInfos[i]->segInfos;
        for (size_t seg = 0; seg < segInfos.size(); seg++) {
          if (const auto* info = segInfos.at(seg)) {
            bool text = info->type == FieldType::TEXT;
            assert(!fieldHasNorms || *fieldHasNorms == text);
            fieldHasNorms = text;
            break;
          }
        }
        const auto& stats = fieldInfos[i]->fieldStats;
        docCount = std::max(docCount, stats.docsWithField);
        sumTotalTermFreq += (double)query.weights[i] * (double)stats.sumTotalTermFreq;
      }
      hasNorms = fieldHasNorms.value_or(false);
      for (size_t i = 0; i < query.terms.size(); i++) {
        const auto& term = query.terms[i];
        auto* fieldInfo = fieldInfos[term.field];
        termInfos[i] = fieldInfo == nullptr ? nullptr
            : context.getCachedTerminfo(*fieldInfo, term.text);
        if (termInfos[i] != nullptr) {
          anyTerm = true;
          const auto& stats = termInfos[i]->termStats;
          termStats.docFreq = std::max(termStats.docFreq, stats.docFreq);
          termStats.totalTermFreq += stats.totalTermFreq;
        }
      }
      if (anyTerm && docCount > 0) {
        // Fields of a group share one parameter set; the builder groups by it.
        Bm25Params params;
        for (auto* info : fieldInfos) {
          if (info != nullptr) { params = info->bm25; break; }
        }
        Similarity similarity(params);
        float idf = similarity.idf(termStats.docFreq, docCount);
        float avgdl = (float)(sumTotalTermFreq / (double)docCount);
        // Keep membership even with empty statistics or float overflow from
        // finite field weights. BM25 needs a finite, positive average length.
        if (!(avgdl > 0.0f) || !std::isfinite(avgdl)) avgdl = 1.0f;
        simScorer = context.pool.make<Similarity::BM25Scorer>(
            multiplier, similarity.k1, similarity.b, idf, avgdl, hasNorms);
      }
      if (simScorer != nullptr && hasNorms) useNormTables(context);
    }

    const Similarity::TermStats& getTermStats() const { return termStats; }

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
      auto postings = targetPool.make_span<Scorer::Postings>(query.terms.size());
      for (size_t i = 0; i < query.terms.size(); i++) {
        if (termInfos[i] == nullptr) continue;
        auto* docs = termInfos[i]->useDocsEnum<DocsEnumTier::FREQS>(targetPool, segment);
        if (docs != nullptr) {
          postings[numPostings++] = {docs, query.weights[query.terms[i].field] * query.terms[i].weight};
        }
      }
      auto norms = targetPool.make_span<Scorer::Norms>(hasNorms ? n : 0);
      size_t lastNormsField = 0;
      for (size_t i = 0; hasNorms && i < n; i++) {
        if (fieldInfos[i] == nullptr) continue;
        const SegFieldInfo* segInfo = fieldInfos[i]->segInfos.at(segment);
        if (segInfo == nullptr) continue;
        assert(segInfo->type == FieldType::TEXT);
        if (segInfo->normsFormat == SegFieldInfo::NORMS_NONE) continue;
        auto* reader = targetPool.make<NormsReader>(segment.postingsReader(), *segInfo);
        if (reader->numBytes() == 0) continue;
        lastNormsField = i;
        const double* lengths = normTables->lengths + i * 256;
        if (reader->isFlat()) {
          norms[numNorms++] = {.lengths = lengths, .flat = reader->flatBase()};
        } else {
          norms[numNorms++] = {.lengths = lengths, .sparse = targetPool.make<NormsReader::Iterator>(*reader)};
        }
      }
      if (numPostings == 0) return nullptr;
      // Only a two-field query has a pair table, so two norms are fields 0 and 1.
      const uint8_t* singleNorms = numNorms == 1 ? normTables->singleNorms + lastNormsField * 256 : nullptr;
      const uint8_t* pairNorms = numNorms == 2 ? normTables->pairNorms : nullptr;
      return targetPool.make<Scorer>(postings.first(numPostings), norms.first(numNorms), simScorer,
                                     singleNorms, pairNorms);
    }
  };

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;

  bool recoverableAsPredicate() const noexcept override { return true; }
};

static_assert(std::is_trivially_destructible_v<CombinedFieldQuery::Scorer>);

} // namespace luxir
