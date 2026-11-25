// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/Query.h"

#include <array>
#include <bit>
#include <iosfwd>
#include <map>
#include <tuple>

namespace luxir {

// Record wanted products in the pinned view's term cache without resolving
// them. Norms and positions are wanted if the index can supply them.
class ReadDeclarer {
  bool complete = true;

protected:
  virtual void record(Query::PlanningContext&, std::string_view,
                      std::optional<std::string_view>, uint8_t) {}

public:
  virtual ~ReadDeclarer() = default;

  virtual void root(const Query& query, Query::PlanningContext& planning,
            int32_t flags, float multiplier = 1.0f);
  void child(const Query& query, Query::PlanningContext& planning,
             int32_t flags, float multiplier = 1.0f);
  void term(Query::PlanningContext& planning, std::string_view field,
            std::string_view term, uint8_t products);
  void field(Query::PlanningContext& planning, std::string_view field,
             uint8_t products = 0);
  void undeclared(const Query& query);
  bool fullyDeclared() const noexcept { return complete; }
};

// Test bookkeeping is separate from the view's union of wanted products.
// Each mode retains only the lookups declared by that root.
class ReadVerification final : public ReadDeclarer {
  friend class ReadDeclarer;
  // A root's identity includes the planning view, flags, and multiplier bits.
  using RootKey = std::tuple<const Query::PlanningContext*, const Query*, int32_t, uint32_t>;

public:
  using Lookup = std::tuple<const Query::PlanningContext*, std::string_view,
                            std::optional<std::string_view>>;
  struct Demand {
    uint8_t products = 0;
    bool lookedUp = false;

    // For fixtures with independently known product needs, not the lookup oracle.
    int unusedProducts(uint8_t needed) const { return std::popcount((unsigned)(products & ~needed)); }
  };
  using Demands = std::map<Lookup, Demand>;

  struct Counts {
    uint64_t rootsChecked = 0;
    uint64_t rootsExempt = 0;
    uint64_t lookupsChecked = 0;
    uint64_t lookupsExempt = 0;
    uint64_t unusedTerms = 0;
  };

private:
  static constexpr size_t kindCount = (size_t)QueryKind::TEST + 1;
  // Each serial weight-building step owns its active test scope.
  static thread_local ReadVerification* current;
  // Negative tests omit a single kind without changing its exemption status.
  static thread_local std::optional<QueryKind> suppressed;
  ReadVerification* previous;
  // Per-mode lookups, separate from the shared cache's resident union.
  std::map<RootKey, Demands> declarations;
  // The root mode currently being projected, if any.
  Demands* recording = nullptr;
  // The root mode tested by the current weight build, if any.
  Demands* reading = nullptr;
  // The first unsupported ancestor of the current weight call.
  std::optional<QueryKind> exempt;
  // The innermost query making the current lookup.
  QueryKind building = QueryKind::TEST;
  std::array<Counts, kindCount> counts = {};
  bool plugin = false;
  // Focused fixtures can keep their counts out of the suite-wide report.
  bool aggregate;
  uint64_t failures = 0;

  static RootKey key(const Query& query, const Query::PlanningContext& planning,
                     int32_t flags, float multiplier) {
    return {&planning, &query, flags, std::bit_cast<uint32_t>(multiplier)};
  }
  [[noreturn]] void fail(std::string message);
  void record(Query::PlanningContext&, std::string_view,
              std::optional<std::string_view>, uint8_t) override;

public:
  explicit ReadVerification(bool aggregate = true);
  ~ReadVerification() override;
  ReadVerification(const ReadVerification&) = delete;

  static ReadVerification* active() noexcept {
#ifndef NDEBUG
    if (Query::verifyReadDeclarationsForTests) return current;
#endif
    return nullptr;
  }
  static void suppressForTests(std::optional<QueryKind> kind) { suppressed = kind; }
  static bool suppresses(const Query& query) { return suppressed == query.getKind(); }
  static bool report(std::ostream& out);

  void root(const Query& query, Query::PlanningContext& planning,
            int32_t flags, float multiplier = 1.0f) override;
  void pluginOwner() { plugin = true; }
  const Demands& demands(const Query& query, const Query::PlanningContext& planning,
                         int32_t flags, float multiplier = 1.0f) const {
    return declarations.at(key(query, planning, flags, multiplier));
  }
  const Counts& countsFor(QueryKind kind) const { return counts[(size_t)kind]; }

  class RootScope {
    ReadVerification* verification;
  public:
    RootScope(const Query& query, const Query::PlanningContext& planning,
              int32_t flags, float multiplier, bool empty);
    ~RootScope();
  };

  Query::Weight* buildChild(Query& query, Query::Context& context, int32_t flags, float multiplier);
  void check(const Query::PlanningContext& planning, std::string_view field,
             std::optional<std::string_view> term);
};

} // namespace luxir
