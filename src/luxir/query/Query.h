#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <luxir/util/heap.h>
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/MemPool.h"
#include "luxir/util/proto.h"
#include "luxir/util/StrRef.h"
#include "luxir/search/IndexReader.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/search/DocSet.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/FilterKey.h"
#include "luxir/search/Similarity.h"
#include <boost/unordered/unordered_node_map.hpp>
#include <google/protobuf/arena.h>

namespace luxir {

class DocSet;
class DocSetBuilder;
class WindowFilter;

enum class PreparedDomainDependence : uint8_t {
  // Prepared output is the canonical result of the query and may be
  // published under the query's segment/core-stable filter key.
  QUERY_CANONICAL = 0,
  // Prepared output is exact only for the canonical live domain of one
  // reader version. It belongs in the reader-stable cache lane.
  CANONICAL_READER_DOMAIN = 1,
  // Prepared output depends on the arbitrary PrepareContext domain and is
  // request-local unless a separate key names that domain.
  PREPARE_DOMAIN = 2
};

struct ScoreBounds {
  float lo = -std::numeric_limits<float>::infinity();
  float hi = std::numeric_limits<float>::infinity();

  static ScoreBounds unknown() { return {}; }
  static ScoreBounds exact(float value) { return {value, value}; }
  static ScoreBounds nonNegative(float hi) { return {0.0f, hi}; }
};

struct ScoreWindow {
  int32_t min = 0;
  int32_t max = 0;
  int32_t size = 0;
  std::span<int32_t> docs;
  std::span<float> scores;
};

// NOTE: no virtual destructor, so subclasses should not be owned or deleted through this type.
class BulkScorer {
public:
  // Supply the requested result depth and whether competitive pruning is
  // available before scored collection starts. Most scorers do not need it;
  // admission policies that choose between exhaustive and competitive
  // execution should use this explicit seam rather than infer either signal
  // from the evolving competitive threshold.
  virtual void setTopKDepth(int32_t topK, bool allowPruning) {
    unused(topK, allowPruning);
  }

  virtual bool willCountDense() const {
    return false;
  }

  virtual bool supportsMatchWindows() const {
    return false;
  }

  // True when scoreCandidatesExact can rescore an increasing batch of known
  // matches in the exhaustive path's canonical accumulation order.
  virtual bool supportsExactCandidateScoring() const {
    return false;
  }

  // Append this scorer's remaining exact docs directly to a builder when it
  // has a docs-only block stream. Returns false without consuming anything
  // when the scorer does not support that protocol.
  virtual bool appendDocs(DocSetBuilder& builder) {
    unused(builder);
    return false;
  }

  virtual void scoreCandidatesExact(std::span<int32_t> docs,
                                    std::span<float> scores) {
    unused(docs, scores);
    assert(false);
  }

  // Attach a lazy, window-local filter to scored execution. The filter is
  // prepared by the bulk scorer only after it has selected the final
  // production-window bounds. Unsupported bulk scorers reject the attach.
  virtual bool attachWindowFilter(WindowFilter* filter) {
    unused(filter);
    return false;
  }

  // Produce the next window of verified competitive candidates in [min, max),
  // intersected with filter (null = all), filtered by minCompetitiveScore.
  // Returns the docid to resume from (first window not produced), or PostingsReader::END.
  // The spans in out are valid until the next call.
  virtual int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                                  float minCompetitiveScore) = 0;

  // Emit the matching docs of the next window in [min, max), intersected
  // with filter, without scores (out.scores contents are unspecified).
  // Exhaustive like countNextWindow: there is no competitive threshold.
  // Returns the resume docid under the same contract as scoreNextWindow,
  // with one addition all window entry points share: END from a request
  // whose max is below maxDoc only means the bound was reached - it is NOT
  // an exhaustion latch, and the caller may keep issuing later bounded
  // requests (which must return empty windows once the underlying stream is
  // truly exhausted). Only an unbounded request's END means exhaustion.
  virtual int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                                  int32_t min, int32_t max) {
    return scoreNextWindow(out, filter, min, max,
                           std::numeric_limits<float>::lowest());
  }

  // Count (rather than emit) the matches of the next window in [min, max),
  // intersected with filter. Adds to count and returns the resume docid like
  // scoreNextWindow. Exhaustive by definition - no competitive threshold.
  // The default delegates to scoreNextWindow; subclasses override when they
  // can count cheaper than they can emit (no scores, no doc materialization).
  virtual int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                                  DocSet* filter, int32_t min, int32_t max) {
    ScoreWindow window;
    int32_t next = scoreNextWindow(window, filter, min, max,
                                   std::numeric_limits<float>::lowest());
    if (domainOut != nullptr) {
      skipCount(SkipStats::bulkDomainWindowsFed);
      for (int32_t i = 0; i < window.size; i++) {
        domainOut->add(window.docs[(size_t) i]);
      }
    }
    count += window.size;
    return next;
  }
};

// Overview
// ========
// Query construction and segment execution are separated by an explicit plan:
//
//   Query
//     -> createWeight(): per-query, cross-segment state for one IndexReader;
//        Boolean normalization and flattening happen here.
//     -> optional Weight::prepare(): domain-aware whole-index work returning an
//        immutable PreparedWeight. Weight and PreparedWeight are SegmentSources.
//     -> SegmentSource::scorerSupplier(): temporary per-segment planning state.
//     -> ScorerSupplier::resolve(PlanContext{Demand{candidates, span, horizon},
//                                            controls}): select one scorer arm.
//     -> ScorerPlan: segment-pool-owned affine token recording the arm's shape,
//        cost, demand, and construction state. A parent may retain child plans;
//        one of build(), buildIndependent(), or buildDocsOnly() consumes it and
//        produces the recorded cursor scorer or docs-only enum.
//
// Window and count consumers use planBulk(horizon, BulkScorerContext) ->
// BulkPlan. A definite BulkPlan records its capabilities and supplier-owned
// build state; buildBulk() produces the planned window producer, while
// constantCount carries an exact scalar result without constructing an
// executor. Composite plans retain the child ScorerPlans their build consumes.
//
// Scorer contains execution protocols only: exact cursor iteration, the private
// or externally driven two-phase pair, fillWindowBits(), and scoring. Capability
// selection and product construction belong to plans, not built scorers.
//
// Planning doctrine:
//   - A route describes capability.
//   - ExecutionUse declares the consumption horizon.
//   - Exposure is priced in the consumer's unit: candidates for candidate
//     probes, span for window fills.
//   - UNKNOWN is always legal; a falsely definite answer never is.
//   - Decisions are recorded in plans, not predicted beside construction.
//
// createWeight() runs single-threaded before search tasks are dispatched. It may
// populate Query::Context caches and allocate from Context::pool. prepare(),
// scorerSupplier(), resolve(), and plan construction may run on worker threads;
// they may read that immutable state but MUST NOT mutate Context or allocate
// from Context::pool. Their scratch and products belong to local, thread-local,
// prepared-result, or per-segment storage as appropriate.
//
// Domain: the per-segment DocSet a query is restricted to (liveDocs from RootOp,
// intersected with any filter clauses).  PrepareContext carries the per-segment
// domains so a prepared weight can pre-filter its whole-index work; search ops
// and materializers also intersect scorer output with the active domain.
//
// Scorer iteration: next()/advance(target) walk docs in increasing docId order,
// returning PostingsReader::END when exhausted. advance(target) is strict: target
// must be greater than docId(), and the scorer advances to the first doc >=
// target. A compound scorer must guard sub-scorers that may already be on target:
// `if (sub.docId() < target) sub.advance(target)` (see ConjunctionScorer,
// MandOptScorer).
//
// Once a scorer returns END, calling next() or advance() on it again is undefined
// behavior (Lucene's DocIdSetIterator contract): callers latch on END and stop.
// Because END == INT_MAX the strict-advance guard above doubles as that latch for
// advance() (END is never < target, so an exhausted sub is simply never re-advanced);
// next() asserts it has not been re-polled where the exhausted state is cheap to test.
//

