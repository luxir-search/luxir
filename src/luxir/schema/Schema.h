// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/unordered/unordered_flat_map.hpp>
#include <memory_resource>
#include <stdexcept>
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ApiError.h"
#include "luxir/util/StrRef.h"
#include "FieldType.h"

namespace luxir {

// A user error in a schema definition (unknown parent, bad analyzer name,
// reserved-field violation, ...): INVALID_REQUEST / invalid_schema.
class SchemaError : public ApiError {
public:
  explicit SchemaError(const std::string& message)
    : ApiError(ErrorKind::INVALID_REQUEST, "invalid_schema", message) {}
};

// Schema objects are currently immutable after construction.
class Schema {
public:
  using map_type = boost::unordered_flat_map<std::string, std::shared_ptr<FieldType>, PackedTermHash, PackedTermEqual>;
  using iterator = map_type::iterator;
  using const_iterator = map_type::const_iterator;

  map_type fieldTypeMap;
  uint64_t gen_ = 0;          // schema generation, set when persisted
  std::string sourceDef_;     // serialized bytes of the authored SchemaDef proto (before inheritance resolution)

public:
  Schema() {};

  auto end() const {
    return fieldTypeMap.end();
  }

  // Returns a const_iterator to the FieldType for the fieldName or end() if not found.
  // Exact-name lookup matches concrete fields only; suffix matching ("title_w" ->
  // "_w") matches templates (abstract entries) only.
  const_iterator getFieldType(std::string_view fieldName) const {
    auto it = fieldTypeMap.find(fieldName);
    if (it != fieldTypeMap.end()) {
      if (it->second->isAbstract()) return fieldTypeMap.end();
      return it;
    }
    // Not found - try a suffix match against templates
    auto underscorePos = fieldName.find_last_of('_');
    if (underscorePos != std::string_view::npos && underscorePos > 0) {
      std::string_view suffix = fieldName.substr(underscorePos);
      it = fieldTypeMap.find(suffix);
      if (it != fieldTypeMap.end() && it->second->isAbstract()) {
        return it;
      }
    }
    return fieldTypeMap.end();
  }

  // Returns the FieldType for the fieldName or throws an exception if not found.
  // The shared_ptr reference should be copied before the lifetime of the Schema object ends.
  const std::shared_ptr<FieldType>& getFieldTypeEx(std::string_view fieldName) const {
    auto it = getFieldType(fieldName);
    if (it == fieldTypeMap.end()) {
      throw RequestError("Field not found: " + std::string(fieldName), "unknown_field");
    }
    return it->second;
  }

  // Returns the FieldType for the fieldName or null (empty shared_ptr) if not found.
  std::shared_ptr<FieldType> getFieldTypeOrNull(std::string_view fieldName) const {
    auto it = getFieldType(fieldName);
    if (it == fieldTypeMap.end()) {
      return {};
    }
    return it->second;
  }

  // Returns the FieldType for a given field name, or nullptr if not defined.
  // The returned pointer is only valid for the lifetime of this Schema object.
  FieldType* getFieldTypePtr(std::string_view fieldName) const {
    auto it = getFieldType(fieldName);
    if (it == fieldTypeMap.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  // Build a Schema from a SchemaDef proto (fields + templates maps).
  // If base is provided (SET mode), the def is unioned into the base's
  // AUTHORED source def by name (a name in the def replaces the base entry
  // wherever it lived) and the whole merged graph is re-resolved, so replacing
  // a template also rebuilds fields that inherit from it.  The reserved "id"
  // and "_version_" fields are materialized when absent and validated when
  // present.  Throws SchemaError on definition errors.
  static std::shared_ptr<Schema> fromProto(const luxir::api::SchemaDef& def, const Schema* base = nullptr);

  // Emit the AUTHORED source def (parents and sparse presence preserved,
  // deterministic order: fields = id, _version_, then alpha; templates alpha).
  // This is what GET /_schema and gRPC SchemaResponse return; it is a valid
  // fromProto input that reproduces this schema exactly.
  void toProto(luxir::api::SchemaDef* def, std::pmr::memory_resource& arena) const;

  // Name rules.  A concrete field name is id-like: an ASCII letter followed by
  // ASCII letters, digits, or underscores, at most 127 bytes (field names
  // land in filenames).  The leading-underscore namespace is reserved for the
  // engine; "_version_" is the one reserved name accepted here.  A template
  // name is '_' followed by ASCII letters, digits, or underscores: a suffix
  // pattern ("_s") or an abstract parent type ("_body_"; anything with a
  // second underscore can never suffix-match, since matching anchors on the
  // LAST underscore of a field name).
  static bool validFieldName(std::string_view name);
  static bool validTemplateName(std::string_view name);

  // Create the default schema with built-in fields.
  static std::shared_ptr<Schema> createDefaultSchema();

  // Legacy alias
  static std::shared_ptr<Schema> createSchema() {
    return createDefaultSchema();
  }
};

} // namespace luxir
