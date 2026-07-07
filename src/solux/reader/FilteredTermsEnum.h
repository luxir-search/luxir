#pragma once

#include <optional>
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

// Accepts terms in [lower, upper] under byte order; either end may be open
// (nullopt) or exclusive.  Terms are sorted, so the scan seeks to the lower
// endpoint and stops at the first term past the upper one.
class RangeTermsEnum final : public FilteredTermsEnum {
  std::optional<std::string_view> lower;
  std::optional<std::string_view> upper;
  bool includeLower;
  bool includeUpper;

protected:
  bool seekStart() override {
    if (!lower.has_value()) return te.nextTerm();
    if (!te.seekCeil(*lower)) return false;
    if (!includeLower && termView() == *lower) return te.nextTerm();
    return true;
  }

  Status accept() override {
    if (!upper.has_value()) return Status::ACCEPT;
    int cmp = termView().compare(*upper);
    if (cmp < 0 || (cmp == 0 && includeUpper)) return Status::ACCEPT;
    return Status::END;
  }

public:
  RangeTermsEnum(TermsEnum& te, std::optional<std::string_view> lower, bool includeLower,
                 std::optional<std::string_view> upper, bool includeUpper)
    : FilteredTermsEnum(te), lower(lower), upper(upper),
      includeLower(includeLower), includeUpper(includeUpper) {}
};

} // namespace solux