/// A map from KeyType to a vector of pointers to ValType.
/// The map internals, the vector, and the instances of ValType are all pool allocated.
/// The pointers to ValType are unique_ptr with a custom deleter that only calls the destructor.
template <typename KeyType, typename ValType>
class PoolMapVec {
public:
  // Example:
  // using SegFieldInfoMap = PoolMapVec<std::string_view, SegFieldInfo>;
  // SegFieldInfoMap segFieldInfoMap;

  using key_type = KeyType;
  using value_type = ValType;
  using valptr = u_ptr<value_type>;
  using vec_type = std::vector<valptr, MemPool::allocator<valptr>>;
  using mapped_type = vec_type;
  using pair_type = std::pair<const key_type, vec_type>;
  using map_type = boost::unordered_node_map<key_type, vec_type, PackedTermHash, PackedTermEqual, MemPool::allocator<pair_type>>;
  // Using a node-based map in a pool will lead to less memory wasted if the map is resized.


  MemPool& pool;
  map_type map;

  PoolMapVec(MemPool& pool, size_t initialMapSize) : pool(pool), map(initialMapSize, pool.getAllocator()) {}

  vec_type& insertOrGet(const key_type& key) {
    return map.try_emplace(key, pool.getAllocator()).first->second;
  }
};

struct CachedTermInfo {
  Similarity::TermStats termStats = {};
  Similarity::BM25Scorer* simScorer = nullptr;  // This may be null even if other elements are fille in (phrase query would have different one)
  std::span<const TermsEnum::PostingsState*> postingsStates = {};

  /// Construct an independent tiered docs enum from the immutable state captured by the
  /// original term seek. No dictionary re-seek or shared mutable enum is involved.
  template<DocsEnumTier Tier>
  BasicDocsEnum<Tier>* useDocsEnum(MemPool& targetPool, IndexReader::Segment& segment) {
    auto* state = postingsStates[segment.ord];
    if (state == nullptr) {
      return nullptr;
    }
    return targetPool.make<BasicDocsEnum<Tier>>(*state);
  }

  int32_t docFreq(int32_t segmentOrd) const {
    auto* state = postingsStates[segmentOrd];
    return state == nullptr ? 0 : state->docFreq;
  }
};

struct CachedFieldInfo {
  std::span<SegFieldInfo*> segInfos = {};
  Similarity::FieldStats fieldStats = {};
  std::span<TermsEnum*> termsEnums = {};  // TODO: cache align if they will be used in multiple threads
  boost::unordered_node_map<std::string_view, CachedTermInfo, PackedTermHash, PackedTermEqual, MemPool::allocator<std::pair<const std::string_view, CachedTermInfo>>> termInfos;

  explicit CachedFieldInfo(MemPool& pool, size_t initialMapSize=4) : termInfos(initialMapSize, pool.getAllocator()) {}
};


// NOTE: no virtual destructor, so subclasses of Query should be made trivially destructible
// Scored TOP_k filter routing crossover, shared by the boolean window-mask
// gate and the filter cache's scored-route gate: below maxDoc/this the pull
// side (filter leads) wins. Mask viability tracks FILTER DENSITY - the rate
// accepted docs surface is the rate the collector floor rises - so a very
// sparse filter starves theta and degenerates the mask toward an unpruned
// scan, while a denser filter lets the mask keep MaxScore's pruning.
// MEASURED CROSSOVER (5M corpus, full 1,010-query set, pull vs forced mask,
// luxir/lucene at TOP_10/TOP_100, zero count mismatches between routes):
//   1.99%  pull 1.80/1.85   mask 0.85/0.91   -> mask
//   0.99%  pull 1.70/1.61   mask 0.89/0.97   -> mask
//   0.50%  pull 1.44/1.24   mask 0.94/1.03   -> mask
//   0.20%  pull 0.94/0.90   mask 1.03/1.22   -> pull
//   0.02%  pull 0.44/0.50   mask 1.36/1.82   -> pull
// 256 puts the threshold at 0.39% (maxDoc/256), so every measured point lands
// on its winning side. The previous value 32 (3.1%) left the whole 0.4-3%
// band on pull, where it lost up to 1.85x overall and 2.98x on unions.
inline constexpr int64_t kMaskFilterDensityInverse = 256;

class Query {
public:
  class Context;
  class Weight;
  class Scorer;
  class ScorerPlan;
  class ScorerSupplier;
  class SegmentSource;

  /// Input flags for createWeight(). NEED_SCORES is propagated down the query
  /// tree and cleared for clauses whose score the parent never reads.
  static constexpr int32_t NEED_SCORES = 1;
  /// The request may skip non-competitive matches because it does not require
  /// an exact hit count. Propagated independently of NEED_SCORES.
  static constexpr int32_t ALLOW_PRUNING = 1 << 1;
  /// Internal planning flag for clauses under Boolean MUST_NOT. It lets
  /// expensive exact-fill scorers expose the window contract only to
  /// exclusion consumers, without changing positive-side execution.
  static constexpr int32_t EXCLUSION_WINDOW_FILL = 1 << 2;

  // Query-tree score semantics, derived before Weight creation. This is
  // intentionally separate from Weight::IS_CONSTANT_SCORING: the latter is
  // an execution trait and may depend on NEED_SCORES, while this profile
  // records whether a uniform score was implicit or explicitly requested.
  struct ScoreProfile {
    enum class Kind : uint8_t {
      VARIABLE,
      AUTO_UNIFORM,
      EXPLICIT_UNIFORM
    };

    Kind kind = Kind::VARIABLE;
    float value = 0.0f;

    static ScoreProfile variable() { return {}; }
    static ScoreProfile automatic(float value) {
      return {Kind::AUTO_UNIFORM, value};
    }
    static ScoreProfile explicitUniform(float value) {
      return {Kind::EXPLICIT_UNIFORM, value};
    }
  };

  enum class MatchState : uint8_t {
    EMPTY,
    NONEMPTY,
    UNKNOWN,
  };

  enum class UnresolvedSupplierCause : uint8_t {
    NONE,
    MULTITERM,
    PHRASE,
    NUMERIC_GEO,
    OTHER,
  };

  enum class DirectScorerKind : uint8_t {
    TERM,
    DOC_SET,
    OTHER,
    UNKNOWN,
  };

  enum class ReportedTwoPhase : uint8_t {
    YES,
    NO,
    UNKNOWN,
  };

  // Economic shape of membership production. Unlike reportedTwoPhase, this
  // survives a compound scorer that internalizes child verification behind
  // next()/advance() and therefore reports a single-phase outer protocol.
  enum class VerificationWork : uint8_t {
    PRESENT,
    PARTIAL,
    ABSENT,
    UNKNOWN,
  };

  enum class ClauseShape : uint8_t {
    DIRECT,
    FLAT_DISJUNCTION,
    FLAT_CONJUNCTION,
    NONE,
    UNKNOWN,
  };

  enum class IndependentTermAccess : uint8_t {
    SUPPORTED,
    UNSUPPORTED,
    UNKNOWN,
  };

  enum class DocsOnlyAccess : uint8_t {
    SUPPORTED,
    UNSUPPORTED,
    UNKNOWN,
  };

  enum class DirectDocSetAccess : uint8_t {
    SUPPORTED,
    UNSUPPORTED,
    UNKNOWN,
  };

  // The consumer protocol whose exposure the supplier must price. PULL uses
  // cursor iteration; the window and candidate values declare their exact
  // bulk consumption horizon.
  enum class ExecutionUse : uint8_t {
    PULL,
    COUNT_WINDOWS,
    MATCH_WINDOWS,
    EXACT_CANDIDATE_SCORING,
    SCORED_WINDOWS,
  };

  // Supplier construction is ordinarily allowed to compose cached membership
  // with the reader/domain visible to the request. RAW_MEMBERSHIP requires
  // query membership independent of both: compound sources propagate the mode
  // and cache-backed children expose only pinned raw values.
  enum class SupplierExecutionMode : uint8_t {
    ORDINARY,
    RAW_MEMBERSHIP,
  };

