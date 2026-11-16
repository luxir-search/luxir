// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include "MinScoreQuery.h"
#include "Query.h"
#include "QueryPrep.h"
#include "luxir/reader/OwnerMap.h"
#include "luxir/reader/SkipStats.h"

namespace luxir {

/// Children to owner: matches the owner rows of the elements of nested `path`
/// that match `child`. The child query is planned and executed in the path's
/// own row space (IndexReader::scope), so every clause of it evaluates on one
/// element. Element rows are stored in owner order, so child matches arrive
/// grouped by owner and the scorer reduces each run onto its owner as it
/// streams: no structure sized by rows or owners is built per request.
///
/// Scores: max (default), min, sum, avg of the matching elements' scores, or
/// none (0). min_children / max_children bound the number of matching
/// elements. With max and no count bound, the collector floor passes down to
/// the child: an owner cannot beat its best element.
///
/// Liveness: scorers do not filter deletes. Elements of a deleted owner are
/// dead with it, and its (deleted) owner row is dropped by the consumer's
/// domain like any other deleted row.
class ToParentQuery final : public Query {
public:
  enum class ScoreMode : uint8_t { MAX, MIN, SUM, AVG, NONE };

private:
  std::string_view path;
  std::string_view name;
  Query* child;
  ScoreMode mode;
  int32_t minChildren;
  int32_t maxChildren;

  bool countBounded() const { return minChildren > 1 || maxChildren < std::numeric_limits<int32_t>::max(); }

  class Scorer final : public Query::Scorer {
  public:
    // Candidate batches (intersectCandidateBits): owners per batch, and the
    // element rows one batch spans.
    static constexpr int32_t kBatchOwners = 256;
    static constexpr int32_t kBatchElementWords = 256;

  private:
    Query::Scorer* child;
    Query::MembershipProof* proof;  // two-phase: element rows the child cannot match
    const OwnerMap& owners;
    OwnerMap::OwnerCursor ownerOf;
    ScoreMode mode;
    int32_t minChildren;
    int32_t maxChildren;
    float multiplier;
    bool scored;
    bool prune;
    bool twoPhase;           // see verify()
    bool firstMatchDecides = false;  // see verify()
    bool exhausted = false;  // a constant score fell below the collector floor
    int32_t doc = -1;
    int32_t pending = -1;  // child's current doc not yet reduced (-1: not started)
    int32_t verified = -1;  // the owner verify() last decided
    int32_t docEnd = 0;     // two-phase: the end of doc's element rows
    bool verifiedMatch = false;
    float current = 0.0f;
    std::optional<float> constant;  // the score of every match, when uniform
    ScoreBounds bounds;
    std::span<uint64_t> elementBits;  // two-phase: one candidate batch's element rows

    // The owner score as reduce() computes it from an element score `s`
    // shared by all of the owner's matching elements.
    float ownerScore(float s) const { return (float)((double)s * multiplier); }

    // max, min and avg of an owner's matching element scores lie within the
    // element bounds, so they bound the owner's score too (sum does not).
    void initBounds() {
      if (!scored) {
        constant = 0.0f;
        bounds = ScoreBounds::exact(0.0f);
        return;
      }
      if (mode == ScoreMode::SUM || !(multiplier >= 0.0f)) return;
      if (auto c = child->constantContribution()) {
        constant = ownerScore(*c);
        bounds = ScoreBounds::exact(*constant);
        return;
      }
      ScoreBounds b = child->getScoreBounds(PostingsReader::END);
      bounds.hi = std::min(b.hi, child->getMaxScore(PostingsReader::END));
      bounds.lo = b.lo;
      if (std::isfinite(bounds.hi)) bounds.hi = ownerScore(bounds.hi);
      if (std::isfinite(bounds.lo)) bounds.lo = ownerScore(bounds.lo);
    }

    // One owner's reduction over its matching elements.
    struct Run {
      int32_t count = 0;
      double acc = 0.0;
    };

    Run startRun() const {
      return {0, mode == ScoreMode::MIN ? std::numeric_limits<double>::infinity()
                 : mode == ScoreMode::MAX ? -std::numeric_limits<double>::infinity() : 0.0};
    }

