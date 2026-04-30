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
// The wrapper captures string / binary values from proto::Val (and the
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

  void index(Inverter& inverter, const proto::Val& val) override {
    int32_t doc = inverter.getDoc();
    std::string_view name(fieldName);
    if (val.has_s()) {
      writer_->addValue(doc, name, val.s());
    } else if (val.has_bin()) {
      writer_->addValue(doc, name, val.bin());
    } else if (val.has_arr_s()) {
      const auto& arr = val.arr_s().v();
      std::span<const std::string* const> vs(arr.data(), arr.size());
      writer_->addValues(doc, name, vs);
    } else if (val.has_arr_bin()) {
      const auto& arr = val.arr_bin().v();
      std::span<const std::string* const> vs(arr.data(), arr.size());
      writer_->addValues(doc, name, vs);
    }
    // Non-string value types (int/float/double) are not stored in v1.
    inner_->index(inverter, val);
  }

  void index(Inverter& inverter, std::string_view val) override {
    writer_->addValue(inverter.getDoc(), std::string_view(fieldName), val);
    inner_->index(inverter, val);
  }

  void index(Inverter& inverter, std::span<std::string_view> vals) override {
    std::span<const std::string_view> cvals(vals.data(), vals.size());
    writer_->addValues(inverter.getDoc(), std::string_view(fieldName), cvals);
    inner_->index(inverter, vals);
  }

  void index(Inverter& inverter, std::span<const std::string* const> vals) override {
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
