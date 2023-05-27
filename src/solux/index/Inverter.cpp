
#include "Inverter.h"

namespace solux {

Inverter::IndexHandler& Inverter::createIndexHandler(const std::string_view name) {
  // perhaps this part should be moved to Schema?
  auto currSchema = schema.get();
  bool justAcquiredSchema = false;
  if (currSchema == nullptr) {
    schema = schemaProvider();
    currSchema = schema.get();
    justAcquiredSchema = true;
  }
  auto ftIter = currSchema->getFieldType(name);
  // Don't try to refresh the schema if we just acquired it since we don't know how expensive it is.
  if (ftIter == currSchema->end() && !justAcquiredSchema) {
    schema = schemaProvider();

    // if the schema changed, retry the lookup
    if (schema.get() != currSchema) {
      currSchema = schema.get();
      ftIter = currSchema->getFieldType(name);
    }
  }

  if (ftIter == currSchema->end()) {
    throw std::runtime_error("Field not found in schema: " + std::string(name));
  }

  // Create the correct IndexHandler based on the suffix.  This could be moved to FieldType::createIndexHandler()?
  std::unique_ptr<IndexHandler> fieldHandler;

  const std::shared_ptr<FieldType>& fieldType = ftIter->second;

  switch (fieldType->type()) {
    case FieldType::Type::TEXT:
      fieldHandler = std::make_unique<PosIndexHandler>(*this, name, fieldType);
      break;
    case FieldType::Type::STRING:
      fieldHandler = std::make_unique<StringIndexHandler>(*this, name, fieldType);
      break;
    case FieldType::Type::INT:
      fieldHandler = std::make_unique<IntColHandler>(*this, name, fieldType);
      break;
    default:
      throw std::runtime_error("Unknown field type: " + std::string(name));
  }


  auto [newIter, inserted] = indexHandlers.try_emplace(name, std::move(fieldHandler));
  return *(newIter->second);
}

} // end namespace
