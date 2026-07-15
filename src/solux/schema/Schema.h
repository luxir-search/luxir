#pragma once

#include <boost/unordered/unordered_flat_map.hpp>
#include <memory_resource>
#include <stdexcept>
#include "solux/api/solux_types.hpp"
#include "solux/util/StrRef.h"
#include "FieldType.h"

namespace solux {

// A user error in a schema definition (unknown parent, bad analyzer name,
// reserved-field violation, ...).  Transports map this to a client error
// (HTTP 400 / gRPC INVALID_ARGUMENT); other exceptions are server-side.
class SchemaError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
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
      throw std::runtime_error("Field not found: " + std::string(fieldName));
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
  static std::shared_ptr<Schema> fromProto(const solux::api::SchemaDef& def, const Schema* base = nullptr);

  // Emit the AUTHORED source def (parents and sparse presence preserved,
  // deterministic order: fields = id, _version_, then alpha; templates alpha).
  // This is what GET /_schema and gRPC SchemaResponse return; it is a valid
  // fromProto input that reproduces this schema exactly.
  void toProto(solux::api::SchemaDef* def, std::pmr::memory_resource& arena) const;

  // Create the default schema with built-in fields.
  static std::shared_ptr<Schema> createDefaultSchema();

  // Legacy alias
  static std::shared_ptr<Schema> createSchema() {
    return createDefaultSchema();
  }
};

} // namespace solux
