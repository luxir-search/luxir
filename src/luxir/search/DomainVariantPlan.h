#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/query/Query.h"
#include "luxir/search/DocSet.h"
#include "luxir/search/RequestMemTracker.h"

namespace luxir {

// Parse-time sibling-domain identity plus the per-segment production race.
// Variant 0 is implicit: inherited M with every parent filter and no local
// filters. Stored variants extend that identity with a frame, routed parent
// filter subset, and ordered local-filter source list.
class DomainVariantPlan {
public:
  static constexpr size_t MAX_NON_DEFAULT_VARIANTS = 64;

  enum class Frame : uint8_t {
    INHERIT,
    RESET,
  };

  struct QuerySpec {
    Query* query;
    FilterKey identity;
  };

private:
  struct QuerySource {
    Query* query;
    FilterKey identity;
    Query::Weight* weight = nullptr;
    FilterCache::Use* use = nullptr;
  };

  struct InheritBase {
    std::vector<uint64_t> included;
  };

  struct Variant {
    Frame frame = Frame::INHERIT;
    std::vector<uint64_t> included;
    size_t inheritBase = 0;
    size_t resetSource = std::numeric_limits<size_t>::max();
    std::vector<size_t> extraSources;
  };

  struct ChildRoute {
    std::string_view key;
    uint8_t variant = 0;
  };

  size_t filterCount = 0;
  size_t domainOverrideCount = 0;
  std::vector<InheritBase> inheritBases;
  std::vector<Variant> variants;
  std::vector<ChildRoute> childRoutes;
  std::vector<QuerySource> querySources;

  static bool bitIncluded(
      std::span<const uint64_t> words, size_t filter) {
    return (words[filter >> 6] & (1ULL << (filter & 63))) != 0;
  }

  bool baseIncludes(size_t baseVariant, size_t filter) const {
    assert(baseVariant <= inheritBases.size());
    assert(filter < filterCount);
    if (baseVariant == 0) return true;
    return bitIncluded(inheritBases[baseVariant - 1].included, filter);
  }

  size_t internSource(const QuerySpec& spec) {
    auto found = std::find_if(
        querySources.begin(), querySources.end(),
        [&](const QuerySource& source) {
          return source.identity == spec.identity;
        });
    if (found != querySources.end()) {
      return (size_t) (found - querySources.begin());
    }
    querySources.push_back({spec.query, spec.identity});
    return querySources.size() - 1;
  }

  size_t internInheritBase(const std::vector<uint64_t>& included) {
    bool all = true;
    for (size_t filter = 0; filter < filterCount; filter++) {
      if (!bitIncluded(included, filter)) {
        all = false;
        break;
      }
    }
    if (all) return 0;
    auto found = std::find_if(
        inheritBases.begin(), inheritBases.end(),
        [&](const InheritBase& base) {
          return base.included == included;
        });
    if (found != inheritBases.end()) {
      return (size_t) (found - inheritBases.begin()) + 1;
    }
    inheritBases.push_back({included});
    return inheritBases.size();
  }

  static int64_t saturatedAdd(int64_t left, int64_t right) {
    if (right > std::numeric_limits<int64_t>::max() - left) {
      return std::numeric_limits<int64_t>::max();
    }
    return left + right;
  }

  static size_t saturatedMultiply(size_t left, size_t right) {
    if (left != 0 && right > std::numeric_limits<size_t>::max() / left) {
      return std::numeric_limits<size_t>::max();
    }
    return left * right;
  }

  static DomainHandle freeze(DomainHandle domain) {
    if (domain.get() != nullptr) unused(domain.get()->card());
    return domain;
  }

  DomainHandle composeBase(
      size_t baseVariant, DomainHandle identity,
      std::span<const DomainHandle> filters) const {
    assert(filters.size() == filterCount);
    std::vector<DomainHandle> parts;
    parts.reserve(filterCount + 1);
    parts.push_back(std::move(identity));
    for (size_t i = 0; i < filterCount; i++) {
      if (baseIncludes(baseVariant, i)) parts.push_back(filters[i]);
    }
    return compose(std::move(parts));
  }

public:
  enum class Offer : uint8_t {
    IDENTITY,
    SHARED_M,
    INDEPENDENT,
  };

  static inline std::optional<Offer> forcedOfferForTests;

