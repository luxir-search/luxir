#pragma once

#include <memory>
#include <string_view>

#include "solux/index/Inverter.h"
#include "solux/index/StoredFieldsWriter.h"

namespace solux::handler {

// Wraps an inner IndexHandler to additionally route raw field values to the
// segment's StoredFieldsWriter.  Used when a FieldType has the STORED flag set.
// Only instantiated for TEXT fields (see Inverter::createIndexHandler); numeric
// stored-field storage is not supported in v1.
//
// The wrapper captures string / binary values from solux::api::Val (and the
// equivalent direct string_view / span overloads) and forwards untouched to
// the inner handler so normal indexing still runs.
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
    } else if (std::holds_alternative<solux::api::ArrStr>(val.kind)) {
      const auto& arr = std::get<solux::api::ArrStr>(val.kind).v;
      writer_->addValues(doc, name, std::span<const std::string_view>(arr.data(), arr.size()));
    } else if (std::holds_alternative<solux::api::ArrBin>(val.kind)) {
      const auto& arr = std::get<solux::api::ArrBin>(val.kind).v;
      std::vector<std::string_view> views;
      views.reserve(arr.size());
      for (const auto& bin : arr) {
        views.push_back(std::string_view((const char*)bin.data(), bin.size()));
      }
      writer_->addValues(doc, name, std::span<const std::string_view>(views.data(), views.size()));
    }
    // Non-string value types (int/float/double) are not stored in v1.
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

  void flush(Inverter& inverter) override {
    inner_->flush(inverter);
  }

  IndexHandler* inner() { return inner_.get(); }

private:
  u_ptr<IndexHandler> inner_;
  StoredFieldsWriter* writer_;
};

}  // namespace solux::handler
