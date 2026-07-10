#include "Schema.h"
#include <memory_resource>
#include "solux/api/build.h"
#include "solux/api/padded_input.h"
#include "solux/api/solux_types.hpp"

#include <boost/unordered/unordered_flat_set.hpp>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace solux {

using FieldClass = solux::api::FieldDef_::FieldClass;
using IndexMode = solux::api::FieldDef_::IndexMode;
using VectorMetric = solux::api::VectorParams_::Metric;

// Resolved state for a single FieldDef during fromProto processing
struct ResolvedField {
  bool abstract = false;
  bool hasFieldClass = false;
  FieldClass fieldClass = FieldClass::STRING;
  bool hasIndex = false;
  IndexMode index = IndexMode::NONE;
  bool hasColumnStored = false;
  bool columnStored = false;
  bool hasMultiValued = false;
  bool multiValued = false;
  bool hasStored = false;
  bool stored = false;
  // storedResource: empty string means "inherit from parent or use the
  // default".  hasStoredResource tracks whether any field in the chain set
  // it explicitly (even to a value that equals the default).
  bool hasStoredResource = false;
  std::string storedResource;
  bool hasAnalyzer = false;
  std::string tokenizer;
  std::vector<std::string> filters;
  // 0 means "not set / infer from first indexed value"; > 0 means strict.
  int32_t vectorDims = 0;
  // Tracks whether any field in the chain set metric explicitly.  Default is NONE
  // (storage-only); a non-NONE value means an ANN aux index will be built when
  // the field is targeted by UpdateRequest.build_aux_indexes.
  bool hasVectorMetric = false;
  VectorFieldType::Metric vectorMetric = VectorFieldType::METRIC_NONE;
  bool hasVectorNormalized = false;
  bool vectorNormalized = false;
  bool hasVectorNormalizeOnWrite = false;
  bool vectorNormalizeOnWrite = false;
};

using sv_flat_map =
  boost::unordered_flat_map<std::string_view, const solux::api::FieldDef*, PackedTermHash, PackedTermEqual>;
using sv_resolved_map = boost::unordered_flat_map<std::string_view, ResolvedField, PackedTermHash, PackedTermEqual>;
using sv_flat_set = boost::unordered_flat_set<std::string_view, PackedTermHash, PackedTermEqual>;

static std::string serializeSchemaDef(const solux::api::SchemaDef& def) {
  std::vector<std::byte> serialized;
  if (!solux::api::encode(def, serialized)) {
    throw std::runtime_error("Failed to serialize SchemaDef protobuf");
  }
  return std::string((const char*)serialized.data(), serialized.size());
}

// Decode into `def`, whose non-owning views are backed by `arena`; the input bytes are
// copied there with the padding required by solux::api::decode().
static void parseSchemaDef(std::string_view bytes, solux::api::SchemaDef& def,
                           std::pmr::memory_resource& arena) {
  auto padded =
    solux::api::copyToPaddedInput(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())), arena);
  if (!solux::api::decode(def, padded, arena)) {
    throw std::runtime_error("Failed to parse SchemaDef protobuf");
  }
}

