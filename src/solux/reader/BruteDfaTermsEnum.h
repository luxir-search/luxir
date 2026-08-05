#pragma once

#include "FilteredTermsEnum.h"
#include "solux/util/automaton/ByteDfa.h"

namespace solux {

class BruteDfaTermsEnum final : public FilteredTermsEnum {
  automaton::ByteDfaView dfa;
  std::string prefix;

protected:
  bool seekStart() override { return te.seekCeil(prefix); }

  Status accept() override {
    std::string_view term = termView();
    if (!term.starts_with(prefix)) return Status::END;
    return dfa.matches(term) ? Status::ACCEPT : Status::REJECT;
  }

public:
  BruteDfaTermsEnum(TermsEnum& te, automaton::ByteDfaView dfa)
      : FilteredTermsEnum(te), dfa(dfa), prefix(dfa.commonPrefix()) {}
};

} // namespace solux
