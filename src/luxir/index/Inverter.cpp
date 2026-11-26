// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Inverter.h"
#include "StoredValues.h"
#include "NestedInversion.h"

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


Inverter::Inverter(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
    const std::function<std::shared_ptr<Schema>()>& schemaProvider,
    IndexRamBudget* ramBudget, StoredDictionaryProvider dictionaryProvider)
    : Inverter(dir, segId, creatorEpoch, schemaProvider ? schemaProvider() : Schema::createDefaultSchema(),
               ramBudget, std::move(dictionaryProvider)) {}

Inverter::Inverter(Directory& dir, uint64_t segId, uint64_t creatorEpoch, std::shared_ptr<Schema> schema,
    IndexRamBudget* ramBudget, StoredDictionaryProvider dictionaryProvider)
    : ramGuard(ramBudget == nullptr ? IndexRamBudget::Guard() : IndexRamBudget::Guard(*ramBudget, 0)),
      postingsWriter(dir, segId, creatorEpoch), schema(std::move(schema)),
      storedDictionaryProvider(std::move(dictionaryProvider)) {
  postingsWriter.setStagedRamAccounting([this](int64_t delta) noexcept {
    [[maybe_unused]] int64_t previous = stagedRamBytes.fetch_add(delta, std::memory_order_relaxed);
    assert(previous + delta >= 0);
    if (ramGuard) ramGuard.adjustSize(delta);
  });
}

Inverter::~Inverter() = default;

void Inverter::requireInputSpace(std::string_view name) const {
  auto path = spaceParentPath(name);
  auto current = nested && nested->active ? std::string_view(nested->active->name) : std::string_view{};
  if (path != current) rejectInputSpace(name);
}

void Inverter::rejectInputSpace(std::string_view name) const {
  throw DocumentError("Field must be supplied inside nested path '" + std::string(spaceParentPath(name))
      + "': " + std::string(name));
}

bool Inverter::hasChildRows() const { return maxChildRows > 0; }

bool Inverter::shouldFlush(size_t ramCap, size_t docCap) {
  auto rows = std::max(getMaxDoc(), maxChildRows);
  if (maxChildRows > 0) {
    docCap = std::min(docCap, (size_t)PostingsReader::MAX_SEGMENT_DOCS);
  }
  return memSize() > ramCap || (size_t)rows > docCap;
}


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
  requireInputSpace(name);
  auto space = inputSpace;
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
    writer = &getOrCreateStoredFields(space, resourceName, resConfig);
  }

  auto input = pool.make_unique<InputHandler>(*this, resolved, fieldType, space, writer);
  auto addBranch = [&](const std::string& label, std::string_view physicalName,
                       const std::shared_ptr<FieldType>& type) {
    if (auto* handler = createPhysicalHandler(physicalName, type, space)) {
      input->branches.push_back({label, handler});
    }
  };
  // A root document's source record holds its top-level keys; nested rows have
  // no record of their own, so their fields keep a stored copy.
  auto type = fieldType->type();
  input->sourceBacked = writer && space == SpaceOrdinal::ROOT && schema->source.enabled && !fieldType->storedCopy_ &&
                        (type == FieldType::Type::TEXT || type == FieldType::Type::STRING);
  if (input->sourceBacked) writer->markSourceBacked(resolved.logicalName);
  addBranch("self", resolved.physicalName, fieldType);
  if (resolved.owner) {
    for (const auto& [label, type] : resolved.owner->variants) {
      addBranch(label, resolved.logicalName + "__" + label, type);
    }
  }
  auto [iter, inserted] = inputHandlers.try_emplace(std::string(name), std::move(input));
  assert(inserted);
  return *iter->second;
}