// Walk the parent chain and resolve all properties for a field.
static void resolveField(std::string_view name,
                         const sv_flat_map& defMap,
                         sv_resolved_map& resolved,
                         sv_flat_set& visiting) {
  if (resolved.contains(name)) return;

  if (visiting.contains(name)) {
    throw std::runtime_error("Circular schema inheritance detected involving field: " + std::string(name));
  }
  visiting.insert(name);

  auto defIt = defMap.find(name);
  if (defIt == defMap.end()) {
    throw std::runtime_error("Parent field not found in schema: " + std::string(name));
  }
  const solux::api::FieldDef& def = *defIt->second;

  // Resolve parent first if exists
  ResolvedField parentResolved{};
  if (!def.parent.empty()) {
    std::string_view parentName = def.parent;
    resolveField(parentName, defMap, resolved, visiting);
    parentResolved = resolved.find(parentName)->second;
  }

  ResolvedField r;
  r.abstract = def.abstract;  // NOT inherited

  // field_class: use this field's if set, else parent's
  if (def.field_class.has_value()) {
    r.hasFieldClass = true;
    r.fieldClass = *def.field_class;
  } else {
    r.hasFieldClass = parentResolved.hasFieldClass;
    r.fieldClass = parentResolved.fieldClass;
  }

  // index mode (UNSET is treated the same as absent)
  if (def.index.has_value() && *def.index != IndexMode::UNSET) {
    r.hasIndex = true;
    r.index = *def.index;
  } else {
    r.hasIndex = parentResolved.hasIndex;
    r.index = parentResolved.index;
  }

  // column_stored
  if (def.column_stored.has_value()) {
    r.hasColumnStored = true;
    r.columnStored = *def.column_stored;
  } else {
    r.hasColumnStored = parentResolved.hasColumnStored;
    r.columnStored = parentResolved.columnStored;
  }

  // multi_valued
  if (def.multi_valued.has_value()) {
    r.hasMultiValued = true;
    r.multiValued = *def.multi_valued;
  } else {
    r.hasMultiValued = parentResolved.hasMultiValued;
    r.multiValued = parentResolved.multiValued;
  }

  // stored
  if (def.stored.has_value()) {
    r.hasStored = true;
    r.stored = *def.stored;
  } else {
    r.hasStored = parentResolved.hasStored;
    r.stored = parentResolved.stored;
  }

  // stored_resource
  if (!def.stored_resource.empty()) {
    r.hasStoredResource = true;
    r.storedResource = def.stored_resource;
  } else {
    r.hasStoredResource = parentResolved.hasStoredResource;
    r.storedResource = parentResolved.storedResource;
  }

  // analyzer: atomic - first non-empty AnalyzerDef in chain wins
  if (def.analyzer.has_value() && (!def.analyzer->tokenizer.empty() || !def.analyzer->filters.empty())) {
    r.hasAnalyzer = true;
    r.tokenizer = std::string(def.analyzer->tokenizer);
    for (const auto& filter : def.analyzer->filters) {
      r.filters.push_back(std::string(filter));
    }
  } else {
    r.hasAnalyzer = parentResolved.hasAnalyzer;
    r.tokenizer = parentResolved.tokenizer;
    r.filters = parentResolved.filters;
  }

  // vector: VectorParams.dims > 0 overrides; otherwise inherit from parent
  // (0 = not set / infer from first value).
  r.vectorDims = (def.vector.has_value() && def.vector->dims > 0)
    ? def.vector->dims
    : parentResolved.vectorDims;

  // vector metric: explicit set on this field overrides; otherwise inherit.
  // proto NONE (0) is treated as "not set" so default-initialized VectorParams
  // doesn't clobber an inherited metric.
  if (def.vector.has_value() && def.vector->metric != VectorMetric::NONE) {
    r.hasVectorMetric = true;
    r.vectorMetric = (VectorFieldType::Metric)def.vector->metric;
  } else {
    r.hasVectorMetric = parentResolved.hasVectorMetric;
    r.vectorMetric = parentResolved.vectorMetric;
  }

  // vector normalized: explicit set on this field overrides; otherwise inherit.
  if (def.vector.has_value() && def.vector->normalized.has_value()) {
    r.hasVectorNormalized = true;
    r.vectorNormalized = *def.vector->normalized;
  } else {
    r.hasVectorNormalized = parentResolved.hasVectorNormalized;
    r.vectorNormalized = parentResolved.vectorNormalized;
  }

  // vector normalize_on_write: explicit set on this field overrides; otherwise inherit.
  if (def.vector.has_value() && def.vector->normalize_on_write.has_value()) {
    r.hasVectorNormalizeOnWrite = true;
    r.vectorNormalizeOnWrite = *def.vector->normalize_on_write;
  } else {
    r.hasVectorNormalizeOnWrite = parentResolved.hasVectorNormalizeOnWrite;
    r.vectorNormalizeOnWrite = parentResolved.vectorNormalizeOnWrite;
  }

  visiting.erase(name);
  resolved[name] = std::move(r);
}


