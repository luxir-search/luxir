// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/unordered/unordered_flat_map.hpp>
#include <memory_resource>
#include <map>
#include <stdexcept>
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ApiError.h"
#include "luxir/util/StrRef.h"
#include "FieldType.h"
#include "FieldSignature.h"

namespace luxir {

// A user error in a schema definition (unknown parent, bad analyzer name,
// reserved-field violation, ...): INVALID_REQUEST / invalid_schema.
class SchemaError : public ApiError {
public:
  explicit SchemaError(const std::string& message)
    : ApiError(ErrorKind::INVALID_REQUEST, "invalid_schema", message) {}
};

using PhysicalFieldMap = boost::unordered_flat_map<std::string, std::shared_ptr<FieldType>,
                                                    PackedTermHash, PackedTermEqual>;

enum class OpClass { SEARCH, VALUE, EXISTS, RETRIEVE, PRIMARY };
enum class FieldRole { PRIMARY, VARIANT };

// One input shape, with source storage on the primary and independent physical bundles.
// Dynamic roots use the template's prototypes; their actual names live in handles.
struct LogicalField {
  enum class Shape { SCALAR, VECTOR, GEO };
  std::string name;
  std::shared_ptr<FieldType> primary;
  std::map<std::string, std::shared_ptr<FieldType>, std::less<>> variants;
  std::string search = "self";
  std::string value = "self";
  Shape shape = Shape::SCALAR;
  bool multi = false;
  size_t maxRootLength = 127;
};

// Names are owned by the handle; pointers remain valid for the Schema lifetime.
// A dynamic root uses its template's FieldTypes and LogicalField. For hand-built
// schemas, owner == nullptr means a single representation with both bindings self.
// Consumers carry the handle through lowering instead of resolving its name again.
struct ResolvedFieldHandle {
  std::string logicalName;
  std::string physicalName;
  FieldType* fieldType;
  FieldRole role;
  const LogicalField* owner;
};

// Schema lookup is immutable and never retains names supplied by a request.
class Schema {
public:
  using map_type = PhysicalFieldMap;

  map_type fieldTypeMap;
  uint64_t gen_ = 0;          // schema generation, set when persisted
  std::string sourceDef_;     // serialized authored SchemaDef, before inheritance
  // Concrete and template physical identities, plus materialized dynamic names
  // whose introduction differs from the current template's introduction.
  std::map<std::string, uint64_t, std::less<>> introducedGen;

private:
  using logical_map = boost::unordered_flat_map<std::string, std::unique_ptr<LogicalField>,
                                               PackedTermHash, PackedTermEqual>;
  logical_map logicalFields;
  struct RootField {
    const std::shared_ptr<FieldType>* primary = nullptr;
    const LogicalField* owner = nullptr;
  };

  RootField findRoot(std::string_view root) const;
  const std::shared_ptr<FieldType>* findPhysical(std::string_view name) const;

public:
  Schema() = default;

  // Ingest accepts logical document keys only. The returned primary handle
  // exposes the owner and its variants for the later ingest dispatcher.
  ResolvedFieldHandle resolveInput(std::string_view docKey) const;
  ResolvedFieldHandle resolveFor(std::string_view name, OpClass op) const;
  // Already-resolved physical access: no defaults or __self aliases.
  FieldType* physical(std::string_view name) const;

  FieldSignatures signatures(bool includeTemplates = false) const;
  void addRootSignatures(std::string_view root, FieldSignatures& out) const;
  void inheritIntroductions(const Schema* previous, const FieldSignatures& materialized);
  uint64_t introduction(std::string_view physicalName) const;
  std::string encodeStored() const;
  static std::shared_ptr<Schema> decodeStored(std::span<const std::byte> bytes);

  // Physical lookup includes suffix-template prototypes, never bindings or
  // __self aliases. Copy a shared_ptr before the Schema's lifetime ends.
  const std::shared_ptr<FieldType>& getFieldTypeEx(std::string_view name) const {
    auto* type = findPhysical(name);
    if (!type) throw RequestError("Field not found: " + std::string(name), "unknown_field");
    return *type;
  }

  std::shared_ptr<FieldType> getFieldTypeOrNull(std::string_view name) const {
    auto* type = findPhysical(name);
    return type ? *type : std::shared_ptr<FieldType>{};
  }

  FieldType* getFieldTypePtr(std::string_view name) const {
    auto* type = findPhysical(name);
    return type ? type->get() : nullptr;
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
  static bool validVariantLabel(std::string_view label);

  // Create the default schema with built-in fields.
  static std::shared_ptr<Schema> createDefaultSchema();

  // Legacy alias
  static std::shared_ptr<Schema> createSchema() {
    return createDefaultSchema();
  }
};

} // namespace luxir
