// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/index/MergePolicy.h"
#include "luxir/util/ParamReader.h"

namespace luxir {

// What a merge policy maker may read when a collection builds its policy.
// References are valid only for the duration of the maker call.
struct MergePolicyContext {
  const ParamReader& params;  // the collection's merge_policy.params
  // The node's --indexing.merge-factor: the default for a policy with a
  // merge factor.
  int32_t nodeMergeFactor;
};

// A collection's merge policy selection (SchemaDef.merge_policy), owning its
// params.
class MergePolicySpec {
  std::pmr::monotonic_buffer_resource arena;

public:
  api::MergePolicyDef def;  // views into this spec

  explicit MergePolicySpec(const api::MergePolicyDef& source);
  MergePolicySpec(const MergePolicySpec&) = delete;
  MergePolicySpec& operator=(const MergePolicySpec&) = delete;
};

// Named merge policies. "tiered" (TieredMergePolicy) is built in and is what
// a collection without a merge_policy uses. Filled during startup
// (initPlugins), then frozen.
class MergePolicyRegistry {
public:
  using Maker = std::function<std::unique_ptr<MergePolicy>(const MergePolicyContext&)>;

private:
  std::map<std::string, Maker, std::less<>> makers;
  bool isFrozen = false;

public:
  static constexpr std::string_view DEFAULT_POLICY = "tiered";

  // The process-wide registry, seeded with the built-in policies.
  static MergePolicyRegistry& global();

  // Throws std::invalid_argument on a bad or duplicate name, std::logic_error
  // after the freeze. Names are [a-z0-9_]+.
  void add(std::string name, Maker maker);

  // Builds the policy `name` from `params`. Throws std::invalid_argument
  // listing the registered names when `name` is unknown, and ParamError for a
  // bad or unread param (makers read through ctx.params; the registry rejects
  // the keys they did not read).
  std::unique_ptr<MergePolicy> make(std::string_view name, const ParamReader::Entries& params,
                                    int32_t nodeMergeFactor) const;
  // The policy `spec` selects, or the default policy when it is null.
  std::unique_ptr<MergePolicy> make(const MergePolicySpec* spec, int32_t nodeMergeFactor) const;

  bool contains(std::string_view name) const { return makers.contains(name); }
  // Sorted.
  std::vector<std::string> names() const;

  bool frozen() const { return isFrozen; }
  void freeze() { isFrozen = true; }
};

// Registers the built-in policies: tiered. Called when the global registry is
// built.
void registerBuiltinMergePolicies(MergePolicyRegistry& registry);

}  // namespace luxir
