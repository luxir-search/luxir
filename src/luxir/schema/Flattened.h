// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The FLATTENED field layout. A document value is a JSON object; its leaves
// are indexed into the field's one STRING term space:
//
//   "<key path>\0<leaf>"   one per leaf, the key path joining keys with '.'
//   "\0<leaf>"             the same leaf without its key (bare-value terms)
//
// Leaves become their canonical text (numbers and booleans render as in a
// string field), and an array contributes each element at its own path. So a
// key path is a term prefix: exact, set, prefix and range queries on
// "field.key.path" are term operations below "<key path>\0", and the ord
// column holds every term of a document, which is enough to rebuild the object
// for retrieval when the field is not stored. NUL cannot occur in JSON text
// without escaping, so stored source and terms are told apart by it.

#include "luxir/api/build.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/FieldType.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/ApiError.h"

#include <fmt/format.h>

#include <map>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luxir::flattened {

namespace detail {

struct Expander {
  const StrFieldType& type;
  std::string_view fieldName;
  std::vector<std::string>& out;
  std::string path;

  void object(const api::Map& map, int32_t depth) {
    if (depth > type.depthLimit) {
      throw DocumentError(fmt::format("Field '{}': object nesting exceeds depth_limit {}",
                                      fieldName, type.depthLimit));
    }
    for (const auto& [key, value] : map.fields) {
      if (key.empty() || key.find('\0') != std::string_view::npos) {
        throw DocumentError(fmt::format("Field '{}': object keys must be nonempty and contain no NUL",
                                        fieldName));
      }
      size_t mark = path.size();
      if (mark != 0) path.push_back('.');
      path.append(key);
      this->value(*value, depth);
      path.resize(mark);
    }
  }

  void value(const api::Val& val, int32_t depth) {
    if (coerce::isNull(val)) return;
    if (auto* map = std::get_if<api::Map>(&val.kind)) {
      object(*map, depth + 1);
      return;
    }
    if (coerce::forEachElement(val, [&](const api::Val& element) { value(element, depth); })) return;
    char buf[coerce::TEXT_BUF_SIZE];
    std::string_view leaf = coerce::toText(val, fieldName, buf);
    if (type.ignoreAbove > 0 && leaf.size() > (size_t)type.ignoreAbove) return;
    std::string& term = out.emplace_back(path);
    term.push_back('\0');
    term.append(leaf);
    std::string& bare = out.emplace_back(1, '\0');
    bare.append(leaf);
  }
};

// One rebuilt key: leaf values sorted as their terms are, and child keys.
struct Node {
  std::vector<std::string_view> values;
  std::map<std::string_view, Node> children;
};

inline api::Val leafVal(const std::vector<std::string_view>& values, std::pmr::memory_resource& mr) {
  api::Val val;
  if (values.size() == 1) {
    val.kind = values[0];
  } else {
    auto& arr = val.kind.emplace<api::ArrStr>();
    std::string_view* out = api::build::allocArray(arr.v, values.size(), mr);
    std::copy(values.begin(), values.end(), out);
  }
  return val;
}

inline api::Val* arenaVal(api::Val val, std::pmr::memory_resource& mr) {
  auto* out = api::build::allocMessage<api::Val>(mr);
  *out = std::move(val);
  return out;
}

using Entries = std::vector<std::pair<std::string_view, hpp_proto::indirect_view<api::Val>>>;

inline api::Val objectVal(const Node& node, std::pmr::memory_resource& mr);

// Emit node under key. A key that is both a leaf and an object (a dotted
// source key beside a nested one) keeps its leaf and spells its children's
// keys out in full, so no term is lost.
inline void emit(Entries& entries, std::string_view key, const Node& node, std::pmr::memory_resource& mr) {
  if (node.children.empty()) {
    entries.push_back({key, arenaVal(leafVal(node.values, mr), mr)});
  } else if (node.values.empty()) {
    entries.push_back({key, arenaVal(objectVal(node, mr), mr)});
  } else {
    entries.push_back({key, arenaVal(leafVal(node.values, mr), mr)});
    for (const auto& [childKey, child] : node.children) {
      std::string joined = std::string(key) + "." + std::string(childKey);
      emit(entries, api::build::arenaStr(mr, joined), child, mr);
    }
  }
}

inline api::Val objectVal(const Node& node, std::pmr::memory_resource& mr) {
  Entries entries;
  for (const auto& [key, child] : node.children) emit(entries, key, child, mr);
  api::Val val;
  auto& map = val.kind.emplace<api::Map>();
  using Pair = std::pair<std::string_view, hpp_proto::indirect_view<api::Val>>;
  auto* pairs = (Pair*)mr.allocate(sizeof(Pair) * entries.size(), alignof(Pair));
  std::uninitialized_copy(entries.begin(), entries.end(), pairs);
  map.fields = api::map_view<std::string_view, hpp_proto::indirect_view<api::Val>>(
      std::span<const Pair>(pairs, entries.size()));
  return val;
}

} // namespace detail

// Append one document value's terms to out. The value is an object, an array
// of objects, or null. Throws DocumentError for any other value, for an empty
// key or one holding NUL, and for nesting deeper than the field's depth_limit.
inline void appendTerms(const api::Val& val, const StrFieldType& type, std::string_view fieldName,
                        std::vector<std::string>& out) {
  detail::Expander expander{type, fieldName, out, {}};
  auto top = [&](const api::Val& element) {
    if (coerce::isNull(element)) return;
    auto* map = std::get_if<api::Map>(&element.kind);
    if (map == nullptr) {
      throw DocumentError(fmt::format("Field '{}' is flattened and expects a JSON object, not {}",
                                      fieldName, coerce::describe(element)));
    }
    expander.object(*map, 1);
  };
  if (!coerce::forEachElement(val, top)) top(val);
}

// True when a retrieved value is a term rather than stored JSON source.
inline bool isTerm(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

// Rebuild the object a document's terms describe, allocated in mr. Terms must
// outlive the result. Leaves come back as strings, several values under one
// key path as an array in term order.
inline api::Val rebuildObject(std::span<const std::string_view> terms, std::pmr::memory_resource& mr) {
  detail::Node root;
  for (std::string_view term : terms) {
    size_t sep = term.find('\0');
    if (sep == 0 || sep == std::string_view::npos) continue;  // a bare value
    detail::Node* node = &root;
    std::string_view path = term.substr(0, sep);
    for (;;) {
      size_t dot = path.find('.');
      node = &node->children[path.substr(0, dot)];
      if (dot == std::string_view::npos) break;
      path.remove_prefix(dot + 1);
    }
    node->values.push_back(term.substr(sep + 1));
  }
  return detail::objectVal(root, mr);
}

// The retrieved form of one document's values: its stored source parsed back
// to JSON values, or its object rebuilt from its terms.
inline api::Val retrieved(std::span<const std::string_view> values, std::pmr::memory_resource& mr) {
  if (values.empty() || isTerm(values[0])) return rebuildObject(values, mr);
  api::Val val;
  if (!api::read_json(val, api::build::arenaStr(mr, values[0]), mr)) {
    throw std::runtime_error("flattened field: stored source is not valid JSON");
  }
  return val;
}

} // namespace luxir::flattened
