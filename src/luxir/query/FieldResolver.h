// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <format>
#include <map>
#include <vector>
#include "luxir/schema/Schema.h"

namespace luxir {

// Parse-local handles bridge schema-aware syntax and query lowering without
// changing request spellings. Neither names nor mutable chains enter Schema.
class FieldResolver {
  Schema& schema;
  std::vector<std::string>* explanations;
  std::map<std::pair<std::string, OpClass>, ResolvedFieldHandle> fields;
  std::map<FieldType*, std::unique_ptr<TokenChain>> chains;

public:
  explicit FieldResolver(Schema& schema, std::vector<std::string>* explanations = nullptr)
      : schema(schema), explanations(explanations) {}

  const ResolvedFieldHandle& resolve(std::string_view name, OpClass op,
                                     std::string_view context = {}) {
    auto key = std::make_pair(std::string(name), op);
    auto found = fields.find(key);
    if (found == fields.end()) {
      auto target = schema.resolveFor(name, op);
      found = fields.emplace(std::move(key), std::move(target)).first;
    }
    const auto& target = found->second;
    if (explanations && !context.empty() && name != target.physicalName) {
      auto note = std::format("{}: {} -> {}", context, name, target.physicalName);
      if (!std::ranges::contains(*explanations, note)) explanations->push_back(std::move(note));
    }
    return target;
  }

  TokenChain* chain(const ResolvedFieldHandle& target) {
    auto* type = target.fieldType;
    auto [entry, added] = chains.try_emplace(type);
    if (added) {
      if (type->type() == FieldType::TEXT) {
        entry->second = ((TextFieldType*)type)->createAnalyzer(target.physicalName);
      } else if (type->type() == FieldType::STRING) {
        auto& normalizer = ((StrFieldType*)type)->normalizer;
        if (normalizer) entry->second = normalizer->createChain();
      }
    }
    return entry->second.get();
  }
};

} // namespace luxir
