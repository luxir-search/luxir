// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Inverter.h"

#include <algorithm>
#include <boost/sort/spreadsort/string_sort.hpp>
#include "luxir/schema/Schema.h"

// include the actual index handlers
#include "handler/IdHandler.h"
#include "handler/StrColHandler.h"
#include "handler/StoredFieldWrapperHandler.h"
#include "handler/VectorHandler.h"
#include "luxir/index/handler/IntColHandler.h"
#include "luxir/index/handler/StrHandler.h"
#include "luxir/index/handler/FullTextHandler.h"

namespace luxir {


void Inverter::deleteId(std::string_view id, uint64_t version) {
  if (idHandler_ == nullptr) {
    // Bootstrap: create the id handler via schema lookup if no docs have been indexed yet.
    // "id" is the schema-defined name for the unique id field.
    getIndexHandler("id");
    assert(idHandler_ != nullptr);
  }
  static_cast<handler::IdHandler&>(*idHandler_).addDelete(id, version);
}


Inverter::UndoMark Inverter::undoMark() const {
  UndoMark mark;
  if (idHandler_ != nullptr) {
    mark.idUndoSize = static_cast<handler::IdHandler&>(*idHandler_).undoSize();
  }
  mark.numDeleted = deleted.size();
  return mark;
}

void Inverter::rollbackTo(const UndoMark& mark) {
  if (idHandler_ != nullptr) {
    static_cast<handler::IdHandler&>(*idHandler_).rollbackTo(mark.idUndoSize);
  }
  assert(deleted.size() >= mark.numDeleted);
  deleted.resize(mark.numDeleted);
}

void Inverter::clearUndoLog() {
  if (idHandler_ != nullptr) {
    static_cast<handler::IdHandler&>(*idHandler_).clearUndoLog();
  }
}


Inverter::IndexHandler& Inverter::createIndexHandler(const std::string_view name) {
  // First sight of a doc-supplied field name: without this check a template
  // suffix match would admit any string into segment metadata.
  if (!Schema::validFieldName(name)) {
    throw RequestError("Invalid field name: " + std::string(name), "unknown_field");
  }
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
    throw RequestError("Field not found in schema: " + std::string(name), "unknown_field");
  }

  // Create the correct IndexHandler based on the suffix.  This could be moved to FieldType::createIndexHandler()?
  u_ptr<IndexHandler> fieldHandler;

  const std::shared_ptr<FieldType>& fieldType = ftIter->second;

  switch (fieldType->type()) {
    case FieldType::Type::TEXT:
      fieldHandler = pool.make_unique<handler::FullTextHandler>(*this, name, fieldType);
      break;
    case FieldType::Type::ID:
      fieldHandler = pool.make_unique<handler::IdHandler>(*this, name, fieldType);
      idHandler_ = fieldHandler.get();
      break;
    case FieldType::Type::STRING:
      if (!fieldType->indexed() && fieldType->hasColumn()) {
        // If the field is not indexed (column stored only), use StrColHandler
        fieldHandler = pool.make_unique<handler::StrColHandler>(*this, name, fieldType);
      } else {
        // indexed and column stored (via ord)
        fieldHandler = pool.make_unique<handler::StrHandler>(*this, name, fieldType);
      }
      break;
    case FieldType::Type::INT:
    case FieldType::Type::FLOAT:
    case FieldType::Type::DOUBLE:
    case FieldType::Type::DATE:
      // One handler pair serves the whole int-column family: the stored
      // encoding (raw ints, sortable bits, epoch millis) comes from
      // FieldType::coerceColInt64, not the handler.
      if (fieldType->multiValued()) {
        fieldHandler = pool.make_unique<handler::MultiIntColHandler>(*this, name, fieldType);
      } else {
        fieldHandler = pool.make_unique<handler::IntColHandler>(*this, name, fieldType);
      }
      break;
    case FieldType::Type::GEO_POINT:
      if (fieldType->multiValued()) {
        fieldHandler = pool.make_unique<handler::MultiGeoPointHandler>(*this, name, fieldType);
      } else {
        fieldHandler = pool.make_unique<handler::GeoPointHandler>(*this, name, fieldType);
      }
      break;
    case FieldType::Type::VECTOR:
      fieldHandler = pool.make_unique<handler::VectorHandler>(*this, name, fieldType);
      break;
    default:
      throw std::runtime_error("Unknown field type: " + std::string(name));
  }


