// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/ApiError.h"

#include "luxir/index/DocStream.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/NormsWriter.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/search/Similarity.h"
#include "luxir/util/TermValHash.h"

#include <algorithm>
#include <vector>

using namespace luxir;
namespace luxir::handler {

// Indexes a sparse_vector, rank_features or rank_feature field (see
// FeatureFieldType): each feature name becomes a term whose frequency is the
// encoded weight. No positions are written, so the postings carry no norms;
// a norm entry (the feature count) still records which docs have the field.
class FeatureHandler : public Inverter::IndexHandler {
  friend Inverter;

  struct Feature {
    std::string_view term;
    int32_t tf;
  };

  TermValHash<DocTfStream> termsHash;
  FeatureFieldType& featureType;
  Stream normBytes;
  DocStream normDocsWithField;
  int32_t numDocsWithField = 0;
  // Per-document scratch, validated before any stream is touched.
  std::vector<Feature> features;
  std::vector<PackedTerm::TermBuffer> scratch;

public:
  FeatureHandler(Inverter& inverter, const std::string_view& fieldName,
                 const std::shared_ptr<FieldType>& fieldType)
    : IndexHandler(PackedTerm(inverter.pool, fieldName), fieldType),
      termsHash(inverter.pool, 4),
      featureType(dynamic_cast<FeatureFieldType&>(*fieldType)),
      normDocsWithField(inverter.pool) {}

  ~FeatureHandler() override = default;

  void index(Inverter& inverter, std::string_view val) override {
    index(inverter, coerce::scalarVal(val));
  }

  void index(Inverter& inverter, const IndexVal& val) override {
    if (coerce::isNull(val)) return;
    features.clear();
    std::string_view name(fieldName);
    if (featureType.kind == FeatureFieldType::RANK_FEATURE) {
      if (std::holds_alternative<api::Map>(val.kind) || coerce::isArray(val)) {
        throw DocumentError(fmt::format(
            "Field '{}': a rank_feature value must be a single number", name));
      }
      features.push_back({FeatureFieldType::SINGLE_FEATURE, encode(val, name, name)});
    } else {
      const auto* map = std::get_if<api::Map>(&val.kind);
      if (map == nullptr) {
        throw DocumentError(fmt::format(
            "Field '{}': a {} value must be an object of feature name to weight",
            name, featureType.kindName()));
      }
      scratch.resize(map->fields.size());
      for (size_t i = 0; i < map->fields.size(); i++) {
        const auto& [key, weight] = map->fields[i];
        auto term = PackedTerm::fitTerm(featureType.longTerms, key, scratch[i]);
        if (!term || term->empty()) {
          throw DocumentError(fmt::format(
              "Field '{}': feature name must be 1 to {} bytes (got {})",
              name, PackedTerm::MAX_LEN, key.size()));
        }
        features.push_back({*term, encode(*weight, name, key)});
      }
      std::sort(features.begin(), features.end(),
                [](const Feature& a, const Feature& b) { return a.term < b.term; });
      for (size_t i = 1; i < features.size(); i++) {
        if (features[i].term == features[i - 1].term) {
          throw DocumentError(fmt::format(
              "Field '{}': feature '{}' appears more than once", name, features[i].term));
        }
      }
      if (features.empty()) return;
    }

    int docid = inverter.getDoc();
    for (const auto& feature : features) {
      auto [entry, inserted] = termsHash.try_emplace(
          feature.term, termsHash.getMemPool(), docid, feature.tf);
      if (!inserted) {
        entry->val().addDoc(termsHash.getMemPool(), docid, feature.tf);
      }
    }
    normBytes.writeByte(inverter.pool, SmallFloat::intToByte4((int32_t) features.size()));
    normDocsWithField.addDoc(inverter.pool, docid);
    numDocsWithField++;
    accountExtraRam(inverter, termsHash.memSize());
  }

  void flush(Inverter& inverter) override {
    if (numDocsWithField == 0) return;
    auto sz = termsHash.size();
    auto terms = sortForFlush(inverter, termsHash);

    PostingsWriter::IndexFieldInfo& fieldInfo = inverter.getPostingsWriter().addField(fieldName);
    fieldInfo.type = fieldType->type();
    fieldInfo.flags = fieldType->segmentFlags();

    auto preparedNorms = NormsWriter::prepare(inverter.pool, inverter.getPostingsWriter(),
                                              fieldInfo, normBytes, normDocsWithField,
                                              numDocsWithField);
    {
      TextWriter textWriter(inverter.getPostingsWriter());
      textWriter.startField(&fieldInfo);
      for (size_t tnum = 0; tnum < sz; tnum++) {
        auto term = terms[tnum];
        textWriter.startTerm(term);
        term.val().pushDocs(inverter.pool, textWriter);
        textWriter.endTerm(term);
      }
      textWriter.endField();
    }
    termsHash.free();

    NormsWriter::writeValues(inverter.pool, inverter.getPostingsWriter(), fieldInfo,
                             preparedNorms, normDocsWithField);
  }

private:
  static int32_t encode(const IndexVal& val, std::string_view field, std::string_view feature) {
    if (!std::holds_alternative<int64_t>(val.kind) && !std::holds_alternative<double>(val.kind)
        && !std::holds_alternative<float>(val.kind)
        && !std::holds_alternative<std::string_view>(val.kind)) {
      throw DocumentError(fmt::format(
          "Field '{}': the weight of '{}' must be a number", field, feature));
    }
    double weight = coerce::toDouble(val, field);
    if (!FeatureFieldType::validWeight(weight)) {
      throw DocumentError(fmt::format(
          "Field '{}': the weight of '{}' must be positive and finite (got {})",
          field, feature, weight));
    }
    return FeatureFieldType::encodeWeight((float) weight);
  }
};

}
