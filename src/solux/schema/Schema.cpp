#include "Schema.h"
#include "protos/solux_types.pb.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace solux {

// Resolved state for a single FieldDef during fromProto processing
struct ResolvedField {
  std::string name;
  bool abstract = false;
  bool hasFieldClass = false;
  proto::FieldDef::FieldClass fieldClass = proto::FieldDef::STRING;
  bool hasIndexed = false;
  bool indexed = false;
  bool hasColumnStored = false;
  bool columnStored = false;
  bool hasMultiValued = false;
  bool multiValued = false;
  bool hasAnalyzer = false;
  std::string tokenizer;
  std::vector<std::string> filters;
};

// Walk the parent chain and resolve all properties for a field.
static void resolveField(const std::string& name,
                         const std::unordered_map<std::string, const proto::FieldDef*>& defMap,
                         std::unordered_map<std::string, ResolvedField>& resolved,
                         std::unordered_set<std::string>& visiting) {
  if (resolved.contains(name)) return;

  if (visiting.contains(name)) {
    throw std::runtime_error("Circular schema inheritance detected involving field: " + name);
  }
  visiting.insert(name);

  auto defIt = defMap.find(name);
  if (defIt == defMap.end()) {
    throw std::runtime_error("Parent field not found in schema: " + name);
  }
  const proto::FieldDef& def = *defIt->second;

  // Resolve parent first if exists
  ResolvedField parentResolved{};
  if (!def.parent().empty()) {
    resolveField(std::string(def.parent()), defMap, resolved, visiting);
    parentResolved = resolved[std::string(def.parent())];
  }

  ResolvedField r;
  r.name = name;
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

  // If MERGE mode, copy base schema's fields
  if (base) {
    schema->fieldTypeMap = base->fieldTypeMap;
  }

  // Build a name->FieldDef map
  std::unordered_map<std::string, const proto::FieldDef*> defMap;
  for (int i = 0; i < def.fields_size(); i++) {
    defMap[std::string(def.fields(i).name())] = &def.fields(i);
  }

  // Resolve all fields
  std::unordered_map<std::string, ResolvedField> resolved;
  std::unordered_set<std::string> visiting;

  // Pre-populate resolved map with base schema fields so new fields can reference them as parents.
  // Fields being overridden by the new def are skipped — they'll be re-resolved from the SchemaDef.
  if (base) {
    for (const auto& [name, ft] : base->fieldTypeMap) {
      if (defMap.contains(name)) continue;
      ResolvedField r;
      r.name = name;
      r.abstract = ft->isAbstract();
      r.hasFieldClass = true;
      switch (ft->type()) {
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
      throw std::runtime_error("Field '" + name + "' has no field_class and no parent to inherit from");
    }

    // Apply defaults based on field_class if properties were not explicitly set
    bool indexed = r.indexed;
    bool columnStored = r.columnStored;
    bool multiValued = r.multiValued;

    if (!r.hasIndexed) {
      // defaults by field_class
      switch (r.fieldClass) {
        case proto::FieldDef::STRING: indexed = true; break;
        case proto::FieldDef::TEXT:   indexed = true; break;
        case proto::FieldDef::INT:    indexed = false; break;
        default: indexed = false; break;
      }
    }
    if (!r.hasColumnStored) {
      switch (r.fieldClass) {
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

    std::shared_ptr<FieldType> ft;

    switch (r.fieldClass) {
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
        throw std::runtime_error("Unsupported field_class for field: " + name);
    }

    if (r.abstract) ft->flags_ |= FieldType::ABSTRACT;
    schema->fieldTypeMap[name] = std::move(ft);
  }

  return schema;
}


void Schema::toProto(proto::SchemaDef* def) const {
  for (const auto& [name, ft] : fieldTypeMap) {
    auto* fieldDef = def->add_fields();
    fieldDef->set_name(name);
    fieldDef->set_abstract(ft->isAbstract());

    // Map FieldType::Type to FieldDef::FieldClass
    switch (ft->type()) {
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
                      std::vector<std::string> filters = {}) {
    auto* f = def.add_fields();
    f->set_name(name);
    f->set_abstract(abstract);
    f->set_field_class(fc);
    f->set_indexed(indexed);
    f->set_column_stored(columnStored);
    f->set_multi_valued(multiValued);
    if (tokenizer) {
      auto* a = f->mutable_analyzer();
      a->set_tokenizer(tokenizer);
      for (auto& filter : filters) {
        a->add_filters(filter);
      }
    }
  };

  // Concrete fields
  addField("id", proto::FieldDef::STRING, false, true, true);
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

  return fromProto(def);
}


} // namespace solux