  struct CostedOffer {
    Offer offer = Offer::IDENTITY;
    int64_t cpuCost = 0;
    size_t peakBytes = 0;
  };

  struct Selection {
    std::array<CostedOffer, 3> offers;
    size_t count = 0;
  };

  class Reservation {
    static constexpr std::string_view BREAKER = "domain variants";

    RequestMemTracker* tracker;
    std::string detail;
    size_t charged = 0;

  public:
    Reservation(
        RequestMemTracker& tracker, size_t bytes, std::string detail)
      : tracker(&tracker), detail(std::move(detail)) {
      tracker.charge(bytes, BREAKER, this->detail);
      charged = bytes;
    }

    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    ~Reservation() {
      tracker->release(charged);
    }

    void grow(size_t bytes) {
      tracker->charge(bytes, BREAKER, detail);
      charged += bytes;
    }

    bool tryGrow(size_t bytes) {
      if (!tracker->tryCharge(bytes)) return false;
      charged += bytes;
      return true;
    }
  };

  DomainVariantPlan() = default;

  void begin(size_t count) {
    assert(variants.empty());
    assert(childRoutes.empty());
    assert(querySources.empty());
    filterCount = count;
  }

  template <typename Excluded>
  bool addChild(
      std::string_view key, Excluded&& excluded,
      const QuerySpec* resetQuery = nullptr,
      bool applyParentFilters = false,
      std::span<const QuerySpec> extraFilters = {}) {
    Frame frame = resetQuery == nullptr ? Frame::INHERIT : Frame::RESET;
    std::vector<uint64_t> included((filterCount + 63) >> 6, 0);
    if (frame == Frame::INHERIT || applyParentFilters) {
      for (size_t i = 0; i < filterCount; i++) {
        if (!excluded(i)) included[i >> 6] |= 1ULL << (i & 63);
      }
    }

    Variant candidate;
    candidate.frame = frame;
    candidate.included = std::move(included);
    if (frame == Frame::INHERIT) {
      candidate.inheritBase = internInheritBase(candidate.included);
    } else {
      candidate.resetSource = internSource(*resetQuery);
    }
    candidate.extraSources.reserve(extraFilters.size());
    for (const auto& filter : extraFilters) {
      candidate.extraSources.push_back(internSource(filter));
    }

    if (candidate.frame == Frame::INHERIT
        && candidate.inheritBase == 0
        && candidate.extraSources.empty()) {
      childRoutes.push_back({key, 0});
      return false;
    }

    auto found = std::find_if(
        variants.begin(), variants.end(), [&](const Variant& variant) {
          return variant.frame == candidate.frame
              && variant.included == candidate.included
              && variant.resetSource == candidate.resetSource
              && variant.extraSources == candidate.extraSources;
        });
    size_t variant;
    bool created = found == variants.end();
    if (created) {
      if (variants.size() >= MAX_NON_DEFAULT_VARIANTS) {
        bool domainOverride = resetQuery != nullptr || !extraFilters.empty();
        throw std::runtime_error(
            "op '" + std::string(key)
            + (domainOverride
                    ? "': domain variant cap 64 exceeded"
                    : "': routed filter variant cap 64 exceeded"));
      }
      variants.push_back(std::move(candidate));
      if (resetQuery != nullptr || !extraFilters.empty()) {
        domainOverrideCount++;
      }
      variant = variants.size();
    } else {
      variant = (size_t) (found - variants.begin()) + 1;
    }
    childRoutes.push_back({key, (uint8_t) variant});
    return created;
  }

  void buildWeights(
      Query::Context& context, Query::PlanningContext& planning,
      int32_t requestFlags) {
    int32_t filterFlags = requestFlags
        & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
    for (auto& source : querySources) {
      source.weight = source.query->createWeight(context, filterFlags);
      source.use = planning.getFilterUse(*source.query);
    }
  }

  bool empty() const { return variants.empty(); }
  size_t nonDefaultVariantCount() const { return variants.size(); }
  size_t variantCount() const { return empty() ? 1 : variants.size() + 1; }
  size_t inheritBaseCount() const { return inheritBases.size() + 1; }
  size_t sourceCount() const { return querySources.size(); }

  Query::Weight* sourceWeight(size_t source) const {
    assert(source < querySources.size());
    return querySources[source].weight;
  }

  FilterCache::Use* sourceUse(size_t source) const {
    assert(source < querySources.size());
    return querySources[source].use;
  }

