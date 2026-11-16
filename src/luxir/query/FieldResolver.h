// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <map>
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
  friend class ParseView;
  using Handles = boost::unordered_node_map<std::string, ResolvedFieldHandle,
                                            PackedTermHash, PackedTermEqual>;
  Schema& schema;
  std::vector<std::string>* explanations;
  using Memos = std::array<Handles, (size_t)OpClass::PRIMARY + 1>;
  Memos fields;  // one memo per OpClass
  std::map<std::string, Memos, std::less<>> scopedFields;
  Memos* current = nullptr;
  std::string_view viewPath;
  boost::unordered_flat_map<FieldType*, std::unique_ptr<TokenChain>> chains;

  // Names the view in errors raised below the root.
  std::string where() const {
    return viewPath.empty() ? std::string() : " in view '" + std::string(viewPath) + "'";
  }

  void setPath(std::string_view path) {
    if (path.empty()) {
      current = nullptr;
      viewPath = {};
    } else {
      auto entry = scopedFields.find(path);
      if (entry == scopedFields.end()) entry = scopedFields.try_emplace(std::string(path)).first;
      current = &entry->second;
      viewPath = entry->first;
    }
  }

public:
  explicit FieldResolver(Schema& schema, std::vector<std::string>* explanations = nullptr,
                         std::string_view path = {})
      : schema(schema), explanations(explanations) { setPath(path); }

  std::string_view path() const { return viewPath; }

  const ResolvedFieldHandle& resolvePath(std::string_view name) {
    const auto& target = resolve(name, OpClass::PRIMARY);
    if (target.fieldType->type() != FieldType::NESTED) {
      throw RequestError("Field '" + std::string(name) + "' is not a nested path" + where());
    }
    return target;
  }

  const ResolvedFieldHandle& resolve(std::string_view name, OpClass op,
                                     std::string_view context = {}) {
    auto& memo = (current ? *current : fields)[(size_t)op];
    auto found = memo.find(name);
    if (found == memo.end()) {
      ResolvedFieldHandle target;
      // Birth versions are virtual metadata of every view.
      if (!current || (name == "_version_" && op == OpClass::RETRIEVE)) {
        target = schema.resolveFor(name, op);
      } else {
        try {
          target = schema.resolveFor(Schema::qualify(viewPath, name), op);
        } catch (const RequestError& e) {
          throw RequestError("Field '" + std::string(name) + "'" + where() + ": " + e.what(), e.code);
        }
      }
      found = memo.emplace(std::string(name), std::move(target)).first;
    }
    const auto& target = found->second;
    if (explanations && !context.empty() && name != target.physicalName) {
      auto note = std::format("{}: {} -> {}", context, name, target.physicalName);
      if (!std::ranges::contains(*explanations, note)) explanations->push_back(std::move(note));
    }
    return target;
  }

  const ResolvedFieldHandle& resolveLocal(std::string_view name, OpClass op,
                                          std::string_view context = {}) {
    const auto& target = resolve(name, op, context);
    if (target.path != viewPath && target.physicalName != "_version_") {
      throw RequestError("Field '" + std::string(name) + "'" + where()
          + " belongs to nested path '" + target.path
          + "'; the operation must run in that view through Domain 'to'");
    }
    return target;
  }

  void requireLocalPattern(std::string_view pattern) {
    auto dot = pattern.rfind('.');
    if (dot == std::string_view::npos) return;
    auto prefix = pattern.substr(0, dot);
    if (prefix.find_first_of("*?") != std::string_view::npos) {
      throw RequestError("Wildcard nested paths are not supported: " + std::string(pattern));
    }
    const auto& path = resolvePath(prefix);
    throw RequestError("Field pattern '" + std::string(pattern) + "'" + where()
        + " names nested path '" + path.physicalName
        + "'; the operation must run in that view through Domain 'to'");
  }

  // Adds a line to ?explain=resolved (a no-op outside explain), e.g. what a
  // plugin query resolved its parameters to. Repeats are dropped.
  void note(std::string text) {
    if (explanations && !std::ranges::contains(*explanations, text)) explanations->push_back(std::move(text));
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
