#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <utility>

#include "AllQuery.h"
#include "AutomatonQuery.h"
#include "BooleanQuery.h"
#include "BoostQuery.h"
#include "ConstantScoreQuery.h"
#include "ExistsQuery.h"
#include "ForcePrepareQuery.h"
#include "FuzzyQuery.h"
#include "GeoBoxQuery.h"
#include "GeoDistanceQuery.h"
#include "KnnQuery.h"
#include "MatchNoDocsQuery.h"
#include "NumericPredicateQuery.h"
#include "PhraseQuery.h"
#include "PrefixQuery.h"
#include "RescoreQuery.h"
#include "TermInSetQuery.h"
#include "TermQuery.h"
#include "TermRangeQuery.h"

namespace luxir {

inline const Query* peelBoost(const Query* query, float& boost) {
  switch (query->getKind()) {
    case QueryKind::BOOST: {
      const auto* boostQuery = static_cast<const BoostQuery*>(query);
      boost = Query::checkedBoostProduct(boost, boostQuery->getBoost());
      return peelBoost(boostQuery->getChild(), boost);
    }
    case QueryKind::TERM: {
      const auto* termQuery = static_cast<const TermQuery*>(query);
      boost = Query::checkedBoostProduct(boost, termQuery->getBoost());
      return termQuery;
    }
    case QueryKind::PHRASE:
    case QueryKind::BOOLEAN:
    case QueryKind::CONSTANT_SCORE:
    case QueryKind::FORCE_PREPARE:
    case QueryKind::FUZZY:
    case QueryKind::NUMERIC_PREDICATE:
    case QueryKind::KNN:
    case QueryKind::AUTOMATON:
    case QueryKind::PREFIX:
    case QueryKind::TERM_RANGE:
    case QueryKind::TERM_IN_SET:
    case QueryKind::EXISTS:
    case QueryKind::GEO_BOX:
    case QueryKind::GEO_DISTANCE:
    case QueryKind::ALL:
    case QueryKind::NONE:
    case QueryKind::RESCORE:
    case QueryKind::TEST:
      return query;
  }
  assert(false);
  std::unreachable();
}

// The peeled core of a mutable clause stays mutable: merged clauses are
// rebuilt around it.
inline Query* peelBoost(Query* query, float& boost) {
  return const_cast<Query*>(peelBoost(static_cast<const Query*>(query), boost));
}

inline bool sameScoringClause(const Query* a, const Query* b) {
  float aBoost = 1.0f;
  float bBoost = 1.0f;
  const Query* aCore = peelBoost(a, aBoost);
  const Query* bCore = peelBoost(b, bBoost);
  return std::bit_cast<uint32_t>(aBoost) == std::bit_cast<uint32_t>(bBoost)
      && queryEquals(*aCore, *bCore);
}

inline uint64_t scoringClauseHash(const Query* query) {
  float boost = 1.0f;
  const Query* core = peelBoost(query, boost);
  uint32_t bits = std::bit_cast<uint32_t>(boost);
  return Hash::hash(&bits, sizeof(bits), queryHash(*core));
}

inline bool queryEquals(const Query& a, const Query& b) {
  if (a.getKind() != b.getKind()) return false;
  switch (a.getKind()) {
    case QueryKind::TERM: {
      const auto& lhs = static_cast<const TermQuery&>(a);
      const auto& rhs = static_cast<const TermQuery&>(b);
      return !lhs.hasInjectedStats() && !rhs.hasInjectedStats()
          && lhs.getField() == rhs.getField()
          && lhs.getTerm() == rhs.getTerm()
          && lhs.shouldUseFrontierBound() == rhs.shouldUseFrontierBound();
    }
    case QueryKind::PHRASE: {
      const auto& lhs = static_cast<const PhraseQuery&>(a);
      const auto& rhs = static_cast<const PhraseQuery&>(b);
      return lhs.getField() == rhs.getField()
          && lhs.getSlop() == rhs.getSlop()
          && lhs.getTerms().size() == rhs.getTerms().size()
          && lhs.getPositions().size() == rhs.getPositions().size()
          && std::equal(lhs.getTerms().begin(), lhs.getTerms().end(),
                        rhs.getTerms().begin())
          && std::equal(lhs.getPositions().begin(), lhs.getPositions().end(),
                        rhs.getPositions().begin());
    }
    case QueryKind::BOOLEAN:
      return static_cast<const BooleanQuery&>(a).shapeEquals(
          static_cast<const BooleanQuery&>(b));
    case QueryKind::BOOST:
      return sameScoringClause(&a, &b);
    case QueryKind::CONSTANT_SCORE: {
      const auto& lhs = static_cast<const ConstantScoreQuery&>(a);
      const auto& rhs = static_cast<const ConstantScoreQuery&>(b);
      return std::bit_cast<uint32_t>(lhs.getConstantScore())
              == std::bit_cast<uint32_t>(rhs.getConstantScore())
          && queryEquals(*lhs.getChild(), *rhs.getChild());
    }
    case QueryKind::FORCE_PREPARE: {
      const auto& lhs = static_cast<const ForcePrepareQuery&>(a);
      const auto& rhs = static_cast<const ForcePrepareQuery&>(b);
      return sameScoringClause(lhs.getChild(), rhs.getChild());
    }
    case QueryKind::FUZZY: {
      const auto& lhs = static_cast<const FuzzyQuery&>(a);
      const auto& rhs = static_cast<const FuzzyQuery&>(b);
      return lhs.getField() == rhs.getField()
          && lhs.getTerm() == rhs.getTerm()
          && lhs.getMaxEdits() == rhs.getMaxEdits()
          && lhs.getPrefixLength() == rhs.getPrefixLength()
          && lhs.getMaxExpansions() == rhs.getMaxExpansions()
          && std::bit_cast<uint32_t>(lhs.getBoost())
              == std::bit_cast<uint32_t>(rhs.getBoost());
    }
    case QueryKind::NUMERIC_PREDICATE: {
      const auto& lhs = static_cast<const NumericPredicateQuery&>(a);
      const auto& rhs = static_cast<const NumericPredicateQuery&>(b);
      if (lhs.getField() != rhs.getField()
          || lhs.getLo() != rhs.getLo() || lhs.getHi() != rhs.getHi()
          || lhs.getExactValues().size() != rhs.getExactValues().size()
          || lhs.getExactIntervals().size()
              != rhs.getExactIntervals().size()
          || !std::equal(
              lhs.getExactValues().begin(), lhs.getExactValues().end(),
              rhs.getExactValues().begin())) {
        return false;
      }
      return std::equal(
          lhs.getExactIntervals().begin(), lhs.getExactIntervals().end(),
          rhs.getExactIntervals().begin(),
          [](const PointsReader::ValueRange& lhsRange,
             const PointsReader::ValueRange& rhsRange) {
            return lhsRange.lo == rhsRange.lo && lhsRange.hi == rhsRange.hi;
          });
    }
    case QueryKind::KNN: {
      const auto& lhs = static_cast<const KnnQuery&>(a);
      const auto& rhs = static_cast<const KnnQuery&>(b);
      const VectorFieldType& lhsType = lhs.getFieldType();
      const VectorFieldType& rhsType = rhs.getFieldType();
      auto lhsVec = lhs.getQueryVec();
      auto rhsVec = rhs.getQueryVec();
      return lhs.getField() == rhs.getField()
          && lhs.getK() == rhs.getK()
          && lhs.getNProbe() == rhs.getNProbe()
          && lhs.getRefineCandidates() == rhs.getRefineCandidates()
          && std::bit_cast<uint32_t>(lhs.getMinScanFraction())
              == std::bit_cast<uint32_t>(rhs.getMinScanFraction())
          && lhs.getExact() == rhs.getExact()
          && lhsVec.size() == rhsVec.size()
          && lhsType.dims() == rhsType.dims()
          && lhsType.metric() == rhsType.metric()
          && lhsType.normalized() == rhsType.normalized()
          && lhsType.normalizeOnWrite() == rhsType.normalizeOnWrite()
          && (lhsVec.empty()
              || memcmp(lhsVec.data(), rhsVec.data(), lhsVec.size_bytes()) == 0);
    }
    case QueryKind::AUTOMATON: {
      const auto& lhs = static_cast<const AutomatonQuery&>(a);
      const auto& rhs = static_cast<const AutomatonQuery&>(b);
      return lhs.getAutomatonKind() == rhs.getAutomatonKind()
          && lhs.getField() == rhs.getField()
          && lhs.getPattern() == rhs.getPattern();
    }
    case QueryKind::PREFIX: {
      const auto& lhs = static_cast<const PrefixQuery&>(a);
      const auto& rhs = static_cast<const PrefixQuery&>(b);
      return lhs.getField() == rhs.getField()
          && lhs.getPrefix() == rhs.getPrefix();
    }
    case QueryKind::TERM_RANGE: {
      const auto& lhs = static_cast<const TermRangeQuery&>(a);
      const auto& rhs = static_cast<const TermRangeQuery&>(b);
      return lhs.getField() == rhs.getField()
          && lhs.getLower() == rhs.getLower()
          && lhs.getUpper() == rhs.getUpper()
          && lhs.lowerInclusive() == rhs.lowerInclusive()
          && lhs.upperInclusive() == rhs.upperInclusive();
    }
    case QueryKind::TERM_IN_SET: {
      const auto& lhs = static_cast<const TermInSetQuery&>(a);
      const auto& rhs = static_cast<const TermInSetQuery&>(b);
      return lhs.getField() == rhs.getField()
          && lhs.getTerms().size() == rhs.getTerms().size()
          && std::equal(lhs.getTerms().begin(), lhs.getTerms().end(),
                        rhs.getTerms().begin());
    }
    case QueryKind::EXISTS:
      return static_cast<const ExistsQuery&>(a).getField()
          == static_cast<const ExistsQuery&>(b).getField();
    case QueryKind::GEO_BOX: {
      const auto& lhs = static_cast<const GeoBoxQuery&>(a);
      const auto& rhs = static_cast<const GeoBoxQuery&>(b);
      return lhs.getField() == rhs.getField()
          && lhs.getMinLatitude() == rhs.getMinLatitude()
          && lhs.getMaxLatitude() == rhs.getMaxLatitude()
          && lhs.getMinLongitude() == rhs.getMinLongitude()
          && lhs.getMaxLongitude() == rhs.getMaxLongitude()
          && lhs.isEmpty() == rhs.isEmpty();
    }
    case QueryKind::GEO_DISTANCE: {
      const auto& lhs = static_cast<const GeoDistanceQuery&>(a);
      const auto& rhs = static_cast<const GeoDistanceQuery&>(b);
      return lhs.getField() == rhs.getField()
          && std::bit_cast<uint64_t>(lhs.getCenterLatitude())
              == std::bit_cast<uint64_t>(rhs.getCenterLatitude())
          && std::bit_cast<uint64_t>(lhs.getCenterLongitude())
              == std::bit_cast<uint64_t>(rhs.getCenterLongitude())
          && std::bit_cast<uint64_t>(lhs.getRadiusMeters())
              == std::bit_cast<uint64_t>(rhs.getRadiusMeters());
    }
    case QueryKind::ALL:
      return true;
    case QueryKind::NONE:
      return true;
    case QueryKind::RESCORE:
      return false;
    case QueryKind::TEST:
      return false;
  }
  assert(false);
  std::unreachable();
}

inline uint64_t queryHash(const Query& query) {
  if (query.cachedHash != 0) return query.cachedHash;
  QueryKind kind = query.getKind();
  uint64_t value = Hash::hash(&kind, sizeof(kind));
  switch (kind) {
    case QueryKind::TERM: {
      const auto& term = static_cast<const TermQuery&>(query);
      value = Query::mixHash(value, term.getField());
      value = Query::mixHash(value, term.getTerm());
      value = Query::mixHash(value, term.shouldUseFrontierBound());
      value = Query::mixHash(value, term.hasInjectedStats());
      break;
    }
    case QueryKind::PHRASE: {
      const auto& phrase = static_cast<const PhraseQuery&>(query);
      value = Query::mixHash(value, phrase.getField());
      value = Query::mixHash(value, phrase.getSlop());
      value = Query::mixSampledSequence(
          value, phrase.getTerms(),
          [](uint64_t seed, std::string_view term) {
            return Query::mixHash(seed, term);
          });
      value = Query::mixSampledSequence(
          value, phrase.getPositions(),
          [](uint64_t seed, int32_t position) {
            return Query::mixHash(seed, position);
          });
      break;
    }
    case QueryKind::BOOLEAN:
      value = static_cast<const BooleanQuery&>(query).shapeHash();
      break;
    case QueryKind::BOOST:
      value = Query::mixHash(value, scoringClauseHash(&query));
      break;
    case QueryKind::CONSTANT_SCORE: {
      const auto& constant = static_cast<const ConstantScoreQuery&>(query);
      value = Query::mixHash(
          value, std::bit_cast<uint32_t>(constant.getConstantScore()));
      value = Query::mixHash(value, queryHash(*constant.getChild()));
      break;
    }
    case QueryKind::FORCE_PREPARE: {
      const auto& force = static_cast<const ForcePrepareQuery&>(query);
      value = Query::mixHash(value, scoringClauseHash(force.getChild()));
      break;
    }
    case QueryKind::FUZZY: {
      const auto& fuzzy = static_cast<const FuzzyQuery&>(query);
      value = Query::mixHash(value, fuzzy.getField());
      value = Query::mixHash(value, fuzzy.getTerm());
      value = Query::mixHash(value, fuzzy.getMaxEdits());
      value = Query::mixHash(value, fuzzy.getPrefixLength());
      value = Query::mixHash(value, fuzzy.getMaxExpansions());
      value = Query::mixHash(
          value, std::bit_cast<uint32_t>(fuzzy.getBoost()));
      break;
    }
    case QueryKind::NUMERIC_PREDICATE: {
      const auto& numeric =
          static_cast<const NumericPredicateQuery&>(query);
      value = Query::mixHash(value, numeric.getField());
      value = Query::mixHash(value, numeric.getLo());
      value = Query::mixHash(value, numeric.getHi());
      value = Query::mixSampledSequence(
          value, numeric.getExactValues(),
          [](uint64_t seed, int64_t exact) {
            return Query::mixHash(seed, exact);
          });
      value = Query::mixSampledSequence(
          value, numeric.getExactIntervals(),
          [](uint64_t seed, const PointsReader::ValueRange& interval) {
            seed = Query::mixHash(seed, interval.lo);
            return Query::mixHash(seed, interval.hi);
          });
      break;
    }
    case QueryKind::KNN: {
      const auto& knn = static_cast<const KnnQuery&>(query);
      const VectorFieldType& fieldType = knn.getFieldType();
      value = Query::mixHash(value, knn.getField());
      value = Query::mixHash(value, knn.getK());
      value = Query::mixHash(value, knn.getNProbe());
      value = Query::mixHash(value, knn.getRefineCandidates());
      value = Query::mixHash(
          value, std::bit_cast<uint32_t>(knn.getMinScanFraction()));
      value = Query::mixHash(value, knn.getExact());
      value = Query::mixSampledSequence(
          value, std::as_bytes(knn.getQueryVec()),
          [](uint64_t seed, std::byte byte) {
            return Query::mixHash(seed, byte);
          });
      value = Query::mixHash(value, fieldType.dims());
      value = Query::mixHash(value, (int32_t) fieldType.metric());
      value = Query::mixHash(value, fieldType.normalized());
      value = Query::mixHash(value, fieldType.normalizeOnWrite());
      break;
    }
    case QueryKind::AUTOMATON: {
      const auto& automaton = static_cast<const AutomatonQuery&>(query);
      value = Query::mixHash(value, automaton.getAutomatonKind());
      value = Query::mixHash(value, automaton.getField());
      value = Query::mixHash(value, automaton.getPattern());
      break;
    }
    case QueryKind::PREFIX: {
      const auto& prefix = static_cast<const PrefixQuery&>(query);
      value = Query::mixHash(value, prefix.getField());
      value = Query::mixHash(value, prefix.getPrefix());
      break;
    }
    case QueryKind::TERM_RANGE: {
      const auto& range = static_cast<const TermRangeQuery&>(query);
      value = Query::mixHash(value, range.getField());
      value = Query::mixHash(value, range.getLower().has_value());
      if (range.getLower().has_value()) {
        value = Query::mixHash(value, *range.getLower());
      }
      value = Query::mixHash(value, range.lowerInclusive());
      value = Query::mixHash(value, range.getUpper().has_value());
      if (range.getUpper().has_value()) {
        value = Query::mixHash(value, *range.getUpper());
      }
      value = Query::mixHash(value, range.upperInclusive());
      break;
    }
    case QueryKind::TERM_IN_SET: {
      const auto& set = static_cast<const TermInSetQuery&>(query);
      value = Query::mixHash(value, set.getField());
      value = Query::mixSampledSequence(
          value, set.getTerms(),
          [](uint64_t seed, std::string_view term) {
            return Query::mixHash(seed, term);
          });
      break;
    }
    case QueryKind::EXISTS:
      value = Query::mixHash(
          value, static_cast<const ExistsQuery&>(query).getField());
      break;
    case QueryKind::GEO_BOX: {
      const auto& box = static_cast<const GeoBoxQuery&>(query);
      value = Query::mixHash(value, box.getField());
      value = Query::mixHash(value, box.getMinLatitude());
      value = Query::mixHash(value, box.getMaxLatitude());
      value = Query::mixHash(value, box.getMinLongitude());
      value = Query::mixHash(value, box.getMaxLongitude());
      value = Query::mixHash(value, box.isEmpty());
      break;
    }
    case QueryKind::GEO_DISTANCE: {
      const auto& distance = static_cast<const GeoDistanceQuery&>(query);
      value = Query::mixHash(value, distance.getField());
      value = Query::mixHash(
          value, std::bit_cast<uint64_t>(distance.getCenterLatitude()));
      value = Query::mixHash(
          value, std::bit_cast<uint64_t>(distance.getCenterLongitude()));
      value = Query::mixHash(
          value, std::bit_cast<uint64_t>(distance.getRadiusMeters()));
      break;
    }
    case QueryKind::ALL:
      break;
    case QueryKind::NONE:
      break;
    case QueryKind::RESCORE:
      break;
    case QueryKind::TEST:
      break;
  }
  query.cachedHash = value == 0 ? 1 : value;
  return query.cachedHash;
}

inline Query::ScoreProfile scoreProfile(const Query& query) {
  switch (query.getKind()) {
    case QueryKind::BOOLEAN:
      return static_cast<const BooleanQuery&>(query).shapeScoreProfile();
    case QueryKind::BOOST: {
      const auto& boost = static_cast<const BoostQuery&>(query);
      Query::ScoreProfile profile = scoreProfile(*boost.getChild());
      if (profile.kind == Query::ScoreProfile::Kind::VARIABLE) return profile;
      profile.value *= boost.getBoost();
      return profile;
    }
    case QueryKind::CONSTANT_SCORE: {
      const auto& constant = static_cast<const ConstantScoreQuery&>(query);
      return Query::ScoreProfile::explicitUniform(constant.getConstantScore());
    }
    case QueryKind::RESCORE: {
      const auto& rescore = static_cast<const RescoreQuery&>(query);
      return rescore.getConstantOutput().has_value()
          ? Query::ScoreProfile::explicitUniform(*rescore.getConstantOutput())
          : Query::ScoreProfile::variable();
    }
    case QueryKind::NUMERIC_PREDICATE:
    case QueryKind::AUTOMATON:
    case QueryKind::PREFIX:
    case QueryKind::TERM_RANGE:
    case QueryKind::TERM_IN_SET:
    case QueryKind::EXISTS:
    case QueryKind::GEO_BOX:
    case QueryKind::GEO_DISTANCE:
    case QueryKind::ALL:
      return Query::ScoreProfile::automatic(1.0f);
    case QueryKind::TERM:
    case QueryKind::PHRASE:
    case QueryKind::FORCE_PREPARE:
    case QueryKind::FUZZY:
    case QueryKind::KNN:
    case QueryKind::NONE:
    case QueryKind::TEST:
      return Query::ScoreProfile::variable();
  }
  assert(false);
  std::unreachable();
}

inline bool canOmitWeightForCacheFirstMembership(const Query& query) {
  switch (query.getKind()) {
    case QueryKind::BOOLEAN:
      return static_cast<const BooleanQuery&>(query)
          .shapeCanOmitWeightForCacheFirstMembership();
    case QueryKind::BOOST:
      return canOmitWeightForCacheFirstMembership(
          *static_cast<const BoostQuery&>(query).getChild());
    case QueryKind::CONSTANT_SCORE:
      return canOmitWeightForCacheFirstMembership(
          *static_cast<const ConstantScoreQuery&>(query).getChild());
    case QueryKind::RESCORE:
      return canOmitWeightForCacheFirstMembership(
          *static_cast<const RescoreQuery&>(query).getChild());
    case QueryKind::TERM:
    case QueryKind::PHRASE:
    case QueryKind::FUZZY:
    case QueryKind::NUMERIC_PREDICATE:
    case QueryKind::KNN:
    case QueryKind::AUTOMATON:
    case QueryKind::PREFIX:
    case QueryKind::TERM_RANGE:
    case QueryKind::TERM_IN_SET:
    case QueryKind::EXISTS:
    case QueryKind::GEO_BOX:
    case QueryKind::GEO_DISTANCE:
    case QueryKind::ALL:
    case QueryKind::NONE:
      return true;
    case QueryKind::FORCE_PREPARE:
    case QueryKind::TEST:
      return false;
  }
  assert(false);
  std::unreachable();
}

inline bool exactDomainIdentity(const Query& query) {
  switch (query.getKind()) {
    case QueryKind::BOOST:
      return exactDomainIdentity(
          *static_cast<const BoostQuery&>(query).getChild());
    case QueryKind::CONSTANT_SCORE:
      return exactDomainIdentity(
          *static_cast<const ConstantScoreQuery&>(query).getChild());
    case QueryKind::FORCE_PREPARE:
      return exactDomainIdentity(
          *static_cast<const ForcePrepareQuery&>(query).getChild());
    case QueryKind::RESCORE:
      return exactDomainIdentity(
          *static_cast<const RescoreQuery&>(query).getChild());
    case QueryKind::ALL:
      return true;
    case QueryKind::TERM:
    case QueryKind::PHRASE:
    case QueryKind::BOOLEAN:
    case QueryKind::FUZZY:
    case QueryKind::NUMERIC_PREDICATE:
    case QueryKind::KNN:
    case QueryKind::AUTOMATON:
    case QueryKind::PREFIX:
    case QueryKind::TERM_RANGE:
    case QueryKind::TERM_IN_SET:
    case QueryKind::EXISTS:
    case QueryKind::GEO_BOX:
    case QueryKind::GEO_DISTANCE:
    case QueryKind::NONE:
    case QueryKind::TEST:
      return false;
  }
  assert(false);
  std::unreachable();
}

inline bool directCountAvailable(const Query& query, IndexReader& reader) {
  switch (query.getKind()) {
    case QueryKind::TERM:
      return std::none_of(
          reader.segments().begin(), reader.segments().end(),
          [](const IndexReader::Segment& segment) {
            return segment.liveDocs() != nullptr;
          });
    case QueryKind::BOOLEAN:
      return static_cast<const BooleanQuery&>(query)
          .shapeDirectCountAvailable(reader);
    case QueryKind::BOOST:
      return directCountAvailable(
          *static_cast<const BoostQuery&>(query).getChild(), reader);
    case QueryKind::CONSTANT_SCORE:
      return directCountAvailable(
          *static_cast<const ConstantScoreQuery&>(query).getChild(), reader);
    case QueryKind::RESCORE:
      return directCountAvailable(
          *static_cast<const RescoreQuery&>(query).getChild(), reader);
    case QueryKind::ALL:
    case QueryKind::NONE:
      return true;
    case QueryKind::PHRASE:
    case QueryKind::FORCE_PREPARE:
    case QueryKind::FUZZY:
    case QueryKind::NUMERIC_PREDICATE:
    case QueryKind::KNN:
    case QueryKind::AUTOMATON:
    case QueryKind::PREFIX:
    case QueryKind::TERM_RANGE:
    case QueryKind::TERM_IN_SET:
    case QueryKind::EXISTS:
    case QueryKind::GEO_BOX:
    case QueryKind::GEO_DISTANCE:
    case QueryKind::TEST:
      return false;
  }
  assert(false);
  std::unreachable();
}

inline Query::VerificationWork membershipVerificationWork(const Query& query) {
  switch (query.getKind()) {
    case QueryKind::TERM:
      return Query::VerificationWork::ABSENT;
    case QueryKind::PHRASE:
      return Query::VerificationWork::PRESENT;
    case QueryKind::BOOLEAN:
      return static_cast<const BooleanQuery&>(query)
          .shapeMembershipVerificationWork();
    case QueryKind::BOOST:
      return membershipVerificationWork(
          *static_cast<const BoostQuery&>(query).getChild());
    case QueryKind::CONSTANT_SCORE:
      return membershipVerificationWork(
          *static_cast<const ConstantScoreQuery&>(query).getChild());
    case QueryKind::FORCE_PREPARE:
    case QueryKind::FUZZY:
    case QueryKind::NUMERIC_PREDICATE:
    case QueryKind::KNN:
    case QueryKind::AUTOMATON:
    case QueryKind::PREFIX:
    case QueryKind::TERM_RANGE:
    case QueryKind::TERM_IN_SET:
    case QueryKind::EXISTS:
    case QueryKind::GEO_BOX:
    case QueryKind::GEO_DISTANCE:
    case QueryKind::ALL:
    case QueryKind::NONE:
    case QueryKind::RESCORE:
    case QueryKind::TEST:
      return Query::VerificationWork::UNKNOWN;
  }
  assert(false);
  std::unreachable();
}

} // namespace luxir
