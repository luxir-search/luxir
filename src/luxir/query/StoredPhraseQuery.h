// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "Query.h"
#include "QueryPrep.h"
#include "luxir/analysis/Analyzer.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/schema/FieldType.h"

namespace luxir {

// A phrase on a text field indexed without positions (index_options docs or
// freqs), verified against the field's stored source, like Elasticsearch's
// match_only_text. The conjunction of the phrase's terms supplies candidates
// through postings. Verification is the second phase of a two-phase scorer:
// it runs only on candidates that every cheaper clause has accepted, reads
// the stored value(s), re-analyzes them with the field's analyzer, and
// checks the term sequence with the same slop and multi-valued position gap
// as an indexed phrase. A match scores as the conjunction of its terms;
// positions add no phrase frequency. Segments without the stored source
// cannot be verified and match nothing.
class StoredPhraseQuery final : public Query {
  Query* conjunction;
  TextFieldType* fieldType;
  std::string_view field;
  std::string_view storedField;
  std::string_view storedResource;
  std::span<std::string_view> terms;
  std::span<const int32_t> positions;
  int32_t slop;

public:
  // Relative to postings work: a stored read decompresses a chunk and
  // re-analyzes the value, so cheaper verifications run first.
  static constexpr float VERIFY_MATCH_COST = 1000.0f;

  // The stored-source phrase check for one segment. It owns a stored-fields
  // reader and an analyzer chain, so it lives in the request arena.
  class Verifier {
    const StoredPhraseQuery& query;
    std::unique_ptr<StoredFieldsReader> reader;
    int32_t fid;
    std::unique_ptr<TokenChain> chain;
    std::vector<std::string_view> distinct;           // distinct phrase terms
    std::vector<std::vector<int32_t>> slotsByTerm;    // slots per distinct term, by position
    std::vector<std::vector<int32_t>> termPositions;  // per distinct term, in the current doc
    std::vector<int64_t> starts;

    // Can every slot take a position of its term within [start, start + slop],
    // with slots of a repeated term on distinct positions? Each slot's window
    // has the same width, so taking the earliest free position slot by slot
    // in query-position order finds an assignment whenever one exists.
    bool fits(int64_t start) const {
      for (size_t t = 0; t < distinct.size(); t++) {
        const auto& docPositions = termPositions[t];
        size_t next = 0;
        for (int32_t slot : slotsByTerm[t]) {
          int64_t lo = start + query.positions[(size_t) slot];
          while (next < docPositions.size() && docPositions[next] < lo) next++;
          if (next == docPositions.size() || docPositions[next] > lo + query.slop) return false;
          next++;
        }
      }
      return true;
    }

    void analyze(std::span<const std::string_view> values, int64_t& pos, bool& first) {
      Token& tok = chain->head.getToken();
      TokenStream& tail = *chain->tail;
      PackedTerm::TermBuffer scratch;
      for (std::string_view value : values) {
        if (!first) pos += TextFieldType::POSITION_INCREMENT_GAP;
        first = false;
        chain->head.setValue(value);
        chain->reset();
        while (tail.incrementToken()) {
          pos += tok.positionIncrement;
          auto term = PackedTerm::fitTerm(query.fieldType->longTerms, tok.text, scratch);
          if (!term) continue;
          for (size_t t = 0; t < distinct.size(); t++) {
            if (*term != distinct[t]) continue;
            auto& docPositions = termPositions[t];
            if (docPositions.empty() || docPositions.back() != (int32_t) pos) {
              docPositions.push_back((int32_t) pos);
            }
            break;
          }
        }
      }
    }

  public:
    static inline int64_t matchCallsForTests = 0;