  bool sourcesNeedPrepare() const {
    return std::ranges::any_of(querySources, [](const QuerySource& source) {
      assert(source.weight != nullptr);
      return source.weight->needsPrepare();
    });
  }

  bool needsInheritProduction() const {
    return std::ranges::any_of(variants, [](const Variant& variant) {
      return variant.frame == Frame::INHERIT;
    });
  }

  bool hasResetVariants() const {
    return std::ranges::any_of(variants, [](const Variant& variant) {
      return variant.frame == Frame::RESET;
    });
  }

  bool needsResetParentFilters() const {
    for (const auto& variant : variants) {
      if (variant.frame != Frame::RESET) continue;
      for (size_t filter = 0; filter < filterCount; filter++) {
        if (bitIncluded(variant.included, filter)) return true;
      }
    }
    return false;
  }

  Frame frame(size_t variant) const {
    assert(variant < variantCount());
    return variant == 0 ? Frame::INHERIT : variants[variant - 1].frame;
  }

  size_t inheritBase(size_t variant) const {
    assert(frame(variant) == Frame::INHERIT);
    return variant == 0 ? 0 : variants[variant - 1].inheritBase;
  }

  size_t resetSource(size_t variant) const {
    assert(variant > 0 && frame(variant) == Frame::RESET);
    return variants[variant - 1].resetSource;
  }

  std::span<const size_t> extraSources(size_t variant) const {
    assert(variant < variantCount());
    if (variant == 0) return {};
    return variants[variant - 1].extraSources;
  }

  bool includesFilter(size_t variant, size_t filter) const {
    assert(variant < variantCount());
    assert(filter < filterCount);
    if (variant == 0) return true;
    return bitIncluded(variants[variant - 1].included, filter);
  }

  size_t estimatedSetBytes(int32_t maxDoc) const {
    size_t bitset = sizeof(RAMBitDocSet)
        + FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
    size_t array = sizeof(ArrDocSet)
        + (size_t) DocSetBuilder::arrayLimitFor(maxDoc) * sizeof(int32_t);
    return std::max(bitset, array);
  }

  std::shared_ptr<Reservation> reserveProduction(
      RequestMemTracker& tracker, int32_t maxDoc,
      std::string detail) const {
    // Three full-set scratch allowances cover ARRAY growth and promotion
    // transients. Produced outputs are reserved by the selected offer and
    // owned query/filter artifacts grow the charge at materialization time.
    size_t bytes = saturatedMultiply(
        3 + domainOverrideCount, estimatedSetBytes(maxDoc));
    return std::make_shared<Reservation>(
        tracker, bytes, std::move(detail));
  }

  size_t variantForChild(std::string_view key) const {
    if (empty()) return 0;
    auto found = std::find_if(
        childRoutes.begin(), childRoutes.end(), [&](const ChildRoute& route) {
          return route.key == key;
        });
    return found == childRoutes.end() ? 0 : found->variant;
  }

