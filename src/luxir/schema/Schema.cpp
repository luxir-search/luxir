// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Schema.h"
#include <algorithm>
#include <memory_resource>
#include "luxir/api/build.h"
#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_types.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace luxir {

using FieldClass = luxir::api::FieldDef_::FieldClass;
using IndexMode = luxir::api::FieldDef_::IndexMode;
using LongTerms = luxir::api::FieldDef_::LongTerms;
using VectorMetric = luxir::api::VectorMetric;

// One authored (source) entry: a name plus its FieldDef view and which map it
// came from.  Views are backed by the request / the decoded base source, both
// of which outlive fromProto.
struct SourceEntry {
  std::string_view name;
  luxir::api::FieldDef def;
  bool isTemplate = false;
};

// Resolved state for a single FieldDef during fromProto processing
struct ResolvedField {
  bool isTemplate = false;
  bool hasType = false;
  FieldClass type = FieldClass::STRING;
  bool hasIndex = false;
  IndexMode index = IndexMode::NONE;
  std::optional<LongTerms> longTerms;
  bool hasColumn = false;
  bool column = false;
  // The authored analyzer in force: the first non-empty AnalyzerDef in the
  // parent chain (a view into the source entries); null = the type default.
  const luxir::api::AnalyzerDef* analyzer = nullptr;
  const luxir::api::NormalizerDef* normalizer = nullptr;
  // 0 means "not set / infer from first indexed value"; > 0 means strict.
  int32_t vectorDims = 0;
  bool hasVectorDims = false;
  bool hasVectorMetric = false;
  VectorFieldType::Metric vectorMetric = VectorFieldType::METRIC_NONE;
  bool hasVectorNormalized = false;
  bool vectorNormalized = false;
  bool hasVectorNormalizeOnWrite = false;
  bool vectorNormalizeOnWrite = false;
};

struct ResolvedOwnership {
  bool hasMulti = false;
  bool multi = false;
  bool hasStored = false;
  bool stored = false;
  // storedResource: empty string means "inherit from parent or use the
  // default".  hasStoredResource tracks whether any field in the chain set
  // it explicitly (even to a value that equals the default).
  bool hasStoredResource = false;
  std::string storedResource;
  const api::FieldVariants* variants = nullptr;
  const api::FieldDefaults* defaults = nullptr;
};
using ownership_map = boost::unordered_flat_map<std::string_view, ResolvedOwnership,
                                                PackedTermHash, PackedTermEqual>;

using sv_entry_map =
  boost::unordered_flat_map<std::string_view, const SourceEntry*, PackedTermHash, PackedTermEqual>;
using sv_resolved_map = boost::unordered_flat_map<std::string_view, ResolvedField, PackedTermHash, PackedTermEqual>;
using sv_flat_set = boost::unordered_flat_set<std::string_view, PackedTermHash, PackedTermEqual>;

static bool isIdChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool Schema::validFieldName(std::string_view name) {
  // Field names land in filenames (block-bounds, vector overlays), so bound
  // them well under the 255-byte filesystem component limit.
  constexpr size_t kMaxFieldNameBytes = 127;
  if (name == "_version_") return true;
  if (name.empty() || name.size() > kMaxFieldNameBytes || name.contains("__")) return false;
  char c0 = name.front();
  if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) return false;
  for (char c : name) {
    if (!isIdChar(c)) return false;
  }
  return true;
}

bool Schema::validTemplateName(std::string_view name) {
  if (name.size() < 2 || name.front() != '_' || name.contains("__")) return false;
  for (char c : name.substr(1)) {
    if (!isIdChar(c)) return false;
  }
  return true;
}

static std::string foldName(std::string_view name) {
  std::string folded(name);
  for (char& c : folded) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  return folded;
}

bool Schema::validVariantLabel(std::string_view label) {
  if (label.empty() || label.contains("__") || foldName(label) == "self") return false;
  char c = label.front();
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
  return std::ranges::all_of(label, isIdChar);
}

static std::string serializeSchemaDef(const luxir::api::SchemaDef& def) {
  std::vector<std::byte> serialized;
  if (!luxir::api::encode(def, serialized)) {
    throw std::runtime_error("Failed to serialize SchemaDef protobuf");
  }
  return std::string((const char*)serialized.data(), serialized.size());
}

// Decode into `def`, whose non-owning views are backed by `arena`; the input bytes are
// copied there with the padding required by luxir::api::decode().
static void parseSchemaDef(std::string_view bytes, luxir::api::SchemaDef& def,
                           std::pmr::memory_resource& arena) {
  auto padded =
    luxir::api::copyToPaddedInput(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())), arena);
  if (!luxir::api::decode(def, padded, arena)) {
    throw std::runtime_error("Failed to parse SchemaDef protobuf");
  }
}

// An authored analyzer that says something: a tokenizer component (which must
// then name a tokenizer - params-only input is an error, not a silent default)
// or any filter.  {} inherits; filters-only replaces the whole parent analyzer
// and gets the whitespace tokenizer.
static bool analyzerSet(const luxir::api::FieldDef& def) {
  return def.analyzer.has_value() &&
         (def.analyzer->tokenizer.has_value() || !def.analyzer->filters.empty());
}