  struct Demand {
    int64_t candidates = std::numeric_limits<int64_t>::max();
    int64_t span = std::numeric_limits<int64_t>::max();
    ExecutionUse horizon = ExecutionUse::PULL;

    static Demand fromCandidatesAndSpan(
        int64_t candidates, int64_t span,
        ExecutionUse horizon = ExecutionUse::PULL) noexcept {
      return {candidates, span, horizon};
    }

    // Convenience form for callers whose candidate and span estimates are the
    // same scalar.
    static Demand fromLeadCost(
        int64_t leadCost,
        ExecutionUse horizon = ExecutionUse::PULL) noexcept {
      return {leadCost, leadCost, horizon};
    }
  };

  // Exact inputs that may affect which scorer a supplier constructs. Test
  // controls are snapshots carried with the demand rather than separate
  // planning-time inputs.
  struct PlanContext {
    enum class MultiTermScorerMode : uint8_t {
      AUTO,
      FORCE_EAGER,
      FORCE_WINDOWED,
      FORCE_HEAP,
    };

    Demand demand;
    MultiTermScorerMode multiTermScorerModeForTests =
        MultiTermScorerMode::AUTO;
    size_t multiTermMaxLazyStateBytes = 32u << 20;
    bool multiTermDisableDenseFillForTests = false;
    bool phraseDisableSortForTests = false;
    bool phraseDisableRepeatDedupForTests = false;
    bool phraseDisableShapesForTests = false;
    bool numericRangeDisableShapesForTests = false;
    bool disableBooleanTwoPhaseForTests = false;
    bool disableDisjunctionTwoPhaseForTests = false;
    bool disableMandNotTwoPhaseForTests = false;
    bool disableFilteredUnionWandForTests = false;

    static PlanContext fromLeadCost(
        int64_t leadCost,
        ExecutionUse horizon = ExecutionUse::PULL) noexcept {
      PlanContext context;
      context.demand = Demand::fromLeadCost(leadCost, horizon);
      return context;
    }
  };

  // Description of every non-null scorer produced for one build context.
  // UNKNOWN is conservative and always legal; definite answers must describe
  // the declared scorer protocols rather than an equivalent representation.
  struct ScorerShape {
    MatchState matchState = MatchState::UNKNOWN;
    DirectScorerKind directKind = DirectScorerKind::UNKNOWN;
    ReportedTwoPhase reportedTwoPhase = ReportedTwoPhase::UNKNOWN;
    ClauseShape windowFillClause = ClauseShape::UNKNOWN;
    ClauseShape termDisjunctionClause = ClauseShape::UNKNOWN;
    ClauseShape termConjunctionClause = ClauseShape::UNKNOWN;
    IndependentTermAccess independentTerm = IndependentTermAccess::UNKNOWN;
    DocsOnlyAccess docsOnly = DocsOnlyAccess::UNKNOWN;
    DirectDocSetAccess directDocSet = DirectDocSetAccess::UNKNOWN;

    bool hasUnknown() const noexcept {
      return matchState == MatchState::UNKNOWN
          || directKind == DirectScorerKind::UNKNOWN
          || reportedTwoPhase == ReportedTwoPhase::UNKNOWN
          || windowFillClause == ClauseShape::UNKNOWN
          || termDisjunctionClause == ClauseShape::UNKNOWN
          || termConjunctionClause == ClauseShape::UNKNOWN
          || independentTerm == IndependentTermAccess::UNKNOWN
          || docsOnly == DocsOnlyAccess::UNKNOWN
          || directDocSet == DirectDocSetAccess::UNKNOWN;
    }
  };

  // Unknown and custom queries are conservatively variable-scoring.
  virtual ScoreProfile scoreProfile() const { return ScoreProfile::variable(); }

  // Query-tree fact used when the consumer cannot gain a segment-specific
  // best-first route. Definite PRESENT must mean membership production has
  // verification work that a materialized whole-query set avoids.
  virtual VerificationWork membershipVerificationWork() const {
    return VerificationWork::UNKNOWN;
  }

  // True only when omitting createWeight on a fully resident membership hit
  // preserves every request-visible validation and warning for this logical
  // query. Unknown and custom queries remain conservative by default.
  virtual bool supportsCacheFirstMembership() const { return false; }