    void addMatch(Run& run) {
      if (scored) {
        float s = child->score();
        switch (mode) {
          case ScoreMode::MAX: run.acc = std::max(run.acc, (double)s); break;
          case ScoreMode::MIN: run.acc = std::min(run.acc, (double)s); break;
          case ScoreMode::SUM: case ScoreMode::AVG: run.acc += s; break;
          case ScoreMode::NONE: break;
        }
      }
      run.count++;
    }

    // Whether the run admits its owner; sets the owner's score when it does.
    bool finishRun(const Run& run) {
      if (run.count < minChildren || run.count > maxChildren) return false;
      double value = 0.0;
      if (scored) {
        value = mode == ScoreMode::AVG ? run.acc / run.count : mode == ScoreMode::NONE ? 0.0 : run.acc;
      }
      current = (float)(value * multiplier);
      return true;
    }

    int32_t reduce(int32_t c) {
      while (c != PostingsReader::END) {
        int32_t owner = ownerOf.ownerOf(c);
        Run run = startRun();
        int32_t end = owners.begin(owner + 1);
        while (c != PostingsReader::END && c < end) {
          addMatch(run);
          c = child->next();
        }
        pending = c;
        if (finishRun(run)) return doc = owner;
      }
      pending = PostingsReader::END;
      return doc = PostingsReader::END;
    }

    // Two-phase (a two-phase child, chosen when a sparser lead drives this
    // clause): the approximation is the owner of the child's next
    // approximation element, and verification confirms only that owner's
    // elements, so a rejected owner never scans the elements after it. The
    // child is driven only through its own two-phase protocol. A membership
    // proof from the child (column zones) skips elements no value of which
    // can match, so an owner whose elements all lie there is never verified
    // and the approximation lands past such runs of owners at once.
    int32_t approximate(int32_t target) {
      if (exhausted || target >= owners.ownerMaxDoc()) return doc = PostingsReader::END;
      // Point lookups: the candidates of a sparse lead jump, which a bulk
      // cursor pays for with a frame decode each.
      auto [begin, end] = owners.range(target);
      for (;;) {
        if (proof != nullptr) {
          int32_t possible = proof->nextPossible(begin);
          if (possible == PostingsReader::END) return doc = PostingsReader::END;
          if (possible >= end) {
            target = owners.owner(possible);
            std::tie(begin, end) = owners.range(target);
          }
          begin = possible;
        }
        int32_t c = child->approximationDocId();
        if (c < begin) c = child->approximationAdvance(begin);
        if (c == PostingsReader::END) return doc = PostingsReader::END;
        if (c >= end) {
          target = owners.owner(c);
          std::tie(begin, end) = owners.range(target);
          begin = c;
          if (proof != nullptr) continue;
        }
        docEnd = end;
        return doc = target;
      }
    }

    // When neither the count bounds nor the score depend on how many
    // elements match, the first matching element decides the owner.
    bool verify() {
      if (verified == doc) return verifiedMatch;
      verified = doc;
      int32_t end = docEnd;
      Run run = startRun();
      for (int32_t c = child->approximationDocId(); c < end; c = child->approximationNext()) {
        if (child->matches()) {
          addMatch(run);
          if (firstMatchDecides) break;
        }
      }
      return verifiedMatch = finishRun(run);
    }

    // Whether the matching element count admits an owner, by count alone.
    bool admits(int32_t count) const { return count >= minChildren && count <= maxChildren; }

    // One owner's element rows [begin, end) counted through the child's
    // two-phase protocol; stops once more matches cannot change the answer.
    int32_t countMatching(int32_t begin, int32_t end) {
      if (proof != nullptr && proof->nextPossible(begin) >= end) return 0;
      int32_t c = child->approximationDocId();
      if (c < begin) c = child->approximationAdvance(begin);
      int32_t count = 0;
      for (; c < end; c = child->approximationNext()) {
        if (!child->matches()) continue;
        count++;
        if (count >= minChildren && maxChildren == std::numeric_limits<int32_t>::max()) break;
      }
      return count;
    }

