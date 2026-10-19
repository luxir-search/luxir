// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Diversify.h"
#include <boost/unordered/unordered_flat_map.hpp>
#include "luxir/util/ApiError.h"

namespace luxir {

api::Collapse diversifyKeySpec(const api::TopDocs& spec) {
  if (spec.collapse) throw RequestError("top_docs collapse and diversify are mutually exclusive");
  const auto& cap = *spec.diversify;
  if (!cap.max_per_key || *cap.max_per_key <= 0) {
    throw RequestError("diversify.max_per_key must be positive");
  }
  api::Collapse keys;
  keys.field = cap.field;
  keys.missing = cap.missing;
  return keys;
}

struct DiversifyCounts::State {
  const DiversifyPlan& plan;
  RequestMemResource resource;
  boost::unordered_flat_map<CollapseKey, size_t, CollapseKeyHash, std::equal_to<CollapseKey>,
      std::pmr::polymorphic_allocator<std::pair<const CollapseKey, size_t>>> counts;
  size_t tails = 0;
  int64_t total = 0;
  int64_t singletons = 0;

  explicit State(const DiversifyPlan& plan)
      : plan(plan), resource(plan.tracker, "per-key totals", "diversify"), counts(&resource) {}
  ~State() { plan.tracker.release(tails); }

  void add(const CollapseKey& key, size_t amount) {
    if (key.singleton) { singletons += (int64_t)amount; total += (int64_t)amount; return; }
    auto found = counts.find(key);
    if (found == counts.end()) {
      CollapseKey owned = key;
      size_t bytes = owned.ramBytesUsed();
      plan.tracker.charge(bytes, "diversify", "tuple totals");
      try { found = counts.emplace(std::move(owned), 0).first; }
      catch (...) { plan.tracker.release(bytes); throw; }
      tails += bytes;
    }
    size_t added = std::min(amount, plan.perKey - found->second);
    found->second += added;
    total += (int64_t)added;
  }
};

DiversifyCounts::DiversifyCounts(const DiversifyPlan& plan) {
  if (plan.keys.getNumber) state = std::make_unique<State>(plan);
}
DiversifyCounts::~DiversifyCounts() = default;
void DiversifyCounts::count(const CollapseKey& key) { if (state) state->add(key, 1); }
void DiversifyCounts::merge(const DiversifyCounts& other) {
  if (!state) return;
  state->singletons += other.state->singletons;
  state->total += other.state->singletons;
  for (const auto& [key, count] : other.state->counts) state->add(key, count);
}
int64_t DiversifyCounts::documents() const { return state ? state->total : 0; }
int64_t DiversifyCounts::groups() const {
  return state ? state->singletons + (int64_t)state->counts.size() : 0;
}

} // namespace luxir