// Pass 1: physical settings only. Variant parent edges consult this completed
// graph, never the logical graph, so borrowing one's owner's settings is safe.
static ResolvedField physicalSettings(std::string_view name, const api::FieldDef& def,
                                      const ResolvedField& parentResolved) {
  ResolvedField r;

  // type: use this field's if set, else parent's
  if (def.type.has_value()) {
    r.hasType = true;
    r.type = *def.type;
  } else {
    r.hasType = parentResolved.hasType;
    r.type = parentResolved.type;
  }

  // index mode.  Enum reads accept bare integers (JSON) and unknown values
  // (gRPC), so range-check before the value can be persisted or drive flags.
  if (def.index.has_value()) {
    if (*def.index != IndexMode::NONE && *def.index != IndexMode::MATCH &&
        *def.index != IndexMode::RANGE) {
      throw SchemaError("unknown index mode value " + std::to_string((int)*def.index) +
                        " (field: " + std::string(name) + "); valid: none, match, range");
    }
    r.hasIndex = true;
    r.index = *def.index;
  } else {
    r.hasIndex = parentResolved.hasIndex;
    r.index = parentResolved.index;
  }

  r.longTerms = def.long_terms ? def.long_terms : parentResolved.longTerms;
  if (r.longTerms && *r.longTerms != LongTerms::TRUNCATE && *r.longTerms != LongTerms::REJECT
      && *r.longTerms != LongTerms::HASH128) {
    throw SchemaError("unknown long_terms value " + std::to_string((int)*r.longTerms) +
                      " (field: " + std::string(name) + "); valid: hash128, truncate, reject");
  }

  // column
  if (def.column.has_value()) {
    r.hasColumn = true;
    r.column = *def.column;
  } else {
    r.hasColumn = parentResolved.hasColumn;
    r.column = parentResolved.column;
  }

  // analyzer: atomic - first non-empty AnalyzerDef in chain wins
  r.analyzer = analyzerSet(def) ? &*def.analyzer : parentResolved.analyzer;
  r.normalizer = def.normalizer ? &*def.normalizer : parentResolved.normalizer;

  // dims: explicit set on this field overrides (0 = infer from first value);
  // otherwise inherit.
  if (def.dims.has_value()) {
    if (*def.dims < 0) {
      throw SchemaError("dims must be >= 0 (field: " + std::string(name) + ")");
    }
    r.hasVectorDims = true;
    r.vectorDims = *def.dims;
  } else {
    r.hasVectorDims = parentResolved.hasVectorDims;
    r.vectorDims = parentResolved.vectorDims;
  }

  // metric: explicit set on this field overrides (including an explicit
  // "none" = storage-only); otherwise inherit.  Range-checked like index.
  if (def.metric.has_value()) {
    if (*def.metric != VectorMetric::NONE && *def.metric != VectorMetric::L2 &&
        *def.metric != VectorMetric::IP && *def.metric != VectorMetric::COSINE) {
      throw SchemaError("unknown metric value " + std::to_string((int)*def.metric) +
                        " (field: " + std::string(name) + "); valid: none, l2, ip, cosine");
    }
    r.hasVectorMetric = true;
    r.vectorMetric = (VectorFieldType::Metric)*def.metric;
  } else {
    r.hasVectorMetric = parentResolved.hasVectorMetric;
    r.vectorMetric = parentResolved.vectorMetric;
  }

  // normalized: explicit set on this field overrides; otherwise inherit.
  if (def.normalized.has_value()) {
    r.hasVectorNormalized = true;
    r.vectorNormalized = *def.normalized;
  } else {
    r.hasVectorNormalized = parentResolved.hasVectorNormalized;
    r.vectorNormalized = parentResolved.vectorNormalized;
  }

  // normalize_on_write: explicit set on this field overrides; otherwise inherit.
  if (def.normalize_on_write.has_value()) {
    r.hasVectorNormalizeOnWrite = true;
    r.vectorNormalizeOnWrite = *def.normalize_on_write;
  } else {
    r.hasVectorNormalizeOnWrite = parentResolved.hasVectorNormalizeOnWrite;
    r.vectorNormalizeOnWrite = parentResolved.vectorNormalizeOnWrite;
  }

  return r;
}

static void resolveField(std::string_view name, const sv_entry_map& defMap,
                         sv_resolved_map& resolved, sv_flat_set& visiting) {
  if (resolved.contains(name)) return;
  if (!visiting.insert(name).second) {
    throw SchemaError("Circular schema inheritance detected involving field: " + std::string(name));
  }
  auto it = defMap.find(name);
  if (it == defMap.end()) {
    throw SchemaError("Parent must name an authored field or template: " + std::string(name));
  }
  const auto& entry = *it->second;
  ResolvedField parent;
  if (!entry.def.parent.empty()) {
    resolveField(entry.def.parent, defMap, resolved, visiting);
    parent = resolved.at(entry.def.parent);
  }
  auto r = physicalSettings(name, entry.def, parent);
  r.isTemplate = entry.isTemplate;
  resolved.emplace(name, std::move(r));
  visiting.erase(name);
}

