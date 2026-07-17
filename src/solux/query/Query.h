#pragma once

#include <cstring>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <solux/util/heap.h>
#include "solux/api/solux_types.hpp"
#include "solux/util/MemPool.h"
#include "solux/util/proto.h"
#include "solux/util/StrRef.h"
#include "solux/search/IndexReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/reader/DocsEnum.h"
#include "solux/search/DocSet.h"
#include "solux/search/Similarity.h"
#include <boost/unordered/unordered_node_map.hpp>
#include <google/protobuf/arena.h>

namespace solux {

class DocSet;
class DocSetBuilder;

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
  // Produce the next window of verified competitive candidates in [min, max),
  // intersected with filter (null = all), filtered by minCompetitiveScore.
  // Returns the docid to resume from (first window not produced), or PostingsReader::END.
  // The spans in out are valid until the next call.
  virtual int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                                  float minCompetitiveScore) = 0;

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
// Execution follows Lucene's shape: Query -> Weight -> Scorer, plus one
// addition, prepare(), for queries that need a whole-index pass before
// per-segment scoring.
//
//   Query   : the user's query, independent of any index.
//   Weight  : created by a Query for one IndexReader (carried by Query::Context)
//             via createWeight(); holds index-level state (term stats, cached
//             enums, and child Weights for compound queries like BooleanQuery).
//   SegmentSource
//           : execution-facing source implemented by Weight and PreparedWeight.
//   ScorerSupplier
//           : temporary segment-local planning state that creates a Scorer.
//   Scorer  : created by a ScorerSupplier for a single segment; iterates that
//             segment's matching docs.
//
// Two phases, with different threading and allocation rules. Getting these
// wrong can be a data race, so they are part of the contract:
//
// 1. Build: single-threaded, before search tasks are dispatched.
//    createWeight() walks the Query tree and builds the Weight tree.  Weight
//    ctors may read/populate the shared Query::Context (field/term caches) and
//    allocate from Context::pool (the per-request MemPool).  Safe ONLY because
//    it runs before any task is dispatched.
//
// 2. Execute: parallel, on the TBB task pool.
//    a. prepare(): optional.  A Weight with the NEEDS_PREPARE trait is prepared
//       before its segment scorers are created.  This is for any weight that
//       must resolve state across the whole index before per-segment scorers
//       exist: a leaf computing an index-level result (e.g. KnnQuery), or a
//       compound query (e.g. BooleanQuery) that must prepare children after
//       deriving their per-segment domains.
//       prepare() returns an immutable PreparedWeight holding the whole-index
//       result; scorerSupplier() then reads from it per segment.  Compound
//       weights set NEEDS_PREPARE from child traits and recursively prepare
//       children, threading per-segment domains down.
//    b. scorerSupplier(targetPool, segment): called as needed for a segment,
//       often from parallel tasks.  The supplier's get() creates the Scorer.
//
//    Because (a) and (b) can run on worker threads, they MUST NOT allocate from
//    Context::pool or populate/mutate the Context caches (both are
//    single-thread-only, populated in phase 1).  They may read immutable state
//    built in phase 1.  scorerSupplier() and Scorer creation allocate from the
//    per-call targetPool; prepare() uses stack/std containers, local or
//    thread-local MemPools, or heap allocations for scratch, and returns state
//    owned by the PreparedWeight.
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
  // This is mostly a helper class since it was hard to get the types right the first time.
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

  /// Construct an independent DocsEnum from the immutable state captured by the
  /// original term seek. No dictionary re-seek or shared mutable enum is involved.
  DocsEnum* useDocsEnum(MemPool& targetPool, IndexReader::Segment& segment,
                        bool trackPositions = true) {
    auto* state = postingsStates[segment.ord];
    if (state == nullptr) {
      return nullptr;
    }
    auto* docsEnum = targetPool.make<DocsEnum>(targetPool, *state);
    docsEnum->setTrackPositions(trackPositions);
    return docsEnum;
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
class Query {
public:
  class Context;
  class Weight;
  class Scorer;
  class ScorerSupplier;
  class SegmentSource;

  /// Input flags for createWeight(). NEED_SCORES is propagated down the query
  /// tree and cleared for clauses whose score the parent never reads.
  static constexpr int32_t NEED_SCORES = 1;
  /// The request may skip non-competitive matches because it does not require
  /// an exact hit count. Propagated independently of NEED_SCORES.
  static constexpr int32_t ALLOW_PRUNING = 1 << 1;

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

  /// Per-segment planning state. Suppliers are allocated from the segment-local
  /// targetPool and only need to live until their parent has called get().
  // NOTE: no virtual destructor, so subclasses should not be owned or deleted through this type.
  class ScorerSupplier {
  public:
    /// Estimated number of matching docs in this segment. This should be cheap
    /// to compute; an upper bound is safe. Compound suppliers use it to choose
    /// lead iterators before creating scorers.
    virtual int64_t cost() = 0;

    /// Create the scorer. leadCost is the estimated cost of the parent-selected
    /// lead iterator that will drive this scorer, or INT64_MAX when there is no
    /// lead constraint. Suppliers may use it to choose eager vs lazy setup.
    virtual Query::Scorer* get(MemPool& targetPool, int64_t leadCost) = 0;

    virtual BulkScorer* bulkScorer(MemPool& targetPool) {
      unused(targetPool);
      return nullptr;
    }
  };

  // NOTE: no virtual destructor, so subclasses should not be owned or deleted through this type.
  class SegmentSource {
  public:
    /// Return temporary per-segment planning state allocated from targetPool.
    /// A null supplier means this source cannot match the segment.
    virtual Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment);

    /// Compatibility hook for existing scorer implementations. New call sites
    /// should go through scorerSupplier(); the default supplier delegates here.
    virtual Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) = 0;

    /// Advisory flag for callers that can optimize domain filtering. Return
    /// true only when every emitted doc is already within the PrepareContext
    /// domain used to build this SegmentSource; false is always safe.
    virtual bool outputIsSubsetOfDomain() const noexcept { return false; }
  };

  /// Gives context to a Query (i.e. what index it's being used on amongst other things) when creating weights
  /// A Context is not generally thread-safe, so don't create weights from multiple threads with the same Context.
  class Context {
  public:
    using FieldInfoMap = boost::unordered_node_map<std::string_view, CachedFieldInfo, PackedTermHash, PackedTermEqual, MemPool::allocator<std::pair<const std::string_view, CachedFieldInfo>>>;

    struct Limits {
      int32_t fuzzyMaxExpansions = 10000;

      constexpr Limits(int32_t fuzzyMaxExpansions = 10000)
        : fuzzyMaxExpansions(fuzzyMaxExpansions) {}
    };

    MemPool& pool;
    IndexReader& topReader;
    // Weight* top = nullptr;  // if we don't need a top-weight, we can reuse a Context for multiple queries in the same request.

    std::span<FieldReader> fieldReaders;
    FieldInfoMap fieldInfoMap;
    Limits limits;
    std::vector<api::Warning>* warnings = nullptr;

    // Ctor used by Context::create for arena allocation: the factory does the
    // work that can throw (pool allocation, FieldReader init, map bucket
    // allocation) and passes the results in, so this only binds/moves members.
    // (solux::arenaCreate would make a throwing arena ctor safe now; the factory
    // split is kept as structure, not a safety requirement.)
    Context(MemPool& pool, IndexReader& topReader,
            std::span<FieldReader> fieldReaders, FieldInfoMap&& fieldInfoMap,
            Limits limits = {}, std::vector<api::Warning>* warnings = nullptr)
      : pool(pool), topReader(topReader),
        fieldReaders(fieldReaders), fieldInfoMap(std::move(fieldInfoMap)),
        limits(limits), warnings(warnings) {
    }

    // Convenience ctor for stack-allocated Contexts (tests, non-arena code):
    // does its own allocation/init inline.
    Context(MemPool& pool, IndexReader& topReader, Limits limits = {},
            std::vector<api::Warning>* warnings = nullptr)
      : pool(pool), topReader(topReader), fieldInfoMap(4, pool.getAllocator()),
        limits(limits), warnings(warnings) {
      auto numSegs = topReader.segments().size();
      fieldReaders = {(FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader)), numSegs};
      for (size_t i = 0; i < numSegs; i++) {
        new (&fieldReaders[i]) FieldReader(pool, topReader.segments()[i].postingsReader());
      }
    }

    static Context* create(google::protobuf::Arena* arena, MemPool& pool, IndexReader& topReader,
                           Limits limits = {}, std::vector<api::Warning>* warnings = nullptr) {
      auto numSegs = topReader.segments().size();
      auto* readers = (FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader));
      for (size_t i = 0; i < numSegs; i++) {
        new (&readers[i]) FieldReader(pool, topReader.segments()[i].postingsReader());
      }
      FieldInfoMap map(4, pool.getAllocator());
      return solux::arenaCreate<Context>(
        *arena, pool, topReader, std::span<FieldReader>(readers, numSegs), std::move(map),
        limits, warnings);
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

    /// Immutable result of prepare(), used to create segment scorers after
    /// whole-index work has completed.
    class PreparedWeight : public Query::SegmentSource {
    public:
      /// Create a scorer from prepared state. Follows the same threading and
      /// allocation rules as Weight::createScorer().
      virtual Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) = 0;

      /// Advisory flag for callers that can optimize domain filtering. Return
      /// true only when every emitted doc is already within the PrepareContext
      /// domain used to build this PreparedWeight; false is always safe.
      bool outputIsSubsetOfDomain() const noexcept override { return false; }
      virtual ~PreparedWeight() = default;
    };

    /// Execution traits computed once at construction. These are a distinct
    /// bit-space from createWeight() input flags: input flags say what the
    /// parent requested, traits say what is true of the built weight.
    static constexpr int32_t NEEDS_PREPARE = 1 << 0;        // needs a whole-index prepare() pass
    static constexpr int32_t IS_CONSTANT_SCORING = 1 << 1;  // every matching doc scores the same

    /// Raw execution trait bitmask.
    int32_t getFlags() const noexcept { return traits; }

    /// True if this Weight needs prepare() before segment scorer creation.
    bool needsPrepare() const noexcept { return (traits & NEEDS_PREPARE) != 0; }

    /// True if every matching doc receives the same score. Leaf terms/phrases
    /// set this when built without NEED_SCORES; wrappers and compounds set it
    /// from their scoring semantics. Advisory: false is always safe.
    bool isConstantScoring() const noexcept { return (traits & IS_CONSTANT_SCORING) != 0; }

    /// True when the request permits scorer-level competitive pruning.
    bool allowsPruning() const noexcept { return (inputFlags & ALLOW_PRUNING) != 0; }

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

    /// Exact number of matching docs in this segment, or -1 when that is not
    /// known cheaply. A non-negative return must equal what iterating the
    /// scorer would count. Implementations must return -1 when the segment
    /// has deleted docs they do not account for; callers must not use this
    /// when an external filter or domain further restricts eligibility.
    virtual int64_t count(IndexReader::Segment& segment) {
      unused(segment);
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
    /// Two-phase iteration contract: a consumer picks one protocol for a
    /// scorer lifetime. If hasTwoPhase() is true, consumers that opt in drive
    /// approximation*()+matches() only and never call next()/advance() on that
    /// scorer. A non-empty approximationEnums() exposes the distinct iterators
    /// whose conjunction is the approximation; a consumer may drive them and
    /// call matchesAt() only after all are on that doc. Verification leaves
    /// score state ready and must be idempotent for the current doc, or the
    /// consumer must call it at most once per approximation doc. score() is only
    /// valid after a successful match.
    virtual bool hasTwoPhase() const {
      return false;
    }
    virtual int32_t approximationNext() {
      return next();
    }
    virtual int32_t approximationAdvance(int32_t target) {
      return advance(target);
    }
    virtual int32_t approximationDocId() {
      return docId();
    }
    virtual std::span<DocsEnum*> approximationEnums() {
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

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };
};

namespace query_detail {

// Transitional supplier for SegmentSource implementations that still only
// implement createScorer(). Specialized suppliers should override cost() with a
// better estimate and may use leadCost in get().
class DefaultScorerSupplier final : public Query::ScorerSupplier {
  Query::SegmentSource& source;
  IndexReader::Segment& segment;

public:
  DefaultScorerSupplier(Query::SegmentSource& source, IndexReader::Segment& segment)
    : source(source), segment(segment) {}

  // Conservative upper bound when the wrapped source has no cheaper estimate.
  int64_t cost() override {
    return segment.maxDoc();
  }

  Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
    unused(leadCost);
    return source.createScorer(targetPool, segment);
  }
};

} // namespace query_detail

inline Query::ScorerSupplier* Query::SegmentSource::scorerSupplier(MemPool& targetPool,
                                                                   IndexReader::Segment& segment) {
  return targetPool.make<query_detail::DefaultScorerSupplier>(*this, segment);
}


} // end namespace
