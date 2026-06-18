#pragma once

#include <memory>
#include <solux/util/heap.h>
#include "solux/util/MemPool.h"
#include "solux/util/StrRef.h"
#include "solux/search/IndexReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/reader/DocsEnum.h"
#include "solux/search/Similarity.h"
#include <boost/unordered/unordered_node_map.hpp>
#include <google/protobuf/arena.h>

namespace solux {

class DocSet;

// Overview
// ========
// Execution follows Lucene's shape -- Query -> Weight -> Scorer -- plus one
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
// Two phases, with different threading and allocation rules -- getting these
// wrong can be a data race, so they are part of the contract:
//
// 1. Build -- single-threaded, before search tasks are dispatched.
//    createWeight() walks the Query tree and builds the Weight tree.  Weight
//    ctors may read/populate the shared Query::Context (field/term caches) and
//    allocate from Context::pool (the per-request MemPool).  Safe ONLY because
//    it runs before any task is dispatched.
//
// 2. Execute -- parallel, on the TBB task pool.
//    a. prepare() -- optional.  A Weight whose needsPrepare() returns true is
//       prepared before its segment scorers are created.  It is for any weight
//       that must resolve state across the whole index before per-segment
//       scorers exist: a leaf computing an index-level result (e.g. KnnQuery,
//       whose FAISS index is index-level and whose search runs once,
//       pre-filtered by the active per-segment domains), or a compound (a query
//       with child queries, e.g. BooleanQuery) that must prepare its children
//       and hand them the per-segment domains they need before they run.
//       prepare() returns an
//       immutable
//       PreparedWeight holding the whole-index result; scorerSupplier() then
//       reads from it per segment.  Compound queries propagate: needsPrepare()
//       ORs over children, and prepare() recursively prepares children,
//       threading per-segment domains down (e.g. Boolean materializes its
//       filter clauses into a domain that narrows the scoring children's
//       search).
//    b. scorerSupplier(targetPool, segment) -- called as needed for a segment,
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
// target. Use advanceExact() or guard sub-scorers that may already be on target.
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
  int32_t sharedCount = 0;
  Similarity::TermStats termStats = {};
  Similarity::BM25Scorer* simScorer = nullptr;  // This may be null even if other elements are fille in (phrase query would have different one)
  std::span<DocsEnum*> docsEnums = {}; // TODO: cache align if they will be used in multiple threads

  /// Get a possibly-cached DocsEnum for use. This means cloning it if the cached one is shared.
  DocsEnum* useDocsEnum(MemPool& targetPool, IndexReader::Segment& segment) {
    auto* docsEnum = docsEnums[segment.ord];
    if (docsEnum == nullptr) {
      return nullptr;
    }
    if (sharedCount > 0) {
      // This cachedTerm is shared, so we need to make a copy of the DocsEnum
      docsEnum = targetPool.make<DocsEnum>(targetPool, *docsEnum);
    }
    return docsEnum;
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

  /// Returns a non-owning pointer to the created weight.  The Query::Context
  /// is responsible for the lifecycle of the created Weight.
  /// A Context is not generally thread-safe, so don't create weights from multiple threads with the same Context.
  // TODO: pass down flags like NEED_SCORES, etc
  virtual Query::Weight* createWeight(Query::Context& context) = 0;

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

    MemPool& pool;
    IndexReader& topReader;
    // Weight* top = nullptr;  // if we don't need a top-weight, we can reuse a Context for multiple queries in the same request.

    std::span<FieldReader> fieldReaders;
    FieldInfoMap fieldInfoMap;

    // Nothrow ctor used by Context::create for arena allocation.
    // Arena::Create registers ~Context before running the ctor body, so any
    // throw mid-construction (pool allocation, FieldReader init, map bucket
    // allocation) would leave a cleanup entry pointing at uninitialized
    // memory.  This ctor only does noexcept member binds/moves; the
    // throwing work is done by the factory before allocation.
    Context(MemPool& pool, IndexReader& topReader,
            std::span<FieldReader> fieldReaders, FieldInfoMap&& fieldInfoMap)
      : pool(pool), topReader(topReader),
        fieldReaders(fieldReaders), fieldInfoMap(std::move(fieldInfoMap)) {
    }

    // Convenience ctor for stack-allocated Contexts (tests, non-arena code).
    // Safe to throw here since there's no arena cleanup entry that could be
    // left dangling.
    Context(MemPool& pool, IndexReader& topReader)
      : pool(pool), topReader(topReader), fieldInfoMap(4, pool.getAllocator()) {
      auto numSegs = topReader.segments().size();
      fieldReaders = {(FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader)), numSegs};
      for (size_t i = 0; i < numSegs; i++) {
        new (&fieldReaders[i]) FieldReader(pool, topReader.segments()[i].postingsReader());
      }
    }