// Pass 2: primary shape/storage and atomic variants/defaults inheritance.
static void resolveOwnership(std::string_view name, const sv_entry_map& defMap,
                             ownership_map& resolved) {
  if (resolved.contains(name)) return;
  const auto& def = defMap.at(name)->def;
  ResolvedOwnership parentResolved;
  if (!def.parent.empty()) {
    resolveOwnership(def.parent, defMap, resolved);
    parentResolved = resolved.at(def.parent);
  }
  ResolvedOwnership r;
  // multi
  if (def.multi.has_value()) {
    r.hasMulti = true;
    r.multi = *def.multi;
  } else {
    r.hasMulti = parentResolved.hasMulti;
    r.multi = parentResolved.multi;
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
  if (def.stored_resource && !def.stored_resource->empty()) {
    r.hasStoredResource = true;
    r.storedResource = *def.stored_resource;
  } else {
    r.hasStoredResource = parentResolved.hasStoredResource;
    r.storedResource = parentResolved.storedResource;
  }

  r.variants = def.variants ? &*def.variants : parentResolved.variants;
  r.defaults = def.defaults ? &*def.defaults : parentResolved.defaults;
  resolved.emplace(name, std::move(r));
}

// Direct (non-inherited) property/type consistency: catches an authored def
// that sets properties its resolved type cannot use.  Inherited values are
// exempt (a child that overrides a text parent's type does not have to fight
// the parent's analyzer).
static void validateDirectProps(const SourceEntry& entry, const ResolvedField& r) {
  const auto& def = entry.def;
  std::string name(entry.name);
  if (analyzerSet(def) && r.type != FieldClass::TEXT) {
    throw SchemaError("analyzer is only valid for text fields (field: " + name + ")");
  }
  if (def.normalizer && r.type != FieldClass::STRING) {
    throw SchemaError("normalizer is only valid for string fields (field: " + name + ")");
  }
  bool vectorProp = def.dims.has_value() || def.metric.has_value() ||
                    def.normalized.has_value() || def.normalize_on_write.has_value();
  if (vectorProp && r.type != FieldClass::VECTOR) {
    throw SchemaError(
      "dims/metric/normalized/normalize_on_write are only valid for vector fields (field: " + name + ")");
  }
}

// Reserved-field invariants.  "id" and "_version_" are operationally hard-coded
// (document-id extraction, delete-by-id, overwrite versioning), so their shapes
// are enforced rather than trusted.
static void validateReservedFields(const sv_resolved_map& resolved, const ownership_map& ownership) {
  for (const auto& [name, r] : resolved) {
    const auto& logical = ownership.at(name);
    if ((name == "id" || name == "_version_") && logical.variants && !logical.variants->entries.empty()) {
      throw SchemaError("reserved field cannot have variants: " + std::string(name));
    }
    if (name == "id") {
      if (r.isTemplate) throw SchemaError("reserved field 'id' cannot be a template");
      if (!r.hasType || r.type != FieldClass::ID) {
        throw SchemaError("reserved field 'id' must have type 'id'");
      }
      if (logical.hasMulti && logical.multi) throw SchemaError("reserved field 'id' cannot be multi-valued");
      if (r.hasIndex && r.index != IndexMode::MATCH) {
        throw SchemaError("reserved field 'id' must be match-indexed (overwrite and delete-by-id use it)");
      }
      if (r.hasColumn && !r.column) {
        throw SchemaError("reserved field 'id' requires column=true");
      }
    } else if (name == "_version_") {
      if (r.isTemplate) throw SchemaError("reserved field '_version_' cannot be a template");
      if (!r.hasType || r.type != FieldClass::INT) {
        throw SchemaError("reserved field '_version_' must have type 'int'");
      }
      if (logical.hasMulti && logical.multi) throw SchemaError("reserved field '_version_' cannot be multi-valued");
      if (r.hasColumn && !r.column) {
        throw SchemaError("reserved field '_version_' requires column=true");
      }
    } else if (r.hasType && r.type == FieldClass::ID) {
      throw SchemaError("type 'id' is reserved for the 'id' field (field: " + std::string(name) +
                        "); the engine keys overwrite/delete on 'id' alone");
    }
  }
}

// Pass 3 compiles each authored analysis definition once and creates physical
// bundles with the logical owner's shape. Storage can only belong to the primary.
struct FieldCompiler {
  boost::unordered_flat_map<const api::AnalyzerDef*, std::shared_ptr<const Analyzer>> analyzers;
  boost::unordered_flat_map<const api::NormalizerDef*, std::shared_ptr<const Analyzer>> normalizers;

  std::shared_ptr<FieldType> create(std::string_view name, const ResolvedField& r,
                                   const ResolvedOwnership& logical, bool derived = false) {
    if (!r.hasType) {
      throw SchemaError("Field '" + std::string(name) + "' has no type and no parent to inherit from");
    }

    // Apply defaults based on type if properties were not explicitly set
    IndexMode index = r.index;
    bool column = r.column;
    bool multi = logical.multi;
    bool stored = logical.stored;

    if (!r.hasIndex) {
      // defaults by type
      switch (r.type) {
        case FieldClass::ID:     index = IndexMode::MATCH; break;
        case FieldClass::STRING: index = IndexMode::MATCH; break;
        case FieldClass::TEXT:   index = IndexMode::MATCH; break;
        case FieldClass::INT:    index = IndexMode::NONE; break;
        default: index = IndexMode::NONE; break;
      }
    }

    if (!r.hasColumn) {
      switch (r.type) {
        case FieldClass::ID:     column = true; break;
        case FieldClass::STRING: column = true; break;
        case FieldClass::TEXT:   column = false; break;
        case FieldClass::INT:    column = true; break;
        default: column = true; break;
      }
    }

    if (r.type == FieldClass::TEXT && column) {
      throw SchemaError(
        "column=true is not supported for analyzed text fields; use string for a sortable/facetable lexical value "
        "(field: " + std::string(name) + ")");
    }

    if (r.longTerms) {
      if (r.type != FieldClass::STRING && r.type != FieldClass::TEXT && r.type != FieldClass::ID) {
        throw SchemaError("long_terms is only valid for string, text or id fields (field: " + std::string(name) + ")");
      }
      if (r.type == FieldClass::STRING && index == IndexMode::NONE) {
        throw SchemaError("long_terms is not valid for a string field with index=none; "
                          "there is no term space (field: " + std::string(name) + ")");
      }
    }

    // Reject index modes the engine cannot honor. RANGE covers 1-D numeric
    // ranges and 2-D GEO_POINT boxes; both require a column in this phase.
    bool numericClass = r.type == FieldClass::INT || r.type == FieldClass::FLOAT ||
                        r.type == FieldClass::DOUBLE || r.type == FieldClass::DATE;
    bool geoClass = r.type == FieldClass::GEO_POINT;
    if (index == IndexMode::RANGE) {
      if (!numericClass && !geoClass) {
        throw SchemaError(
          "index=range is not supported for this type (field: " + std::string(name) +
          "); MATCH-indexed string/id fields answer range queries through the terms dictionary");
      }
      if (!column) {
        throw SchemaError("index=range requires column=true for field: " + std::string(name));
      }
    }
    if (index == IndexMode::MATCH) {
      if (numericClass || geoClass) {
        throw SchemaError(
          "index=match (numeric term postings) is not yet implemented for field: " + std::string(name));
      }
      if (r.type == FieldClass::VECTOR || r.type == FieldClass::BIN) {
        throw SchemaError("index=match is not supported for this type (field: " + std::string(name) + ")");
      }
    }
    if (!logical.hasMulti) {
      multi = false;
    }
    if (!logical.hasStored) {
      // TEXT fields are stored by default so the raw (pre-analysis) value can
      // be returned in search results.  Other types default to false;
      // STRING/ID already expose their value via the column store.
      stored = (r.type == FieldClass::TEXT);
    }

    // Build flags
    FieldType::flag_type flags = 0;
    if (index == IndexMode::MATCH) {
      if (r.type == FieldClass::TEXT) {
        flags |= FieldType::INDEX_DOCS_FREQS_POSITIONS;
      } else {
        flags |= FieldType::INDEX_DOCS;
      }
    }
    if (index == IndexMode::RANGE) flags |= FieldType::INDEX_RANGE;
    if (column) flags |= FieldType::COLUMN_STORED;
    if (multi) flags |= FieldType::MULTI_VALUED;
    if (stored && !derived) flags |= FieldType::STORED;
    if (derived) flags |= FieldType::DERIVED;

    std::shared_ptr<FieldType> ft;

    switch (r.type) {
      case FieldClass::ID:
        ft = std::make_shared<IdFieldType>(name, flags);
        break;
      case FieldClass::STRING: {
        auto& normalizer = normalizers[r.normalizer];
        if (r.normalizer && !normalizer) {
          api::AnalyzerDef def;
          def.tokenizer.emplace().name = "keyword";
          def.filters = r.normalizer->filters;
          try {
            normalizer = Analyzer::compile(def);
            for (const auto& filter : normalizer->filters) {
              if (!filter->normalizing) {
                throw std::invalid_argument("filter '" + filter->name + "' cannot be used in a normalizer");
              }
            }
          } catch (const std::invalid_argument& e) {
            throw SchemaError(std::string(e.what()) + " (normalizer on field: " + std::string(name) + ")");
          }
        }
        ft = std::make_shared<StrFieldType>(name, flags, normalizer);
        break;
      }
      case FieldClass::TEXT: {
        auto& analyzer = analyzers[r.analyzer];
        if (!analyzer) {
          // The registry's teaching error (unknown component, unexpected
          // parameter) plus the field it was found on.
          try {
            analyzer = Analyzer::compile(r.analyzer ? *r.analyzer : luxir::api::AnalyzerDef{});
          } catch (const std::invalid_argument& e) {
            throw SchemaError(std::string(e.what()) + " (field: " + std::string(name) + ")");
          }
        }
        ft = std::make_shared<TextFieldType>(name, flags, analyzer);
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
      case FieldClass::GEO_POINT:
        ft = std::make_shared<GeoPointFieldType>(name, flags);
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
        throw SchemaError("Unsupported type for field: " + std::string(name));
    }

    switch (r.longTerms.value_or(LongTerms::HASH128)) {
      case LongTerms::HASH128: ft->longTerms = TermPolicy::HASH128; break;
      case LongTerms::TRUNCATE: ft->longTerms = TermPolicy::TRUNCATE; break;
      case LongTerms::REJECT: ft->longTerms = TermPolicy::REJECT; break;
    }
    if (r.isTemplate) ft->flags_ |= FieldType::ABSTRACT;
    // Apply any resolved storedResource_ override; empty means "keep default".
    if (!derived && !logical.storedResource.empty()) ft->storedResource_ = logical.storedResource;
    return ft;
  }
};

static LogicalField::Shape shapeFamily(FieldClass type) {
  if (type == FieldClass::VECTOR) return LogicalField::Shape::VECTOR;
  if (type == FieldClass::GEO_POINT) return LogicalField::Shape::GEO;
  return LogicalField::Shape::SCALAR;
}

static std::string binding(std::optional<std::string_view> label, const LogicalField& logical) {
  if (!label || foldName(*label) == "self") return "self";
  if (!logical.variants.contains(*label)) {
    throw SchemaError("dangling defaults label '" + std::string(*label) + "' on field: " + logical.name);
  }
  return std::string(*label);
}

std::shared_ptr<Schema> Schema::fromProto(const luxir::api::SchemaDef& def, const Schema* base) {
  auto schema = std::make_shared<Schema>();

  // ---- assemble the authored source entries (merge or replace) ----
  // All FieldDef views are backed by the request or by baseArena; both live
  // until the entries have been serialized AND resolved below.
  std::pmr::monotonic_buffer_resource baseArena;
  luxir::api::SchemaDef baseSrc;
  if (base != nullptr && !base->sourceDef_.empty()) {
    parseSchemaDef(base->sourceDef_, baseSrc, baseArena);
  }

  sv_flat_set newNames;
  for (const auto& [name, fd] : def.fields) newNames.insert(name);
  for (const auto& [name, fd] : def.templates) {
    if (!newNames.insert(name).second) {
      throw SchemaError("'" + std::string(name) + "' appears in both fields and templates");
    }
  }

  std::vector<SourceEntry> entries;
  entries.reserve(baseSrc.fields.size() + baseSrc.templates.size() +
                  def.fields.size() + def.templates.size() + 2);
  // Base entries not overridden by the new def (a new name replaces the base
  // entry wherever it lived, so a field can be re-declared as a template and
  // vice versa).
  for (const auto& [name, fd] : baseSrc.fields) {
    if (!newNames.contains(name)) entries.push_back({name, *fd, false});
  }
  for (const auto& [name, fd] : baseSrc.templates) {
    if (!newNames.contains(name)) entries.push_back({name, *fd, true});
  }
  for (const auto& [name, fd] : def.fields) entries.push_back({name, *fd, false});
  for (const auto& [name, fd] : def.templates) entries.push_back({name, *fd, true});

  // Materialize the reserved fields when absent: a REPLACE_ALL that omits them gets
  // working defaults instead of a silently broken collection.
  auto hasEntry = [&](std::string_view name) {
    return std::ranges::any_of(entries, [&](const SourceEntry& e) { return e.name == name; });
  };
  if (!hasEntry("id")) {
    SourceEntry e{"id", {}, false};
    e.def.type = FieldClass::ID;
    entries.push_back(e);
  }
  if (!hasEntry("_version_")) {
    SourceEntry e{"_version_", {}, false};
    e.def.type = FieldClass::INT;
    entries.push_back(e);
  }

  // Deterministic authored order: fields = id, _version_, then alpha; templates alpha.
  auto rank = [](const SourceEntry& e) -> int {
    if (e.isTemplate) return 3;
    if (e.name == "id") return 0;
    if (e.name == "_version_") return 1;
    return 2;
  };
  std::ranges::sort(entries, [&](const SourceEntry& a, const SourceEntry& b) {
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return a.name < b.name;
  });

  for (auto& e : entries) {
    // Empty wrappers retain presence; there is no backing array to copy/sort.
    if (!e.def.variants || e.def.variants->entries.empty()) continue;
    auto& map = e.def.variants->entries;
    using Pair = std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>;
    std::span<const Pair> sorted;
    auto* pairs = api::build::allocArray(sorted, map.size(), baseArena);
    std::copy(map.begin(), map.end(), pairs);
    std::sort(pairs, pairs + map.size(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    });
    map = api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(sorted);
  }

  // ---- serialize the authored source (this is what GET echoes and what persists) ----
  {
    std::vector<std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>> fieldPairs;
    std::vector<std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>> templatePairs;
    for (auto& e : entries) {
      (e.isTemplate ? templatePairs : fieldPairs).push_back({e.name, &e.def});
    }
    luxir::api::SchemaDef src;
    src.fields = luxir::api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(
      std::span<const std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>>(fieldPairs.data(), fieldPairs.size()));
    src.templates = luxir::api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(
      std::span<const std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>>(templatePairs.data(), templatePairs.size()));
    schema->sourceDef_ = serializeSchemaDef(src);
  }

  // ---- resolve the whole merged graph ----
  sv_entry_map defMap;
  defMap.reserve(entries.size());
  // folded(name) -> authored name: two names differing only by ASCII case are
  // almost certainly a typo, so reject at authoring time.
  boost::unordered_flat_map<std::string, std::string_view> folded;
  folded.reserve(entries.size());
  for (const auto& e : entries) {
    if (e.isTemplate) {
      if (!validTemplateName(e.name)) {
        throw SchemaError("invalid template name '" + std::string(e.name) +
                          "': must be '_' followed by ASCII letters, digits, or underscores");
      }
    } else if (!validFieldName(e.name)) {
      throw SchemaError("invalid field name '" + std::string(e.name) +
                        "': ASCII letters, digits, and underscores, starting with a letter");
    }
    if (!defMap.emplace(e.name, &e).second) {
      throw SchemaError("duplicate definition of '" + std::string(e.name) + "'");
    }
    auto [it, inserted] = folded.emplace(foldName(e.name), e.name);
    if (!inserted) {
      throw SchemaError("'" + std::string(e.name) + "' differs only by case from '" +
                        std::string(it->second) + "'");
    }
  }

  sv_resolved_map resolved;
  sv_flat_set visiting;
  for (const auto& e : entries) {
    resolveField(e.name, defMap, resolved, visiting);
    validateDirectProps(e, resolved.find(e.name)->second);
  }
  ownership_map ownership;
  for (const auto& e : entries) resolveOwnership(e.name, defMap, ownership);
  validateReservedFields(resolved, ownership);

  FieldCompiler compiler;
  for (const auto& e : entries) {
    const auto& r = resolved.at(e.name);
    const auto& owned = ownership.at(e.name);
    auto logical = std::make_unique<LogicalField>();
    logical->name = e.name;
    logical->primary = compiler.create(e.name, r, owned);
    logical->multi = logical->primary->multiValued();
    logical->shape = shapeFamily(r.type);
    if (owned.variants) {
      boost::unordered_flat_set<std::string> labels;
      for (const auto& [label, view] : owned.variants->entries) {
        std::string name = logical->name + "__" + std::string(label);
        if (!validVariantLabel(label)) {
          throw SchemaError("invalid variant label '" + std::string(label) + "' on field: " + logical->name);
        }
        if (!labels.insert(foldName(label)).second) {
          throw SchemaError("duplicate or case-colliding variant label: " + name);
        }
        if (label.size() > 124) throw SchemaError("variant label cannot fit in a physical name: " + name);
        logical->maxRootLength = std::min(logical->maxRootLength, 127 - 2 - label.size());
        if (!e.isTemplate && name.size() > 127) {
          throw SchemaError("generated physical field name exceeds 127 bytes: " + name);
        }
        const auto& def = *view;
        if (def.multi || def.stored || def.stored_resource) {
          throw SchemaError("multi/stored/stored_resource belong to the logical field, not variant: " + name);
        }
        if (def.variants || def.defaults) {
          throw SchemaError("variant cannot declare variants or defaults: " + name);
        }
        ResolvedField parent;
        if (!def.parent.empty()) {
          auto it = resolved.find(def.parent);
          if (it == resolved.end()) {
            throw SchemaError("Variant parent must name an authored field or template: " + std::string(def.parent));
          }
          parent = it->second;
        }
        auto branch = physicalSettings(name, def, parent);
        validateDirectProps({name, def, false}, branch);
        if (branch.type == FieldClass::ID) throw SchemaError("variant cannot have type id: " + name);
        if (shapeFamily(branch.type) != logical->shape) {
          throw SchemaError("variant must have the primary's shape family: " + name);
        }
        if (logical->shape == LogicalField::Shape::VECTOR &&
            (r.vectorDims <= 0 || branch.vectorDims != r.vectorDims)) {
          throw SchemaError("vector variants require the same positive dims as the primary: " + name);
        }
        branch.isTemplate = e.isTemplate;
        auto ft = compiler.create(name, branch, owned, true);
        logical->variants.emplace(label, std::move(ft));
      }
    }
    if (owned.defaults) {
      logical->search = binding(owned.defaults->search, *logical);
      logical->value = binding(owned.defaults->value, *logical);
    }
    // Abstract variant prototypes are kept only in their logical template.
    // Concrete physical entries are also exposed through the legacy registry.
    schema->fieldTypeMap.emplace(e.name, logical->primary);
    if (!e.isTemplate) {
      for (const auto& [label, ft] : logical->variants) schema->fieldTypeMap.emplace(ft->name(), ft);
    }
    schema->logicalFields.emplace(e.name, std::move(logical));
  }

  // Ensure the default stored-fields resource is available in every schema.
  // StoredFieldType entries are not part of the authored source def, so we
  // materialize the default unconditionally.  Users who have registered named
  // column families must re-add them programmatically.
  std::string defaultName(Postings::STORED_DEFAULT_RESOURCE);
  if (schema->fieldTypeMap.find(defaultName) == schema->fieldTypeMap.end()) {
    schema->fieldTypeMap[defaultName] = std::make_shared<StoredFieldType>(defaultName);
  }

  return schema;
}


Schema::RootField Schema::findRoot(std::string_view root) const {
  if (!validFieldName(root)) return {};
  auto entry = fieldTypeMap.find(root);
  if (entry != fieldTypeMap.end()) {
    if (entry->second->isAbstract() || entry->second->isDerived()) return {};
  } else {
    auto split = root.find_last_of('_');
    if (split == std::string_view::npos || split == 0) return {};
    entry = fieldTypeMap.find(root.substr(split));
    if (entry == fieldTypeMap.end() || !entry->second->isAbstract()) return {};
  }
  const LogicalField* owner = nullptr;
  auto logical = logicalFields.find(entry->first);
  // Tests can insert or replace FieldTypes directly without a logical definition.
  if (logical != logicalFields.end() && logical->second->primary == entry->second) {
    owner = logical->second.get();
    if (root.size() > owner->maxRootLength) {
      throw RequestError("Template instance root exceeds " + std::to_string(owner->maxRootLength) +
                         " bytes needed to fit its variants: " + std::string(root), "invalid_field_name");
    }
  }
  return {&entry->second, owner};
}

const std::shared_ptr<FieldType>* Schema::findPhysical(std::string_view name) const {
  auto exact = fieldTypeMap.find(name);
  if (exact != fieldTypeMap.end()) return exact->second->isAbstract() ? nullptr : &exact->second;
  // Generated names are resolved root-first. Never suffix-match their tails.
  auto split = name.rfind("__");
  if (split == std::string_view::npos) return findRoot(name).primary;
  auto label = name.substr(split + 2);
  if (!validVariantLabel(label)) return nullptr;
  auto root = findRoot(name.substr(0, split));
  if (!root.owner) return nullptr;
  auto variant = root.owner->variants.find(label);
  return variant == root.owner->variants.end() ? nullptr : &variant->second;
}

static ResolvedFieldHandle handle(std::string_view root, std::string_view label,
                                   FieldType& type, const LogicalField* owner) {
  std::string physicalName(root);
  if (label != "self") physicalName.append("__").append(label);
  return {std::string(root), std::move(physicalName), &type,
          label == "self" ? FieldRole::PRIMARY : FieldRole::VARIANT, owner};
}

ResolvedFieldHandle Schema::resolveInput(std::string_view docKey) const {
  if (!validFieldName(docKey)) {
    throw RequestError("Invalid logical document key (derived selectors are not input fields): " +
                       std::string(docKey), "invalid_field_name");
  }
  auto root = findRoot(docKey);
  if (!root.primary) throw RequestError("Field not found: " + std::string(docKey), "unknown_field");
  return handle(docKey, "self", **root.primary, root.owner);
}

ResolvedFieldHandle Schema::resolveFor(std::string_view name, OpClass op) const {
  auto split = name.rfind("__");
  auto rootName = split == std::string_view::npos ? name : name.substr(0, split);
  if (!validFieldName(rootName)) {
    throw RequestError("Invalid field or selector root: " + std::string(name), "invalid_field_name");
  }
  auto root = findRoot(rootName);
  if (!root.primary) throw RequestError("Field not found: " + std::string(rootName), "unknown_field");
  std::string_view label = "self";
  if (split != std::string_view::npos) {
    label = name.substr(split + 2);
    if (foldName(label) == "self") return handle(rootName, "self", **root.primary, root.owner);
    if (!validVariantLabel(label)) {
      throw RequestError("Invalid variant selector: " + std::string(name), "invalid_field_name");
    }
  } else if (root.owner && op == OpClass::SEARCH) {
    label = root.owner->search;
  } else if (root.owner && op == OpClass::VALUE) {
    label = root.owner->value;
  }
  if (label == "self") return handle(rootName, label, **root.primary, root.owner);
  if (root.owner) {
    auto variant = root.owner->variants.find(label);
    if (variant != root.owner->variants.end()) {
      return handle(rootName, label, *variant->second, root.owner);
    }
  }
  throw RequestError("Unknown variant label '" + std::string(label) + "' on field: " +
                     std::string(rootName), "unknown_field");
}

FieldType* Schema::physical(std::string_view name) const {
  auto* type = findPhysical(name);
  if (!type) throw RequestError("Physical field not found: " + std::string(name), "unknown_field");
  return type->get();
}

void Schema::toProto(luxir::api::SchemaDef* def, std::pmr::memory_resource& arena) const {
  // The authored source def IS the external representation; decode it into the
  // caller's arena.  fromProto already normalized order and materialized the
  // reserved fields, so this is deterministic and round-trippable as-is.
  if (sourceDef_.empty()) return;
  parseSchemaDef(sourceDef_, *def, arena);
}


std::shared_ptr<Schema> Schema::createDefaultSchema() {
  // Authored-source build: sparse defs that lean on the per-type defaults, so
  // the persisted/echoed schema reads the way a person would have written it.
  // All strings are literals; the pair vectors + arena outlive the fromProto call.
  std::pmr::monotonic_buffer_resource arena;
  using Pair = std::pair<std::string_view, hpp_proto::indirect_view<api::FieldDef>>;
  std::vector<Pair> fields;
  std::vector<Pair> templates;

  auto add = [&](std::vector<Pair>& out, const char* name, FieldClass fc) -> luxir::api::FieldDef& {
    auto* def = api::build::allocMessage<api::FieldDef>(arena);
    def->type = fc;
    out.push_back({name, def});
    return *def;
  };
  auto setAnalyzer = [&](luxir::api::FieldDef& f, const char* tokenizer,
                         std::vector<std::string_view> filters = {}) {
    auto& a = f.analyzer.emplace();
    a.tokenizer.emplace().name = tokenizer;
    auto* fl = luxir::api::build::allocArray(a.filters, filters.size(), arena);
    for (size_t i = 0; i < filters.size(); i++) fl[i].name = filters[i];
  };

  // Concrete fields (id defaults to MATCH+column, _version_ to NONE+column).
  add(fields, "id", FieldClass::ID);
  add(fields, "_version_", FieldClass::INT);

  // Dynamic suffix templates.  Type defaults cover index/column/stored; only
  // deviations are spelled out.  Multi-valued variants use the trailing "s".
  add(templates, "_s", FieldClass::STRING);
  add(templates, "_sc", FieldClass::STRING).index = IndexMode::NONE;
  add(templates, "_ss", FieldClass::STRING).multi = true;
  {
    auto& f = add(templates, "_ssc", FieldClass::STRING);
    f.index = IndexMode::NONE;
    f.multi = true;
  }
  add(templates, "_i", FieldClass::INT);
  add(templates, "_is", FieldClass::INT).multi = true;
  add(templates, "_f", FieldClass::FLOAT);
  add(templates, "_fs", FieldClass::FLOAT).multi = true;
  add(templates, "_d", FieldClass::DOUBLE);
  add(templates, "_ds", FieldClass::DOUBLE).multi = true;
  add(templates, "_dt", FieldClass::DATE);
  add(templates, "_dts", FieldClass::DATE).multi = true;
  // Text suffixes, from raw to folded and stemmed. In the whitespace
  // family a trailing `l` means "plus lowercase"; the unicode family uses `n`
  // for NFKC_CF ("normalize": casefold + compatibility forms + canonical
  // equivalence), because bare lowercase on unicode-segmented text leaves
  // equal-looking strings unequal (NFC vs NFD, final sigma) and is a trap.
  //   _w   raw whitespace tokens (case- and accent-sensitive)
  //   _wl  whitespace + Unicode lowercase: touch nothing but case (for
  //        identifier-ish or pre-normalized content)
  //   _u   Unicode word segmentation, raw (case- and accent-sensitive)
  //   _un  Unicode word segmentation + NFKC_CF, accents PRESERVED (the opt-out
  //        for accent-sensitive languages: Swedish a-ring, Spanish n-tilde, ...)
  //   _t   the general default: _un + accent fold + KStem English stemming.
  //        cafe matches cafe-with-accent; ponies matches pony. Non-ASCII tokens
  //        pass through KStem; use _un to preserve accents and skip stemming.
  setAnalyzer(add(templates, "_w", FieldClass::TEXT), "whitespace");
  setAnalyzer(add(templates, "_wl", FieldClass::TEXT), "whitespace", {"lowercase"});
  setAnalyzer(add(templates, "_u", FieldClass::TEXT), "unicode_word");
  setAnalyzer(add(templates, "_un", FieldClass::TEXT), "unicode_word", {"nfkc_cf"});
  setAnalyzer(add(templates, "_t", FieldClass::TEXT), "unicode_word", {"nfkc_cf", "fold", "kstem"});
  // VECTOR suffixes: single-valued (_v) and multi-valued (_vs).  dims is left
  // unset on the template; concrete fields may pin it.
  add(templates, "_v", FieldClass::VECTOR);
  add(templates, "_vs", FieldClass::VECTOR).multi = true;

  luxir::api::SchemaDef def;
  def.fields = luxir::api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(
    std::span<const Pair>(fields.data(), fields.size()));
  def.templates = luxir::api::map_view<std::string_view, hpp_proto::indirect_view<api::FieldDef>>(
    std::span<const Pair>(templates.data(), templates.size()));
  return fromProto(def);
}

} // namespace luxir
