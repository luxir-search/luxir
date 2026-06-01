#pragma once

#include "StrColHandler.h"
#include "solux/schema/FieldType.h"

namespace solux::handler {

/// Dense-float-vector field handler.  Values arrive as proto::Vector
/// (single) or proto::ArrVector (multi-valued); we reinterpret each f32
/// vector as a fixed-size byte blob and delegate to the binary-column path
/// inherited from StrColHandler.  The column's existing fixed-size fast path
/// captures per-segment dims automatically (mono2MetaOff = dims*4).
///
/// dims_ tracks the vector size observed so far in this segment.  If the
/// owning VectorFieldType declares a non-zero dims, it seeds dims_ and any
/// mismatched input is rejected.  Otherwise the first indexed value sets
/// dims_ and subsequent values must match.
class VectorHandler final : public StrColHandler {
  int32_t dims_ = 0;          // 0 until first value seen (or set from schema)

public:
  VectorHandler(Inverter& inverter, const std::string_view& fieldName,
                const std::shared_ptr<FieldType>& fieldType)
    : StrColHandler(inverter, fieldName, fieldType) {
    // Caller (Inverter::createIndexHandler) only constructs us for VECTOR-typed
    // fields, which fromProto always builds as VectorFieldType.
    auto* vft = (VectorFieldType*)(fieldType.get());
    dims_ = vft->dims_;
  }

  int32_t dims() const { return dims_; }

protected:
  // Persist the valueRank -> docId map for multi-valued vector fields so the kNN
  // engine can group per-vector hits back to their owning document.
  bool writesValueDocMap() const override { return true; }

public:
  void index(Inverter& inverter, const proto::Val& val) override {
    bool multi = (fieldType->flags_ & FieldType::MULTI_VALUED) != 0;
    if (val.has_vec()) {
      if (multi) {
        // A single Vector on a multi-valued field is treated as a one-element list.
        indexOne(inverter, val.vec());
      } else {
        indexSingleVec(inverter, val.vec());
      }
    } else if (val.has_arr_vec()) {
      if (!multi) {
        throw std::runtime_error(fmt::format(
            "VectorHandler: field '{}' is single-valued but received arr_vec",
            std::string_view(fieldName)));
      }
      indexMultiVec(inverter, val.arr_vec());
    } else {
      throw std::runtime_error("VectorHandler: expected vec or arr_vec, got " + val.DebugString());
    }
  }

private:
  // Validate a Vector against current dims_, learning dims_ on first call.
  // Returns the f32 floats; throws if the Vector uses an unsupported encoding.
  const proto::ArrFloat& validate(const proto::Vector& vec) {
    if (!vec.has_f32()) {
      throw std::runtime_error(fmt::format(
          "VectorHandler: field '{}' got Vector with unsupported / unset encoding",
          std::string_view(fieldName)));
    }
    auto& f = vec.f32();
    int32_t n = f.v_size();
    if (n == 0) {
      throw std::runtime_error(fmt::format(
          "VectorHandler: empty vector in field '{}'", std::string_view(fieldName)));
    }
    if (dims_ == 0) {
      dims_ = n;
    } else if (n != dims_) {
      throw std::runtime_error(fmt::format(
          "VectorHandler: field '{}' expects dims={}, got {}",
          std::string_view(fieldName), dims_, n));
    }
    return f;
  }

  static std::string_view bytesOf(const proto::ArrFloat& vec) {
    return std::string_view((const char*)vec.v().data(),
                            (size_t)vec.v_size() * sizeof(float));
  }

  void indexSingleVec(Inverter& inverter, const proto::Vector& vec) {
    auto& f = validate(vec);
    indexSingle(inverter, bytesOf(f));
  }

  void indexOne(Inverter& inverter, const proto::Vector& vec) {
    auto& f = validate(vec);
    std::string_view views[] = { bytesOf(f) };
    indexMulti(inverter, std::span<std::string_view>(views));
  }

  void indexMultiVec(Inverter& inverter, const proto::ArrVector& arr) {
    auto& vecs = arr.v();
    // Empty list = "no value for this doc" (indistinguishable from field
    // unset); skip without recording the doc in docsWithValue.
    if (vecs.empty()) return;
    // Validate first so dims_ is locked before we capture byte views.
    std::vector<std::string_view> views;
    views.reserve(vecs.size());
    for (auto& v : vecs) views.push_back(bytesOf(validate(v)));
    indexMulti(inverter, std::span<std::string_view>(views));
  }
};

} // namespace solux::handler