  // Structural membership key: the filter projection of this query, with
  // score-only state omitted. Consumers that need query identity, such as a
  // future request cache, must not reuse this method. Queries whose membership
  // depends on scores must return UNCACHEABLE. Every concrete query must make
  // an explicit cacheability decision.
  virtual FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                         const FilterKeyContext& ctx) const = 0;

  /// Returns a non-owning pointer to the created weight.  The Query::Context
  /// is responsible for the lifecycle of the created Weight.
  /// A Context is not generally thread-safe, so don't create weights from multiple threads with the same Context.
  virtual Query::Weight* createWeight(Query::Context& context, int32_t flags,
                                      float multiplier = 1.0f) = 0;

  static float checkedBoostProduct(float inherited, float local) {
    float product = inherited * local;
    if (!std::isfinite(product)) {
      throw std::runtime_error("query boost product must be finite");
    }
    return product;
  }

  /// The constant a uniform-scoring weight contributes: its boost product
  /// when scores are requested, 0 otherwise (built unscored means score 0
  /// and zero bounds).
  static float constantWhenScored(int32_t flags, float multiplier,
                                  float local = 1.0f) {
    return (flags & NEED_SCORES) != 0 ? checkedBoostProduct(multiplier, local)
                                      : 0.0f;
  }

  /// Per-segment planning state. Suppliers are allocated from the segment-local
  /// targetPool and only need to live until their resolved plans are built.
  // NOTE: no virtual destructor, so subclasses should not be owned or deleted through this type.
  class ScorerSupplier {
  public:
    using BulkUse = ExecutionUse;

    enum class BulkAnswer : uint8_t {
      YES,
      NO,
    };

    enum class ScoreBlockFillKind : uint8_t {
      DEFAULT_SCALAR,
      BLOCK_ITERATION,
    };

    // Constraints imposed by the caller around a prospective bulk scorer.
    // A supplier can use these to reject bulk execution when its algorithm is
    // slower than pull under the enclosing domain.
    struct BulkScorerContext {
      int64_t filterCost = -1;
      std::span<ScorerSupplier* const> filterSuppliers;
      // Strong constraint: a supplier must return consumesFilters=true or
      // decline with {}; it must not construct a non-consuming fallback.
      bool requireFilterConsumption = false;
      // Ask this same planner only for an exact scalar count arm. This never
      // authorizes a bulk build whose output would then be discarded.
      bool requireConstantCount = false;

      bool hasFilter() const noexcept { return filterCost >= 0; }
    };

    // Supplier-owned, pool-allocated state carried from a definite plan into
    // construction. Only the supplier that created the state may interpret it.
    struct BulkBuildState {
      const ScorerSupplier* owner = nullptr;
    };

    struct BulkPlan {
      BulkAnswer available = BulkAnswer::NO;
      BulkAnswer supportsMatchWindows = BulkAnswer::NO;
      BulkAnswer supportsExactCandidateScoring = BulkAnswer::NO;
      BulkAnswer consumesFilters = BulkAnswer::NO;
      const BulkBuildState* buildState = nullptr;
      int64_t constantCount = -1;
      BulkAnswer acceptsWindowFilter = BulkAnswer::NO;
      // YES declares that another planBulk() call on this supplier with an
      // identical context may coexist with this plan: neither call
      // invalidates the other, their build states are disjoint, each plan is
      // built at most once, and both products enumerate the same exact
      // matches under the same filter contract. Consumers that need two
      // independent forward executions (the seeded field-sort driver) require
      // YES and build both products before either runs.
      BulkAnswer independentReplan = BulkAnswer::NO;

      bool hasConstantCount() const noexcept {
        return constantCount >= 0;
      }
    };

    struct FilteredBulkResult {
      BulkScorer* bulk = nullptr;
      // All-or-none ownership: true requires a non-null bulk that enforces
      // every supplier in BulkScorerContext::filterSuppliers. False leaves
      // every enclosing supplier owned by the caller.
      bool consumesFilters = false;
    };

    struct ExactCountTopKCosts {
      int64_t filter = -1;
      int64_t unionSide = -1;

      bool available() const noexcept {
        return filter >= 0 && unionSide >= 0;
      }
    };

    /// Estimated number of matching docs in this segment. This should be cheap
    /// to compute; an upper bound is safe. Compound suppliers use it to choose
    /// lead iterators before creating scorers.
    virtual int64_t cost() = 0;

    /// Pure description of the scorer returned by resolve() for this exact build
    /// context. Implementations must not construct scorers, increment counters,
    /// or allocate from the target pool while answering.
    virtual ScorerShape describeScorer(
        const PlanContext& buildContext) const {
      unused(buildContext);
      return {};
    }

    virtual VerificationWork verificationWork(
        const PlanContext& buildContext) const {
      switch (describeScorer(buildContext).reportedTwoPhase) {
        case ReportedTwoPhase::YES:
          return VerificationWork::PRESENT;
        case ReportedTwoPhase::NO:
          return VerificationWork::ABSENT;
        case ReportedTwoPhase::UNKNOWN:
          return VerificationWork::UNKNOWN;
      }
      std::unreachable();
    }

    /// Resolve one exact construction arm into a pool-owned affine token.
    virtual ScorerPlan* resolve(
        MemPool& planPool, const PlanContext& planContext) = 0;

    /// Snapshot supplier-family controls for a top-level pull resolve. Parents
    /// that compute child Demand construct and pass the full PlanContext
    /// directly; standalone consumers use this factory before resolve().
    virtual PlanContext makePlanContext(const Demand& demand) const {
      PlanContext context;
      context.demand = demand;
      return context;
    }

    /// Attribute a shape uncertainty to the supplier family that must resolve
    /// it. Compound and transparent wrapper suppliers should delegate to the
    /// unresolved child.
    virtual UnresolvedSupplierCause unresolvedScorerCause(
        const PlanContext& buildContext) const {
      unused(buildContext);
      return UnresolvedSupplierCause::OTHER;
    }

    /// Resolve any dictionary-expansion-dependent shape answers for this
    /// segment. The default has nothing to resolve. Implementations may recurse
    /// into wrapped or compound suppliers, but must leave describeScorer()
    /// itself pure.
    virtual bool fillExpansionMemo(
        const PlanContext& buildContext) {
      unused(buildContext);
      return false;
    }

    /// Separately estimated filter and union costs for exact-count top-k
    /// composition. The default means this supplier does not expose that
    /// decomposition.
    virtual ExactCountTopKCosts exactCountTopKCosts() {
      return {};
    }

    /// Return a non-owning pointer to the exact matching DocSet when one is
    /// already materialized. The supplier does not transfer ownership; the
    /// default preserves the ordinary scorer/bulk-scorer collection contract.
    virtual DocSet* exactDocSet() {
      return nullptr;
    }

    /// How the resolved scorer implements fillScoreBlock(). This is an execution-cost
    /// capability, not a correctness requirement. DEFAULT_SCALAR means the
    /// scorer may use Scorer's next()+score() loop; BLOCK_ITERATION means it
    /// amortizes postings iteration over blocks even if score calculation
    /// within the block still needs per-doc values.
    virtual ScoreBlockFillKind scoreBlockFillKind() const noexcept {
      return ScoreBlockFillKind::DEFAULT_SCALAR;
    }

    /// Describe bulk construction for one consumer intent without building.
    /// The answer is definite and must match the product built for this
    /// context.
    virtual BulkPlan planBulk(
        BulkUse use, const BulkScorerContext& bulkContext) {
      unused(use, bulkContext);
      return {
        BulkAnswer::NO,
        BulkAnswer::NO,
        BulkAnswer::NO,
        BulkAnswer::NO,
      };
    }

    /// Record route decisions from a definite pre-build decline. Planning is
    /// pure; consumers call this only when they commit to skipping the build.
    virtual void recordBulkPlanCommitment(
        BulkUse use, const BulkScorerContext& bulkContext,
        const BulkPlan& plan) {
      unused(use, bulkContext, plan);
    }

    virtual BulkScorer* bulkScorer(MemPool& targetPool) {
      BulkScorerContext bulkContext;
      BulkPlan plan = planBulk(BulkUse::MATCH_WINDOWS, bulkContext);
      if (plan.available == BulkAnswer::NO) {
        recordBulkPlanCommitment(
            BulkUse::MATCH_WINDOWS, bulkContext, plan);
      }
      return plan.available == BulkAnswer::YES && !plan.hasConstantCount()
          ? buildBulk(targetPool, plan) : nullptr;
    }

    /// Consume supplier-owned state from a definite plan. The default has no
    /// bulk product.
    virtual BulkScorer* buildBulk(
        MemPool& targetPool, const BulkPlan& plan) {
      unused(targetPool, plan);
      return nullptr;
    }

    // Build a bulk scorer that will run beneath an enclosing filter. Unknown
    // suppliers decline by default; implementations that understand their
    // bulk route must explicitly accept or use the context.
    virtual FilteredBulkResult filteredBulkScorer(
        MemPool& targetPool, const BulkScorerContext& bulkContext) {
      BulkPlan plan = planBulk(BulkUse::MATCH_WINDOWS, bulkContext);
      if (plan.available != BulkAnswer::YES || plan.hasConstantCount()) {
        if (plan.available == BulkAnswer::NO) {
          recordBulkPlanCommitment(
              BulkUse::MATCH_WINDOWS, bulkContext, plan);
        }
        return {};
      }
      BulkScorer* bulk = buildBulk(targetPool, plan);
      return {
        bulk,
        bulk != nullptr && plan.consumesFilters == BulkAnswer::YES,
      };
    }
  };

  // NOTE: no virtual destructor, so subclasses should not be owned or deleted through this type.
  class SegmentSource {
  public:
    /// Return temporary per-segment planning state allocated from targetPool.
    /// A null supplier means this source cannot match the segment.
    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, IndexReader::Segment& segment,
        SupplierExecutionMode executionMode =
            SupplierExecutionMode::ORDINARY) {
      return scorerSupplierImpl(targetPool, segment, executionMode);
    }

    virtual Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        SupplierExecutionMode executionMode) = 0;

    /// Direct scorer construction implemented by SegmentSources whose supplier
    /// delegates its build to the source.
    virtual Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) = 0;

    /// Advisory flag for callers that can optimize domain filtering. Return
    /// true only when every emitted doc is already within this SegmentSource's
    /// PrepareContext construction domain; false is always safe.
    virtual bool outputIsSubsetOfDomain() const noexcept { return false; }
  };

  /// Gives context to a Query (i.e. what index it's being used on amongst other things) when creating weights
  /// A Context is not generally thread-safe, so don't create weights from multiple threads with the same Context.
  class Context {
    std::unique_ptr<google::protobuf::Arena> standaloneArena;
    google::protobuf::Arena* allocationArena;

  public:
    using FieldInfoMap = boost::unordered_node_map<std::string_view, CachedFieldInfo, PackedTermHash, PackedTermEqual, MemPool::allocator<std::pair<const std::string_view, CachedFieldInfo>>>;

    struct Limits {
      int32_t fuzzyMaxExpansions = 10000;

      constexpr Limits(int32_t fuzzyMaxExpansions = 10000)
        : fuzzyMaxExpansions(fuzzyMaxExpansions) {}
    };

    MemPool& pool;
    IndexReader& topReader;
    std::shared_ptr<FilterCache::UseRegistry> filterUses;
    FilterKeyContext filterKeyContext;
    // Weight* top = nullptr;  // if we don't need a top-weight, we can reuse a Context for multiple queries in the same request.

    std::span<FieldReader> fieldReaders;
    FieldInfoMap fieldInfoMap;
    Limits limits;
    std::vector<api::Warning>* warnings = nullptr;

    // Ctor used by Context::create for arena allocation: the factory does the
    // work that can throw (pool allocation, FieldReader init, map bucket
    // allocation) and passes the results in, so this only binds/moves members.
    Context(google::protobuf::Arena& arena, MemPool& pool,
            IndexReader& topReader,
            std::span<FieldReader> fieldReaders, FieldInfoMap&& fieldInfoMap,
            Limits limits = {}, std::vector<api::Warning>* warnings = nullptr,
            FilterKeyContext filterKeyContext = {},
            std::shared_ptr<FilterCache::UseRegistry> filterUses = nullptr)
      : allocationArena(&arena), pool(pool), topReader(topReader),
        filterUses(std::move(filterUses)),
        filterKeyContext(filterKeyContext),
        fieldReaders(fieldReaders), fieldInfoMap(std::move(fieldInfoMap)),
        limits(limits), warnings(warnings) {
      this->filterKeyContext.coreGen = topReader.coreGen();
      this->filterKeyContext.fuzzyMaxExpansions = limits.fuzzyMaxExpansions;
      auto* filterCache = topReader.filterCache();
      if (this->filterUses == nullptr) {
        this->filterUses = std::make_shared<FilterCache::UseRegistry>(
            filterCache, topReader);
      }
    }

    // Convenience ctor for stack-allocated Contexts (tests, non-arena code):
    // does its own allocation/init inline.
    Context(MemPool& pool, IndexReader& topReader, Limits limits = {},
            std::vector<api::Warning>* warnings = nullptr,
            FilterKeyContext filterKeyContext = {},
            std::shared_ptr<FilterCache::UseRegistry> filterUses = nullptr)
      : standaloneArena(std::make_unique<google::protobuf::Arena>()),
        allocationArena(standaloneArena.get()),
        pool(pool), topReader(topReader), filterUses(std::move(filterUses)),
        filterKeyContext(filterKeyContext),
        fieldInfoMap(4, pool.getAllocator()),
        limits(limits), warnings(warnings) {
      this->filterKeyContext.coreGen = topReader.coreGen();
      this->filterKeyContext.fuzzyMaxExpansions = limits.fuzzyMaxExpansions;
      auto* filterCache = topReader.filterCache();
      if (this->filterUses == nullptr) {
        this->filterUses = std::make_shared<FilterCache::UseRegistry>(
            filterCache, topReader);
      }
      auto numSegs = topReader.segments().size();
      fieldReaders = {(FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader)), numSegs};
      for (size_t i = 0; i < numSegs; i++) {
        new (&fieldReaders[i]) FieldReader(topReader.segments()[i].postingsReader());
      }
    }

    static Context* create(google::protobuf::Arena* arena, MemPool& pool, IndexReader& topReader,
                           Limits limits = {}, std::vector<api::Warning>* warnings = nullptr,
                           FilterKeyContext filterKeyContext = {},
                           std::shared_ptr<FilterCache::UseRegistry> filterUses = nullptr) {
      auto numSegs = topReader.segments().size();
      auto* readers = (FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader));
      for (size_t i = 0; i < numSegs; i++) {
        new (&readers[i]) FieldReader(topReader.segments()[i].postingsReader());
      }
      FieldInfoMap map(4, pool.getAllocator());
      return luxir::arenaCreate<Context>(
        *arena, *arena, pool, topReader,
        std::span<FieldReader>(readers, numSegs), std::move(map),
        limits, warnings, filterKeyContext, std::move(filterUses));
    }

    google::protobuf::Arena& arena() const noexcept {
      return *allocationArena;
    }

    FilterCache::Use* getFilterUse(
        const Query& query,
        FilterCache::AdmissionLane lane =
            FilterCache::AdmissionLane::CLAUSE) {
      if (filterUses == nullptr) return nullptr;
      FilterKeyBuilder builder;
      FilterKeyScope scope = query.appendFilterKey(builder, filterKeyContext);
      auto key = std::move(builder).finish(scope, filterKeyContext);
      return key ? filterUses->get(*key, scope, lane) : nullptr;
    }

    // Shape-specific consumers can require a cache lifetime without letting a
    // rejected scope create admission traffic. The key is structural work only;
    // the registry is touched after the scope predicate accepts it.
    FilterCache::Use* getFilterUse(
        const Query& query, FilterKeyScope requiredScope,
        FilterCache::AdmissionLane lane) {
      if (filterUses == nullptr) return nullptr;
      FilterKeyBuilder builder;
      FilterKeyScope scope = query.appendFilterKey(builder, filterKeyContext);
      if (scope != requiredScope) return nullptr;
      auto key = std::move(builder).finish(scope, filterKeyContext);
      return key ? filterUses->get(*key, scope, lane) : nullptr;
    }

    std::optional<FilterCache::ExistingCandidate> lookupExistingFilterUse(
        const Query& query,
        FilterKeyScope requiredScope = FilterKeyScope::SEGMENT_STABLE) {
      if (filterUses == nullptr) return std::nullopt;
      FilterKeyBuilder builder;
      FilterKeyScope scope = query.appendFilterKey(builder, filterKeyContext);
      if (scope != requiredScope) return std::nullopt;
      auto key = std::move(builder).finish(scope, filterKeyContext);
      if (!key) return std::nullopt;
      return filterUses->lookupExisting(*key, scope);
    }

    FilterCache::Use* acceptExistingFilterUse(
        FilterCache::ExistingCandidate&& candidate,
        FilterCache::AdmissionLane lane) {
      return filterUses == nullptr ? nullptr
          : filterUses->acceptExisting(std::move(candidate), lane);
    }

    // code must have static storage duration.
    void warn(std::string_view code, std::string_view message) {
      if (warnings == nullptr) return;
      char* copy = pool.alloc(message.size());
      std::memcpy(copy, message.data(), message.size());
      warnings->push_back({code, std::string_view(copy, message.size())});
    }

    // return number of segments
    size_t numSegments() const noexcept {
      return topReader.segments().size();
    }

    std::span<SegFieldInfo*> readSegInfos(std::string_view field) {
      auto numSegs = numSegments();
      int foundCount = 0;
      auto savepoint = pool.getSavePoint();
      std::span<SegFieldInfo*> segInfos = {pool.make_arr<SegFieldInfo*>(numSegs), numSegs};
      for (int i = 0; i < numSegs; ++i) {
        if (fieldReaders[i].seek(field)) {
          segInfos[i] = pool.make<SegFieldInfo>();
          fieldReaders[i].readFieldInfo(*segInfos[i]);
          foundCount++;
        } else {
          segInfos[i] = nullptr;
        }
      }
      if (foundCount == 0) {
        // no segments have this field, so we can rewind the pool to deallocate the arr
        pool.rewind(savepoint);
        return {};
      }
      return segInfos;
    }

    /// Returns nullptr if the field is not found in any segment
    CachedFieldInfo* getCachedFieldInfo(std::string_view field) {
      auto [iter, inserted] = fieldInfoMap.try_emplace(field, pool, 4);
      CachedFieldInfo& result = iter->second;
      if (!inserted) {
        return &result;
      }

      result.segInfos = readSegInfos(field);
      if (result.segInfos.empty()) {
        // field doesn't exist in any segment.
        fieldInfoMap.erase(iter);
        // we can't rewind the pool here because we still emplaced on the map.  We could do a lookup first if it's important.
        return nullptr;
      }

      // TODO: make caching of the terms enums optional?
      auto numSegs = numSegments();
      result.termsEnums = {pool.make_arr<TermsEnum *>(numSegs), numSegs};
      for (int i = 0; i < numSegs; ++i) {
        auto &segFieldInfo = result.segInfos[i];
        if (!segFieldInfo) {
          result.termsEnums[i] = nullptr;
          continue;
        }
        TermsEnum *termsEnum = pool.make<TermsEnum>(pool, topReader.segments()[i].postingsReader(), *segFieldInfo);
        result.termsEnums[i] = termsEnum;

        // add the stats from this termsEnum to fieldStats
        result.fieldStats.sumTotalTermFreq += termsEnum->sumTotalTermFreq();
        result.fieldStats.sumDocFreq += termsEnum->sumDocFreq();
        result.fieldStats.docsWithField += termsEnum->docsWithField();
      }

      return &result;
    }

    CachedTermInfo* getCachedTerminfo(CachedFieldInfo& cachedFieldInfo, std::string_view term) {
      auto [iter, inserted] = cachedFieldInfo.termInfos.try_emplace(term);
      CachedTermInfo& result = iter->second;
      if (!inserted) {
        return &result;
      }

      auto savepoint = pool.getSavePoint();
      auto numSegs = numSegments();
      int foundInSegCount = 0;
      result.postingsStates = {
          pool.make_arr<const TermsEnum::PostingsState*>(numSegs), numSegs};
      for (int i = 0; i < numSegs; ++i) {
        auto& termsEnum = cachedFieldInfo.termsEnums[i];
        if (!termsEnum || !termsEnum->seek(term)) {
          result.postingsStates[i] = nullptr;
          continue;
        }
        foundInSegCount++;
        auto* state = pool.make<TermsEnum::PostingsState>(termsEnum->postingsState());
        result.postingsStates[i] = state;
        result.termStats.docFreq += state->docFreq;
        result.termStats.totalTermFreq += state->totalTermFreq;
      }

      if (foundInSegCount == 0) {
        pool.rewind(savepoint);
        cachedFieldInfo.termInfos.erase(iter);
        return nullptr;
      }

      return &result;
    }

    bool lookupTermStats(CachedFieldInfo& cachedFieldInfo, std::string_view term, Similarity::TermStats& result) {
      result = {};
      bool found = false;
      for (int i = 0; i < (int)numSegments(); ++i) {
        auto& termsEnum = cachedFieldInfo.termsEnums[i];
        if (!termsEnum || !termsEnum->seek(term)) {
          continue;
        }
        found = true;
        result.docFreq += termsEnum->docFreq();
        result.totalTermFreq += termsEnum->totalTermFreq();
      }
      return found;
    }

    bool lookupTermStats(std::string_view field, std::string_view term, Similarity::TermStats& result) {
      CachedFieldInfo* cachedFieldInfo = getCachedFieldInfo(field);
      if (cachedFieldInfo == nullptr) {
        result = {};
        return false;
      }
      return lookupTermStats(*cachedFieldInfo, term, result);
    }

  };

  // A weight is created by a query for execution over a specific index
  // It does not have a virtual destructor, so subclasses should be made trivially destructible.
  class Weight : public Query::SegmentSource {
  protected:
    Query::Context& context;
    // Input flags passed to createWeight(), such as NEED_SCORES. These are
    // construction directives; public callers should use traits for properties
    // computed by the weight.
    int32_t inputFlags;
    // Execution traits computed at construction. Default 0 is conservative:
    // no prepare pass and no constant-scoring fast path.
    int32_t traits = 0;
  public:
    enum class SparseFilteredTopKFamily {
      CONJUNCTION,
      UNION,
    };

    Weight(Query::Context& context, int32_t inputFlags) : context(context), inputFlags(inputFlags) {}

    /// Per-segment domains available during prepare(). An empty domain span
    /// means unrestricted aside from whatever the caller applies later.
    ///
    /// Domain contract: a segment's domain, when present, is LIVE-FILTERED -
    /// it is liveDocs intersected with any enclosing filter clauses, and is
    /// the complete eligibility predicate (consumers must not re-check
    /// liveDocs).  A null domain means the segment has no deletes and no
    /// enclosing filters; all docs in the segment are eligible.  RootOp
    /// establishes this, and every domain-deriving path preserves it.
    ///
    /// parallel is true when the request runs under a TBB task group; a
    /// prepare() implementation may then spawn internal worker tasks, provided
    /// they are joined before prepare() returns.  false means the request is
    /// serial and prepare() must not spawn tasks.
    struct PrepareContext {
      IndexReader& reader;
      std::span<DocSet* const> domainPerSeg;
      bool parallel;

      PrepareContext(IndexReader& reader, std::span<DocSet* const> domainPerSeg,
                     bool parallel) noexcept
        : reader(reader), domainPerSeg(domainPerSeg), parallel(parallel) {}
    };

    /// Immutable result of prepare() for segment-scorer creation after
    /// whole-index work has completed.
    class PreparedWeight : public Query::SegmentSource {
    public:
      /// Create a scorer from prepared state. Follows the same threading and
      /// allocation rules as Weight::createScorer().
      virtual Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) = 0;

      /// Advisory flag for callers that can optimize domain filtering. Return
      /// true only when every emitted doc is already within this
      /// PreparedWeight's PrepareContext construction domain; false is always
      /// safe.
      bool outputIsSubsetOfDomain() const noexcept override { return false; }

      /// Cache provenance for materialized output. The conservative default
      /// prevents a prepared result from being published under a query-only
      /// key unless the implementation affirmatively declares it canonical.
      virtual PreparedDomainDependence domainDependence() const noexcept {
        return PreparedDomainDependence::PREPARE_DOMAIN;
      }
      virtual ~PreparedWeight() = default;
    };

    /// Execution traits computed once at construction. These are a distinct
    /// bit-space from createWeight() input flags: input flags say what the
    /// parent requested, traits say what is true of the built weight.
    static constexpr int32_t NEEDS_PREPARE = 1 << 0;        // needs a whole-index prepare() pass
    static constexpr int32_t IS_CONSTANT_SCORING = 1 << 1;  // every matching doc scores the same
    static constexpr int32_t PREFER_PULL_FOR_SPARSE_ARRAY_DOMAIN = 1 << 2;
    static constexpr int32_t MATCHES_ALL_DOCS = 1 << 3;     // matches every doc in the segment
    // The query shape can supply an exact unscored count plus either a
    // competitively pruned scoring pass or a bounded constant-score first-K
    // pass. Profitability and exact-count product availability remain
    // per-segment planner decisions.
    static constexpr int32_t CAN_COMPOSE_EXACT_COUNT_TOPK = 1 << 4;

    /// Raw execution trait bitmask.
    int32_t getFlags() const noexcept { return traits; }

    /// True if this Weight needs prepare() before segment scorer creation.
    bool needsPrepare() const noexcept { return (traits & NEEDS_PREPARE) != 0; }

    /// True if every matching doc receives the same score. Leaf terms/phrases
    /// set this when built without NEED_SCORES; wrappers and compounds set it
    /// from their scoring semantics. Advisory: false is always safe.
    bool isConstantScoring() const noexcept { return (traits & IS_CONSTANT_SCORING) != 0; }

    bool prefersPullForSparseArrayDomain() const noexcept {
      return (traits & PREFER_PULL_FOR_SPARSE_ARRAY_DOMAIN) != 0;
    }

    bool canComposeExactCountTopK() const noexcept {
      return (traits & CAN_COMPOSE_EXACT_COUNT_TOPK) != 0;
    }

    /// True when every doc in the segment matches, so the domain is already
    /// the match set: callers can take the hit count from the domain's
    /// cardinality and hand the domain straight to sub-ops without iterating.
    /// This is a match-set property only - it says nothing about scores, so a
    /// caller that also wants to shortcut RANKING must check isConstantScoring
    /// (a rescore over a match-all matches everything but reorders it).
    /// Advisory: false is always safe.
    bool matchesAllDocs() const noexcept { return (traits & MATCHES_ALL_DOCS) != 0; }

    /// True when the request permits scorer-level competitive pruning.
    bool allowsPruning() const noexcept { return (inputFlags & ALLOW_PRUNING) != 0; }
    bool needsScores() const noexcept { return (inputFlags & NEED_SCORES) != 0; }

    /// Exact O(1) per-segment membership count without constructing a
    /// supplier. A null result means the caller must execute the query.
    virtual std::optional<int64_t> constantCount(
        IndexReader::Segment& segment, DocSet* domain) {
      unused(segment, domain);
      return std::nullopt;
    }

    /// Policy family for choosing the sparse-filtered top-k density knee.
    /// Existing and ineligible weights retain the conjunction-family default.
    virtual SparseFilteredTopKFamily sparseFilteredTopKFamily() const noexcept {
      return SparseFilteredTopKFamily::CONJUNCTION;
    }

    /// A-priori cost of the folded filter clause for a scored query that may
    /// profitably abandon top-k pruning, or -1 when this weight's shape is not
    /// eligible. Implementations must inspect suppliers only: this planner hook
    /// runs before request execution and must not populate filter caches.
    virtual int64_t sparseFilteredTopKCost(
        MemPool& target, IndexReader::Segment& segment) {
      unused(target, segment);
      return -1;
    }

    /// Optional execution-time preparation for weights that need the domain for
    /// all segments before they can create a scorer for any individual segment,
    /// such as shard-level kNN over a filtered domain.
    ///
    /// Threading contract: prepare() may run on TBB worker tasks, concurrently
    /// with prepare() for other weights from the same request. Implementations
    /// must not mutate Query::Context state or allocate from context.pool here:
    /// Context and its MemPool are request-scoped but not synchronized. Use
    /// stack, std containers, a local MemPool, or thread-local MemPool guards
    /// for prepare-only temporaries. Any state needed after prepare() returns
    /// must be owned by the returned PreparedWeight or another synchronized
    /// request object.
    ///
    /// The returned PreparedWeight may later be asked to create per-segment
    /// scorers from segment tasks, so createScorer() should treat its stored
    /// prepared state as read-only.
    virtual std::unique_ptr<PreparedWeight> prepare(PrepareContext& ctx) {
      unused(ctx);
      return nullptr;
    }

    /// Create a scorer for a specific segment in the specific MemPool. Can
    /// return null if no docs match. This may run concurrently on worker tasks:
    /// allocate scorer state from target, and do not allocate from context.pool
    /// or populate/mutate Query::Context caches.
    virtual Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) = 0;

    /// Return a constant-count plan result, or -1 when this segment requires
    /// execution to count.
    virtual int64_t count(IndexReader::Segment& segment) {
      auto guard = MemPool::threadLocalPoolGuard();
      ScorerSupplier* supplier = scorerSupplier(guard.pool(), segment);
      if (supplier == nullptr) return 0;
      ScorerSupplier::BulkScorerContext constantContext;
      constantContext.requireConstantCount = true;
      auto constantPlan = supplier->planBulk(
          ExecutionUse::COUNT_WINDOWS, constantContext);
      if (constantPlan.hasConstantCount()) {
        return constantPlan.constantCount;
      }
      return -1;
    }

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };

  // NOTE: no virtual destructor, so subclasses of Scorer should be made trivially destructible
  class Scorer {
  public:
    virtual int32_t next() = 0;
    virtual int32_t advance(int32_t target) {
      // Strict advance: callers must pass a target beyond the current doc.
      assert(docId() < target);
      int32_t doc;
      while ((doc = next()) < target) {}
      return doc;
    }
    /// doc we are positioned on
    virtual int32_t docId() = 0;
    /// Iteration contract: a consumer picks exactly one protocol for a scorer
    /// lifetime. Exact iteration uses next()/advance(). A plan that selects
    /// private two-phase iteration instead drives
    /// approximation*()+matches(), while external two-phase iteration drives
    /// every enum returned by approximationEnums() and calls matchesAt(). The
    /// protocols must not be mixed. A non-empty approximationEnums() exposes
    /// distinct iterators
    /// whose conjunction is an externally drivable superset of the scorer's
    /// exact matches. It need not be the same approximation used by
    /// approximation*(): wrappers may expose a child's iterators and retain
    /// dynamic restrictions for matchesAt(). A consumer may drive the enums
    /// and call matchesAt() only after all are on that doc. A successful
    /// matchesAt(doc) leaves docId()==doc and score state ready. Verification
    /// must be idempotent for the current doc, or the consumer must call it at
    /// most once per approximation doc. score() is only valid after a
    /// successful match.
    virtual int32_t approximationNext() {
      return next();
    }
    virtual int32_t approximationAdvance(int32_t target) {
      return advance(target);
    }
    virtual int32_t approximationDocId() {
      return docId();
    }
    virtual std::span<DocsPosEnum*> approximationEnums() {
      return {};
    }
    virtual bool matches() {
      return true;
    }
    virtual bool matchesAt(int32_t doc) {
      unused(doc);
      assert(false);
      std::unreachable();
    }
    /// Flat SHOULD clauses that this scorer merges as a disjunction. Consumers
    /// may decompose the scorer only when they understand every returned child;
    /// an empty span keeps the scorer opaque.
    virtual std::span<Scorer*> flatDisjunctionScorers() {
      return {};
    }
    /// Flat MUST clauses whose conjunction exactly matches this scorer without
    /// verification. Consumers may decompose the scorer only when they
    /// understand every returned child; an empty span keeps it opaque.
    virtual std::span<Scorer*> flatConjunctionScorers() {
      return {};
    }
    /// Record that an exclusion builder committed to this scorer's window
    /// fill protocol. Most scorers have no per-decision instrumentation.
    virtual void recordWindowFilterCommit(bool supported) const {
      unused(supported);
    }
    /// Optional docs-only probe specialization. WindowFilter falls back to
    /// exact advance() when an opted-in scorer does not expose one.
    virtual DocsFreqEnum* windowFilterProbeDocsEnum() {
      return nullptr;
    }
    virtual float matchCost() {
      return 0.0f;
    }
    /// term frequency for current doc
    virtual float score() = 0;
    /// Sweep this single-phase scorer over a sorted candidate buffer.
    ///
    /// For every candidate doc that this scorer matches, add score() into the
    /// parallel scores[] slot. When required is false the candidate buffer is
    /// left in place and size is returned. When required is true, non-matching
    /// docs are removed from docs[]/scores[] in-place and the compacted size is
    /// returned.
    ///
    /// Protocol rule: the default implementation drives exact advance()+score()
    /// for this scorer lifetime. Do not call it on a scorer that is already
    /// being consumed through approximation*()+matches() unless this method is
    /// first made explicitly two-phase-aware.
    virtual int32_t applyToCandidates(int32_t* docs, float* scores,
                                      int32_t size, bool required) {
      assert(size >= 0);
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        int32_t target = docs[i];
        if (docId() < target) {
          advance(target);
        }
        bool matched = docId() == target;
        if (matched) {
          scores[i] += score();
        }
        if (!required) {
          continue;
        }
        if (matched) {
          if (write != i) {
            docs[write] = docs[i];
            scores[write] = scores[i];
          }
          write++;
        }
      }
      return required ? write : size;
    }
    /// Fill docs/scores from the current positioned doc while docid < upTo.
    /// The scorer is advanced after each emitted doc, so repeated calls continue
    /// at the first unfilled doc.
    virtual int32_t fillScoreBlock(int32_t* docs, float* scores, int32_t count, int32_t upTo) {
      assert(count >= 0);
      int32_t filled = 0;
      int32_t doc = docId();
      if (doc < 0) {
        doc = next();
      }
      while (filled < count && doc < upTo) {
        docs[filled] = doc;
        scores[filled] = score();
        filled++;
        doc = next();
      }
      return filled;
    }
    virtual void fillWindowBits(std::span<uint64_t> windowBits, int32_t windowStart,
                                int32_t windowEnd) {
      skipCount(SkipStats::countBulkFillCalls);
      if (windowEnd <= windowStart) {
        return;
      }
      int32_t doc = docId();
      if (doc < windowStart) {
        doc = advance(windowStart);
      }
      unused(doc);
      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      int32_t n;
      while ((n = fillScoreBlock(blockDocs, blockScores,
                                 Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
        for (int32_t i = 0; i < n; i++) {
          int32_t index = blockDocs[i] - windowStart;
          windowBits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
        }
      }
    }
    virtual void setMinCompetitiveScore(float minScore) {
      unused(minScore);
    }
    virtual float getMaxScore(int32_t upTo) {
      unused(upTo);
      return std::numeric_limits<float>::infinity();
    }
    virtual float refineMaxScore(int32_t upTo) {
      return getMaxScore(upTo);
    }
    virtual ScoreBounds getScoreBounds(int32_t upTo) {
      unused(upTo);
      return ScoreBounds::unknown();
    }
    virtual ScoreBounds refineScoreBounds(int32_t upTo) {
      return getScoreBounds(upTo);
    }
    virtual int32_t advanceShallow(int32_t target) {
      unused(target);
      return PostingsReader::END;
    }
    virtual float getMaxScoreForSetup(int32_t upTo) {
      skipCount(SkipStats::maxScoreSetupFallbackBlockBounds);
      return getMaxScore(upTo);
    }
    virtual int32_t advanceShallowForSetup(int32_t target) {
      skipCount(SkipStats::maxScoreSetupFallbackBlockBounds);
      return advanceShallow(target);
    }
    virtual std::string_view pruningBlockerForDebug() const {
      return {};
    }

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };

  /// A segment-pool-owned, single-consumption scorer construction token.
  /// Resolving records the exact shape, demand, and supplier cost before any
  /// scorer is built. A parent may retain the token, but must consume it at
  /// most once and before the segment pool is rewound.
  // NOTE: no virtual destructor; plans are pool-owned and never deleted
  // through this type.
  class ScorerPlan {
    PlanContext planContext;
    ScorerShape recordedShape;
    int64_t resolvedCost;
    bool consumed = false;

    void consume() {
      assert(!consumed);
      consumed = true;
    }

  protected:
    ScorerPlan(const PlanContext& planContext,
               const ScorerShape& recordedShape, int64_t resolvedCost)
      : planContext(planContext), recordedShape(recordedShape),
        resolvedCost(resolvedCost) {}

    virtual Scorer* buildScorer(MemPool& targetPool) = 0;
    virtual Scorer* buildIndependentScorer(MemPool& targetPool) {
      unused(targetPool);
      return nullptr;
    }
    virtual DocsOnlyEnum* buildDocsOnlyEnum(MemPool& targetPool) {
      unused(targetPool);
      return nullptr;
    }

    const PlanContext& context() const noexcept { return planContext; }

  public:
    ScorerPlan(const ScorerPlan&) = delete;
    ScorerPlan& operator=(const ScorerPlan&) = delete;
    ScorerPlan(ScorerPlan&&) = delete;
    ScorerPlan& operator=(ScorerPlan&&) = delete;

    const ScorerShape& shape() const noexcept { return recordedShape; }
    const Demand& demand() const noexcept { return planContext.demand; }
    int64_t cost() const noexcept { return resolvedCost; }

    Scorer* build(MemPool& targetPool) {
      consume();
      return buildScorer(targetPool);
    }

    Scorer* buildIndependent(MemPool& targetPool) {
      consume();
      return buildIndependentScorer(targetPool);
    }

    DocsOnlyEnum* buildDocsOnly(MemPool& targetPool) {
      consume();
      return buildDocsOnlyEnum(targetPool);
    }
  };

  /// Base for scorers whose every match scores the same constant: a flat,
  /// exact bound with no shallow structure. Once the collector's floor rises
  /// above the constant no remaining doc can compete (ties stay competitive,
  /// same convention as impact skipping), so setMinCompetitiveScore calls
  /// exhaust(). The hint is advisory: subclasses that can end future
  /// iteration for free override exhaust() to clamp an existing bound (the
  /// current position stays valid; only future iteration ends). The default
  /// ignores it - a per-call exhausted test on the hot iteration paths costs
  /// far more than the latch ever saves (measured on the zone-map and
  /// full-scan range arms).
  class ConstantScorer : public Scorer {
  protected:
    float constantScore;

    explicit ConstantScorer(float constantScore) : constantScore(constantScore) {}

    virtual void exhaust() {}

  public:
    float score() override { return constantScore; }
    void setMinCompetitiveScore(float minScore) override {
      if (minScore > constantScore) exhaust();
    }
    float getMaxScore(int32_t upTo) override {
      unused(upTo);
      return constantScore;
    }
    ScoreBounds getScoreBounds(int32_t upTo) override {
      unused(upTo);
      return ScoreBounds::exact(constantScore);
    }
    float getMaxScoreForSetup(int32_t upTo) override {
      unused(upTo);
      return constantScore;
    }
    int32_t advanceShallowForSetup(int32_t target) override {
      unused(target);
      return PostingsReader::END;
    }
  };
};

