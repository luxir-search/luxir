// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string_view>

#include "luxir/store/AcquireJoin.h"

namespace luxir {

// An overlay's proof that a term is not in a segment's dictionary. It decides
// that a segment holds no document for the term without the segment's data
// being opened, so a wrong "absent" loses results: an implementation must
// cover every dictionary term of every field it claims, deleted documents
// included, and answer false whenever it is not certain. Its kind registers
// with SegmentOverlayKindInfo::termMembership, which needs the ADVISORY policy:
// an overlay that cannot open proves nothing.
class TermMembership {
public:
  // One lookup a probe answers: a physical field and a dictionary-form term.
  struct Probe {
    std::string_view field;
    std::string_view term;
  };

  virtual ~TermMembership() = default;

  // Issues on `batches` the reads provesAbsent will make for these probes,
  // one batch per overlay file, and keeps their holds, so the probes that
  // follow read resident bytes. Optional: a probe reads what it needs on first
  // touch. The caller keeps the overlay alive until the batches complete.
  // Opening the overlay is not part of this: its first use reads its header
  // synchronously, outside any declared round.
  virtual void acquireProbes(std::span<const Probe> probes, AcquireBatches& batches) const {}

  // True only if `term`, in its dictionary form (long terms already fitted),
  // is absent from `physicalField` in this segment. An uncovered field, a
  // failed read or a checksum mismatch answers false.
  virtual bool provesAbsent(std::string_view physicalField, std::string_view term) const noexcept = 0;

  // Names the proof in explain and profile notes.
  virtual std::string_view label() const noexcept = 0;
};

}  // namespace luxir
