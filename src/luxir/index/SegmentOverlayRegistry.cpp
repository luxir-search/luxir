// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/index/SegmentOverlayRegistry.h"

#include <algorithm>
#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/reader/PostingsReader.h"

namespace luxir {

namespace {

bool validName(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool matchesField(std::string_view pattern, std::string_view field) {
  auto star = pattern.find('*');
  if (star == std::string_view::npos) return pattern == field;
  std::string_view head = pattern.substr(0, star);
  std::string_view tail = pattern.substr(star + 1);
  return field.size() >= head.size() + tail.size() && field.starts_with(head) && field.ends_with(tail);
}

}  // namespace

SegmentOverlaySpec::SegmentOverlaySpec(const api::SegmentOverlayDef& source) {
  std::vector<std::byte> bytes;
  if (!api::encode(source, bytes)) throw std::runtime_error("Failed to serialize SegmentOverlayDef");
  if (!api::decode(def, api::copyToPaddedInput(bytes, arena), arena)) {
    throw std::runtime_error("Failed to parse SegmentOverlayDef");
  }
}

std::string SegmentOverlaySpec::overlayName() const {
  std::string name(def.kind);
  name.push_back('.');
  name.append(def.name);
  return name;
}

std::vector<std::string> SegmentOverlaySpec::resolveFields(PostingsReader& segment) const {
  std::vector<std::string> covered;
  for (int32_t ordinal = 0; ordinal < segment.numFields(); ordinal++) {
    const auto* info = segment.fieldInfo(ordinal);
    if (info == nullptr) continue;
    auto field = (std::string_view)info->fieldname;
    if (std::ranges::any_of(def.fields, [&](std::string_view pattern) { return matchesField(pattern, field); })) {
      covered.emplace_back(field);
    }
  }
  std::ranges::sort(covered);
  return covered;
}

void SegmentOverlayRegistry::add(std::string kind, SegmentOverlayKindInfo info) {
  if (isFrozen) throw std::logic_error("segment overlay registry is frozen; register during startup");
  if (!validName(kind)) throw std::invalid_argument("invalid segment overlay kind '" + kind + "'; use [a-z0-9_]+");
  if (!info.maker || !info.open) throw std::invalid_argument("segment overlay kind '" + kind + "' needs a maker and an opener");
  if (kinds.contains(kind)) throw std::invalid_argument("duplicate segment overlay kind '" + kind + "'");
  kinds.emplace(std::move(kind), std::move(info));
}

const SegmentOverlayKindInfo* SegmentOverlayRegistry::find(std::string_view kind) const {
  auto found = kinds.find(kind);
  return found == kinds.end() ? nullptr : &found->second;
}

void SegmentOverlayRegistry::validate(const api::SegmentOverlayDef& def) const {
  if (!validName(def.name)) throw std::invalid_argument("invalid overlay name '" + std::string(def.name) + "'; use [a-z0-9_]+");
  if (def.fields.empty()) throw std::invalid_argument("overlay '" + std::string(def.name) + "' names no fields");
  if (def.min_docs < 0) throw std::invalid_argument("overlay '" + std::string(def.name) + "': min_docs must not be negative");
  for (std::string_view pattern : def.fields) {
    if (pattern.empty() || std::ranges::count(pattern, '*') > 1) {
      throw std::invalid_argument("overlay '" + std::string(def.name) + "': bad field pattern '" + std::string(pattern) + "'");
    }
  }
  SegmentOverlaySpec spec(def);
  make(spec);
}

std::unique_ptr<SegmentOverlayKind> SegmentOverlayRegistry::make(const SegmentOverlaySpec& spec) const {
  const auto* info = find(spec.def.kind);
  if (!info) {
    std::string valid;
    for (const auto& entry : kinds) valid += (valid.empty() ? "" : ", ") + entry.first;
    throw std::invalid_argument("unknown segment overlay kind '" + std::string(spec.def.kind) + "'; valid kinds: " + valid);
  }
  ParamReader reader("segment overlay '" + std::string(spec.def.name) + "'", spec.def.params);
  auto kind = info->maker(SegmentOverlayContext{reader});
  reader.rejectUnknown();
  if (!kind) throw std::logic_error("segment overlay maker '" + std::string(spec.def.kind) + "' returned null");
  return kind;
}

std::vector<std::string> SegmentOverlayRegistry::names() const {
  std::vector<std::string> result;
  for (const auto& entry : kinds) result.push_back(entry.first);
  return result;
}

SegmentOverlayRegistry& SegmentOverlayRegistry::global() {
  static SegmentOverlayRegistry instance;
  return instance;
}

}  // namespace luxir