    static void setRange(std::span<uint64_t> words, int32_t begin, int32_t end) {
      for (int32_t i = begin; i < end;) {
        int32_t bit = i & 63;
        int32_t n = std::min(64 - bit, end - i);
        uint64_t mask = n == 64 ? ~0ULL : ((1ULL << n) - 1) << bit;
        words[(size_t)(i >> 6)] |= mask;
        i += n;
      }
    }

    static int32_t countRange(std::span<const uint64_t> words, int32_t begin, int32_t end) {
      int32_t count = 0;
      for (int32_t i = begin; i < end;) {
        int32_t bit = i & 63;
        int32_t n = std::min(64 - bit, end - i);
        uint64_t mask = n == 64 ? ~0ULL : ((1ULL << n) - 1) << bit;
        count += std::popcount(words[(size_t)(i >> 6)] & mask);
        i += n;
      }
      return count;
    }

    // Decides a batch of candidate owners at once: their element rows go to
    // the child as one candidate mask (after the membership proof clears rows
    // that cannot match), so element values are read in one pass over rows
    // in order, and each owner keeps its bit when the count of its surviving
    // rows admits it.
    void decideBatch(std::span<uint64_t> words, int32_t base, const int32_t* batch,
                     const int32_t* begins, const int32_t* ends, int32_t n) {
      if (n == 0) return;
      skipCount(SkipStats::toParentCandidateBatches);
      int32_t elementBase = begins[0] & ~63;
      int32_t span = ends[n - 1] - elementBase;
      auto bits = elementBits.first((size_t)((span + 63) >> 6));
      std::fill(bits.begin(), bits.end(), 0);
      for (int32_t i = 0; i < n; i++) setRange(bits, begins[i] - elementBase, ends[i] - elementBase);
      if (proof != nullptr) proof->intersect(bits, elementBase);
      child->intersectCandidateBits(bits, elementBase);
      for (int32_t i = 0; i < n; i++) {
        if (admits(countRange(bits, begins[i] - elementBase, ends[i] - elementBase))) continue;
        int32_t bit = batch[i] - base;
        words[(size_t)(bit >> 6)] &= ~(1ULL << (bit & 63));
      }
    }

    int32_t nextVerified() {
      if (doc == PostingsReader::END) return doc;
      while (approximate(doc + 1) != PostingsReader::END) {
        if (verify()) return doc;
      }
      return doc;
    }

  public:
    Scorer(Query::Scorer* child, Query::MembershipProof* proof, const OwnerMap& owners, ScoreMode mode,
           int32_t minChildren, int32_t maxChildren, float multiplier, bool scored, bool prune, bool twoPhase,
           std::span<uint64_t> elementBits = {})
      : child(child), proof(proof), owners(owners), ownerOf(owners), mode(mode), minChildren(minChildren),
        maxChildren(maxChildren), multiplier(multiplier), scored(scored && mode != ScoreMode::NONE),
        prune(prune), twoPhase(twoPhase), elementBits(elementBits) {
      initBounds();
      firstMatchDecides = minChildren <= 1 && maxChildren == std::numeric_limits<int32_t>::max()
          && (!this->scored || (constant && mode != ScoreMode::SUM));
    }

    int32_t next() override {
      if (twoPhase) return nextVerified();
      if (pending == PostingsReader::END || exhausted) return doc = PostingsReader::END;
      return reduce(pending == -1 ? child->next() : pending);
    }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      if (twoPhase) {
        if (approximate(target) == PostingsReader::END || verify()) return doc;
        return nextVerified();
      }
      if (pending == PostingsReader::END || exhausted) return doc = PostingsReader::END;
      int32_t begin = owners.begin(target);
      int32_t c = pending;
      if (c < begin) c = begin == 0 ? child->next() : child->advance(begin);
      return reduce(c);
    }

    int32_t docId() override { return doc; }

