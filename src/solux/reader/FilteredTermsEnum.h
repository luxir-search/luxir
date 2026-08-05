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
  bool exhausted = false;

  bool finish() {
    exhausted = true;
    return false;
  }

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
  // A false return is final: subclasses track state about the term they last
  // classified, and the underlying enum may have moved past it by the time the
  // scan gave up, so advancing again would reason from a term that is no longer
  // there.
  virtual bool next() = 0;
};

// Drives a scan that classifies every term the dictionary hands it, one
// virtual call per term.  Enums that can prove whole runs of terms unacceptable
// implement next() directly instead, so that they can skip and can fuse the
// per-term work into one loop; they are not ScanTermsEnum and so cannot silently
// inherit an accept() that nothing calls.
class ScanTermsEnum : public FilteredTermsEnum {
protected:
  // Position `te` on the first candidate term.
  virtual bool seekStart() { return te.nextTerm(); }

  // Classify the term `te` is currently positioned on.
  virtual Status accept() = 0;

  // Move the underlying enum to the next candidate term.
  virtual bool advance() { return te.nextTerm(); }

public:
  using FilteredTermsEnum::FilteredTermsEnum;

  bool next() final {
    if (exhausted) return false;
    if (!started) {
      started = true;
      if (!seekStart()) return finish();
    } else {
      if (!advance()) return finish();
    }
    for (;;) {
      switch (accept()) {
        case Status::ACCEPT: return true;
        case Status::END:    return finish();
        case Status::REJECT: break;
      }
      if (!advance()) return finish();
    }
  }
};

// Accepts every term that begins with `prefix`.
class PrefixTermsEnum final : public ScanTermsEnum {
  std::string_view prefix;

protected:
  bool seekStart() override { return te.seekCeil(prefix); }

  Status accept() override {
    return termView().starts_with(prefix) ? Status::ACCEPT : Status::END;
  }

public:
  PrefixTermsEnum(TermsEnum& te, std::string_view prefix)
    : ScanTermsEnum(te), prefix(prefix) {}
};

// Accepts terms in [lower, upper] under byte order; either end may be open
// (nullopt) or exclusive.  Terms are sorted, so the scan seeks to the lower
// endpoint and stops at the first term past the upper one.
class RangeTermsEnum final : public ScanTermsEnum {
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
    : ScanTermsEnum(te), lower(lower), upper(upper),
      includeLower(includeLower), includeUpper(includeUpper) {}
};

} // namespace solux