std::shared_ptr<Schema> Schema::fromProto(const solux::api::SchemaDef& def, const Schema* base) {
  auto schema = std::make_shared<Schema>();

  // Build the source def to persist. For MERGE mode, merge new fields into the base's source def.
  if (base && !base->sourceDef_.empty()) {
    // Decode the base's source def into an arena that lives through the merge + serialize.
    std::pmr::monotonic_buffer_resource baseArena;
    solux::api::SchemaDef baseSrc;
    parseSchemaDef(base->sourceDef_, baseSrc, baseArena);

    // Build a set of field names from the new def for quick lookup
    sv_flat_set newFieldNames;
    for (const auto& field : def.fields) {
      newFieldNames.insert(field.name);
    }

    // Merge: base fields NOT being overridden, then all new fields. Collect the FieldDef
    // views into a local vector (backed by baseSrc's arena + def's source) and point the
    // merged SchemaDef's span at it, then serialize.
    std::vector<solux::api::FieldDef> mergedFields;
    for (const auto& field : baseSrc.fields) {
      if (!newFieldNames.contains(field.name)) {
        mergedFields.push_back(field);
      }
    }
    for (const auto& field : def.fields) {
      mergedFields.push_back(field);
    }
    solux::api::SchemaDef mergedSrc;
    mergedSrc.fields = std::span<const solux::api::FieldDef>(mergedFields.data(), mergedFields.size());
    schema->sourceDef_ = serializeSchemaDef(mergedSrc);
  } else {
    schema->sourceDef_ = serializeSchemaDef(def);
  }

  // If MERGE mode, copy base schema's fields
  if (base) {
    schema->fieldTypeMap = base->fieldTypeMap;
  }

  // Build a name->FieldDef map
  sv_flat_map defMap;
  for (const auto& field : def.fields) {
    defMap[field.name] = &field;
  }

  // Resolve all fields
  sv_resolved_map resolved;
  sv_flat_set visiting;

  // Pre-populate resolved map with base schema fields so new fields can reference them as parents.
  // Fields being overridden by the new def are skipped - they'll be re-resolved from the SchemaDef.
  if (base) {
    for (const auto& [name, ft] : base->fieldTypeMap) {
      if (defMap.contains(name)) continue;
      // StoredFieldType entries aren't user fields and have no proto form;
      // they're already preserved via the earlier fieldTypeMap = base->fieldTypeMap
      // copy, so skip the resolve pipeline for them.
      if (dynamic_cast<const StoredFieldType*>(ft.get()) != nullptr) continue;
      ResolvedField r;
      r.abstract = ft->isAbstract();
      r.hasFieldClass = true;
      switch (ft->type()) {
        case FieldType::ID:     r.fieldClass = FieldClass::ID; break;
        case FieldType::STRING: r.fieldClass = FieldClass::STRING; break;
        case FieldType::TEXT:   r.fieldClass = FieldClass::TEXT; break;
        case FieldType::INT:    r.fieldClass = FieldClass::INT; break;
        case FieldType::FLOAT:  r.fieldClass = FieldClass::FLOAT; break;
        case FieldType::DOUBLE: r.fieldClass = FieldClass::DOUBLE; break;
        case FieldType::DATE:   r.fieldClass = FieldClass::DATE; break;
        case FieldType::VECTOR: r.fieldClass = FieldClass::VECTOR; break;
        default:                r.fieldClass = FieldClass::BIN; break;
      }
      r.hasIndex = true;
      r.index = ft->indexed() ? IndexMode::MATCH : IndexMode::NONE;
      r.hasColumnStored = true;
      r.columnStored = ft->hasColumn();
      r.hasMultiValued = true;
      r.multiValued = ft->multiValued();
      r.hasStored = true;
      r.stored = ft->isStored();
      // Always capture storedResource_: whether the base field had a
      // non-default value or the default, the rebuilt FieldType below must
      // end up with the same value.
      r.hasStoredResource = true;
      r.storedResource = ft->storedResource_;
      if (ft->type() == FieldType::TEXT) {
        auto* textFt = (TextFieldType*)(ft.get());
        r.hasAnalyzer = true;
        r.tokenizer = textFt->tokenizer_;
        r.filters = textFt->filters_;
      } else if (ft->type() == FieldType::VECTOR) {
        auto* vecFt = (VectorFieldType*)(ft.get());
        r.vectorDims = vecFt->dims_;
        r.hasVectorMetric = true;
        r.vectorMetric = vecFt->metric_;
        r.hasVectorNormalized = true;
        r.vectorNormalized = vecFt->normalized_;
        r.hasVectorNormalizeOnWrite = true;
        r.vectorNormalizeOnWrite = vecFt->normalizeOnWrite_;
      }
      resolved[name] = std::move(r);
    }
  }

  for (auto& [name, _] : defMap) {
    resolveField(name, defMap, resolved, visiting);
  }

  // Create FieldType objects from resolved fields
  for (auto& [name, r] : resolved) {
    if (!r.hasFieldClass) {
      throw std::runtime_error("Field '" + std::string(name) + "' has no field_class and no parent to inherit from");
    }

    // Apply defaults based on field_class if properties were not explicitly set
    IndexMode index = r.index;
    bool columnStored = r.columnStored;
    bool multiValued = r.multiValued;
    bool stored = r.stored;

    if (!r.hasIndex) {
      // defaults by field_class
      switch (r.fieldClass) {
        case FieldClass::ID:     index = IndexMode::MATCH; break;
        case FieldClass::STRING: index = IndexMode::MATCH; break;
        case FieldClass::TEXT:   index = IndexMode::MATCH; break;
        case FieldClass::INT:    index = IndexMode::NONE; break;
        default: index = IndexMode::NONE; break;
      }
    }

    // Reject index modes the engine cannot honor yet (or ever): the schema
    // must not accept a contract it cannot deliver.
    bool numericClass = r.fieldClass == FieldClass::INT || r.fieldClass == FieldClass::FLOAT ||
                        r.fieldClass == FieldClass::DOUBLE || r.fieldClass == FieldClass::DATE;
    if (index == IndexMode::RANGE) {
      if (numericClass) {
        throw std::runtime_error("index=RANGE is not yet implemented for field: " + std::string(name));
      }
      throw std::runtime_error(
        "index=RANGE is not supported for this field_class (field: " + std::string(name) +
        "); MATCH-indexed string/id fields answer range queries through the terms dictionary");
    }
    if (index == IndexMode::MATCH) {
      if (numericClass) {
        throw std::runtime_error(
          "index=MATCH (numeric term postings) is not yet implemented for field: " + std::string(name));
      }
      if (r.fieldClass == FieldClass::VECTOR || r.fieldClass == FieldClass::BIN) {
        throw std::runtime_error("index=MATCH is not supported for this field_class (field: " + std::string(name) + ")");
      }
    }
    if (!r.hasColumnStored) {
      switch (r.fieldClass) {
        case FieldClass::ID:     columnStored = true; break;
        case FieldClass::STRING: columnStored = true; break;
        case FieldClass::TEXT:   columnStored = false; break;
        case FieldClass::INT:    columnStored = true; break;
        default: columnStored = true; break;
      }
    }
    if (!r.hasMultiValued) {
      multiValued = false;
    }
    if (!r.hasStored) {
      // TEXT fields are stored by default so the raw (pre-analysis) value can
      // be returned in search results.  Other field classes default to false;
      // STRING/ID already expose their value via the column store.
      stored = (r.fieldClass == FieldClass::TEXT);
    }

    // Build flags
    FieldType::flag_type flags = 0;
    if (index == IndexMode::MATCH) {
      if (r.fieldClass == FieldClass::TEXT) {
        flags |= FieldType::INDEX_DOCS_FREQS_POSITIONS;
      } else {
        flags |= FieldType::INDEX_DOCS;
      }
    }
    if (columnStored) flags |= FieldType::COLUMN_STORED;
    if (multiValued) flags |= FieldType::MULTI_VALUED;
    if (stored) flags |= FieldType::STORED;

    std::shared_ptr<FieldType> ft;

    switch (r.fieldClass) {
      case FieldClass::ID:
        ft = std::make_shared<IdFieldType>(name, flags);
        break;
      case FieldClass::STRING:
        ft = std::make_shared<StrFieldType>(name, flags);
        break;
      case FieldClass::TEXT: {
        std::string tokenizer = r.hasAnalyzer ? r.tokenizer : "whitespace";
        if (tokenizer.empty()) tokenizer = "whitespace";
        ft = std::make_shared<TextFieldType>(name, flags, tokenizer, r.filters);
        break;
      }
      case FieldClass::INT:
        ft = std::make_shared<IntFieldType>(name, flags);
        break;
      case FieldClass::FLOAT:
        ft = std::make_shared<FloatFieldType>(name, flags);
        break;
      case FieldClass::DOUBLE:
        ft = std::make_shared<DoubleFieldType>(name, flags);
        break;
      case FieldClass::DATE:
        ft = std::make_shared<DateFieldType>(name, flags);
        break;
      case FieldClass::VECTOR: {
        // VECTOR is always fixed-size (every value in a segment must share dims).
        // dims may be 0, meaning "infer from first indexed value".
        bool normalizeOnWrite;
        normalizeOnWrite = !r.vectorNormalized && (r.hasVectorNormalizeOnWrite
          ? r.vectorNormalizeOnWrite
          : (r.vectorMetric == VectorFieldType::METRIC_COSINE));
        ft = std::make_shared<VectorFieldType>(name, r.vectorDims, flags | FieldType::FIXED_SIZE,
                                               r.vectorMetric, r.vectorNormalized, normalizeOnWrite);
        break;
      }
      default:
        throw std::runtime_error("Unsupported field_class for field: " + std::string(name));
    }

    if (r.abstract) ft->flags_ |= FieldType::ABSTRACT;
    // Apply any resolved storedResource_ override; empty means "keep default".
    if (!r.storedResource.empty()) ft->storedResource_ = r.storedResource;
    schema->fieldTypeMap[name] = std::move(ft);
  }

  // Ensure the default stored-fields resource is available in every schema.
  // StoredFieldType entries are not serialized through proto (see toProto),
  // so we materialize the default unconditionally on load.  Users who have
  // registered named column families must re-add them programmatically.
  std::string defaultName(Postings::STORED_DEFAULT_RESOURCE);
  if (schema->fieldTypeMap.find(defaultName) == schema->fieldTypeMap.end()) {
    schema->fieldTypeMap[defaultName] = std::make_shared<StoredFieldType>(defaultName);
  }

  return schema;
}