    int32_t approximationNext() override {
      if (!twoPhase) return next();
      return doc == PostingsReader::END ? doc : approximate(doc + 1);
    }
    int32_t approximationAdvance(int32_t target) override {
      return twoPhase ? approximate(target) : advance(target);
    }
    int32_t approximationDocId() override { return doc; }
    // Membership windows: the approximation alone, so the other clauses can
    // reject a candidate before its elements are read.
    int32_t advanceApproximationWindow(int32_t min, int32_t max) override {
      if (!twoPhase) return advanceWindow(min, max);
      if (min >= max) return max;
      return doc < min ? approximate(min) : doc;
    }
    bool matches() override { return !twoPhase || verify(); }

    // A candidate filter's batch: owner ranges are read for every candidate
    // first, then their element rows are verified together (decideBatch),
    // instead of one owner at a time behind a chain of dependent reads.
    void intersectCandidateBits(std::span<uint64_t> words, int32_t base) override {
      if (!twoPhase || elementBits.empty()) {
        Query::Scorer::intersectCandidateBits(words, base);
        return;
      }
      constexpr int32_t kBatchRows = kBatchElementWords * 64 - 64;
      int32_t batch[kBatchOwners];
      int32_t begins[kBatchOwners];
      int32_t ends[kBatchOwners];
      int32_t n = 0;
      for (size_t w = 0; w < words.size(); w++) {
        for (uint64_t candidates = words[w]; candidates != 0; candidates &= candidates - 1) {
          int32_t bit = (int32_t)std::countr_zero(candidates);
          int32_t owner = base + (int32_t)(w * 64) + bit;
          auto [begin, end] = owners.range(owner);
          if (end - begin > kBatchRows) {
            // Too many elements for a batch: decide this owner alone.
            decideBatch(words, base, batch, begins, ends, n);
            n = 0;
            if (!admits(countMatching(begin, end))) words[w] &= ~(1ULL << bit);
            continue;
          }
          if (begin == end && minChildren > 0) {
            words[w] &= ~(1ULL << bit);
            continue;
          }
          if (n == kBatchOwners || (n > 0 && end - (begins[0] & ~63) > kBatchRows)) {
            decideBatch(words, base, batch, begins, ends, n);
            n = 0;
          }
          batch[n] = owner;
          begins[n] = begin;
          ends[n] = end;
          n++;
        }
      }
      decideBatch(words, base, batch, begins, ends, n);
    }
    float matchCost() override {
      if (!twoPhase) return 0.0f;
      float perOwner = (float)owners.rows() / (float)std::max(1, owners.ownerMaxDoc());
      return perOwner * std::max(1.0f, child->matchCost());
    }

    float score() override { return current; }

    std::optional<float> constantContribution() const override { return constant; }
    float getMaxScore(int32_t upTo) override { unused(upTo); return bounds.hi; }
    float getMaxScoreForSetup(int32_t upTo) override { unused(upTo); return bounds.hi; }
    ScoreBounds getScoreBounds(int32_t upTo) override { unused(upTo); return bounds; }
    int32_t advanceShallowForSetup(int32_t target) override {
      unused(target);
      return PostingsReader::END;
    }

    void setMinCompetitiveScore(float floor) override {
      // Ties at the floor stay competitive; above a uniform score nothing
      // left can compete, so iteration ends (the current doc stays valid).
      if (constant && floor > *constant) exhausted = true;
      if (!prune || !scored || multiplier <= 0.0f) return;
      child->setMinCompetitiveScore(
          MinScoreQuery::childFloor(floor, -std::numeric_limits<float>::infinity(), multiplier, true));
    }
  };

  // Owner rows none of whose element rows the child's membership proof
  // admits: lets a membership conjunction skip such owners (and whole
  // windows of them) before any candidate reaches verification.
  class OwnerProof final : public Query::MembershipProof {
    Query::MembershipProof& child;
    const OwnerMap& owners;

  protected:
    Range nextRange(int32_t target) override {
      constexpr int32_t END = PostingsReader::END;
      if (target >= owners.ownerMaxDoc()) return {END, END};
      int32_t row = owners.begin(target);
      int32_t possible = row >= owners.rows() ? END : child.nextPossible(row);
      if (possible == END || possible >= owners.rows()) return {END, END};
      int32_t begin = std::max(target, owners.owner(possible));
      int32_t endRow = child.possibleEnd();
      int32_t end = endRow >= owners.rows() ? owners.ownerMaxDoc() : owners.owner(endRow - 1) + 1;
      return {begin, std::max(end, begin + 1)};
    }

