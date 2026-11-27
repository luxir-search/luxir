// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>
#include <string_view>
#ifndef NDEBUG
#include <thread>
#endif

#include <boost/unordered/unordered_node_map.hpp>

#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/Similarity.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/StrRef.h"

namespace luxir {

enum ReadProduct : uint8_t {
  TERM_STATS = 1, TERM_DOCS = 2, TERM_POSITIONS = 4, FIELD_NORMS = 8,
};

struct CachedTermInfo {
  Similarity::TermStats termStats = {};
  // Scored ordinary terms can reuse this view-wide, unit-boost scorer.
  Similarity::BM25Scorer* simScorer = nullptr;
  // Empty for a miss or an unresolved declaration. Nonempty states include
  // stats, docs and any positions physically present in each segment.
  std::span<const TermsEnum::PostingsState*> postingsStates = {};
  // Requested products remain meaningful even after resolution or a miss.
  uint8_t requested = 0;
  // Per segment, once asked: 1 a proof that the term is absent, 2 no proof.
  // Empty until a segment with a membership overlay is asked about the term.
  std::span<uint8_t> absence = {};
  // True until dictionary resolution succeeds, including a successful miss.
  // A resolved absent field settles all its terms regardless of this bit.
  bool unresolved = false;

  // A captured state provides stats and a docs stream. Positions are
  // available per segment in that state. Norms are outside the term cache.
  uint8_t available(size_t segment) const {
    if (postingsStates.empty() || postingsStates[segment] == nullptr) return 0;
    return TERM_STATS | TERM_DOCS | (postingsStates[segment]->hasPositions ? TERM_POSITIONS : 0);
  }

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
  IndexReader::FieldSegments segInfos;
  Similarity::FieldStats fieldStats = {};
  // Mutable dictionary cursors used only by serial lookup.
  std::span<TermsEnum*> termsEnums = {};
  boost::unordered_node_map<std::string_view, CachedTermInfo, PackedTermHash, PackedTermEqual, MemPool::allocator<std::pair<const std::string_view, CachedTermInfo>>> termInfos;
  // Metadata for declared fields is opened only on a weight lookup.
  // A resolved absent field leaves no pending term work in this entry.
  bool unresolved = false;
  // Field products independent of any term in this field.
  uint8_t requested = 0;

  explicit CachedFieldInfo(MemPool& pool, size_t initialMapSize=4) : termInfos(initialMapSize, pool.getAllocator()) {}
};

// Exact-term facts for one reader view. Weights read the captured postings
// states after planning, independently of the mutable dictionary cursors.
class TermFactsCache {
  using FieldInfoMap = boost::unordered_node_map<std::string_view, CachedFieldInfo,
      PackedTermHash, PackedTermEqual,
      MemPool::allocator<std::pair<const std::string_view, CachedFieldInfo>>>;

  MemPool& pool;
  IndexReader& reader;
  FieldInfoMap fieldInfoMap;
#ifndef NDEBUG
  // Thread affinity for today's single-thread planning, set at first binding.
  // Resumable planning will need serial-stage ownership instead.
  std::thread::id owningThread;
#endif

  size_t numSegments() const noexcept { return reader.segments().size(); }

  std::string_view ownKey(std::string_view key) {
    if (key.empty()) return {};
    auto* dest = pool.alloc(key.size());
    std::memcpy(dest, key.data(), key.size());
    return {dest, key.size()};
  }

public:
  TermFactsCache(MemPool& pool, IndexReader& reader, [[maybe_unused]] bool shared = false)
      : pool(pool), reader(reader), fieldInfoMap(4, pool.getAllocator()) {
#ifndef NDEBUG
    if (shared) owningThread = std::this_thread::get_id();
#endif
  }

#ifndef NDEBUG
  void assertOwner() const {
    assert(owningThread == std::thread::id() || owningThread == std::this_thread::get_id());
  }
#endif

  CachedFieldInfo& declareField(std::string_view field, uint8_t products = 0) {
    auto iter = fieldInfoMap.find(field);
    if (iter == fieldInfoMap.end()) {
      iter = fieldInfoMap.try_emplace(ownKey(field), pool, 4).first;
      iter->second.unresolved = true;
    }
    iter->second.requested |= products;
    return iter->second;
  }

  void declareTerm(std::string_view field, std::string_view term, uint8_t products) {
    auto& terms = declareField(field).termInfos;
    auto iter = terms.find(term);
    if (iter == terms.end()) {
      iter = terms.try_emplace(ownKey(term)).first;
      iter->second.unresolved = true;
    }
    iter->second.requested |= products;
  }