  // Wrap in a StoredFieldWrapperHandler if the field should also have its
  // raw values persisted to a stored-fields resource.  Applies to TEXT,
  // STRING, and ID fields.  STORED on numeric fields is currently ignored -
  // their COLUMN_STORED path already keeps raw values per-doc.
  //
  // The target resource is FieldType::storedResource_ (default:
  // Postings::STORED_DEFAULT_RESOURCE).  Config is looked up in the schema
  // (a StoredFieldType), falling back to writer defaults if unregistered.
  if (fieldType->isStored()
      && (fieldType->type() == FieldType::Type::TEXT
          || fieldType->type() == FieldType::Type::STRING
          || fieldType->type() == FieldType::Type::ID)) {
    const std::string& resourceName = fieldType->storedResource_;
    const StoredFieldType* resConfig = nullptr;
    auto resIt = currSchema->getFieldType(resourceName);
    if (resIt != currSchema->end()) {
      resConfig = dynamic_cast<const StoredFieldType*>(resIt->second.get());
      if (resConfig == nullptr) {
        throw std::runtime_error(
            "Field '" + std::string(name) + "' has STORED set and references '"
            + resourceName + "', but that schema entry is not a StoredFieldType");
      }
    }
    // resConfig == nullptr means the resource isn't in the schema; writer
    // will use defaults.  fromProto auto-registers "_stored_", so this only
    // happens for custom-named resources the user forgot to register.
    auto& writer = getOrCreateStoredFields(resourceName, resConfig);
    fieldHandler = pool.make_unique<handler::StoredFieldWrapperHandler>(
        *this, name, fieldType, std::move(fieldHandler), &writer);
  }

  auto [newIter, inserted] = indexHandlers.try_emplace(std::string(name), std::move(fieldHandler));
  assert(inserted);  // we should never (currently) be trying to overwrite an existing handler
  return *(newIter->second);
}


bool Inverter::flush(std::vector<std::string>* filenames) {
  getPostingsWriter().setMaxDoc(getMaxDoc());  // TODO: this won't always be accurate currently?
  // Index-time stored/vector streams begin as candidates because their final
  // segment size is not known when they are checked out. Once flushing starts,
  // a large inverter spills every existing candidate before further output.
  postingsWriter.configureRamDelegation(
      memSize() < PostingsWriter::SMALL_SEGMENT_BYTES);

  // We could either sort fields first, or after they have been indexed.  Merging segments will presumably
  // go in sorted field order, so lets do the same thing here and sort first.
  std::vector<IndexHandler*> fields;
  fields.reserve(indexHandlers.size());
  for (auto& entry : indexHandlers) {
    fields.push_back(entry.second.get());
  }

  // For normal usecases, spreadsort will fall back to pdqsort (less than 1000 fields, but we want to
  // take care of the outliers as well (esp when it doesn't hurt the average case)
  boost::sort::spreadsort::string_sort(fields.begin(), fields.end(),
                                       [](const IndexHandler* x, size_t offset) {return x->fieldName[offset];},
                                       [](const IndexHandler* x) {return x->fieldName.size();},
                                       [](const IndexHandler* x, const IndexHandler* y) {return *x < *y;});

  // Relinquish outputs held through indexing before any field flush asks for
  // its working set of streams. Vector columns publish their directly-written
  // byte extent here.
  for (auto fieldHandler : fields) {
    fieldHandler->finishIndexing(*this);
  }

  // Stored-fields chunk streams are also held through indexing. Finish all
  // resources now so their chunk and metadata streams are available for reuse
  // by normal field flushing.
  for (auto& [_, writer] : storedFields_) {
    writer->finish(getMaxDoc());
  }

  for (auto fieldHandler : fields) {
    fieldHandler->flush(*this);
  }

  // Handle deleted documents if any
  if (!deleted.empty()) {
    // Create a FixedBitSet with all bits initially set (all docs are live)
    int32_t maxDocId = getMaxDoc();
    screaming::RAMFixedBitSet liveBits(maxDocId, true);
    
    // Mark deleted documents
    int32_t numDeleted = 0;
    for (int32_t docId : deleted) {
      assert(docId >= 0 && docId < maxDocId);
      if (liveBits.get(docId)) {
        liveBits.clear(docId);
        numDeleted++;
      }
    }
    
    // Calculate number of live documents
    int32_t numLiveDocs = maxDocId - numDeleted;


    // Write the liveDocs file and get the liveGen
    liveGen = 1;  // start with generation 1 for the first liveDocs file
    liveDocs = numLiveDocs;
    if (filenames) {
      LiveDocsWriter::writeLiveDocs(postingsWriter.getDirectory(), postingsWriter.segId, liveGen, liveBits, maxDocId, numLiveDocs, *filenames);
    } else {
      LiveDocsWriter::writeLiveDocs(postingsWriter.getDirectory(), postingsWriter.segId, liveGen, liveBits, maxDocId, numLiveDocs);
    }
  } else {
    // No deletes
    liveGen = 0;
    liveDocs = getMaxDoc();
  }

  return getPostingsWriter().finish(filenames);
}


} // end namespace