  public:
    OwnerProof(Query::MembershipProof& child, const OwnerMap& owners) : child(child), owners(owners) {}

    void reset() override {
      Query::MembershipProof::reset();
      child.reset();
    }
  };

  class Supplier final : public Query::ScorerSupplier {
    Query::ScorerSupplier* childSupplier;
    const OwnerMap& owners;
    ScoreMode mode;
    int32_t minChildren;
    int32_t maxChildren;
    float multiplier;
    bool scored;
    bool prune;

    class Plan final : public Query::ScorerPlan {
      Query::ScorerPlan* childPlan;
      const Supplier& supplier;
      bool twoPhase;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        Query::Scorer* childScorer = childPlan->build(targetPool);
        if (childScorer == nullptr) return nullptr;
        Query::MembershipProof* proof = twoPhase ? childPlan->buildMembershipProof(targetPool) : nullptr;
        // A candidate filter decides batches of owners (intersectCandidateBits).
        std::span<uint64_t> elementBits;
        if (twoPhase && shape().membershipConsumption == Query::MembershipConsumption::CANDIDATES) {
          elementBits = targetPool.make_span<uint64_t>(Scorer::kBatchElementWords);
        }
        return targetPool.make<Scorer>(childScorer, proof, supplier.owners, supplier.mode, supplier.minChildren,
                                       supplier.maxChildren, supplier.multiplier, supplier.scored,
                                       supplier.prune, twoPhase, elementBits);
      }

    public:
      Query::MembershipProof* buildMembershipProof(MemPool& targetPool) override {
        if (!twoPhase) return nullptr;
        Query::MembershipProof* childProof = childPlan->buildMembershipProof(targetPool);
        if (childProof == nullptr) return nullptr;
        return targetPool.make<OwnerProof>(*childProof, supplier.owners);
      }

      Plan(const Query::PlanContext& planContext, const Query::ScorerShape& shape, int64_t cost,
           Query::ScorerPlan* childPlan, const Supplier& supplier, bool twoPhase)
        : Query::ScorerPlan(planContext, shape, cost), childPlan(childPlan), supplier(supplier),
          twoPhase(twoPhase) {}
    };

    // The child's demand. A lead sparser than the owner rows reaches only
    // its candidates' elements (their share of the element rows), but they
    // spread over the whole element space, which a streamed child would
    // scan to its end; so the child may choose to verify them instead.
    // Otherwise the reduction pulls every matching element in order.
    Query::PlanContext childContext(const Query::PlanContext& planContext) const {
      Query::Demand demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
      const Query::Demand& owner = planContext.demand;
      int32_t ownerRows = owners.ownerMaxDoc();
      if (owner.candidates < ownerRows && ownerRows > 0) {
        double perOwner = (double)owners.rows() / ownerRows;
        auto scale = [&](double v) { return (int64_t)std::ceil(std::min(v * perOwner, (double)owners.rows())); };
        demand = Query::Demand::fromCandidatesAndSpan(scale((double)owner.candidates), owners.rows());
        demand.expectedCandidates = (double)scale(owner.expectedCandidates >= 0
            ? std::min(owner.expectedCandidates, (double)owner.candidates) : (double)owner.candidates);
      }
      Query::PlanContext context = childSupplier->makePlanContext(demand);
      context.demand.horizon = Query::ExecutionUse::PULL;
      return context;
    }

    // A verifying to_parent serves a membership conjunction as a candidate
    // filter: it takes the lead's candidates a window at a time
    // (intersectCandidateBits) instead of producing windows of its own.
    static Query::MembershipConsumption consumption(const Query::PlanContext& context, bool twoPhase) {
      using Consumption = Query::MembershipConsumption;
      if (context.membershipConsumption != Consumption::EITHER) return context.membershipConsumption;
      return twoPhase ? Consumption::CANDIDATES : Consumption::WINDOWS;
    }

