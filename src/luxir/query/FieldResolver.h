// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <vector>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_node_map.hpp>
#include "luxir/schema/Schema.h"
#include "luxir/util/StrRef.h"

namespace luxir {

// Parse-local handles bridge schema-aware syntax and query lowering without
// changing request spellings. Neither names nor mutable chains enter Schema.
// A request resolves the same few names clause after clause, so a hit must
// not allocate: the memos find by string_view, and the node map keeps the
// returned handles at stable addresses while later clauses add entries.
class FieldResolver {
  using Handles = boost::unordered_node_map<std::string, ResolvedFieldHandle,
                                            PackedTermHash, PackedTermEqual>;
  Schema& schema;
  std::vector<std::string>* explanations;
  std::array<Handles, (size_t)OpClass::PRIMARY + 1> fields;  // one memo per OpClass
  boost::unordered_flat_map<FieldType*, std::unique_ptr<TokenChain>> chains;
  // Query-named analyzers ("analyzer" option), memoized by spelling.
  boost::unordered_flat_map<std::string, std::unique_ptr<TokenChain>,
                            PackedTermHash, PackedTermEqual> namedChains;

public:
  explicit FieldResolver(Schema& schema, std::vector<std::string>* explanations = nullptr)
      : schema(schema), explanations(explanations) {}

  const ResolvedFieldHandle& resolve(std::string_view name, OpClass op,
                                     std::string_view context = {}) {
    auto& memo = fields[(size_t)op];
    auto found = memo.find(name);
    if (found == memo.end()) {
      auto target = schema.resolveFor(name, op);
      found = memo.emplace(std::string(name), std::move(target)).first;
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

  // The chain for a query-named analyzer (Schema::queryAnalyzer resolves the
  // name and throws for an unknown one).
  TokenChain& namedChain(std::string_view analyzer) {
    auto found = namedChains.find(analyzer);
    if (found == namedChains.end()) {
      auto chain = schema.queryAnalyzer(analyzer).createChain();
      found = namedChains.emplace(std::string(analyzer), std::move(chain)).first;
    }
    return *found->second;
  }
};

} // namespace luxir
