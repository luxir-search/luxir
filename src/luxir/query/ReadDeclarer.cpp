// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/ReadDeclarer.h"

#include <format>
#include <mutex>
#include <ostream>
#include "luxir/query/QueryPruning.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

namespace {

bool undeclaredKind(QueryKind kind) {
  switch (kind) {
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
    case QueryKind::GEO_POLYGON:
    case QueryKind::RANGE_FIELD:
    case QueryKind::PLUGIN:
    case QueryKind::TEST:
      return true;
    default:
      return false;
  }
}

TermFactsCache* demandCache(Query::PlanningContext& planning) {
  auto* slot = planning.sharedTermFacts;
  if (slot == nullptr) return nullptr;
  if (!*slot) slot->emplace(planning.pool, planning.topReader, true);
#ifndef NDEBUG
  (*slot)->assertOwner();
  assert(planning.inTermPlanningPhase());
#endif
  return &**slot;
}

std::mutex totalsMutex;
std::array<ReadVerification::Counts, (size_t)QueryKind::TEST + 1> totals;
uint64_t pluginOwners = 0;
uint64_t totalFailures = 0;

} // namespace

void ReadDeclarer::root(const Query& query, Query::PlanningContext& planning,
                        int32_t flags, float multiplier) {
  if (!planning.pruning) planning.pruning = std::make_shared<QueryPruning>();
  auto& mask = planning.pruning->membership(query, planning).mask;
  if (!mask.none() || query.mayRequirePreparation()) {
    child(query, planning, flags, multiplier);
  }
}

void ReadDeclarer::child(const Query& query, Query::PlanningContext& planning,
                         int32_t flags, float multiplier) {
  if (ReadVerification::active() && ReadVerification::suppresses(query)) return;
  query.declareReads(*this, planning, flags, multiplier);
}

void ReadDeclarer::term(Query::PlanningContext& planning, std::string_view field,
                        std::string_view term, uint8_t products) {
  if (auto* cache = demandCache(planning)) cache->declareTerm(field, term, products);
  record(planning, field, std::nullopt, 0);
  record(planning, field, term, products);
}

void ReadDeclarer::field(Query::PlanningContext& planning, std::string_view field, uint8_t products) {
  if (auto* cache = demandCache(planning)) cache->declareField(field, products);
  record(planning, field, std::nullopt, products);
}

void ReadDeclarer::undeclared(const Query& query) {
  complete = false;
  if (auto* verification = ReadVerification::active(); verification && !undeclaredKind(query.getKind())) {
    verification->fail(std::format("unexpected undeclared query kind {}", (int)query.getKind()));
  }
}

void Query::declareReads(ReadDeclarer& reads, PlanningContext&, int32_t, float) const {
  reads.undeclared(*this);
}

thread_local ReadVerification* ReadVerification::current = nullptr;
thread_local std::optional<QueryKind> ReadVerification::suppressed;

ReadVerification::ReadVerification(bool aggregate)
    : previous(std::exchange(current, this)), aggregate(aggregate) {}

ReadVerification::~ReadVerification() {
  current = previous;
  if (!aggregate) return;
  for (auto& [key, demands] : declarations) {
    auto kind = std::get<1>(key)->getKind();
    for (const auto& [lookup, demand] : demands) {
      counts[(size_t)kind].unusedTerms += std::get<2>(lookup).has_value() && !demand.lookedUp;
    }
  }
  const std::lock_guard lock(totalsMutex);
  for (size_t i = 0; i < counts.size(); ++i) {
    totals[i].rootsChecked += counts[i].rootsChecked;
    totals[i].rootsExempt += counts[i].rootsExempt;
    totals[i].lookupsChecked += counts[i].lookupsChecked;
    totals[i].lookupsExempt += counts[i].lookupsExempt;
    totals[i].unusedTerms += counts[i].unusedTerms;
  }
  pluginOwners += plugin;
  totalFailures += failures;
}