    Verifier(const StoredPhraseQuery& query, std::unique_ptr<StoredFieldsReader> reader, int32_t fid)
      : query(query), reader(std::move(reader)), fid(fid),
        chain(query.fieldType->createAnalyzer(query.field)) {
      for (size_t i = 0; i < query.terms.size(); i++) {
        auto it = std::find(distinct.begin(), distinct.end(), query.terms[i]);
        size_t t = (size_t) (it - distinct.begin());
        if (it == distinct.end()) {
          distinct.push_back(query.terms[i]);
          slotsByTerm.emplace_back();
        }
        slotsByTerm[t].push_back((int32_t) i);
      }
      for (auto& slots : slotsByTerm) {
        std::stable_sort(slots.begin(), slots.end(), [&](int32_t a, int32_t b) {
          return query.positions[(size_t) a] < query.positions[(size_t) b];
        });
      }
      termPositions.resize(distinct.size());
    }

    // Null when this segment has no stored source for the field.
    static Verifier* open(google::protobuf::Arena& arena, const StoredPhraseQuery& query,
                          IndexReader::Segment& segment) {
      auto* resource = segment.postingsReader().fieldInfo(query.storedResource);
      if (resource == nullptr) return nullptr;
      auto reader = std::make_unique<StoredFieldsReader>(segment.postingsReader(), *resource);
      int32_t fid = reader->fieldId(query.storedField);
      if (fid < 0) return nullptr;
      return luxir::arenaCreate<Verifier>(arena, query, std::move(reader), fid);
    }

    bool matches(int32_t doc) {
      matchCallsForTests++;
      for (auto& docPositions : termPositions) docPositions.clear();
      int64_t pos = -1;
      bool first = true;
      reader->readFieldById(doc, fid, [&](std::span<const std::string_view> values) {
        analyze(values, pos, first);
      });
      starts.clear();
      for (size_t t = 0; t < distinct.size(); t++) {
        if (termPositions[t].empty()) return false;
        for (int32_t slot : slotsByTerm[t]) {
          for (int32_t p : termPositions[t]) {
            starts.push_back((int64_t) p - query.positions[(size_t) slot]);
          }
        }
      }
      // A satisfying window can always start at the smallest rebased position
      // it uses, so trying each rebased position as a start is exhaustive.
      std::sort(starts.begin(), starts.end());
      starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
      for (int64_t start : starts) {
        if (fits(start)) return true;
      }
      return false;
    }
  };

private:
  // Approximation: the conjunction's matches. Verification: the stored
  // source. Iteration, scores and score bounds come from the conjunction;
  // bounds stay valid because verification only removes docs.
  class Scorer final : public Query::Scorer {
    Query::Scorer* child;
    Verifier* verifier;
    int32_t checkedDoc = -1;
    bool checkedMatch = false;

    bool verify(int32_t doc) {
      if (doc == checkedDoc) return checkedMatch;
      checkedDoc = doc;
      checkedMatch = doc != PostingsReader::END && verifier->matches(doc);
      return checkedMatch;
    }

    int32_t nextVerified(int32_t doc) {
      while (doc != PostingsReader::END && !verify(doc)) doc = child->next();
      return doc;
    }

  public:
    Scorer(Query::Scorer* child, Verifier* verifier) : child(child), verifier(verifier) {}

    int32_t next() override { return nextVerified(child->next()); }
    int32_t advance(int32_t target) override { return nextVerified(child->advance(target)); }
    int32_t docId() override { return child->docId(); }

    int32_t approximationNext() override { return child->next(); }
    int32_t approximationAdvance(int32_t target) override { return child->advance(target); }
    int32_t approximationDocId() override { return child->docId(); }
    bool matches() override { return verify(child->docId()); }
    float matchCost() override { return child->matchCost() + VERIFY_MATCH_COST; }

    int32_t advanceWindow(int32_t min, int32_t max) override {
      if (min >= max) return max;
      int32_t doc = child->advanceWindow(min, max);
      while (doc < max) {
        if (verify(doc)) return doc;
        doc = child->advanceWindow(doc + 1, max);
      }
      return doc;
    }

