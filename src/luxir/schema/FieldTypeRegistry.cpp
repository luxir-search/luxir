// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/schema/FieldTypeRegistry.h"

#include <algorithm>
#include <stdexcept>

#include <glaze/glaze.hpp>

#include "luxir/util/StrRef.h"

namespace luxir {

namespace {

constexpr std::string_view BUILTIN_NAMES[] = {"date", "double", "float", "geo_point", "id",
                                              "int", "string", "text", "vector"};

bool validName(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

std::string_view kindName(DerivedField::Kind kind) {
  switch (kind) {
    case DerivedField::Kind::INT: return "int";
    case DerivedField::Kind::DATE: return "date";
    case DerivedField::Kind::STRING: return "string";
    case DerivedField::Kind::TEXT: return "text";
    case DerivedField::Kind::GEO_POINT: return "geo_point";
  }
  return "unknown";
}

std::string joined(std::span<const std::string> names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out.empty() ? std::string("none") : out;
}

// A plugin's declaration error: the type's author, not the schema's, fixes it,
// but it surfaces when a schema first names the type.
void checkFields(std::string_view name, const PluginFieldType& type) {
  auto fail = [&](const DerivedField* field, std::string_view what) {
    std::string where = field ? " derived field '" + std::string(1, CompositeFieldType::SEPARATOR) +
                                    field->suffix + "'" : std::string();
    throw std::invalid_argument("field type '" + std::string(name) + "'" + where + ": " + std::string(what));
  };
  if (type.fields.empty()) fail(nullptr, "declares no derived fields");
  for (size_t i = 0; i < type.fields.size(); i++) {
    const auto& field = type.fields[i];
    if (!validName(field.suffix)) fail(&field, "suffix must be [a-z0-9_]+");
    if (field.suffix.size() > DerivedField::MAX_SUFFIX_LEN) {
      fail(&field, "suffix is longer than " + std::to_string(DerivedField::MAX_SUFFIX_LEN) + " bytes");
    }
    for (size_t j = 0; j < i; j++) {
      if (type.fields[j].suffix == field.suffix) fail(&field, "declared twice");
    }
    if (field.kind == DerivedField::Kind::TEXT && field.column) fail(&field, "text has no column");
    if (field.kind != DerivedField::Kind::TEXT && !field.analyzer.empty()) {
      fail(&field, "analyzer is only valid for text");
    }
    bool points = field.kind == DerivedField::Kind::INT || field.kind == DerivedField::Kind::DATE ||
                  field.kind == DerivedField::Kind::GEO_POINT;
    if (points && field.index && !field.column) fail(&field, "the points index needs the column");
    if (!field.index && !field.column) fail(&field, "has neither an index nor a column");
  }
}

}  // namespace

std::span<const std::string_view> FieldTypeRegistry::builtinNames() { return BUILTIN_NAMES; }

FieldTypeRegistry& FieldTypeRegistry::global() {
  static FieldTypeRegistry instance;
  return instance;
}

void FieldTypeRegistry::add(std::string name, Maker maker) {
  if (isFrozen) throw std::logic_error("field type registry is frozen; field types must be added during startup");
  if (!validName(name)) throw std::invalid_argument("invalid field type name '" + name + "'; use [a-z0-9_]+");
  if (name == "plugin" || std::ranges::find(BUILTIN_NAMES, name) != std::end(BUILTIN_NAMES)) {
    throw std::invalid_argument("field type name '" + name + "' is a built-in type");
  }
  if (!maker) throw std::invalid_argument("field type '" + name + "' has no maker");
  if (makers.contains(name)) throw std::invalid_argument("duplicate field type '" + name + "'");
  makers.emplace(std::move(name), std::move(maker));
}

std::shared_ptr<const RegisteredFieldType> FieldTypeRegistry::make(const api::AnalyzerComponent& def) const {
  auto it = makers.find(def.name);
  if (it == makers.end()) {
    std::string builtins;
    for (auto builtin : BUILTIN_NAMES) {
      if (!builtins.empty()) builtins += ", ";
      builtins += builtin;
    }
    auto plugins = names();
    throw std::invalid_argument((def.name.empty() ? std::string("plugin field type has no name")
                                                  : "unknown field type '" + std::string(def.name) + "'") +
                                "; valid types: " + builtins + "; plugin types: " + joined(plugins));
  }
  ParamReader params("field type '" + std::string(def.name) + "'", def.params);
  auto type = it->second(params);
  if (!type) throw std::invalid_argument("field type '" + std::string(def.name) + "' made no type");
  params.rejectUnknown();
  checkFields(def.name, *type);
  // Sort the parameter objects so equal definitions have equal signatures.
  std::string json;
  if (!api::write_json(def, json)) throw std::runtime_error("Cannot serialize field type");
  glz::generic_sorted_u64 value;
  if (glz::read_json(value, json)) throw std::runtime_error("Cannot canonicalize field type");
  if (value.is_string()) {
    // A definition without params writes as its bare name.
    std::string name = value.get<std::string>();
    value = glz::generic_sorted_u64::object_t{};
    value["name"] = std::move(name);
  }
  if (auto effective = type->canonicalParams(); !effective.empty()) {
    glz::generic_sorted_u64 params;
    if (glz::read_json(params, effective) || !params.is_object()) {
      throw std::invalid_argument("field type '" + std::string(def.name) +
                                  "': canonicalParams() is not a JSON object: " + effective);
    }
    value["params"] = std::move(params);
  }
  if (auto version = type->version(); version != 0) value["version"] = (uint64_t)version;
  if (glz::write_json(value, json)) throw std::runtime_error("Cannot serialize canonical field type");
  return std::make_shared<const RegisteredFieldType>(std::string(def.name), std::move(json), std::move(type));
}

std::vector<std::string> FieldTypeRegistry::names() const {
  std::vector<std::string> out;
  out.reserve(makers.size());
  for (const auto& entry : makers) out.push_back(entry.first);  // std::map: already sorted
  return out;
}

bool DerivedValues::exact(size_t i) const {
  const auto& field = fields[i];
  if (field.kind != DerivedField::Kind::STRING || !field.index || values[i].isNull()) return true;
  if (const auto* one = std::get_if<std::string_view>(&values[i].kind)) return !PackedTerm::mayBeFitted(*one);
  if (const auto* all = std::get_if<api::ArrStr>(&values[i].kind)) {
    return std::ranges::none_of(all->v, [](std::string_view v) { return PackedTerm::mayBeFitted(v); });
  }
  return true;
}

DerivedSink::DerivedSink(const RegisteredFieldType& owner) : owner(owner), values(owner.type->fields.size()) {}

DerivedSink::Values& DerivedSink::add(size_t field, DerivedField::Kind kind, std::string_view call, bool one) {
  const auto& fields = owner.type->fields;
  if (field >= fields.size()) {
    throw std::logic_error("field type '" + owner.name + "': " + std::string(call) + "() on derived field " +
                           std::to_string(field) + " of " + std::to_string(fields.size()));
  }
  const auto& spec = fields[field];
  if (spec.kind != kind) {
    throw std::logic_error("field type '" + owner.name + "': " + std::string(call) + "() on derived field '" +
                           CompositeFieldType::SEPARATOR + spec.suffix + "' of kind " +
                           std::string(kindName(spec.kind)));
  }
  auto& slot = values[field];
  if (one && !spec.multi && slot.count() > 0) {
    throw std::invalid_argument("derived field '" + std::string(1, CompositeFieldType::SEPARATOR) + spec.suffix +
                                "' is single-valued but received multiple values");
  }
  return slot;
}

void DerivedSink::string(size_t field, std::string_view value) {
  auto& slot = add(field, DerivedField::Kind::STRING, "string");
  slot.bytes.append(value);
  slot.ends.push_back((uint32_t)slot.bytes.size());
}

void DerivedSink::text(size_t field, std::string_view value) {
  auto& slot = add(field, DerivedField::Kind::TEXT, "text");
  if (!slot.positions.empty()) {
    throw std::logic_error("field type '" + owner.name + "': text() and token() on one derived field");
  }
  slot.bytes.append(value);
  slot.ends.push_back((uint32_t)slot.bytes.size());
}

void DerivedSink::token(size_t field, std::string_view term, int32_t position) {
  // Tokens make up one value, so a single-valued field takes any number.
  auto& slot = add(field, DerivedField::Kind::TEXT, "token", false);
  if (slot.positions.size() != slot.ends.size()) {
    throw std::logic_error("field type '" + owner.name + "': text() and token() on one derived field");
  }
  if (position < 0 || (!slot.positions.empty() && position < slot.positions.back())) {
    throw std::logic_error("field type '" + owner.name + "': token positions must be >= 0 and non-decreasing");
  }
  slot.bytes.append(term);
  slot.ends.push_back((uint32_t)slot.bytes.size());
  slot.positions.push_back(position);
}

void DerivedSink::integer(size_t field, int64_t value) {
  const auto& fields = owner.type->fields;
  auto kind = field < fields.size() && fields[field].kind == DerivedField::Kind::DATE ? DerivedField::Kind::DATE
                                                                                     : DerivedField::Kind::INT;
  add(field, kind, "integer").ints.push_back(value);
}

void DerivedSink::point(size_t field, double latitude, double longitude) {
  auto& slot = add(field, DerivedField::Kind::GEO_POINT, "point");
  if (!geo::validLatitude(latitude)) throw std::invalid_argument("latitude must be finite and in [-90, 90]");
  if (!geo::validLongitude(longitude)) throw std::invalid_argument("longitude must be finite and in [-180, 180]");
  slot.points.push_back({latitude, longitude});
}

void DerivedSink::clear() {
  for (auto& slot : values) {
    slot.bytes.clear();
    slot.ends.clear();
    slot.positions.clear();
    slot.ints.clear();
    slot.points.clear();
  }
}

size_t DerivedSink::retainedBytes() const {
  size_t bytes = 0;
  for (const auto& slot : values) {
    bytes += slot.bytes.capacity() + slot.ends.capacity() * sizeof(uint32_t) +
             slot.positions.capacity() * sizeof(int32_t) + slot.ints.capacity() * sizeof(int64_t) +
             slot.points.capacity() * sizeof(GeoPoint);
  }
  return bytes;
}

}  // namespace luxir
