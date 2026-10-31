// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Inverter.h"

#include <algorithm>
#include <stdexcept>
#include <boost/sort/spreadsort/string_sort.hpp>
#include "luxir/schema/Schema.h"
#include "luxir/schema/GeoPointCoerce.h"

// include the actual index handlers
#include "handler/IdHandler.h"
#include "handler/StrColHandler.h"
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
  static_cast<handler::IdHandler&>(*idHandler_).addDelete(*this, id, version);
}

size_t Inverter::retainedDeleteBytes() const {
  if (failed() || idHandler_ == nullptr) return 0;
  return static_cast<handler::IdHandler&>(*idHandler_).retainedDeleteBytes();
}

size_t Inverter::deleteCount() const {
  return idHandler_ == nullptr ? 0 : static_cast<handler::IdHandler&>(*idHandler_).nonzeroWatermarks;
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
    static_cast<handler::IdHandler&>(*idHandler_).rollbackTo(*this, mark.idUndoSize);
  }
  assert(deleted.size() >= mark.numDeleted);
  deleted.resize(mark.numDeleted);
}

void Inverter::clearUndoLog() {
  if (idHandler_ != nullptr) {
    static_cast<handler::IdHandler&>(*idHandler_).clearUndoLog();
  }
}


Inverter::InputHandler& Inverter::createInputHandler(std::string_view name) {
  auto resolved = schema->resolveInput(name);
  auto fieldType = resolved.owner ? resolved.owner->primary : schema->getFieldTypeEx(resolved.physicalName);
  StoredFieldsWriter* writer = nullptr;
  if (fieldType->isStored()
      && (fieldType->type() == FieldType::Type::TEXT
          || fieldType->type() == FieldType::Type::STRING
          || fieldType->type() == FieldType::Type::ID)) {
    const std::string& resourceName = fieldType->storedResource_;
    const StoredFieldType* resConfig = nullptr;
    auto* resourceType = schema->getFieldTypePtr(resourceName);
    if (resourceType != nullptr) {
      resConfig = dynamic_cast<const StoredFieldType*>(resourceType);
      if (resConfig == nullptr) {
        throw std::runtime_error(
            "Field '" + std::string(name) + "' has STORED set and references '"
            + resourceName + "', but that schema entry is not a StoredFieldType");
      }
    }
    // resConfig == nullptr means the resource isn't in the schema; writer
    // will use defaults.  fromProto registers "_stored_" and rejects
    // references to undeclared resources, so this only happens for schemas
    // assembled directly.
    writer = &getOrCreateStoredFields(resourceName, resConfig);
  }

  auto input = pool.make_unique<InputHandler>(*this, resolved, fieldType, writer);
  input->branches.push_back({"self", &createPhysicalHandler(resolved.physicalName, fieldType)});
  if (resolved.owner) {
    for (const auto& [label, type] : resolved.owner->variants) {
      input->branches.push_back({label, &createPhysicalHandler(resolved.logicalName + "__" + label, type)});
    }
  }
  auto [iter, inserted] = inputHandlers.try_emplace(std::string(name), std::move(input));
  assert(inserted);
  return *iter->second;
}

Inverter::IndexHandler& Inverter::createPhysicalHandler(
    std::string_view name, const std::shared_ptr<FieldType>& fieldType) {
  // An earlier dispatcher construction may have stopped after this handler.
  auto existing = indexHandlers.find(name);
  if (existing != indexHandlers.end()) return *existing->second;
  // Names are already resolved; dynamic FieldTypes can be template prototypes.
  u_ptr<IndexHandler> fieldHandler;

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
        if (fieldType->multiArray()) {
          fieldHandler = pool.make_unique<handler::StrHandler<true>>(*this, name, fieldType);
        } else {
          fieldHandler = pool.make_unique<handler::StrHandler<>>(*this, name, fieldType);
        }
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


  auto [newIter, inserted] = indexHandlers.try_emplace(std::string(name), std::move(fieldHandler));
  assert(inserted);  // we should never (currently) be trying to overwrite an existing handler
  return *(newIter->second);
}


Inverter::InputHandler::InputHandler(Inverter& inverter, const ResolvedFieldHandle& resolved,
                                     std::shared_ptr<FieldType> fieldType, StoredFieldsWriter* writer)
    : fieldName(inverter.pool, resolved.logicalName), fieldType(std::move(fieldType)),
      shape(resolved.owner ? resolved.owner->shape :
            resolved.fieldType->type() == FieldType::VECTOR ? LogicalField::Shape::VECTOR :
            resolved.fieldType->type() == FieldType::GEO_POINT ? LogicalField::Shape::GEO :
            LogicalField::Shape::SCALAR),
      multi(resolved.owner ? resolved.owner->multi : resolved.fieldType->multiValued()), writer(writer) {}

