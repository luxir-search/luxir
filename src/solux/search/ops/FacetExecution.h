#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "SearchOp.h"

namespace solux {

// These identities coincide for an ordinary top-N result, but are distinct in
// the execution model. Owner slots are reusable counter routes; output slots
// are positions in a materialized response; bucket ids are local to the
// reader/ordinal epoch.
struct FacetBucketId {
  int64_t value;
};

struct FacetOwnerSlot {
  int32_t value;
};

struct FacetOutputSlot {
  int32_t value;
};

enum class FacetBucketFlags : uint8_t {
  NONE = 0,
  PINNED = 1 << 0
};

template <typename Key>
struct SelectedFacetBucket {
  // Borrowed keys are valid because post-selection execution is synchronous.
  // An asynchronous executor must own or otherwise pin them.
  Key key;
  std::optional<FacetBucketId> id;
  int64_t count;
  FacetOwnerSlot owner;
  FacetOutputSlot output;
  FacetBucketFlags flags = FacetBucketFlags::NONE;
};

// Baseline post-selection binding. The parent supplies the selected buckets
// and the concrete source that materializes one repeatable domain per segment.
// SearchOp::createCalculator remains the child-owned binding factory.
//
// Execution is deliberately synchronous and segment-at-a-time within an owner
// block. Each freshly materialized domain reaches all children while it is hot;
// children that need cross-segment state retain the owned handles through their
// ordinary calc contract. A null task group keeps submitted work inline so the
// calculator bindings cannot outlive the executor.
class FacetBucketDomainExecutor {
public:
  template <typename Key, typename DomainSource>
  static void execute(
      SearchOp::Calculator& parent,
      std::span<SearchOp* const> children,
      std::span<const SelectedFacetBucket<Key>> buckets,
      int32_t numSegments,
      DomainSource&& materializeDomain) {
    if (children.empty()) return;

    for (const auto& bucket : buckets) {
      assert(bucket.owner.value >= 0);
      assert(bucket.output.value >= 0);
      assert(bucket.output.value < (int32_t)buckets.size());

      std::vector<std::unique_ptr<SearchOp::Calculator>> bindings;
      bindings.reserve(children.size());
      for (SearchOp* child : children) {
        bindings.emplace_back(child->createCalculator(
            &parent, bucket.output.value, (int64_t)buckets.size()));
      }

      if (numSegments == 0) {
        std::span<const DomainHandle> noDomains;
        for (auto& binding : bindings) {
          binding->calcAll(nullptr, noDomains);
        }
        continue;
      }

      for (int32_t segment = 0; segment < numSegments; segment++) {
        DomainHandle domain = materializeDomain(segment, bucket);
        assert(domain.isDeliverable());
        for (auto& binding : bindings) {
          binding->calc(nullptr, segment, domain);
        }
      }
    }
  }
};

} // namespace solux