Inverter::IndexHandler* Inverter::createPhysicalHandler(
    std::string_view name, const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space) {
  // An earlier dispatcher construction may have stopped after this handler.
  auto existing = indexHandlers.find(name);
  if (existing != indexHandlers.end()) return existing->second.get();
  // Names are already resolved; dynamic FieldTypes can be template prototypes.
  u_ptr<IndexHandler> fieldHandler;

  if (fieldType->type() == FieldType::ID) {
    fieldHandler = pool.make_unique<handler::IdHandler>(*this, name, fieldType, space);
    idHandler_ = fieldHandler.get();
  } else if (fieldType->type() == FieldType::NESTED) {
    if (!nested) nested = std::make_unique<NestedState>(*this);
    fieldHandler = pool.make_unique<handler::NestedHandler>(*this, name, fieldType, space,
                                                           *nested, nested->path(name));
  } else if (fieldType->type() == FieldType::INTERVAL) {
    fieldHandler = pool.make_unique<handler::RangeHandler>(*this, name, fieldType, space);
  } else if (fieldType->type() == FieldType::GEO_POINT) {
    if (fieldType->multiValued()) {
      fieldHandler = pool.make_unique<handler::MultiGeoPointHandler>(*this, name, fieldType, space);
    } else {
      fieldHandler = pool.make_unique<handler::GeoPointHandler>(*this, name, fieldType, space);
    }
  } else {
    switch (FieldType::storage(fieldType->type())) {
      case FieldType::Storage::TEXT:
        fieldHandler = pool.make_unique<handler::FullTextHandler>(*this, name, fieldType, space);
        break;
      case FieldType::Storage::TERMS:
        if (!fieldType->indexed()) {
          if (!fieldType->hasColumn()) return nullptr;
          fieldHandler = pool.make_unique<handler::StrColHandler>(*this, name, fieldType, space);
        } else {
          // Indexed strings can share the dictionary with an ord column.
          if (fieldType->multiArray()) {
            fieldHandler = pool.make_unique<handler::StrHandler<true>>(*this, name, fieldType, space);
          } else {
            fieldHandler = pool.make_unique<handler::StrHandler<>>(*this, name, fieldType, space);
          }
        }
        break;
      case FieldType::Storage::INT64:
        // One handler pair serves the whole int-column family: the stored
        // encoding (raw ints, sortable bits, epoch millis) comes from
        // FieldType::coerceColInt64, not the handler.
        if (fieldType->multiValued()) {
          fieldHandler = pool.make_unique<handler::MultiIntColHandler>(*this, name, fieldType, space);
        } else {
          fieldHandler = pool.make_unique<handler::IntColHandler>(*this, name, fieldType, space);
        }
        break;
      case FieldType::Storage::VECTOR:
        fieldHandler = pool.make_unique<handler::VectorHandler>(*this, name, fieldType, space);
        break;
      default:
        throw std::runtime_error("Unknown field type: " + std::string(name));
    }
  }


  auto [newIter, inserted] = indexHandlers.try_emplace(std::string(name), std::move(fieldHandler));
  assert(inserted);  // we should never (currently) be trying to overwrite an existing handler
  return newIter->second.get();
}