void Schema::toProto(solux::api::SchemaDef* def, std::pmr::memory_resource& arena) const {
  // Non-owning build: collect FieldDef views (over this schema's owned strings + `arena`
  // for the filters spans) then point def->fields at an arena-allocated copy.
  std::vector<solux::api::FieldDef> fields;
  for (const auto& [name, ft] : fieldTypeMap) {
    // StoredFieldType entries describe per-segment stored-fields resources.
    // They're managed in-memory (fromProto re-adds the default "_stored_");
    // custom per-resource config doesn't round-trip through proto yet.
    if (dynamic_cast<const StoredFieldType*>(ft.get()) != nullptr) {
      continue;
    }
    solux::api::FieldDef fieldDef;
    fieldDef.name = name;
    fieldDef.abstract = ft->isAbstract();

    // Map FieldType::Type to FieldDef::FieldClass
    switch (ft->type()) {
      case FieldType::ID:
        fieldDef.field_class = FieldClass::ID;
        break;
      case FieldType::STRING:
        fieldDef.field_class = FieldClass::STRING;
        break;
      case FieldType::TEXT:
        fieldDef.field_class = FieldClass::TEXT;
        break;
      case FieldType::INT:
        fieldDef.field_class = FieldClass::INT;
        break;
      case FieldType::FLOAT:
        fieldDef.field_class = FieldClass::FLOAT;
        break;
      case FieldType::DOUBLE:
        fieldDef.field_class = FieldClass::DOUBLE;
        break;
      case FieldType::DATE:
        fieldDef.field_class = FieldClass::DATE;
        break;
      case FieldType::BIN:
        fieldDef.field_class = FieldClass::BIN;
        break;
      case FieldType::VECTOR:
        fieldDef.field_class = FieldClass::VECTOR;
        break;
      default:
        break;
    }

    // Set flags
    fieldDef.index = ft->indexed() ? IndexMode::MATCH : IndexMode::NONE;
    fieldDef.column_stored = ft->hasColumn();
    fieldDef.multi_valued = ft->multiValued();
    fieldDef.stored = ft->isStored();
    // Only emit stored_resource when it deviates from the default - keeps
    // the serialized schema clean for fields that use "_stored_".
    if (ft->storedResource_ != Postings::STORED_DEFAULT_RESOURCE) {
      fieldDef.stored_resource = ft->storedResource_;
    }

    // Serialize analyzer for TEXT fields
    if (ft->type() == FieldType::TEXT) {
      auto* textFt = (TextFieldType*)(ft.get());
      auto& analyzer = fieldDef.analyzer.emplace();
      analyzer.tokenizer = textFt->tokenizer_;
      std::string_view* fl = solux::api::build::allocArray(analyzer.filters, textFt->filters_.size(), arena);
      for (size_t i = 0; i < textFt->filters_.size(); i++) {
        fl[i] = textFt->filters_[i];
      }
    } else if (ft->type() == FieldType::VECTOR) {
      auto* vecFt = (VectorFieldType*)(ft.get());
      auto ensureVector = [&]() -> solux::api::VectorParams& {
        if (!fieldDef.vector.has_value()) {
          fieldDef.vector.emplace();
        }
        return *fieldDef.vector;
      };
      if (vecFt->dims_ > 0) {
        ensureVector().dims = vecFt->dims_;
      }
      if (vecFt->metric_ != VectorFieldType::METRIC_NONE) {
        ensureVector().metric = (VectorMetric)vecFt->metric_;
      }
      if (vecFt->normalized_) {
        ensureVector().normalized = true;
      }
      if (vecFt->metric_ == VectorFieldType::METRIC_COSINE) {
        ensureVector().normalize_on_write = vecFt->normalizeOnWrite_;
      }
    }
    fields.push_back(fieldDef);
  }
  solux::api::FieldDef* arr = solux::api::build::allocArray(def->fields, fields.size(), arena);
  for (size_t i = 0; i < fields.size(); i++) {
    arr[i] = fields[i];
  }
}


