// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <utility>

#include "Query.h"
#include "BoostQuery.h"
#include "ConstantScoreQuery.h"
#include "ForcePrepareQuery.h"
#include "RescoreQuery.h"
#include "BooleanQuery.h"

namespace luxir {

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
