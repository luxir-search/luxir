#pragma once

#include <solux/util/heap.h>
#include "solux/util/MemPool.h"
#include "solux/search/IndexReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/reader/DocsEnum.h"
#include "solux/search/Similarity.h"
#include "gtl/phmap.hpp"

namespace solux {

// Overview:
// - Query represents a user query.
// - Weight is created by a Query for a specific index
// - Scorer is created by a Weight for a specific segment
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
  using map_type = gtl::node_hash_map<key_type, vec_type, std::hash<std::string_view>, std::equal_to<>, MemPool::allocator<pair_type>>;
  // Using a node_hash_map in a pool will lead to less memory wasted if the map is resized.


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
  gtl::node_hash_map<std::string_view, CachedTermInfo, std::hash<std::string_view>, std::equal_to<>, MemPool::allocator<std::pair<const std::string_view, CachedTermInfo>>> termInfos;

  explicit CachedFieldInfo(MemPool& pool, size_t initialMapSize=4) : termInfos(initialMapSize, pool.getAllocator()) {}
};


// NOTE: no virtual destructor, so subclasses of Query should be made trivially destructible
class Query {
public:
  class Context;
  class Weight;
  class Scorer;

  /// Returns a non-owning pointer to the created weight.  The Query::Context
  /// is responsible for the lifecycle of the created Weight.
  // TODO: pass down flags like NEED_SCORES, etc
  virtual Query::Weight* createWeight(Query::Context& context) = 0;

  /// Gives context to a Query (i.e. what index it's being used on amongst other things) when creating weights
  class Context {
  public:
    MemPool& pool;
    IndexReader& topReader;
    // Weight* top = nullptr;  // if we don't need a top-weight, we can reuse a Context for multiple queries in the same request.

    std::span<FieldReader> fieldReaders;
    gtl::node_hash_map<std::string_view, CachedFieldInfo, std::hash<std::string_view>, std::equal_to<>, MemPool::allocator<std::pair<const std::string_view, CachedFieldInfo>>> fieldInfoMap;

    Context(MemPool& pool, IndexReader& topReader)
    : pool(pool), topReader(topReader), fieldInfoMap(4, pool.getAllocator()) {

      auto numSegs = numSegments();

      // allocate the raw space (then use operator placement new) since we don't have a default constructor
      fieldReaders = {(FieldReader*)pool.alloc(sizeof(FieldReader)*numSegs, alignof(FieldReader)), numSegs};

      for (auto i = 0; i < numSegs; i++) {
        new (&fieldReaders[i]) FieldReader(pool, topReader.segments()[i].postingsReader());
      }
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
  class Weight {
  protected:
    Query::Context& context;
  public:
    Weight(Query::Context& context) : context(context) {}

    // Create a scorer for a specific segment in the specific MemPool.  Can return null if no docs match!
    virtual Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) = 0;

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };

  // NOTE: no virtual destructor, so subclasses of Query should be made trivially destructible
  class Scorer {
  public:
    virtual int32_t next() = 0;
    virtual int32_t advance(int32_t docid) {
      int32_t doc;
      while ((doc = next()) < docid) {}
      return doc;
    }
    virtual bool advanceExact (int32_t docid) {
      return advance(docid) == docid;
    }
    /// doc we are positioned on
    virtual int32_t docId() = 0;
    /// term frequency for current doc
    virtual float score() = 0;

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };
};


} // end namespace