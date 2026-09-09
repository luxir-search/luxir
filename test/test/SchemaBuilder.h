// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Arena-backed builder for api::SchemaDef test fixtures: name-keyed fields and
// templates matching the proto's map shape.  Returned FieldDef references are
// stable (deque-backed), so callers set properties directly:
//
//   SchemaBuilder b;
//   auto& f = b.field("price");
//   f.type = api::FieldDef::FieldClass::INT;
//   f.index = api::FieldDef::IndexMode::RANGE;
//   b.set(helper.collection());

#include <deque>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "luxir/server/LuxirNode.h"

namespace luxir {

class SchemaBuilder {
  using Pair = std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>;
  std::pmr::monotonic_buffer_resource arena_;
  std::deque<Pair> fields_;
  std::deque<Pair> templates_;
  std::vector<Pair> fieldsFlat_;
  std::vector<Pair> templatesFlat_;

  api::FieldDef& add(std::deque<Pair>& out, std::string_view name) {
    auto* def = api::build::allocMessage<api::FieldDef>(arena_);
    out.push_back({api::build::arenaStr(arena_, name), def});
    return *def;
  }

public:
  std::pmr::memory_resource& mr() { return arena_; }

  // Add a concrete field / a template (abstract suffix or parent) definition.
  api::FieldDef& field(std::string_view name) { return add(fields_, name); }
  api::FieldDef& templ(std::string_view name) { return add(templates_, name); }

  api::FieldDef& variant(api::FieldDef& field, std::string_view label) {
    auto* def = api::build::allocMessage<api::FieldDef>(arena_);
    if (!field.variants) field.variants.emplace();
    auto previous = field.variants->entries;
    auto* entries = api::build::allocArray(field.variants->entries, previous.size() + 1, arena_);
    if (!previous.empty()) std::copy(previous.begin(), previous.end(), entries);
    entries[previous.size()] = {api::build::arenaStr(arena_, label), def};
    return *def;
  }

  api::FieldDef& normalizer(api::FieldDef& field, std::initializer_list<std::string_view> filters) {
    auto& def = field.normalizer.emplace();
    auto* out = api::build::allocArray(def.filters, filters.size(), arena_);
    size_t i = 0;
    for (auto filter : filters) out[i++].name = api::build::arenaStr(arena_, filter);
    return field;
  }

  // Convenience: set an analyzer of parameterless components on a (text)
  // field def.  nullopt leaves the tokenizer component absent.
  api::FieldDef& analyzer(api::FieldDef& f, std::optional<std::string_view> tokenizer,
                          std::initializer_list<std::string_view> filters = {}) {
    auto& a = f.analyzer.emplace();
    if (tokenizer) a.tokenizer.emplace().name = api::build::arenaStr(arena_, *tokenizer);
    auto* fl = api::build::allocArray(a.filters, filters.size(), arena_);
    std::size_t i = 0;
    for (auto filter : filters) fl[i++].name = api::build::arenaStr(arena_, filter);
    return f;
  }

  // The built def views this builder's storage; keep the builder alive while using it.
  api::SchemaDef def() {
    fieldsFlat_.assign(fields_.begin(), fields_.end());
    templatesFlat_.assign(templates_.begin(), templates_.end());
    api::SchemaDef d;
    d.fields = api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(
      std::span<const Pair>(fieldsFlat_.data(), fieldsFlat_.size()));
    d.templates = api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(
      std::span<const Pair>(templatesFlat_.data(), templatesFlat_.size()));
    return d;
  }

  std::shared_ptr<Schema> build(const Schema* base = nullptr) {
    return Schema::fromProto(def(), base);
  }

  // Apply through the collection's schema transaction.
  std::shared_ptr<Schema> set(Collection& c) {
    return c.updateSchema(def(), api::SchemaRequest_::Mode::SET);
  }
  std::shared_ptr<Schema> replaceAll(Collection& c) {
    return c.updateSchema(def(), api::SchemaRequest_::Mode::REPLACE_ALL);
  }
};

} // namespace luxir
