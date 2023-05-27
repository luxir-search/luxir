#pragma once


#include "FieldType.h"
#include "gtl/phmap.hpp"

namespace solux {

// Schema objects are currently immutable.
class Schema {
public:
  using map_type = gtl::flat_hash_map<std::string, std::shared_ptr<FieldType>>;
  using iterator = map_type::iterator;
  using const_iterator = map_type::const_iterator;

  map_type fieldTypeMap;

public:
  Schema() {};

  auto end() const {
    return fieldTypeMap.end();
  }

  // Returns a const_iterator to the FieldType for the fieldName or end() if not found.
  const_iterator getFieldType(std::string_view fieldName) const {
    auto it = fieldTypeMap.find(fieldName);
    if (it == fieldTypeMap.end()) {
      // try a suffix match
      std::string_view suffix = fieldName.substr(fieldName.find_last_of('_'));
      if (!suffix.empty()) {
        it = fieldTypeMap.find(suffix);
      }
    }
    return it;
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


  static std::shared_ptr<Schema> createSchema() {
    std::shared_ptr<Schema> schema = std::make_shared<Schema>();
    // id, _i, _s, _w, _wl
    schema->fieldTypeMap["id"] = std::make_shared<StrFieldType>("id");
    schema->fieldTypeMap["_s"] = std::make_shared<StrFieldType>("_s");
    schema->fieldTypeMap["_i"] = std::make_shared<IntFieldType>("_i");
    schema->fieldTypeMap["_w"] = std::make_shared<TextFieldType>("_w");
    schema->fieldTypeMap["_wl"] = std::make_shared<TextFieldType>("_wl");  // hacky code in TextFieldType will look at name to produce different token chains
    return schema;
  }
};

} // namespace solux
