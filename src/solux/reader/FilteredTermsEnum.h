#pragma once

#include <string_view>

#include "TermsEnum.h"

namespace solux {

// Forward-only filter over a TermsEnum. Accepted positions keep the underlying
// cursor exposed so callers can build DocsEnum from the current term.
class FilteredTermsEnum {
public:
  // Terms are sorted, so the scan can stop the moment a term proves no later
  // term can be accepted (END), rather than rejecting to the end of the field.
  enum class Status { ACCEPT, REJECT, END };

protected:
  TermsEnum& te;
  bool started = false;

  // Position `te` on the first candidate term.
  virtual bool seekStart() { return te.nextTerm(); }

  // Classify the term `te` is currently positioned on.
  virtual Status accept() = 0;

  // Move the underlying enum to the next candidate term.
  virtual bool advance() { return te.nextTerm(); }

public:
  explicit FilteredTermsEnum(TermsEnum& te) : te(te) {}

  // Underlying enum at the current accepted term.
  TermsEnum& terms() { return te; }

  PackedTerm term() const { return te.term(); }
  std::string_view termView() const { return (std::string_view)te.term(); }

  // Per-term score of the currently accepted term.
  virtual float currentScore() const { return 1.0f; }

  // Advance to the next accepted term, returning false when the scan is
  // exhausted.  After a true return, term()/terms()/currentScore() are valid.
  bool next() {
    if (!started) {
      started = true;
      if (!seekStart()) return false;
    } else {
      if (!advance()) return false;
    }
    for (;;) {
      switch (accept()) {
        case Status::ACCEPT: return true;
        case Status::END:    return false;
        case Status::REJECT: break;
      }
      if (!advance()) return false;
    }
  }
};

// Accepts every term that begins with `prefix`.
class PrefixTermsEnum final : public FilteredTermsEnum {
  std::string_view prefix;

protected:
  bool seekStart() override { return te.seekCeil(prefix); }

  Status accept() override {
    return termView().starts_with(prefix) ? Status::ACCEPT : Status::END;
  }

public:
  PrefixTermsEnum(TermsEnum& te, std::string_view prefix)
    : FilteredTermsEnum(te), prefix(prefix) {}
};

} // namespace solux
