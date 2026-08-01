#pragma once

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

namespace solux {

// A/B baseline for measuring folded Boolean filters against the former passive
// TopDocs domain path. Default false means folding is enabled.
inline bool disableTopDocsFilterFold = false;

// A/B baseline for unscored field-sort match-window collection.
inline bool disableFieldSortBulk = false;

// A/B baseline for exact-count score ranking. The default composes an
// unscored exact-count pass with a competitively pruned top-k pass when the
// query and per-segment density policy admit it.
inline bool disableTopKCountComposition =
    std::getenv("SOLUX_DISABLE_TOPK_COUNT_COMPOSITION") != nullptr;

// Test/bench control (SOLUX_FACET_COUNTER). AUTO uses the selector; the rest
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

enum class StrFacetReplaySelector {
  AUTO, DENSE, SPARSE
};

enum class StrFacetReplayBankStrategy {
  AUTO, VECTOR, SKINNY, PACKED_HASH, WIDE_HASH
};

inline StrFacetStrategy parseStrFacetStrategyEnv() {
  const char* e = std::getenv("SOLUX_FACET_STRATEGY");
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
  const char* e = std::getenv("SOLUX_FACET_FEED");
  if (e != nullptr && std::string_view(e) == "bucket_domains") {
    return FacetFeedStrategy::BUCKET_DOMAINS;
  }
  if (e != nullptr && std::string_view(e) == "string_column") {
    return FacetFeedStrategy::STRING_COLUMN_REPLAY;
  }
  return FacetFeedStrategy::AUTO;
}

inline StrFacetReplaySelector parseStrFacetReplaySelectorEnv() {
  const char* e = std::getenv("SOLUX_FACET_REPLAY_SELECTOR");
  if (e != nullptr && std::string_view(e) == "dense") {
    return StrFacetReplaySelector::DENSE;
  }
  if (e != nullptr && std::string_view(e) == "sparse") {
    return StrFacetReplaySelector::SPARSE;
  }
  return StrFacetReplaySelector::AUTO;
}

inline StrFacetReplayBankStrategy parseStrFacetReplayBankEnv() {
  const char* e = std::getenv("SOLUX_FACET_REPLAY_BANK");
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
  const char* e = std::getenv("SOLUX_FACET_COUNTER");
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
inline StrFacetReplaySelector forcedStrFacetReplaySelector =
    parseStrFacetReplaySelectorEnv();
inline StrFacetReplayBankStrategy forcedStrFacetReplayBank =
    parseStrFacetReplayBankEnv();

} // namespace solux