Inverter::InputHandler::InputHandler(Inverter& inverter, const ResolvedFieldHandle& resolved,
                                     std::shared_ptr<FieldType> fieldType, SpaceOrdinal space,
                                     StoredFieldsWriter* writer)
    : fieldName(inverter.pool, resolved.logicalName), fieldType(std::move(fieldType)), space(space),
      shape(resolved.owner ? resolved.owner->shape :
            resolved.fieldType->type() == FieldType::VECTOR ? LogicalField::Shape::VECTOR :
            resolved.fieldType->type() == FieldType::GEO_POINT ? LogicalField::Shape::GEO :
            resolved.fieldType->type() == FieldType::NESTED ? LogicalField::Shape::NESTED :
            resolved.fieldType->type() == FieldType::INTERVAL ? LogicalField::Shape::RANGE :
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
  // The document's source holds this value as indexed: a fetch reads it there.
  if (sourceBacked && inverter.sourceHoldsFields_) return;
  int32_t doc = inverter.getDoc();
  std::string_view name(fieldName);
  storedValues(val, name, [&](std::span<const std::string_view> values) { writer->addValues(doc, name, values); });
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
  if (nested) nested->finish();
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

  // Finish checkouts held through indexing before starting the field flush.
  // Vector columns record their final value slice here.
  // Any spill files they used are now eligible for reuse.
  for (auto fieldHandler : fields) {
    fieldHandler->finishIndexing(*this);
  }

  // Stored fields also hold chunk checkouts through indexing. Finish those
  // and their metadata now to release any staging before allocating more
  // during field flush.
  for (auto& [key, writer] : storedFields_) {
    int32_t maxDoc = postingsWriter.getMaxDoc(key.space);
    if (auto stored = writer->finish(maxDoc)) {
      postingsWriter.addStoredField(key.space, key.name, maxDoc, *stored);
    }
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
      liveDocsBytes = LiveDocsWriter::writeLiveDocs(postingsWriter.getDirectory(), postingsWriter.segId, postingsWriter.creatorEpoch, 1, postingsWriter.creatorEpoch, liveBits, maxDocId, numLiveDocs, *filenames, descriptors);
    } else {
      liveDocsBytes = LiveDocsWriter::writeLiveDocs(postingsWriter.getDirectory(), postingsWriter.segId, postingsWriter.creatorEpoch, 1, postingsWriter.creatorEpoch, liveBits, maxDocId, numLiveDocs, nullptr);
    }
    liveGen = 1;
    liveDocs = numLiveDocs;
  } else {
    // No deletes
    liveGen = 0;
    liveDocs = getMaxDoc();
    liveDocsBytes = 0;
  }

  bool written = getPostingsWriter().finish(filenames, descriptors);
  if (written && sourceWriter_) flushSource(filenames, descriptors);
  return written;
}

void Inverter::addSource(std::string_view id, std::string_view body, uint64_t version, uint8_t flags) {
  if (!schema->source.enabled) return;
  sourceHoldsFields_ = source::holdsFieldValues(flags);
  if (!sourceWriter_) {
    auto format = schema->source.dictionaryFormat();
    auto dictionary = storedDictionaryProvider && format.dictionaryBytes
        ? storedDictionaryProvider(Postings::SOURCE_RESOURCE, format) : nullptr;
    sourceWriter_ = std::make_unique<SourceWriter>(
        schema->source, Postings::getIndexFileNamePrefix(getSegId(), postingsWriter.creatorEpoch),
        std::move(dictionary));
  }
  // Record i is doc i: docs started without a source before this one get placeholders.
  // A second call for one doc, or a call before startDoc(), would shift every later
  // record onto the wrong document, so it fails instead.
  if (currDoc < 0) throw std::logic_error("Inverter::addSource called before startDoc");
  sourceWriter_->padTo((uint32_t)currDoc);
  if (sourceWriter_->docCount() != (uint32_t)currDoc) {
    throw std::logic_error("Inverter::addSource: source record " + std::to_string(sourceWriter_->docCount()) +
                           " would not be doc " + std::to_string(currDoc));
  }
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
  std::string name = Postings::getSourceFileName(sourceSeq->fetch_add(1), postingsWriter.creatorEpoch);
  std::string mapName = Postings::getSourceMapFileName(Postings::segmentKey(getSegId(), postingsWriter.creatorEpoch), 1);
  FileDescriptor file;
  try {
    sourceWriter_->flush(dir, name, file);
    SourceMap map;
    map.addRun(0, docs, map.fileIndex({name, (uint32_t)docs, file.size}), 0);
    map.write(dir, mapName, filenames, descriptors);
  } catch (...) {
    try { dir.deleteFile(name); } catch (...) {}
    try { dir.deleteFile(mapName); } catch (...) {}
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
