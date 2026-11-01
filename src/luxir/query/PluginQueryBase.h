// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/query/Query.h"

namespace luxir {

class PluginQueryContext;

// Identity for engine queries sharing QueryKind::PLUGIN. The registered name
// borrows the registry's key; payload members must also stay trivially destructible.
class PluginQueryBase : public Query {
  const std::string_view typeName;

protected:
  explicit PluginQueryBase(const PluginQueryContext& ctx);
  ~PluginQueryBase() = default;

  bool equalsSameKind(const Query& other) const final;
  uint64_t hashImpl() const final;

  // Called only after the registered name and concrete C++ type match.
  virtual bool equalsPayload(const PluginQueryBase& other) const = 0;
  virtual uint64_t hashPayload() const = 0;
  virtual FilterKeyScope appendPayloadFilterKey(
      FilterKeyBuilder& out, const FilterKeyContext& ctx) const = 0;

public:
  std::string_view registeredName() const { return typeName; }
  FilterKeyScope appendFilterKey(
      FilterKeyBuilder& out, const FilterKeyContext& ctx) const final;
};

}  // namespace luxir