void Inverter::InputHandler::checkShape(const IndexVal& val) const {
  if (shape != LogicalField::Shape::SCALAR || !coerce::isArray(val)) return;
  size_t count = 0;
  coerce::forEachElement(val, [&](const IndexVal& elem) {
    if (coerce::isArray(elem)) {
      throw DocumentError(fmt::format("Field '{}': scalar fields cannot receive nested arrays",
                                      std::string_view(fieldName)));
    }
    count++;
  });
  if (count > 1 && !multi) {
    throw DocumentError(fmt::format("Field '{}' is single-valued but received multiple values",
                                    std::string_view(fieldName)));
  }
}

void Inverter::InputHandler::store(Inverter& inverter, const IndexVal& val) {
  int32_t doc = inverter.getDoc();
  std::string_view name(fieldName);
  if (std::holds_alternative<std::string_view>(val.kind)) {
    writer->addValue(doc, name, std::get<std::string_view>(val.kind));
  } else if (std::holds_alternative<::hpp_proto::bytes_view>(val.kind)) {
    const auto& b = std::get<::hpp_proto::bytes_view>(val.kind);
    writer->addValue(doc, name, std::string_view((const char*)b.data(), b.size()));
  } else if (std::holds_alternative<luxir::api::ArrStr>(val.kind)) {
    const auto& arr = std::get<luxir::api::ArrStr>(val.kind).v;
    writer->addValues(doc, name, std::span<const std::string_view>(arr.data(), arr.size()));
  } else if (std::holds_alternative<luxir::api::ArrBin>(val.kind)) {
    const auto& arr = std::get<luxir::api::ArrBin>(val.kind).v;
    std::vector<std::string_view> views;
    views.reserve(arr.size());
    for (const auto& bin : arr) {
      views.push_back(std::string_view((const char*)bin.data(), bin.size()));
    }
    writer->addValues(doc, name, std::span<const std::string_view>(views.data(), views.size()));
  } else if (coerce::isNull(val)) {
    // no value: nothing stored
  } else if (coerce::isArray(val)) {
    // numeric / mixed arrays: store each element's canonical rendering
    // (materialized - buf is per-element transient)
    char buf[coerce::TEXT_BUF_SIZE];
    std::vector<std::string> storage;
    coerce::forEachElement(val, [&](const IndexVal& elem) {
      storage.emplace_back(fieldType->coerceTerm(elem, name, buf));
    });
    std::vector<std::string_view> views(storage.begin(), storage.end());
    writer->addValues(doc, name, std::span<const std::string_view>(views.data(), views.size()));
  } else {
    // Numeric / bool scalars: store canonical text before normalization.
    char buf[coerce::TEXT_BUF_SIZE];
    writer->addValue(doc, name, fieldType->coerceTerm(val, name, buf));
  }
}

void Inverter::InputHandler::index(Inverter& inverter, const IndexVal& val) {
  if (shape == LogicalField::Shape::GEO) {
    // Parse once for every branch, and fully before any branch indexes.
    geoPoints.clear();
    auto account = scope_guard([&] {
      size_t bytes = geoPoints.capacity() * sizeof(GeoPoint);
      inverter.addExtraRam((int64_t)bytes - (int64_t)geoScratchBytes);
      geoScratchBytes = bytes;
    });
    const auto& geoType = static_cast<const GeoPointFieldType&>(*fieldType);
    coerce::GeoPointInput{std::string_view(fieldName), geoType.ignoreMalformed, geoType.ignoreZValue}
        .forEachPoint(val, [&](const GeoPoint& point) { geoPoints.push_back(point); });
    indexPoints(inverter, geoPoints);
    return;
  }
  checkShape(val);
  // Capture the original value before any branch's analysis or normalization.
  if (writer) store(inverter, val);
  forEachBranch([&](IndexHandler& branch) { branch.index(inverter, val); });
}

// Scalar overloads retain the submitted kind in a non-owning IndexVal. Every
// physical branch then applies its own coercion, including numeric-to-text.
void Inverter::InputHandler::index(Inverter& inverter, std::string_view val) {
  index(inverter, coerce::scalarVal(val));
}