// Lazy intersection of direct filter scorers over one L1-sized
// production window. Unlike DocSet this has no segment-wide identity or
// cardinality: accepts() is valid only for the most recently prepared window.
class WindowFilter {
  static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
  static constexpr int32_t kWindowWords = kWindowSize / 64;
  static_assert((kWindowSize % 64) == 0);

  std::span<Query::Scorer*> scorers;
  std::span<DocsFreqEnum*> probeEnums;
  std::span<uint64_t> currentBits;
  std::span<uint64_t> scratchBits;
  int32_t windowStart = 0;
  int32_t windowEnd = 0;
  bool probeMode;
  // Segment-wide COST ESTIMATE of the filter (from its supplier), which is a
  // different thing from this class's deliberate lack of windowed cardinality:
  // consumers use it to price whole-segment routing, never to answer accepts().
  int64_t filterCost = 0;

public:
  WindowFilter(MemPool& pool, std::span<Query::Scorer*> scorers,
               bool probeMode = false, int64_t filterCost = 0)
      : scorers(scorers),
        probeEnums(pool.make_span<DocsFreqEnum*>(scorers.size())),
        currentBits(pool.make_arr<uint64_t>((size_t) kWindowWords),
                    (size_t) kWindowWords),
        scratchBits(pool.make_arr<uint64_t>((size_t) kWindowWords),
                    (size_t) kWindowWords),
        probeMode(probeMode), filterCost(filterCost) {
    assert(!scorers.empty());
    for (size_t i = 0; i < scorers.size(); i++) {
      probeEnums[i] = scorers[i]->windowFilterProbeDocsEnum();
    }
  }

