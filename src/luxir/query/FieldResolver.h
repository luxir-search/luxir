// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <optional>
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
  // The oldest schema generation of the searched documents, once the caller
  // knows the reader (see coversAll). Unset: no representation is assumed
  // complete, so query building never substitutes one for another.
  std::optional<uint64_t> coveredGen;

public:
  explicit FieldResolver(Schema& schema, std::vector<std::string>* explanations = nullptr)
      : schema(schema), explanations(explanations) {}

  // Pass IndexReader::oldestSchemaGen(); absent (no live documents) covers everything.
  void setCoverage(std::optional<uint64_t> oldestSchemaGen) {
    coveredGen = oldestSchemaGen.value_or(std::numeric_limits<uint64_t>::max());
  }

  // Every searched document was indexed with this physical representation.
  // A variant added later is missing from older documents (no backfill).
  bool coversAll(std::string_view physicalName) const {
    return coveredGen && schema.introduction(physicalName) <= *coveredGen;
  }

  // Record a request-level rewrite for ?explain=resolved.
  void explain(std::string note) {
    if (explanations && !std::ranges::contains(*explanations, note)) explanations->push_back(std::move(note));
  }

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
};

} // namespace luxir
