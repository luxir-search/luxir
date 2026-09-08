// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/index/Inverter.h"
#include "luxir/index/StoredFieldsWriter.h"
#include "luxir/schema/ValCoerce.h"

namespace luxir::handler {

// Wraps an inner IndexHandler to additionally route field values to the
// segment's StoredFieldsWriter.  Used when a TEXT/STRING/ID FieldType has the
// STORED flag set (see Inverter::createIndexHandler).
//
// The wrapper captures string / binary values from luxir::api::Val (and the
// equivalent direct string_view / span overloads); other value kinds store
// the same canonical rendering the inner handler indexes (coerceTerm), so
// what a search matches and what retrieval returns agree.  The value is
// always forwarded untouched to the inner handler so normal indexing runs.
class StoredFieldWrapperHandler final : public Inverter::IndexHandler {
public:
  StoredFieldWrapperHandler(Inverter& inverter, std::string_view name,
                            const std::shared_ptr<FieldType>& fieldType,
                            u_ptr<IndexHandler> inner,
                            StoredFieldsWriter* writer)
    : IndexHandler(PackedTerm(inverter.pool, name), fieldType),
      inner_(std::move(inner)),
      writer_(writer)
  {
    assert(writer_ != nullptr);
  }

  ~StoredFieldWrapperHandler() override = default;

  void index(Inverter& inverter, const IndexVal& val) override {
    int32_t doc = inverter.getDoc();
    std::string_view name(fieldName);
    if (std::holds_alternative<std::string_view>(val.kind)) {
      writer_->addValue(doc, name, std::get<std::string_view>(val.kind));
    } else if (std::holds_alternative<::hpp_proto::bytes_view>(val.kind)) {
      const auto& b = std::get<::hpp_proto::bytes_view>(val.kind);
      writer_->addValue(doc, name, std::string_view((const char*)b.data(), b.size()));
    } else if (std::holds_alternative<luxir::api::ArrStr>(val.kind)) {
      const auto& arr = std::get<luxir::api::ArrStr>(val.kind).v;
      writer_->addValues(doc, name, std::span<const std::string_view>(arr.data(), arr.size()));
    } else if (std::holds_alternative<luxir::api::ArrBin>(val.kind)) {
      const auto& arr = std::get<luxir::api::ArrBin>(val.kind).v;
      std::vector<std::string_view> views;
      views.reserve(arr.size());
      for (const auto& bin : arr) {
        views.push_back(std::string_view((const char*)bin.data(), bin.size()));
      }
      writer_->addValues(doc, name, std::span<const std::string_view>(views.data(), views.size()));
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
      writer_->addValues(doc, name, std::span<const std::string_view>(views.data(), views.size()));
    } else {
      // numeric / bool scalars: store the canonical rendering, the same
      // bytes the inner handler indexes
      char buf[coerce::TEXT_BUF_SIZE];
      writer_->addValue(doc, name, fieldType->coerceTerm(val, name, buf));
    }
    inner_->index(inverter, val);
  }

  void index(Inverter& inverter, std::string_view val) override {
    writer_->addValue(inverter.getDoc(), std::string_view(fieldName), val);
    inner_->index(inverter, val);
  }

  void index(Inverter& inverter, std::span<const std::string_view> vals) override {
    writer_->addValues(inverter.getDoc(), std::string_view(fieldName), vals);
    inner_->index(inverter, vals);
  }

  void finishIndexing(Inverter& inverter) override {
    inner_->finishIndexing(inverter);
  }

  void flush(Inverter& inverter) override {
    inner_->flush(inverter);
  }

  IndexHandler* inner() { return inner_.get(); }

private:
  u_ptr<IndexHandler> inner_;
  StoredFieldsWriter* writer_;
};

}  // namespace luxir::handler
