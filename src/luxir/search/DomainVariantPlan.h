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

#include "luxir/search/DocSet.h"
#include "luxir/search/RequestMemTracker.h"

namespace luxir {

// Parse-time routing shape plus the per-segment production race for sibling
// domains. Variant 0 is implicit and contains every filter. Stored variants
// are the distinct non-default filter subsets requested by direct children.
class DomainVariantPlan {
  struct Variant {
    std::vector<uint64_t> included;
  };

  struct ChildRoute {
    std::string_view key;
    uint8_t variant = 0;
  };

  size_t filterCount = 0;
  std::vector<Variant> variants;
  std::vector<ChildRoute> childRoutes;

  bool includes(size_t variant, size_t filter) const {
    assert(variant <= variants.size());
    assert(filter < filterCount);
    if (variant == 0) return true;
    const auto& words = variants[variant - 1].included;
    return (words[filter >> 6] & (1ULL << (filter & 63))) != 0;
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

  DomainHandle compose(
      size_t variant, DomainHandle identity,
      std::span<const DomainHandle> filters) const {
    assert(filters.size() == filterCount);
    std::vector<DocSet*> parts;
    parts.reserve(filterCount + 1);
    DomainHandle single;
    if (identity.get() != nullptr) {
      parts.push_back(identity.get());
      single = identity;
    }
    for (size_t i = 0; i < filterCount; i++) {
      if (!includes(variant, i) || filters[i].get() == nullptr) continue;
      parts.push_back(filters[i].get());
      single = filters[i];
    }
    if (parts.empty()) return {};
    if (parts.size() == 1) return freeze(std::move(single));
    return freeze(DomainHandle(DocSet::intersect(parts)));
  }

public:
  static constexpr size_t MAX_NON_DEFAULT_VARIANTS = 64;

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
    assert(count > 0);
    filterCount = count;
  }

  template <typename Excluded>
  void addChild(std::string_view key, Excluded&& excluded) {
    assert(filterCount > 0);
    std::vector<uint64_t> included((filterCount + 63) >> 6, 0);
    bool nonDefault = false;
    for (size_t i = 0; i < filterCount; i++) {
      if (excluded(i)) {
        nonDefault = true;
      } else {
        included[i >> 6] |= 1ULL << (i & 63);
      }
    }
    if (!nonDefault) {
      childRoutes.push_back({key, 0});
      return;
    }

    auto found = std::find_if(
        variants.begin(), variants.end(), [&](const Variant& variant) {
          return variant.included == included;
        });
    size_t variant;
    if (found == variants.end()) {
      if (variants.size() >= MAX_NON_DEFAULT_VARIANTS) {
        throw std::runtime_error(
            "op '" + std::string(key)
            + "': routed filter variant cap 64 exceeded");
      }
      variants.push_back({std::move(included)});
      variant = variants.size();
    } else {
      variant = (size_t) (found - variants.begin()) + 1;
    }
    childRoutes.push_back({key, (uint8_t) variant});
  }

  bool empty() const { return variants.empty(); }
  size_t nonDefaultVariantCount() const { return variants.size(); }
  size_t variantCount() const { return empty() ? 1 : variants.size() + 1; }

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
    // Three full-set scratch allowances cover the ARRAY vector's growth
    // capacity plus the simultaneously live BITSET during promotion, and the
    // equivalent vector shrink/intersection transient, without forcing sparse
    // builders to reserve their maximum capacity. Request-owned filter sets
    // are charged on their Use slots, shared cache values remain charged to
    // the cache, and returned owned filters grow this reservation exactly.
    size_t bytes = saturatedMultiply(3, estimatedSetBytes(maxDoc));
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

  // Initial total-work rule. SHARED_M either reuses an artifact and retains
  // the ordinary default ranking walk, or captures raw M in one exhaustive
  // ranking walk. INDEPENDENT pays one membership execution per variant plus
  // the ordinary default ranking walk unless those exact domains answer the
  // parent directly. Intersection constants are deliberately left equal
  // until measurements refit them.
  Selection selectOffer(
      int64_t rawQueryCost, int32_t maxDoc, DomainHandle base,
      std::span<const DomainHandle> filters, bool implicitMatches,
      bool reusableRawArtifact, bool exactDomainsServeParent) const {
    assert(!empty());
    assert(filters.size() == filterCount);
    int64_t rawCost = std::max<int64_t>(0, rawQueryCost);
    int64_t independentMembershipCost = 0;
    int64_t defaultRankingCost = rawCost;
    for (size_t variant = 0; variant < variantCount(); variant++) {
      int64_t eligibility = base.get() == nullptr
          ? (int64_t) maxDoc : (int64_t) base.get()->card();
      for (size_t filter = 0; filter < filterCount; filter++) {
        if (includes(variant, filter) && filters[filter].get() != nullptr) {
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
    size_t produced = variantCount();
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

  std::vector<DomainHandle> produceShared(
      DomainHandle rawMatches,
      std::span<const DomainHandle> filters) const {
    assert(!empty());
    std::vector<DomainHandle> output(variantCount());
    for (size_t variant = 0; variant < output.size(); variant++) {
      output[variant] = compose(variant, rawMatches, filters);
      assert(output[variant].isDeliverable());
    }
    return output;
  }

  template <typename Producer>
  std::vector<DomainHandle> produceIndependent(
      DomainHandle base,
      std::span<const DomainHandle> filters, Producer&& producer) const {
    assert(!empty());
    std::vector<DomainHandle> output(variantCount());
    for (size_t variant = 0; variant < output.size(); variant++) {
      DomainHandle eligibility = compose(variant, base, filters);
      output[variant] = freeze(producer(eligibility.get()));
      assert(output[variant].isDeliverable());
    }
    return output;
  }

  DomainHandle defaultDomain(
      DomainHandle base, std::span<const DomainHandle> filters) const {
    return compose(0, std::move(base), filters);
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
