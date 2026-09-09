// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <string>
#include "luxir/api/luxir_index.hpp"
#include "luxir/schema/FieldType.h"

namespace luxir {

// Owning manifest state. Values are canonical JSON so diagnostics and the
// resolved view use the same effective properties as compatibility checking.
struct FieldSignature {
  std::string logicalName;
  std::string label;
  std::map<std::string, std::string> properties;

  FieldSignature(std::string_view name, FieldType& type);
  explicit FieldSignature(const api::PhysicalFieldSignature& wire);
  api::PhysicalFieldSignature toWire(std::string_view name, std::pmr::memory_resource& arena) const;
  void checkCompatible(std::string_view name, const FieldSignature& candidate) const;
};

using FieldSignatures = std::map<std::string, FieldSignature, std::less<>>;

} // namespace luxir
