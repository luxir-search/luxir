#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>

// Execution overrides for the search ops: env-driven bench knobs and A/B
// baselines that let a test or a bench grid pin a strategy the selector would
// otherwise choose on its own.
//
// These live in their own leaf header on purpose. The op classes (TopDocsReq,
// StrFacetOp, ...) are heavy - each one transitively pulls the query, value and
// column trees - and are meant to be included only where a request is actually
// built. Tests that just want to flip a knob include this instead, and stay off
// that chain. Keep this header leaf: no op, query or reader includes.

namespace luxir {

// A/B baseline for measuring folded Boolean filters against the former passive
// TopDocs domain path. Default false means folding is enabled.
inline bool disableTopDocsFilterFold = false;

// A/B baseline for unscored field-sort match-window collection.
inline bool disableFieldSortBulk = false;

// Test override for one segment's range-facet bucket-domain builder budget.
// Zero selects the production constant; tests set a tiny nonzero value to
// force bucket chunking.
inline std::size_t forcedRangeFacetBucketDomainByteBudget = 0;

// Test override for the range-facet result-child state chunk. Zero selects the
// production ceiling. The optional counter observes opened blocks.
inline std::size_t forcedRangeFacetBindingStateChunkBytes = 0;
inline std::size_t* rangeFacetBindingBlockCounter = nullptr;

// Test override for the per-request query-memory breaker ceiling. Zero uses
// LuxirConfig.
inline std::size_t forcedRequestMemoryMaxBytes = 0;
inline std::atomic<size_t>* facetAggregateStateReservationCounterForTests =
    nullptr;

struct InlineAggregateStats {
  std::atomic<size_t> finalizedBytes{0};
  std::atomic<size_t> peakFinalizedBytes{0};
  std::atomic<size_t> stateBytesPerBucket{0};
};

inline InlineAggregateStats* inlineAggregateStatsForTests = nullptr;
struct InlineFacetEntryStats {
  std::atomic<size_t> denseTables{0};
  std::atomic<size_t> sparseTables{0};
  std::atomic<size_t> denseMerges{0};
  std::atomic<size_t> mixedMerges{0};
  std::atomic<size_t> denseFallbacks{0};
  std::atomic<size_t> entryStride{0};
};
inline InlineFacetEntryStats* inlineFacetEntryStatsForTests = nullptr;
inline bool disableDenseFacetStateForTests = false;

enum class InlineFacetEntryMode {
  AUTO, FORCE_DENSE, FORCE_SPARSE
};

// Test/bench control (LUXIR_INLINE_FACET_ENTRY). AUTO uses the crossover;
// dense/sparse pin the inline facet entry representation.
inline InlineFacetEntryMode initInlineFacetEntryMode() {
  const char* e = std::getenv("LUXIR_INLINE_FACET_ENTRY");
  if (e == nullptr) return InlineFacetEntryMode::AUTO;
  std::string_view v(e);
  if (v == "dense") return InlineFacetEntryMode::FORCE_DENSE;
  if (v == "sparse") return InlineFacetEntryMode::FORCE_SPARSE;
  return InlineFacetEntryMode::AUTO;
}
inline InlineFacetEntryMode forcedInlineFacetEntryMode =
    initInlineFacetEntryMode();

// A/B baseline for field-sort competitive block pruning. Default false means
// zone-based pruning runs wherever the primary sort clause offers block key
// bounds and the request needs no exact count or domain.
inline bool disableFieldSortPruning =
    std::getenv("LUXIR_DISABLE_FIELD_SORT_PRUNING") != nullptr;

// A/B baseline for the best-first exact-bitset field-sort driver. Default
// false means eligible match-all + cached-bitset field sorts visit key
// blocks in bound order with proof termination instead of doc order.
inline bool disableFieldSortBestFirst =
    std::getenv("LUXIR_DISABLE_FIELD_SORT_BEST_FIRST") != nullptr;
// Test-only: bypass the expected-floor activation gate so small corpora
// (too few key blocks to ever pass it) still drive the best-first arm.
inline bool forceFieldSortBestFirst = false;
// Test-only: override the best-first leaf work cap (0 = production formula)
// so the cap-crossing forward-sweep fallback is reachable on small corpora.
inline int64_t forceFieldSortWorkCapForTests = 0;

// A/B baseline for the seeded two-pass query-driven field-sort driver.
// Default false means eligible query-driven field sorts fill the heap from
// the best-bounded key blocks first, then sweep the complement.
inline bool disableFieldSortSeeding =
    std::getenv("LUXIR_DISABLE_FIELD_SORT_SEEDING") != nullptr;
// Test-only: bypass the economic gates (materiality and matches-per-block)
// so small corpora still drive the seeded arm. Correctness and capability
// gates are never bypassed.
inline bool forceFieldSortSeeding = false;
// Test/bench-only: scales the seed-set size relative to the expected floor
// R_hat (per-mille so the override stays integral). 0 selects the production
// policy (1000 = exactly R_hat).
inline int32_t fieldSortSeedBudgetPerMilleForTests = []() {
  const char* v = std::getenv("LUXIR_FIELD_SORT_SEED_BUDGET_PERMILLE");
  return v != nullptr ? std::atoi(v) : 0;
}();

// A/B baseline for exact-count score ranking. The default composes an
// unscored exact-count pass with a competitively pruned top-k pass when the
// query and per-segment density policy admit it.
inline bool disableTopKCountComposition =
    std::getenv("LUXIR_DISABLE_TOPK_COUNT_COMPOSITION") != nullptr;

// Test/bench control (LUXIR_FACET_COUNTER). AUTO uses the selector; the rest
// force a specific representation so the grid can compare reps head to head and
// measure the crossover thresholds. The FORCE_* modes reuse the selector's own
// count strategies (they only override which rep is chosen); the SPAN_* modes
// take the dedicated sparse fork.
enum class FacetCounterMode {
  AUTO, SPAN_GLOBAL, SPAN_LOCAL,
  FORCE_VECTOR, FORCE_SKINNY, FORCE_HASH,
  // Force the skinny rep AND its global-vs-local staging strategy, for measuring
  // the staging crossover on a multi-segment index (moot at one segment).
  SKINNY_GLOBAL, SKINNY_LOCAL
};

enum class StrFacetStrategy {
  AUTO, TOP_TERMS, COLUMN_DOMAIN, COLUMN_COMPLEMENT, TERM_DRIVEN
};

enum class FacetFeedStrategy {
  AUTO, BUCKET_DOMAINS, STRING_COLUMN_REPLAY
};

// How the bucket-domain feed builds the one domain per returned bucket that it
// hands its result children (LUXIR_FACET_BUCKET_DOMAIN).  POSTINGS seeks each
// bucket's term and intersects its postings with the incoming domain, so it
// reads every posting of every returned term; ORD_COLUMN makes one pass over the
// facet field's ord column and appends each domain document to its bucket's
// builder, so it reads the domain once no matter how many buckets there are.
// Coverage decides: postings reads what the RETURNED buckets hold index-wide,
// the ord column reads the domain.
//
// Distinct from FacetFeedStrategy::STRING_COLUMN_REPLAY, which also walks a
// column but produces no domains at all - it accumulates the child's answer
// directly.  This one still ends at bucket domains; only their construction
// changes, so every result child benefits without knowing about it.
//
// ORD_COLUMN skips the residency budget AUTO applies
// (StrFacetBucketDomainPlan::MAX_BYTES): the ord column holds one DocSet per
// bucket per segment where postings holds one in total, so forcing it on a
// request that returns enormously many buckets, over many segments, or over a
// large domain builds the whole table regardless of what it costs.
enum class FacetBucketDomainSource {
  AUTO, POSTINGS, ORD_COLUMN
};

// Which metric sub-ops a string/ID facet accumulates during the count pass
// (LUXIR_FACET_SUBOP_INLINE).  Inlining pays the whole domain per metric and
// covers every bucket; the post-selection bucket-domain feed pays only the
// documents the RETURNED buckets hold, plus a fixed cost per bucket.  So
// inlining wins when the returned buckets cover most of the domain, or when the
// domain is small enough that the per-bucket fixed cost dominates, and loses
// badly otherwise - on a 300k-document grid, moving two extra metrics onto the
// bucket-domain feed was free from realized cardinality 1,000 upward and cost
// 2.4x at cardinality 10.
//
// AUTO is the shipping rule: a sort key must be inlined (its value decides
// which buckets are returned at all), and limit==-1 inlines everything because
// every bucket is returned, which is the coverage-is-total case.  ALL and
// SORT_KEY_ONLY pin the two sides so the crossover between them can be measured
// at a finite limit before a rule is written for it.
enum class FacetSubOpInlineMode {
  AUTO, ALL, SORT_KEY_ONLY
};

enum class StrFacetReplaySelector {
  AUTO, DENSE, SPARSE
};

enum class StrFacetReplayBankStrategy {
  AUTO, VECTOR, SKINNY, PACKED_HASH, WIDE_HASH
};

inline StrFacetStrategy parseStrFacetStrategyEnv() {
  const char* e = std::getenv("LUXIR_FACET_STRATEGY");
  if (e != nullptr) {
    std::string_view s(e);
    if (s == "top_terms") {
      return StrFacetStrategy::TOP_TERMS;
    }
    if (s == "column") {
      return StrFacetStrategy::COLUMN_DOMAIN;
    }
    if (s == "complement") {
      return StrFacetStrategy::COLUMN_COMPLEMENT;
    }
    if (s == "term") {
      return StrFacetStrategy::TERM_DRIVEN;
    }
  }
  return StrFacetStrategy::AUTO;
}

inline FacetFeedStrategy parseFacetFeedStrategyEnv() {
  // Facet feed overrides are planner A/B controls. Keep accepted values tied
  // to executable feeds rather than advertising planned implementations.
  const char* e = std::getenv("LUXIR_FACET_FEED");
  if (e != nullptr && std::string_view(e) == "bucket_domains") {
    return FacetFeedStrategy::BUCKET_DOMAINS;
  }
  if (e != nullptr && std::string_view(e) == "string_column") {
    return FacetFeedStrategy::STRING_COLUMN_REPLAY;
  }
  return FacetFeedStrategy::AUTO;
}

inline FacetSubOpInlineMode parseFacetSubOpInlineEnv() {
  const char* e = std::getenv("LUXIR_FACET_SUBOP_INLINE");
  if (e != nullptr) {
    std::string_view s(e);
    if (s == "all") {
      return FacetSubOpInlineMode::ALL;
    }
    if (s == "sort_key") {
      return FacetSubOpInlineMode::SORT_KEY_ONLY;
    }
  }
  return FacetSubOpInlineMode::AUTO;
}

inline FacetBucketDomainSource parseFacetBucketDomainSourceEnv() {
  const char* e = std::getenv("LUXIR_FACET_BUCKET_DOMAIN");
  if (e != nullptr) {
    std::string_view s(e);
    if (s == "postings") {
      return FacetBucketDomainSource::POSTINGS;
    }
    if (s == "ord_column") {
      return FacetBucketDomainSource::ORD_COLUMN;
    }
  }
  return FacetBucketDomainSource::AUTO;
}

inline StrFacetReplaySelector parseStrFacetReplaySelectorEnv() {
  const char* e = std::getenv("LUXIR_FACET_REPLAY_SELECTOR");
  if (e != nullptr && std::string_view(e) == "dense") {
    return StrFacetReplaySelector::DENSE;
  }
  if (e != nullptr && std::string_view(e) == "sparse") {
    return StrFacetReplaySelector::SPARSE;
  }
  return StrFacetReplaySelector::AUTO;
}

inline StrFacetReplayBankStrategy parseStrFacetReplayBankEnv() {
  const char* e = std::getenv("LUXIR_FACET_REPLAY_BANK");
  if (e != nullptr) {
    std::string_view s(e);
    if (s == "vector") return StrFacetReplayBankStrategy::VECTOR;
    if (s == "skinny") return StrFacetReplayBankStrategy::SKINNY;
    if (s == "packed_hash") return StrFacetReplayBankStrategy::PACKED_HASH;
    if (s == "wide_hash") return StrFacetReplayBankStrategy::WIDE_HASH;
  }
  return StrFacetReplayBankStrategy::AUTO;
}

// The SPAN_* modes drive the dedicated sparse fork; everything else runs the
// ordinary selector path (with FORCE_* pinning its rep choice).
inline bool isSparseForcedMode(FacetCounterMode m) {
  return m == FacetCounterMode::SPAN_GLOBAL || m == FacetCounterMode::SPAN_LOCAL;
}

// FORCE_SKINNY (auto staging) and SKINNY_GLOBAL/SKINNY_LOCAL (forced staging)
// all pin the skinny rep in the ordinary selector path.
inline bool forcesSkinnyRep(FacetCounterMode m) {
  return m == FacetCounterMode::FORCE_SKINNY
      || m == FacetCounterMode::SKINNY_GLOBAL
      || m == FacetCounterMode::SKINNY_LOCAL;
}

inline FacetCounterMode parseFacetCounterModeEnv() {
  const char* e = std::getenv("LUXIR_FACET_COUNTER");
  if (e != nullptr) {
    std::string_view s(e);
    if (s == "span_global") {
      return FacetCounterMode::SPAN_GLOBAL;
    }
    if (s == "span_local") {
      return FacetCounterMode::SPAN_LOCAL;
    }
    if (s == "vector") {
      return FacetCounterMode::FORCE_VECTOR;
    }
    if (s == "skinny") {
      return FacetCounterMode::FORCE_SKINNY;
    }
    if (s == "hash") {
      return FacetCounterMode::FORCE_HASH;
    }
    if (s == "skinny_global") {
      return FacetCounterMode::SKINNY_GLOBAL;
    }
    if (s == "skinny_local") {
      return FacetCounterMode::SKINNY_LOCAL;
    }
  }
  return FacetCounterMode::AUTO;
}

inline FacetCounterMode forcedFacetCounterMode = parseFacetCounterModeEnv();
inline StrFacetStrategy forcedStrFacetStrategy = parseStrFacetStrategyEnv();
inline FacetFeedStrategy forcedFacetFeedStrategy = parseFacetFeedStrategyEnv();
inline FacetBucketDomainSource forcedFacetBucketDomainSource =
    parseFacetBucketDomainSourceEnv();
inline FacetSubOpInlineMode forcedFacetSubOpInline = parseFacetSubOpInlineEnv();
inline StrFacetReplaySelector forcedStrFacetReplaySelector =
    parseStrFacetReplaySelectorEnv();
inline StrFacetReplayBankStrategy forcedStrFacetReplayBank =
    parseStrFacetReplayBankEnv();

} // namespace luxir