    int32_t advanceApproximationWindow(int32_t min, int32_t max) override {
      return child->advanceWindow(min, max);
    }

    [[gnu::cold]] void prepareMembership() override { child->prepareMembership(); }

    [[gnu::cold]] void reset() override {
      child->reset();
      checkedDoc = -1;
      checkedMatch = false;
    }

    float score() override { return child->score(); }
    void setMinCompetitiveScore(float minScore) override { child->setMinCompetitiveScore(minScore); }
    float getMaxScore(int32_t upTo) override { return child->getMaxScore(upTo); }
    float refineMaxScore(int32_t upTo) override { return child->refineMaxScore(upTo); }
    ScoreBounds getScoreBounds(int32_t upTo) override { return child->getScoreBounds(upTo); }
    ScoreBounds refineScoreBounds(int32_t upTo) override { return child->refineScoreBounds(upTo); }
    int32_t advanceShallow(int32_t target) override { return child->advanceShallow(target); }
  };

  class Supplier final : public Query::ScorerSupplier {
    Query::ScorerSupplier* childSupplier;
    Verifier* verifier;

    static Query::ScorerShape wrappedShape(const Query::ScorerShape& child) {
      return {
        .matchState = child.matchState,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = Query::ReportedTwoPhase::YES,
        .windowFillClause = Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    class Plan final : public Query::ScorerPlan {
      Query::ScorerPlan* childPlan;
      Verifier* verifier;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        Query::Scorer* childScorer = childPlan->build(targetPool);
        if (childScorer == nullptr) return nullptr;
        return targetPool.make<StoredPhraseQuery::Scorer>(childScorer, verifier);
      }

    public:
      [[gnu::cold]] bool supportsReset() const noexcept override {
        return childPlan->supportsReset();
      }

      Plan(const Query::PlanContext& planContext, const Query::ScorerShape& shape,
           int64_t cost, Query::ScorerPlan* childPlan, Verifier* verifier)
        : Query::ScorerPlan(planContext, shape, cost),
          childPlan(childPlan), verifier(verifier) {}
    };

  public:
    Supplier(Query::ScorerSupplier* childSupplier, Verifier* verifier)
      : childSupplier(childSupplier), verifier(verifier) {}

    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(const Query::PlanContext& buildContext) const override {
      return wrappedShape(childSupplier->describeScorer(buildContext));
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::PlanContext& buildContext) const override {
      return childSupplier->unresolvedScorerCause(buildContext);
    }

    bool fillExpansionMemo(const Query::PlanContext& buildContext) override {
      return childSupplier->fillExpansionMemo(buildContext);
    }

    Query::ScorerPlan* resolve(MemPool& planPool, const Query::PlanContext& planContext) override {
      Query::ScorerPlan* childPlan = childSupplier->resolve(planPool, planContext);
      assert(childPlan != nullptr);
      return planPool.make<Plan>(planContext, wrappedShape(childPlan->shape()),
                                 childPlan->cost(), childPlan, verifier);
    }

    Query::PlanContext makePlanContext(const Query::Demand& demand) const override {
      return childSupplier->makePlanContext(demand);
    }
  };

  static Query::ScorerSupplier* wrapSupplier(
      MemPool& targetPool, IndexReader::Segment& segment, Query::SegmentSource& child,
      const StoredPhraseQuery& query, google::protobuf::Arena& arena,
      Query::SupplierExecutionMode executionMode) {
    auto* childSupplier = child.scorerSupplier(targetPool, segment, executionMode);
    if (childSupplier == nullptr) return nullptr;
    Verifier* verifier = Verifier::open(arena, query, segment);
    if (verifier == nullptr) return nullptr;
    return targetPool.make<Supplier>(childSupplier, verifier);
  }

