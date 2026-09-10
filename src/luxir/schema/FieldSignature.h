// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <string>
#include "luxir/schema/FieldType.h"

namespace luxir {

// Effective schema properties, used for introduction history and the resolved view.
// Values are canonical JSON.
struct FieldSignature {
  std::string logicalName;
  std::string label;
  std::map<std::string, std::string> properties;

  FieldSignature(std::string_view name, FieldType& type);
};

using FieldSignatures = std::map<std::string, FieldSignature, std::less<>>;

} // namespace luxir
