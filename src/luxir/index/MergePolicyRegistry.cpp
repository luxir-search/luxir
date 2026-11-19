// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/index/MergePolicyRegistry.h"

#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/index/TieredMergePolicy.h"

namespace luxir {

MergePolicySpec::MergePolicySpec(const api::MergePolicyDef& source) {
  std::vector<std::byte> bytes;
  if (!api::encode(source, bytes)) throw std::runtime_error("Failed to serialize MergePolicyDef");
  if (!api::decode(def, api::copyToPaddedInput(bytes, arena), arena)) {
    throw std::runtime_error("Failed to parse MergePolicyDef");
  }
}

void MergePolicyRegistry::add(std::string name, Maker maker) {
  if (isFrozen) throw std::logic_error("merge policy registry is frozen; register during startup");
  if (name.empty()) throw std::invalid_argument("merge policy name must not be empty");
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      throw std::invalid_argument("invalid merge policy name '" + name + "'; use [a-z0-9_]+");
    }
  }
  if (!maker) throw std::invalid_argument("null merge policy maker '" + name + "'");
  if (makers.contains(name)) throw std::invalid_argument("duplicate merge policy '" + name + "'");
  makers.emplace(std::move(name), std::move(maker));
}

std::vector<std::string> MergePolicyRegistry::names() const {
  std::vector<std::string> result;
  for (const auto& entry : makers) result.push_back(entry.first);
  return result;
}

std::unique_ptr<MergePolicy> MergePolicyRegistry::make(std::string_view name, const ParamReader::Entries& params,
                                                       int32_t nodeMergeFactor) const {
  auto it = makers.find(name);
  if (it == makers.end()) {
    std::string valid;
    for (const auto& entry : makers) valid += (valid.empty() ? "" : ", ") + entry.first;
    throw std::invalid_argument("unknown merge policy '" + std::string(name) + "'; valid policies: " + valid);
  }
  ParamReader reader("merge_policy '" + it->first + "'", params);
  auto policy = it->second(MergePolicyContext{reader, nodeMergeFactor});
  reader.rejectUnknown();
  if (!policy) throw std::logic_error("merge policy maker '" + it->first + "' returned null");
  return policy;
}

std::unique_ptr<MergePolicy> MergePolicyRegistry::make(const MergePolicySpec* spec, int32_t nodeMergeFactor) const {
  if (!spec) return make(DEFAULT_POLICY, {}, nodeMergeFactor);
  return make(spec->def.name, spec->def.params, nodeMergeFactor);
}

void registerBuiltinMergePolicies(MergePolicyRegistry& registry) {
  registry.add(std::string(TieredMergePolicy::NAME), [](const MergePolicyContext& ctx) {
    int64_t factor = ctx.params.getInt("merge_factor", ctx.nodeMergeFactor);
    if (factor < 2 || factor > INT32_MAX) ctx.params.fail("merge_factor", "must be an integer from 2 to 2147483647");
    return std::make_unique<TieredMergePolicy>((int32_t)factor);
  });
}

MergePolicyRegistry& MergePolicyRegistry::global() {
  static MergePolicyRegistry instance = [] {
    MergePolicyRegistry registry;
    registerBuiltinMergePolicies(registry);
    return registry;
  }();
  return instance;
}

}  // namespace luxir
