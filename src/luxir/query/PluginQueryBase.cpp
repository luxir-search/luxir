// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/PluginQueryBase.h"

#include <map>
#include <mutex>
#include <typeindex>

#include "luxir/query/QueryRegistry.h"

namespace luxir {

namespace {

uint64_t concreteTypeToken(const std::type_info& type) {
  // Intern RTTI equality rather than hashing it or assuming that equal
  // type_info objects have the same address. Tokens never leave this process.
  static std::mutex mutex;
  static std::map<std::type_index, uint64_t> tokens;
  std::lock_guard lock(mutex);
  return tokens.try_emplace(type, (uint64_t)tokens.size() + 1).first->second;
}

}  // namespace

PluginQueryBase::PluginQueryBase(const PluginQueryContext& ctx)
  : Query(QueryKind::PLUGIN), typeName(ctx.name) {}

bool PluginQueryBase::equalsSameKind(const Query& other) const {
  if (typeid(*this) != typeid(other)) return false;
  const auto& rhs = static_cast<const PluginQueryBase&>(other);
  return typeName == rhs.typeName && equalsPayload(rhs);
}

uint64_t PluginQueryBase::hashImpl() const {
  uint64_t value = mixHash(Query::hashImpl(), concreteTypeToken(typeid(*this)));
  value = mixHash(value, typeName);
  return mixHash(value, hashPayload());
}

FilterKeyScope PluginQueryBase::appendFilterKey(
    FilterKeyBuilder& out, const FilterKeyContext& ctx) const {
  out.appendKind(QueryKind::PLUGIN);
  out.appendUInt64(concreteTypeToken(typeid(*this)));
  out.appendString(typeName);
  return appendPayloadFilterKey(out, ctx);
}

}  // namespace luxir
