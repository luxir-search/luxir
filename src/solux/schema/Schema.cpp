#include "Schema.h"
#include "protos/solux_types.pb.h"

#include <boost/unordered/unordered_flat_set.hpp>
#include <stdexcept>
#include <vector>

namespace solux {

// Resolved state for a single FieldDef during fromProto processing
struct ResolvedField {
  bool abstract = false;
  bool hasFieldClass = false;
  proto::FieldDef::FieldClass fieldClass = proto::FieldDef::STRING;
  bool hasIndexed = false;
  bool indexed = false;
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
};

using sv_flat_map = boost::unordered_flat_map<std::string_view, const proto::FieldDef*, PackedTermHash, PackedTermEqual>;
using sv_resolved_map = boost::unordered_flat_map<std::string_view, ResolvedField, PackedTermHash, PackedTermEqual>;
using sv_flat_set = boost::unordered_flat_set<std::string_view, PackedTermHash, PackedTermEqual>;

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
  const proto::FieldDef& def = *defIt->second;

  // Resolve parent first if exists
  ResolvedField parentResolved{};
  if (!def.parent().empty()) {
    std::string_view parentName = def.parent();
    resolveField(parentName, defMap, resolved, visiting);
    parentResolved = resolved.find(parentName)->second;
  }

  ResolvedField r;
  r.abstract = def.abstract();  // NOT inherited

  // field_class: use this field's if set, else parent's
  if (def.has_field_class()) {
    r.hasFieldClass = true;
    r.fieldClass = def.field_class();
  } else {
    r.hasFieldClass = parentResolved.hasFieldClass;
    r.fieldClass = parentResolved.fieldClass;
  }

  // indexed
  if (def.has_indexed()) {
    r.hasIndexed = true;
    r.indexed = def.indexed();
  } else {
    r.hasIndexed = parentResolved.hasIndexed;
    r.indexed = parentResolved.indexed;
  }

  // column_stored
  if (def.has_column_stored()) {
    r.hasColumnStored = true;
    r.columnStored = def.column_stored();
  } else {
    r.hasColumnStored = parentResolved.hasColumnStored;
    r.columnStored = parentResolved.columnStored;
  }

  // multi_valued
  if (def.has_multi_valued()) {
    r.hasMultiValued = true;
    r.multiValued = def.multi_valued();
  } else {
    r.hasMultiValued = parentResolved.hasMultiValued;
    r.multiValued = parentResolved.multiValued;
  }

  // stored
  if (def.has_stored()) {
    r.hasStored = true;
    r.stored = def.stored();
  } else {
    r.hasStored = parentResolved.hasStored;
    r.stored = parentResolved.stored;
  }

  // stored_resource
  if (def.has_stored_resource() && !def.stored_resource().empty()) {
    r.hasStoredResource = true;
    r.storedResource = def.stored_resource();
  } else {
    r.hasStoredResource = parentResolved.hasStoredResource;
    r.storedResource = parentResolved.storedResource;
  }

  // analyzer: atomic — first non-empty AnalyzerDef in chain wins
  if (def.has_analyzer() && (!def.analyzer().tokenizer().empty() || def.analyzer().filters_size() > 0)) {
    r.hasAnalyzer = true;
    r.tokenizer = std::string(def.analyzer().tokenizer());
    for (int i = 0; i < def.analyzer().filters_size(); i++) {
      r.filters.push_back(std::string(def.analyzer().filters(i)));
    }
  } else {
    r.hasAnalyzer = parentResolved.hasAnalyzer;
    r.tokenizer = parentResolved.tokenizer;
    r.filters = parentResolved.filters;
  }

  visiting.erase(name);
  resolved[name] = std::move(r);
}