  /// Returns nullptr if the field is not found in any segment.
  CachedFieldInfo* getCachedFieldInfo(std::string_view field) {
    auto [iter, inserted] = fieldInfoMap.try_emplace(field, pool, 4);
    CachedFieldInfo& result = iter->second;
    if (inserted || result.unresolved) {
      result.segInfos = reader.field(field);
      if (result.segInfos.exists()) {
        result.fieldStats = result.segInfos.stats();
        result.termsEnums = pool.make_span_zeroed<TermsEnum*>(numSegments());
      }
      result.unresolved = false;
    }
    return result.segInfos.exists() ? &result : nullptr;
  }

private:
  // The cached dictionary outlives temporary pool rewinds. Call only before
  // taking a savepoint that may be rewound, or outside any rewind scope.
  TermsEnum* getTermsEnum(CachedFieldInfo& field, size_t segnum) {
    auto*& terms = field.termsEnums[segnum];
    if (!terms) {
      if (auto* info = field.segInfos.at(segnum)) {
        terms = pool.make<TermsEnum>(pool, reader.segments()[segnum].postingsReader(), *info);
      }
    }
    return terms;
  }

  // A term outside this segment's catalog bounds contributes zero frequency.
  // `node`, the term's cache entry, memoizes the answer when given.
  bool termAbsent(CachedFieldInfo& field, size_t segnum, std::string_view term, CachedTermInfo* node = nullptr);

public:
  // Whether every segment's catalog or membership overlay proves `term` absent,
  // so the term matches nothing and no segment need be opened for it.
  bool termAbsentEverywhere(CachedFieldInfo& field, std::string_view term) {
    CachedTermInfo* node = nullptr;
    if (std::ranges::any_of(reader.segments(), &IndexReader::Segment::hasTermMembership)) {
      // The answers are kept on the term's entry, which resolves later as usual.
      auto iter = field.termInfos.find(term);
      if (iter == field.termInfos.end()) {
        iter = field.termInfos.try_emplace(ownKey(term)).first;
        iter->second.unresolved = true;
      }
      node = &iter->second;
    }
    for (size_t i = 0; i < numSegments(); ++i) {
      if (!termAbsent(field, i, term, node)) return false;
    }
    return true;
  }

  CachedTermInfo* getCachedTerminfo(CachedFieldInfo& cachedFieldInfo, std::string_view term) {
    auto [iter, inserted] = cachedFieldInfo.termInfos.try_emplace(term);
    CachedTermInfo& result = iter->second;
    if (!inserted && !result.unresolved) {
      return result.postingsStates.empty() ? nullptr : &result;
    }

    try {
      auto numSegs = numSegments();
      // Keep the term node and dictionary cursors below the savepoint. Only
      // the temporary postings data is rewound when the term is absent.
      for (size_t i = 0; i < numSegs; i++) {
        if (!termAbsent(cachedFieldInfo, i, term, &result)) getTermsEnum(cachedFieldInfo, i);
      }
      auto savepoint = pool.getSavePoint();
      bool found = false;
      auto postingsStates = pool.make_span<const TermsEnum::PostingsState*>(numSegs);
      for (size_t i = 0; i < numSegs; ++i) {
        // Statistics cover the whole view. Only a proof about this term's
        // absence can skip a segment, regardless of the root's other predicates.
        auto* termsEnum = cachedFieldInfo.termsEnums[i];
        if (!termsEnum || termAbsent(cachedFieldInfo, i, term, &result) || !termsEnum->seek(term)) {
          postingsStates[i] = nullptr;
          continue;
        }
        found = true;
        auto* state = pool.make<TermsEnum::PostingsState>(termsEnum->postingsState());
        postingsStates[i] = state;
        result.termStats.docFreq += state->docFreq;
        result.termStats.totalTermFreq += state->totalTermFreq;
      }

      result.unresolved = false;
      if (!found) {
        pool.rewind(savepoint);
        return nullptr;
      }
      result.postingsStates = postingsStates;
      return &result;
    } catch (...) {
      if (result.requested == 0) cachedFieldInfo.termInfos.erase(iter);
      else result.termStats = {};
      throw;
    }
  }

  bool lookupTermStats(CachedFieldInfo& cachedFieldInfo, std::string_view term, Similarity::TermStats& result) {
    result = {};
    bool found = false;
    for (int i = 0; i < (int)numSegments(); ++i) {
      if (termAbsent(cachedFieldInfo, (size_t)i, term)) continue;
      auto* termsEnum = getTermsEnum(cachedFieldInfo, (size_t)i);
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

} // namespace luxir
