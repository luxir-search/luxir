#pragma once

#include <boost/unordered/unordered_flat_map.hpp>
#include "solux/util/StrRef.h"
#include "FieldType.h"


namespace solux {

// Schema objects are currently immutable.
class Schema {
public:
  using map_type = boost::unordered_flat_map<std::string, std::shared_ptr<FieldType>, PackedTermHash, PackedTermEqual>;
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
      auto underscorePos = fieldName.find_last_of('_');
      if (underscorePos != std::string_view::npos) {
        std::string_view suffix = fieldName.substr(underscorePos);
        if (!suffix.empty()) {
          it = fieldTypeMap.find(suffix);
        }
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
    // id, _i, _s, _w, _wl, _version_
    schema->fieldTypeMap["id"] = std::make_shared<StrFieldType>("id");
    schema->fieldTypeMap["_s"] = std::make_shared<StrFieldType>("_s");
    schema->fieldTypeMap["_sc"] = std::make_shared<StrFieldType>("_sc", FieldType::COLUMN_STORED);
    schema->fieldTypeMap["_ss"] = std::make_shared<StrFieldType>("_ss", FieldType::INDEX_DOCS | FieldType::COLUMN_STORED | FieldType::MULTI_VALUED);
    schema->fieldTypeMap["_ssc"] = std::make_shared<StrFieldType>("_ssc", FieldType::COLUMN_STORED | FieldType::MULTI_VALUED);
    schema->fieldTypeMap["_i"] = std::make_shared<IntFieldType>("_i");
    schema->fieldTypeMap["_is"] = std::make_shared<IntFieldType>("_is", FieldType::COLUMN_STORED | FieldType::MULTI_VALUED);
    schema->fieldTypeMap["_w"] = std::make_shared<TextFieldType>("_w");
    schema->fieldTypeMap["_wl"] = std::make_shared<TextFieldType>("_wl");  // hacky code in TextFieldType will look at name to produce different token chains
    // schema->fieldTypeMap["_version_"] = std::make_shared<IntFieldType>("_version_", FieldType::COLUMN_STORED);
    schema->fieldTypeMap["_version_"] = std::make_shared<IntFieldType>("_version_", FieldType::COLUMN_STORED);
    return schema;
  }
};

} // namespace solux