  bool probes() const {
    return probeMode;
  }

  int64_t cost() const {
    return filterCost;
  }

  int32_t prepare(int32_t start, int32_t end) {
    assert(start >= 0);
    assert(end >= start);
    assert(end - start <= kWindowSize);
    windowStart = start;
    windowEnd = end;

    std::fill(currentBits.begin(), currentBits.end(), 0);
    scorers[0]->fillWindowBits(currentBits, start, end);
    for (size_t i = 1; i < scorers.size(); i++) {
      std::fill(scratchBits.begin(), scratchBits.end(), 0);
      scorers[i]->fillWindowBits(scratchBits, start, end);
      for (size_t word = 0; word < currentBits.size(); word++) {
        currentBits[word] &= scratchBits[word];
      }
    }

    int32_t card = 0;
    for (uint64_t bits : currentBits) {
      card += (int32_t) std::popcount(bits);
    }
    return card;
  }

  bool accepts(int32_t doc) const {
    assert(doc >= windowStart && doc < windowEnd);
    int32_t index = doc - windowStart;
    return (currentBits[(size_t) (index >> 6)]
            & (1ULL << (index & 63))) != 0;
  }

  // Candidates must arrive in ascending order. Each filter scorer advances
  // monotonically, so a later fill resumes from the resulting cursor without
  // materializing any skipped probe window. Term scorers expose their
  // postings enum for the docs-only fast path; other capable scorers use exact
  // Scorer::advance().
  bool acceptsProbe(int32_t doc) {
    assert(probeEnums.size() == scorers.size());
    for (size_t i = 0; i < scorers.size(); i++) {
      DocsFreqEnum* docsEnum = probeEnums[i];
      int32_t filterDoc = docsEnum == nullptr
          ? scorers[i]->docId() : docsEnum->docId();
      if (filterDoc < doc) {
        filterDoc = docsEnum == nullptr
            ? scorers[i]->advance(doc) : docsEnum->advanceDocOnly(doc);
      }
      if (filterDoc != doc) {
        return false;
      }
    }
    return true;
  }

  void intersect(std::span<uint64_t> bits) const {
    assert(bits.size() == currentBits.size());
    for (size_t word = 0; word < bits.size(); word++) {
      bits[word] &= currentBits[word];
    }
  }
};

} // end namespace