    static Query::ScorerShape shape(bool twoPhase, Query::MembershipConsumption consumption) {
      return {
        .matchState = Query::MatchState::UNKNOWN,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = twoPhase ? Query::ReportedTwoPhase::YES : Query::ReportedTwoPhase::NO,
        // A candidate filter is exact over any candidate mask (the default
        // window fill verifies each document through advanceWindow).
        .windowFillClause = consumption == Query::MembershipConsumption::CANDIDATES
            ? Query::ClauseShape::DIRECT : Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        .membershipConsumption = consumption,
      };
    }

  public:
    Supplier(Query::ScorerSupplier* childSupplier, const OwnerMap& owners, ScoreMode mode,
             int32_t minChildren, int32_t maxChildren, float multiplier, bool scored, bool prune)
      : childSupplier(childSupplier), owners(owners), mode(mode), minChildren(minChildren),
        maxChildren(maxChildren), multiplier(multiplier), scored(scored), prune(prune) {}

    // Owners never outnumber their matching elements.
    int64_t cost() override { return childSupplier->cost(); }

    Query::ScorerShape describeScorer(const Query::PlanContext& buildContext) const override {
      bool twoPhase = childSupplier->describeScorer(childContext(buildContext)).reportedTwoPhase
          == Query::ReportedTwoPhase::YES;
      return shape(twoPhase, consumption(buildContext, twoPhase));
    }

    Query::VerificationWork verificationWork(const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return Query::VerificationWork::PRESENT;
    }

    Query::ScorerPlan* resolve(MemPool& planPool, const Query::PlanContext& planContext) override {
      Query::ScorerPlan* childPlan = childSupplier->resolve(planPool, childContext(planContext));
      assert(childPlan != nullptr);
      bool twoPhase = childPlan->shape().reportedTwoPhase == Query::ReportedTwoPhase::YES;
      return planPool.make<Plan>(planContext, shape(twoPhase, consumption(planContext, twoPhase)),
                                 childPlan->cost(), childPlan, *this, twoPhase);
    }
  };