std::shared_ptr<Schema> Schema::fromProto(const proto::SchemaDef& def, const Schema* base) {
  auto schema = std::make_shared<Schema>();

  // Build the source def to persist. For MERGE mode, merge new fields into the base's source def.
  if (base && !base->sourceDef_.empty()) {
    // Parse the base's source def
    proto::SchemaDef baseSrc;
    baseSrc.ParseFromString(base->sourceDef_);

    // Build a set of field names from the new def for quick lookup
    sv_flat_set newFieldNames;
    for (int i = 0; i < def.fields_size(); i++) {
      newFieldNames.insert(def.fields(i).name());
    }

    // Start with base fields that are NOT being overridden
    proto::SchemaDef mergedSrc;
    for (int i = 0; i < baseSrc.fields_size(); i++) {
      if (!newFieldNames.contains(baseSrc.fields(i).name())) {
        *mergedSrc.add_fields() = baseSrc.fields(i);
      }
    }
    // Add all new fields (including overrides)
    for (int i = 0; i < def.fields_size(); i++) {
      *mergedSrc.add_fields() = def.fields(i);
    }
    schema->sourceDef_ = mergedSrc.SerializeAsString();
  } else {
    schema->sourceDef_ = def.SerializeAsString();
  }

  // If MERGE mode, copy base schema's fields
  if (base) {
    schema->fieldTypeMap = base->fieldTypeMap;
  }

  // Build a name->FieldDef map
  sv_flat_map defMap;
  for (int i = 0; i < def.fields_size(); i++) {
    defMap[def.fields(i).name()] = &def.fields(i);
  }

  // Resolve all fields
  sv_resolved_map resolved;
  sv_flat_set visiting;

  // Pre-populate resolved map with base schema fields so new fields can reference them as parents.
  // Fields being overridden by the new def are skipped — they'll be re-resolved from the SchemaDef.
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
        case FieldType::ID:     r.fieldClass = proto::FieldDef::ID; break;
        case FieldType::STRING: r.fieldClass = proto::FieldDef::STRING; break;
        case FieldType::TEXT:   r.fieldClass = proto::FieldDef::TEXT; break;
        case FieldType::INT:    r.fieldClass = proto::FieldDef::INT; break;
        case FieldType::FLOAT:  r.fieldClass = proto::FieldDef::FLOAT; break;
        case FieldType::DOUBLE: r.fieldClass = proto::FieldDef::DOUBLE; break;
        default:                r.fieldClass = proto::FieldDef::BIN; break;
      }
      r.hasIndexed = true;
      r.indexed = ft->indexed();
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
    bool indexed = r.indexed;
    bool columnStored = r.columnStored;
    bool multiValued = r.multiValued;

    if (!r.hasIndexed) {
      // defaults by field_class
      switch (r.fieldClass) {
        case proto::FieldDef::ID:     indexed = true; break;
        case proto::FieldDef::STRING: indexed = true; break;
        case proto::FieldDef::TEXT:   indexed = true; break;
        case proto::FieldDef::INT:    indexed = false; break;
        default: indexed = false; break;
      }
    }
    if (!r.hasColumnStored) {
      switch (r.fieldClass) {
        case proto::FieldDef::ID:     columnStored = true; break;
        case proto::FieldDef::STRING: columnStored = true; break;
        case proto::FieldDef::TEXT:   columnStored = false; break;
        case proto::FieldDef::INT:    columnStored = true; break;
        default: columnStored = true; break;
      }
    }
    if (!r.hasMultiValued) {
      multiValued = false;
    }

    // Build flags
    FieldType::flag_type flags = 0;
    if (indexed) {
      if (r.fieldClass == proto::FieldDef::TEXT) {
        flags |= FieldType::INDEX_DOCS_FREQS_POSITIONS;
      } else {
        flags |= FieldType::INDEX_DOCS;
      }
    }
    if (columnStored) flags |= FieldType::COLUMN_STORED;
    if (multiValued) flags |= FieldType::MULTI_VALUED;
    if (r.stored) flags |= FieldType::STORED;

    std::shared_ptr<FieldType> ft;

    switch (r.fieldClass) {
      case proto::FieldDef::ID:
        ft = std::make_shared<IdFieldType>(name, flags);
        break;
      case proto::FieldDef::STRING:
        ft = std::make_shared<StrFieldType>(name, flags);
        break;
      case proto::FieldDef::TEXT: {
        std::string tokenizer = r.hasAnalyzer ? r.tokenizer : "whitespace";
        if (tokenizer.empty()) tokenizer = "whitespace";
        ft = std::make_shared<TextFieldType>(name, flags, tokenizer, r.filters);
        break;
      }
      case proto::FieldDef::INT:
        ft = std::make_shared<IntFieldType>(name, flags);
        break;
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


void Schema::toProto(proto::SchemaDef* def) const {
  for (const auto& [name, ft] : fieldTypeMap) {
    // StoredFieldType entries describe per-segment stored-fields resources.
    // They're managed in-memory (fromProto re-adds the default "_stored_");
    // custom per-resource config doesn't round-trip through proto yet.
    if (dynamic_cast<const StoredFieldType*>(ft.get()) != nullptr) {
      continue;
    }
    auto* fieldDef = def->add_fields();
    fieldDef->set_name(name);
    fieldDef->set_abstract(ft->isAbstract());

    // Map FieldType::Type to FieldDef::FieldClass
    switch (ft->type()) {
      case FieldType::ID:
        fieldDef->set_field_class(proto::FieldDef::ID);
        break;
      case FieldType::STRING:
        fieldDef->set_field_class(proto::FieldDef::STRING);
        break;
      case FieldType::TEXT:
        fieldDef->set_field_class(proto::FieldDef::TEXT);
        break;
      case FieldType::INT:
        fieldDef->set_field_class(proto::FieldDef::INT);
        break;
      case FieldType::FLOAT:
        fieldDef->set_field_class(proto::FieldDef::FLOAT);
        break;
      case FieldType::DOUBLE:
        fieldDef->set_field_class(proto::FieldDef::DOUBLE);
        break;
      case FieldType::BIN:
        fieldDef->set_field_class(proto::FieldDef::BIN);
        break;
      default:
        break;
    }

    // Set flags
    if (ft->indexed()) {
      fieldDef->set_indexed(true);
    } else {
      fieldDef->set_indexed(false);
    }
    fieldDef->set_column_stored(ft->hasColumn());
    fieldDef->set_multi_valued(ft->multiValued());
    fieldDef->set_stored(ft->isStored());
    // Only emit stored_resource when it deviates from the default — keeps
    // the serialized schema clean for fields that use "_stored_".
    if (ft->storedResource_ != Postings::STORED_DEFAULT_RESOURCE) {
      fieldDef->set_stored_resource(ft->storedResource_);
    }

    // Serialize analyzer for TEXT fields
    if (ft->type() == FieldType::TEXT) {
      auto* textFt = (TextFieldType*)(ft.get());
      auto* analyzer = fieldDef->mutable_analyzer();
      analyzer->set_tokenizer(textFt->tokenizer_);
      for (const auto& filter : textFt->filters_) {
        analyzer->add_filters(filter);
      }
    }
  }
}


std::shared_ptr<Schema> Schema::createDefaultSchema() {
  proto::SchemaDef def;

  // Helper lambda to add a field
  auto addField = [&](const char* name, proto::FieldDef::FieldClass fc,
                      bool abstract, bool indexed, bool columnStored,
                      bool multiValued = false,
                      const char* tokenizer = nullptr,
                      std::vector<std::string> filters = {},
                      bool stored = false) {
    auto* f = def.add_fields();
    f->set_name(name);
    f->set_abstract(abstract);
    f->set_field_class(fc);
    f->set_indexed(indexed);
    f->set_column_stored(columnStored);
    f->set_multi_valued(multiValued);
    f->set_stored(stored);
    if (tokenizer) {
      auto* a = f->mutable_analyzer();
      a->set_tokenizer(tokenizer);
      for (auto& filter : filters) {
        a->add_filters(filter);
      }
    }
  };

  // Concrete fields
  addField("id", proto::FieldDef::ID, false, true, true);
  addField("_version_", proto::FieldDef::INT, false, false, true);

  // Abstract dynamic suffix fields
  addField("_s", proto::FieldDef::STRING, true, true, true);
  addField("_sc", proto::FieldDef::STRING, true, false, true);
  addField("_ss", proto::FieldDef::STRING, true, true, true, true);
  addField("_ssc", proto::FieldDef::STRING, true, false, true, true);
  addField("_i", proto::FieldDef::INT, true, false, true);
  addField("_is", proto::FieldDef::INT, true, false, true, true);
  addField("_w", proto::FieldDef::TEXT, true, true, false, false, "nocopy_whitespace");
  addField("_wl", proto::FieldDef::TEXT, true, true, false, false, "whitespace", {"lowercase"});
  // _t: indexed + stored text for full-text retrieval of original values.
  // Tokenized with whitespace+lowercase (same as _wl) for query-time matching,
  // and the raw value is kept in the default stored-fields resource so it can
  // be returned verbatim in search results.
  addField("_t", proto::FieldDef::TEXT, true, true, false, false, "whitespace", {"lowercase"}, /*stored=*/true);

  // fromProto ensures the default "_stored_" resource is present.  Users can
  // override or add additional named resources (e.g. "_stored_paragraphs_")
  // by inserting entries in fieldTypeMap before using the schema.
  return fromProto(def);
}


} // namespace solux