std::shared_ptr<Schema> Schema::createDefaultSchema() {
  // Non-owning build: all strings here are string literals (stable); per-field filter
  // spans are allocated in `arena`. `fields` + `arena` outlive the fromProto() call.
  std::pmr::monotonic_buffer_resource arena;
  std::vector<solux::api::FieldDef> fields;

  // Helper lambda to add a field.  `stored` is tri-state: -1 means "leave
  // unset so the field_class default applies", 0/1 set it explicitly.
  auto addField = [&](const char* name, FieldClass fc,
                      bool abstract, IndexMode index, bool columnStored,
                      bool multiValued = false,
                      const char* tokenizer = nullptr,
                      std::vector<std::string_view> filters = {},
                      int stored = -1) {
    solux::api::FieldDef f;
    f.name = name;
    f.abstract = abstract;
    f.field_class = fc;
    f.index = index;
    f.column_stored = columnStored;
    f.multi_valued = multiValued;
    if (stored >= 0) f.stored = stored != 0;
    if (tokenizer) {
      auto& a = f.analyzer.emplace();
      a.tokenizer = tokenizer;
      std::string_view* fl = solux::api::build::allocArray(a.filters, filters.size(), arena);
      for (size_t i = 0; i < filters.size(); i++) {
        fl[i] = filters[i];
      }
    }
    fields.push_back(f);
  };

  // Concrete fields
  addField("id", FieldClass::ID, false, IndexMode::MATCH, true);
  addField("_version_", FieldClass::INT, false, IndexMode::NONE, true);

  // Abstract dynamic suffix fields.  TEXT suffixes inherit the field_class
  // default (stored=true) so the raw value can be returned in search results.
  addField("_s", FieldClass::STRING, true, IndexMode::MATCH, true);
  addField("_sc", FieldClass::STRING, true, IndexMode::NONE, true);
  addField("_ss", FieldClass::STRING, true, IndexMode::MATCH, true, true);
  addField("_ssc", FieldClass::STRING, true, IndexMode::NONE, true, true);
  addField("_i", FieldClass::INT, true, IndexMode::NONE, true);
  addField("_is", FieldClass::INT, true, IndexMode::NONE, true, true);
  addField("_f", FieldClass::FLOAT, true, IndexMode::NONE, true);
  addField("_fs", FieldClass::FLOAT, true, IndexMode::NONE, true, true);
  addField("_d", FieldClass::DOUBLE, true, IndexMode::NONE, true);
  addField("_ds", FieldClass::DOUBLE, true, IndexMode::NONE, true, true);
  addField("_dt", FieldClass::DATE, true, IndexMode::NONE, true);
  addField("_dts", FieldClass::DATE, true, IndexMode::NONE, true, true);
  // Text suffixes, from raw to fully folded:
  //   _w  raw whitespace tokens (case- and accent-sensitive)
  //   _wl Unicode word segmentation + NFKC case folding, accents PRESERVED
  //       (the opt-out for accent-sensitive languages: Swedish a-ring, Spanish n-tilde, ...)
  //   _t  the general default: also folds accents, so cafe matches cafe-with-accent
  //       (US/adoption-centric; lossy for some languages - use _wl there)
  addField("_w", FieldClass::TEXT, true, IndexMode::MATCH, false, false, "whitespace");
  addField("_wl", FieldClass::TEXT, true, IndexMode::MATCH, false, false, "unicode_word", {"nfkc_cf"});
  addField("_t", FieldClass::TEXT, true, IndexMode::MATCH, false, false, "unicode_word", {"nfkc_cf", "fold"});
  // VECTOR suffixes: single-valued (_v) and multi-valued (_vs).  dims is left
  // unset on the abstract suffix; concrete fields may pin it via VectorParams.
  addField("_v", FieldClass::VECTOR, true, IndexMode::NONE, true);
  addField("_vs", FieldClass::VECTOR, true, IndexMode::NONE, true, true);

  // fromProto ensures the default "_stored_" resource is present.  Users can
  // override or add additional named resources (e.g. "_stored_paragraphs_")
  // by inserting entries in fieldTypeMap before using the schema.
  solux::api::SchemaDef def;
  def.fields = std::span<const solux::api::FieldDef>(fields.data(), fields.size());
  return fromProto(def);
}



} // namespace solux