public:
  ToParentQuery(std::string_view path, std::string_view name, Query* child, ScoreMode mode,
                int32_t minChildren, int32_t maxChildren)
    : Query(QueryKind::TO_PARENT), path(path), name(name), child(child), mode(mode),
      minChildren(minChildren), maxChildren(maxChildren) {}

  std::string_view getPath() const { return path; }
  std::string_view getName() const { return name; }
  Query* getChild() const { return child; }
  ScoreMode getMode() const { return mode; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const ToParentQuery&>(other);
    return path == rhs.path && mode == rhs.mode && minChildren == rhs.minChildren
        && maxChildren == rhs.maxChildren && child->equals(*rhs.child);
  }

  uint64_t hashImpl() const override {
    uint64_t h = mixHash(Query::hashImpl(), std::hash<std::string_view>{}(path));
    h = mixHash(h, (uint64_t)mode);
    h = mixHash(h, (uint64_t)(uint32_t)minChildren);
    h = mixHash(h, (uint64_t)(uint32_t)maxChildren);
    return mixHash(h, child->hash());
  }

  void validateLogicalImpl(PlanningContext& context, float multiplier = 1.0f) const override {
    checkedBoostProduct(multiplier, 1.0f);
    child->validateLogical(context.scope(path), 1.0f);
  }

  // Owner membership depends on the child's membership and the count bounds,
  // not on scores, so it is keyed in the owner space by path and child key.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& ctx) const override {
    out.appendKind(QueryKind::TO_PARENT);
    out.appendString(path);
    out.appendInt32(minChildren);
    out.appendInt32(maxChildren);
    return child->appendFilterKey(out, ctx);
  }

  Weight* createWeight(Context& context, int32_t flags, float multiplier = 1.0f) override;

  // Child terms are element fields; owner-space visitors (highlighting) must
  // not see them as owner fields.
  void visitImpl(QueryVisitor& visitor, Context& context, float boost) const override;

  class Weight final : public Query::Weight {
    ToParentQuery& query;
    Query::Weight* childWeight;
    IndexReader& scopeReader;
    float multiplier;
    bool scored;
    bool prune;

    Query::ScorerSupplier* wrap(MemPool& targetPool, IndexReader::Segment& segment,
                                Query::SegmentSource& source, Query::SupplierExecutionMode executionMode) {
      auto& childSegment = scopeReader.segments()[(size_t)segment.ord];
      if (childSegment.maxDoc() == 0) return nullptr;
      Query::ScorerSupplier* childSupplier = source.scorerSupplier(targetPool, childSegment, executionMode);
      if (childSupplier == nullptr) return nullptr;
      return targetPool.make<Supplier>(childSupplier, childSegment.postingsReader().owners(), query.mode,
                                       query.minChildren, query.maxChildren, multiplier, scored, prune);
    }

    static Query::Scorer* buildPull(MemPool& targetPool, Query::ScorerSupplier* supplier) {
      if (supplier == nullptr) return nullptr;
      Query::Demand demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
      return supplier->resolve(targetPool, supplier->makePlanContext(demand))->build(targetPool);
    }

    class Prepared final : public Query::Weight::PreparedWeight {
      Weight& weight;
      QueryPrep::PreparedSource child;
      std::vector<std::shared_ptr<LiveDocs>> domains;  // descended owner domains, kept alive

    public:
      Prepared(Weight& weight, QueryPrep::PreparedSource&& child, std::vector<std::shared_ptr<LiveDocs>>&& domains)
        : weight(weight), child(std::move(child)), domains(std::move(domains)) {}

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return buildPull(targetPool, scorerSupplier(targetPool, segment));
      }

      Query::ScorerSupplier* scorerSupplierImpl(MemPool& targetPool, IndexReader::Segment& segment,
                                                Query::SupplierExecutionMode executionMode) override {
        return weight.wrap(targetPool, segment, child.segmentSource(), executionMode);
      }

      void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
        child.releaseSegmentState(weight.scopeReader.segments()[(size_t)segment.ord]);
      }

      PreparedDomainDependence domainDependence() const noexcept override { return child.domainDependence; }
    };

  public:
    Weight(Context& context, ToParentQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, query, flags), query(query),
        scopeReader(context.scope(query.path).topReader), multiplier(multiplier),
        scored((flags & NEED_SCORES) != 0 && query.mode != ScoreMode::NONE) {
      prune = scored && query.mode == ScoreMode::MAX && !query.countBounded()
           && (flags & ALLOW_PRUNING) != 0;
      int32_t childFlags = flags & ~EXCLUSION_WINDOW_FILL;
      if (!scored) childFlags &= ~(NEED_SCORES | ALLOW_PRUNING);
      if (!prune) childFlags &= ~ALLOW_PRUNING;
      childWeight = query.child->createWeight(context.scope(query.path), childFlags, 1.0f);
      traits = childWeight->getFlags() & (NEEDS_PREPARE | CONSUMES_DOMAIN);
      // max, min and avg of one uniform element score is that score.
      if (!scored || (query.mode != ScoreMode::SUM && childWeight->isConstantScoring())) {
        traits |= IS_CONSTANT_SCORING;
      }
    }

    // The owner domain descends to the element rows of its owners: the
    // child prepares (kNN candidate admission, ...) over live elements of
    // eligible owners only.
    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override;

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      return buildPull(targetPool, scorerSupplier(targetPool, segment));
    }

    Query::ScorerSupplier* scorerSupplierImpl(MemPool& targetPool, IndexReader::Segment& segment,
                                              Query::SupplierExecutionMode executionMode) override {
      return wrap(targetPool, segment, *childWeight, executionMode);
    }

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      childWeight->releaseSegmentState(scopeReader.segments()[(size_t)segment.ord]);
    }

    bool hasReleasableSegmentState() const noexcept override {
      return childWeight->hasReleasableSegmentState();
    }
  };
};

// Owner rows -> element rows: the live elements of the owners in `owners`
// (null = every owner). Returns null when the result is every live element of
// the segment, keeping the domain identity contract (a null domain stays null).
std::shared_ptr<LiveDocs> descendDomain(const IndexReader::Segment& ownerSegment, DocSet* owners,
                                        const IndexReader::Segment& scopeSegment);

} // namespace luxir