    static Context* create(google::protobuf::Arena* arena, MemPool& pool, IndexReader& topReader) {
      auto numSegs = topReader.segments().size();
      auto* readers = (FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader));
      for (size_t i = 0; i < numSegs; i++) {
        new (&readers[i]) FieldReader(pool, topReader.segments()[i].postingsReader());
      }
      FieldInfoMap map(4, pool.getAllocator());
      return google::protobuf::Arena::Create<Context>(
        arena, pool, topReader, std::span<FieldReader>(readers, numSegs), std::move(map));
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
        result.sharedCount++;
        return &result;
      }

      auto savepoint = pool.getSavePoint();
      auto numSegs = numSegments();
      int foundInSegCount = 0;
      result.docsEnums = {pool.make_arr<DocsEnum*>(numSegs), numSegs};
      for (int i = 0; i < numSegs; ++i) {
        auto& termsEnum = cachedFieldInfo.termsEnums[i];
        if (!termsEnum || !termsEnum->seek(term)) {
          result.docsEnums[i] = nullptr;
          continue;
        }
        foundInSegCount++;
        DocsEnum* docsEnum = pool.make<DocsEnum>(pool, topReader.segments()[i].postingsReader(), *termsEnum);
        result.docsEnums[i] = docsEnum;
        result.termStats.docFreq += docsEnum->numDocs();
        result.termStats.totalTermFreq += docsEnum->totalTermFreq();
      }

      if (foundInSegCount == 0) {
        pool.rewind(savepoint);
        cachedFieldInfo.termInfos.erase(iter);
        return nullptr;
      }

      return &result;
    }

  };

  // A weight is created by a query for execution over a specific index
  // It does not have a virtual destructor, so subclasses should be made trivially destructible.
  class Weight : public Query::SegmentSource {
  protected:
    Query::Context& context;
  public:
    Weight(Query::Context& context) : context(context) {}

    /// Per-segment domains available during prepare(). An empty domain span
    /// means unrestricted aside from whatever the caller applies later.
    ///
    /// Domain contract: a segment's domain, when present, is LIVE-FILTERED -
    /// it is liveDocs intersected with any enclosing filter clauses, and is
    /// the complete eligibility predicate (consumers must not re-check
    /// liveDocs).  A null domain means the segment has no deletes and no
    /// enclosing filters.  RootOp establishes this by seeding every
    /// segment's base domain with liveDocs; every domain-deriving seam
    /// (filter materialization, boolean child contexts) preserves it.
    ///
    /// parallel is true when the request runs under a TBB task group; a
    /// prepare() implementation may then spawn internal worker tasks, provided
    /// they are joined before prepare() returns.  false means the request is
    /// serial and prepare() must not spawn tasks.
    ///
    /// The constructor deliberately has no default for parallel: a derived
    /// context built from a parent ctx must forward ctx.parallel, and a
    /// defaulted field would let a new construction site silently strand
    /// nested queries in serial mode (an aggregate would value-initialize the
    /// missing field just as silently).
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

    /// True if this Weight needs prepare() before segment scorer creation.
    /// Compound queries should return true if any child needs preparation.
    virtual bool needsPrepare() const noexcept { return false; }

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
    // Lenient exact probe: stay put when already at or past target.
    virtual bool advanceExact(int32_t target) {
      int32_t doc = docId();
      if (doc < target) {
        doc = advance(target);
      }
      return doc == target;
    }
    /// doc we are positioned on
    virtual int32_t docId() = 0;
    /// term frequency for current doc
    virtual float score() = 0;

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