  static Query::Scorer* buildScorer(MemPool& targetPool, Query::ScorerSupplier* supplier) {
    if (supplier == nullptr) return nullptr;
    Query::Demand demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
    return supplier->resolve(targetPool, supplier->makePlanContext(demand))->build(targetPool);
  }

public:
  StoredPhraseQuery(Query* conjunction, TextFieldType* fieldType, std::string_view field,
                    std::string_view storedField, std::string_view storedResource,
                    std::span<std::string_view> terms, std::span<const int32_t> positions,
                    int32_t slop)
    : Query(QueryKind::STORED_PHRASE), conjunction(conjunction), fieldType(fieldType),
      field(field), storedField(storedField), storedResource(storedResource),
      terms(terms), positions(positions), slop(slop) {
    assert(terms.size() == positions.size());
    assert(terms.size() >= 2);
    assert(slop >= 0);
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const StoredPhraseQuery&>(other);
    return field == rhs.field && slop == rhs.slop
        && storedField == rhs.storedField && storedResource == rhs.storedResource
        && std::ranges::equal(terms, rhs.terms)
        && std::ranges::equal(positions, rhs.positions);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, slop);
    value = mixSampledSequence(value, terms, [](uint64_t seed, std::string_view term) {
      return mixHash(seed, term);
    });
    return mixSampledSequence(value, positions, [](uint64_t seed, int32_t position) {
      return mixHash(seed, position);
    });
  }

  [[nodiscard]] std::string_view getField() const { return field; }
  [[nodiscard]] Query* getConjunction() const { return conjunction; }
  [[nodiscard]] int32_t getSlop() const { return slop; }

  void validateLogicalImpl(PlanningContext& context, float multiplier = 1.0f) const override {
    conjunction->validateLogical(context, multiplier);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendKind(kind);
    out.appendString(field);
    out.appendString(storedField);
    out.appendString(storedResource);
    out.appendSize(terms.size());
    for (std::string_view term : terms) out.appendTerm(term);
    out.appendSize(positions.size());
    for (int32_t position : positions) out.appendInt32(position);
    out.appendInt32(slop);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  Weight* createWeight(Context& context, int32_t flags, float multiplier = 1.0f) override;

  class Weight final : public Query::Weight {
    StoredPhraseQuery& query;
    Query::Weight* childWeight;
    google::protobuf::Arena& arena;

    class Prepared final : public Query::Weight::PreparedWeight {
      QueryPrep::PreparedSource child;
      StoredPhraseQuery& query;
      google::protobuf::Arena& arena;

    public:
      Prepared(QueryPrep::PreparedSource&& child, StoredPhraseQuery& query,
               google::protobuf::Arena& arena)
        : child(std::move(child)), query(query), arena(arena) {}

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return buildScorer(targetPool, scorerSupplierImpl(
            targetPool, segment, Query::SupplierExecutionMode::ORDINARY));
      }

      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& targetPool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        return wrapSupplier(targetPool, segment, child.segmentSource(), query, arena,
                            executionMode);
      }

      void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
        child.releaseSegmentState(segment);
      }

      PreparedDomainDependence domainDependence() const noexcept override {
        return child.domainDependence;
      }
    };

  public:
    Weight(Context& context, StoredPhraseQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, query, flags), query(query), arena(context.arena()) {
      childWeight = query.conjunction->createWeight(context, flags, multiplier);
      traits |= childWeight->getFlags() & (NEEDS_PREPARE | IS_CONSTANT_SCORING);
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(
        Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.setPrepared(childWeight->prepare(ctx));
      }
      return std::make_unique<Prepared>(std::move(source), query, arena);
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      return buildScorer(targetPool, scorerSupplier(targetPool, segment));
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      return wrapSupplier(targetPool, segment, *childWeight, query, arena, executionMode);
    }

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      childWeight->releaseSegmentState(segment);
    }

    bool hasReleasableSegmentState() const noexcept override {
      return childWeight->hasReleasableSegmentState();
    }
  };
};

} // namespace luxir
