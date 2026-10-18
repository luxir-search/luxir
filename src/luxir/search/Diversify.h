// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Collapse.h"
#include "RequestMemTracker.h"

namespace luxir {

api::Collapse diversifyKeySpec(const api::TopDocs& spec);

struct DiversifyPlan {
  const CollapsePlan& keys;
  size_t perKey;
  RequestMemTracker& tracker;
};

// Saturating counts, independent of the bounded ranking heap. A worker can
// discard counts above the cap: min(a + b, cap) is associative and exact.
class DiversifyCounts {
  struct State;
  std::unique_ptr<State> state;
public:
  explicit DiversifyCounts(const DiversifyPlan& plan);
  ~DiversifyCounts();
  void count(const CollapseKey& key);
  void merge(const DiversifyCounts& other);
  int64_t documents() const;
  int64_t groups() const;
};

class DiversifyAdmission : public GroupAdmission<false, true> {
  DiversifyCounts counts;
public:
  using Plan = DiversifyPlan;
  static constexpr bool isDiversified = true;
  explicit DiversifyAdmission(const Plan& spec)
      : GroupAdmission<false, true>(spec.keys, spec.perKey), counts(spec) {}
  void count(const CollapseKey& key) { counts.count(key); }
  void mergeCounts(const DiversifyAdmission& other) { counts.merge(other.counts); }
  int64_t totalDocuments() const { return counts.documents(); }
  int64_t totalGroups() const { return counts.groups(); }
};

} // namespace luxir
