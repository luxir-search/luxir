#pragma once

#include <boost/unordered/unordered_flat_map.hpp>
#include "solux/util/StrRef.h"
#include "FieldType.h"

namespace solux::proto {
class SchemaDef;
}

namespace solux {

// Schema objects are currently immutable after construction.
class Schema {
public:
  using map_type = boost::unordered_flat_map<std::string, std::shared_ptr<FieldType>, PackedTermHash, PackedTermEqual>;
  using iterator = map_type::iterator;
  using const_iterator = map_type::const_iterator;

  map_type fieldTypeMap;
  uint64_t gen_ = 0;          // schema generation, set when persisted
  std::string sourceDef_;     // serialized bytes of the original SchemaDef proto (before inheritance resolution)

public:
  Schema() {};

  auto end() const {
    return fieldTypeMap.end();
  }

  // Returns a const_iterator to the FieldType for the fieldName or end() if not found.
  // Abstract fields are skipped during exact-name lookup but found during suffix matching.
  const_iterator getFieldType(std::string_view fieldName) const {
    auto it = fieldTypeMap.find(fieldName);
    if (it != fieldTypeMap.end()) {
      if (it->second->isAbstract()) return fieldTypeMap.end();
      return it;
    }
    // Not found — try a suffix match
    auto underscorePos = fieldName.find_last_of('_');
    if (underscorePos != std::string_view::npos && underscorePos > 0) {
      std::string_view suffix = fieldName.substr(underscorePos);
      if (!suffix.empty()) {
        it = fieldTypeMap.find(suffix);
        if (it != fieldTypeMap.end()) {
          return it;
        }
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

  // Build a Schema from a SchemaDef proto.
  // If base is provided (MERGE mode), start from the base schema's fields.
  static std::shared_ptr<Schema> fromProto(const proto::SchemaDef& def, const Schema* base = nullptr);

  // Serialize this schema to a SchemaDef proto (all fields, including dynamic suffix fields).
  void toProto(proto::SchemaDef* def) const;

  // Create the default schema with built-in fields.
  static std::shared_ptr<Schema> createDefaultSchema();

  // Legacy alias
  static std::shared_ptr<Schema> createSchema() {
    return createDefaultSchema();
  }
};

} // namespace solux
