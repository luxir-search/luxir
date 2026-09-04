#pragma once

#include <string_view>

#include "MultiTermQuery.h"
#include "luxir/reader/DfaIntersectEnum.h"
#include "luxir/util/automaton/ByteDfa.h"

namespace luxir {

class ExactTermTermsEnum final : public ScanTermsEnum {
  std::string_view term;

protected:
  bool seekStart() override { return te.seekCeil(term); }
  Status accept() override { return termView() == term ? Status::ACCEPT : Status::END; }

public:
  ExactTermTermsEnum(TermsEnum& te, std::string_view term) : ScanTermsEnum(te), term(term) {}
};

class AutomatonQuery final : public MultiTermQuery {
public:
  enum class Kind { WILDCARD, REGEX };

private:
  Kind automatonKind;
  std::string_view pattern;
  automaton::ByteDfaView dfa;
  automaton::ByteDfaKind classification;
  std::string_view exactOrPrefix;
  DfaScanPlan scanPlan;

public:
  AutomatonQuery(std::string_view field, Kind kind, std::string_view pattern,
                 automaton::ByteDfaView dfa, automaton::ByteDfaKind classification,
                 std::string_view exactOrPrefix, DfaScanPlan scanPlan)
      : MultiTermQuery(QueryKind::AUTOMATON, field), automatonKind(kind),
        pattern(pattern), dfa(dfa),
        classification(classification), exactOrPrefix(exactOrPrefix),
        scanPlan(scanPlan) {}

  Kind getAutomatonKind() const { return automatonKind; }
  std::string_view getPattern() const { return pattern; }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    out.appendByte((uint8_t)automatonKind);
    out.appendString(field);
    out.appendTerm(pattern);
    unused(ctx);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    switch (classification) {
      case automaton::ByteDfaKind::SINGLE:
        return pool.make<ExactTermTermsEnum>(te, exactOrPrefix);
      case automaton::ByteDfaKind::PREFIX:
      case automaton::ByteDfaKind::ALL:
        return pool.make<PrefixTermsEnum>(te, exactOrPrefix);
      case automaton::ByteDfaKind::NORMAL:
        return pool.make<DfaIntersectEnum>(pool, te, dfa, scanPlan);
      case automaton::ByteDfaKind::NONE:
        assert(false);
    }
    return nullptr;
  }

  const DfaScanPlan& getScanPlan() const { return scanPlan; }
};

} // namespace luxir