bool ReadVerification::report(std::ostream& out) {
  const std::lock_guard lock(totalsMutex);
  out << "Read declaration coverage (query kind enum, checked roots, exempt roots, "
         "checked lookups, exempt lookups, unused terms)\n";
  for (size_t i = 0; i < totals.size(); ++i) {
    const auto& c = totals[i];
    if (c.rootsChecked || c.rootsExempt || c.lookupsChecked || c.lookupsExempt || c.unusedTerms) {
      out << i << ' ' << c.rootsChecked << ' ' << c.rootsExempt << ' '
          << c.lookupsChecked << ' ' << c.lookupsExempt << ' ' << c.unusedTerms << '\n';
    }
  }
  out << "Undeclared plugin owners: " << pluginOwners << ", mismatches: " << totalFailures << '\n';
  return totalFailures == 0;
}

void ReadVerification::root(const Query& query, Query::PlanningContext& planning,
                            int32_t flags, float multiplier) {
  auto [it, inserted] = declarations.try_emplace(key(query, planning, flags, multiplier));
  if (!inserted) return;
  recording = &it->second;
  auto restore = scope_guard([&] { recording = nullptr; });
  ReadDeclarer::root(query, planning, flags, multiplier);
}

void ReadVerification::record(Query::PlanningContext& planning, std::string_view field,
                              std::optional<std::string_view> term, uint8_t products) {
  if (recording != nullptr) (*recording)[{&planning, field, term}].products |= products;
}

ReadVerification::RootScope::RootScope(const Query& query, const Query::PlanningContext& planning,
                                        int32_t flags, float multiplier, bool empty)
    : verification(active()) {
  if (verification == nullptr) return;
  assert(verification->reading == nullptr);
  auto kind = query.getKind();
  if (verification->plugin || (!empty && undeclaredKind(kind))) {
    verification->counts[(size_t)kind].rootsExempt++;
  } else {
    verification->counts[(size_t)kind].rootsChecked++;
  }
  if (verification->plugin) return;
  auto found = verification->declarations.find(key(query, planning, flags, multiplier));
  if (found == verification->declarations.end()) {
    verification->fail(std::format("undeclared root mode {} flags {} in op {}", (int)kind, flags, planning.opName));
  }
  verification->reading = &found->second;
}

ReadVerification::RootScope::~RootScope() {
  if (verification != nullptr) verification->reading = nullptr;
}

Query::Weight* ReadVerification::buildChild(Query& query, Query::Context& context,
                                       int32_t flags, float multiplier) {
  auto oldExempt = exempt;
  auto oldBuilding = std::exchange(building, query.getKind());
  if (!exempt && undeclaredKind(building)) exempt = building;
  auto restore = scope_guard([&] {
    exempt = oldExempt;
    building = oldBuilding;
  });
  return query.createWeight(context, flags, multiplier);
}

void ReadVerification::check(const Query::PlanningContext& planning, std::string_view field,
                             std::optional<std::string_view> term) {
  if (plugin || (reading != nullptr && exempt)) {
    counts[(size_t)exempt.value_or(building)].lookupsExempt++;
    return;
  }
  if (reading == nullptr) return;
  auto found = reading->find({&planning, field, term});
  if (found == reading->end()) {
    fail(std::format("undeclared lookup in op {}, query kind {}, field {}, term {}",
                     planning.opName, (int)building, field, term.value_or("<field>")));
  }
  found->second.lookedUp = true;
  counts[(size_t)building].lookupsChecked++;
}

void ReadVerification::fail(std::string message) {
  failures++;
  throw std::logic_error(std::move(message));
}

Query::Weight* Query::buildCheckedWeight(Context& context, int32_t flags, float multiplier) {
  if (auto* verification = ReadVerification::active()) {
    return verification->buildChild(*this, context, flags, multiplier);
  }
  return createWeight(context, flags, multiplier);
}

void Query::Context::checkRead(std::string_view field, std::optional<std::string_view> term) {
  if (auto* verification = ReadVerification::active()) verification->check(planning, field, term);
}

} // namespace luxir