  // The stage-3 race is deliberately over inherited-M bases only. Local G
  // filters compose after the selected base is produced, while reset variants
  // are independent executions and never create a raw-M requirement.
  Selection selectOffer(
      int64_t rawQueryCost, int32_t maxDoc, DomainHandle base,
      std::span<const DomainHandle> filters, bool implicitMatches,
      bool reusableRawArtifact, bool exactDomainsServeParent) const {
    assert(needsInheritProduction());
    assert(filters.size() == filterCount);
    int64_t rawCost = std::max<int64_t>(0, rawQueryCost);
    int64_t independentMembershipCost = 0;
    int64_t defaultRankingCost = rawCost;
    for (size_t variant = 0; variant < inheritBaseCount(); variant++) {
      int64_t eligibility = base.get() == nullptr
          ? (int64_t) maxDoc : (int64_t) base.get()->card();
      for (size_t filter = 0; filter < filterCount; filter++) {
        if (baseIncludes(variant, filter)
            && filters[filter].get() != nullptr) {
          eligibility = std::min<int64_t>(
              eligibility, filters[filter].get()->card());
        }
      }
      int64_t variantCost = std::min(rawCost, eligibility);
      independentMembershipCost = saturatedAdd(
          independentMembershipCost, variantCost);
      if (variant == 0) defaultRankingCost = variantCost;
    }
    size_t setBytes = estimatedSetBytes(maxDoc);
    size_t produced = inheritBaseCount();
    Selection selection;
    if (implicitMatches) {
      selection.offers[selection.count++] = {
          Offer::IDENTITY,
          exactDomainsServeParent ? 0 : defaultRankingCost,
          saturatedMultiply(produced + 1, setBytes)};
    }
    int64_t sharedCost = reusableRawArtifact || implicitMatches
        ? (exactDomainsServeParent ? 0 : defaultRankingCost)
        : rawCost;
    selection.offers[selection.count++] = {
        Offer::SHARED_M, sharedCost,
        saturatedMultiply(
            produced + 1
                + (size_t) (!reusableRawArtifact
                            || base.get() != nullptr),
            setBytes)};
    int64_t independentCost = independentMembershipCost;
    if (!exactDomainsServeParent) {
      independentCost = saturatedAdd(
          independentCost, defaultRankingCost);
    }
    selection.offers[selection.count++] = {
        Offer::INDEPENDENT, independentCost,
        saturatedMultiply(produced + 2, setBytes)};
    std::sort(
        selection.offers.begin(),
        selection.offers.begin() + (std::ptrdiff_t) selection.count,
        [](const CostedOffer& left, const CostedOffer& right) {
          if (left.cpuCost != right.cpuCost) {
            return left.cpuCost < right.cpuCost;
          }
          if (left.peakBytes != right.peakBytes) {
            return left.peakBytes < right.peakBytes;
          }
          return (uint8_t) left.offer < (uint8_t) right.offer;
        });
    if (forcedOfferForTests.has_value()) {
      auto forced = std::find_if(
          selection.offers.begin(),
          selection.offers.begin() + (std::ptrdiff_t) selection.count,
          [&](const CostedOffer& offer) {
            return offer.offer == *forcedOfferForTests;
          });
      if (forced == selection.offers.begin()
              + (std::ptrdiff_t) selection.count) {
        throw std::logic_error("forced domain variant offer is unavailable");
      }
      std::iter_swap(selection.offers.begin(), forced);
    }
    return selection;
  }

  DomainHandle constrainRaw(
      DomainHandle rawMatches, DomainHandle base) const {
    if (rawMatches.get() == nullptr) return freeze(std::move(base));
    if (base.get() == nullptr) return freeze(std::move(rawMatches));
    std::array<DocSet*, 2> parts{rawMatches.get(), base.get()};
    return freeze(DomainHandle(DocSet::intersect(parts)));
  }

  std::vector<DomainHandle> produceSharedBases(
      DomainHandle rawMatches,
      std::span<const DomainHandle> filters) const {
    assert(needsInheritProduction());
    std::vector<DomainHandle> output(inheritBaseCount());
    for (size_t variant = 0; variant < output.size(); variant++) {
      output[variant] = composeBase(variant, rawMatches, filters);
      assert(output[variant].isDeliverable());
    }
    return output;
  }

  template <typename Producer>
  std::vector<DomainHandle> produceIndependentBases(
      DomainHandle base,
      std::span<const DomainHandle> filters, Producer&& producer) const {
    assert(needsInheritProduction());
    std::vector<DomainHandle> output(inheritBaseCount());
    for (size_t variant = 0; variant < output.size(); variant++) {
      DomainHandle eligibility = composeBase(variant, base, filters);
      output[variant] = freeze(producer(eligibility.get()));
      assert(output[variant].isDeliverable());
    }
    return output;
  }

  DomainHandle defaultDomain(
      DomainHandle base, std::span<const DomainHandle> filters) const {
    return composeBase(0, std::move(base), filters);
  }

  static DomainHandle compose(std::vector<DomainHandle> parts) {
    std::vector<DocSet*> sets;
    DomainHandle single;
    sets.reserve(parts.size());
    for (auto& part : parts) {
      if (part.get() == nullptr) continue;
      sets.push_back(part.get());
      single = part;
    }
    if (sets.empty()) return {};
    if (sets.size() == 1) return freeze(std::move(single));
    return freeze(DomainHandle(DocSet::intersect(sets)));
  }

  void retainReservation(
      std::vector<DomainHandle>& domains,
      const std::shared_ptr<Reservation>& reservation) const {
    for (auto& domain : domains) {
      domain = std::move(domain).retainedWith(reservation);
    }
  }
};

} // namespace luxir