void Inverter::InputHandler::index(Inverter& inverter, std::span<const std::string_view> vals) {
  index(inverter, coerce::scalarVal(api::ArrStr{.v = vals}));
}

void Inverter::InputHandler::index(Inverter& inverter, int64_t val) {
  index(inverter, coerce::scalarVal(val));
}

void Inverter::InputHandler::index(Inverter& inverter, std::span<const int64_t> vals) {
  index(inverter, coerce::scalarVal(api::ArrInt{.v = vals}));
}

void Inverter::InputHandler::index(Inverter& inverter, double latitude, double longitude) {
  GeoPoint point{latitude, longitude};
  index(inverter, std::span<const GeoPoint>(&point, 1));
}

void Inverter::InputHandler::index(Inverter& inverter, std::span<const GeoPoint> points) {
  for (const auto& point : points) {
    coerce::checkGeoPoint(point, std::string_view(fieldName), "typed geo point");
  }
  indexPoints(inverter, points);
}

// Points already range-checked.
void Inverter::InputHandler::indexPoints(Inverter& inverter, std::span<const GeoPoint> points) {
  if (points.size() > 1 && !multi) {
    throw DocumentError(fmt::format("Field '{}' is single-valued but received multiple values",
                                    std::string_view(fieldName)));
  }
  forEachBranch([&](IndexHandler& branch) { branch.index(inverter, points); });
}


bool Inverter::flush(std::vector<std::string>* filenames, std::vector<FileDescriptor>* descriptors) {
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

  if (getMaxDoc()) noteFlushWorkspace(versionRuns.flushWorkspaceBytes());
  postingsWriter.writeVersionRuns(versionRuns);

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


    // Start with generation 1 for the first liveDocs file.
    if (filenames) {
      LiveDocsWriter::writeLiveDocs(postingsWriter.getDirectory(), postingsWriter.segId, 1, liveBits, maxDocId, numLiveDocs, *filenames, descriptors);
    } else {
      LiveDocsWriter::writeLiveDocs(postingsWriter.getDirectory(), postingsWriter.segId, 1, liveBits, maxDocId, numLiveDocs);
    }
    liveGen = 1;
    liveDocs = numLiveDocs;
  } else {
    // No deletes
    liveGen = 0;
    liveDocs = getMaxDoc();
  }

  bool written = getPostingsWriter().finish(filenames, descriptors);
  if (written && sourceWriter_) flushSource(filenames, descriptors);
  return written;
}

void Inverter::addSource(std::string_view id, std::string_view body, uint64_t version, uint8_t flags) {
  if (!schema->source.enabled) return;
  if (!sourceWriter_) {
    sourceWriter_ = std::make_unique<SourceWriter>(schema->source, Postings::getIndexFileNamePrefix(getSegId()));
  }
  // Record i is doc i: docs started without a source before this one get placeholders.
  sourceWriter_->padTo((uint32_t)currDoc);
  sourceWriter_->add(id, body, version, flags);
  size_t ram = sourceWriter_->ramBytes();
  addExtraRam((int64_t)ram - (int64_t)sourceRamAccounted_);
  sourceRamAccounted_ = ram;
}

// Writes the source file holding this segment's documents and the map that
// points the segment's doc IDs at it. Both are listed with the segment's files.
void Inverter::flushSource(std::vector<std::string>* filenames, std::vector<FileDescriptor>* descriptors) {
  if (!sourceSeq) throw std::logic_error("Inverter::sourceSeq must be set before a segment with sources is flushed");
  auto& dir = postingsWriter.getDirectory();
  int32_t docs = getMaxDoc();
  sourceWriter_->padTo((uint32_t)docs);
  std::string name = Postings::getSourceFileName(sourceSeq->fetch_add(1));
  std::string mapName = Postings::getSourceMapFileName(Postings::getSortableString(getSegId()), 1);
  FileDescriptor file;
  try {
    sourceWriter_->flush(dir, name, file);
    SourceMap map;
    map.addRun(0, docs, map.fileIndex({name, (uint32_t)docs, file.size}), 0);
    map.write(dir, mapName, filenames, descriptors);
  } catch (...) {
    dir.deleteFile(name);
    dir.deleteFile(mapName);
    throw;
  }
  if (filenames) filenames->push_back(name);
  if (descriptors) descriptors->push_back(file);
  sourceGen = 1;
  addExtraRam(-(int64_t)sourceRamAccounted_);
  sourceRamAccounted_ = 0;
  sourceWriter_.reset();
}


} // end namespace
