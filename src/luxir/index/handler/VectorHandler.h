#pragma once

#include "StrColHandler.h"
#include "luxir/schema/FieldType.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/log.h"

#include <cassert>
#include <cmath>
#include <span>
#include <vector>

namespace luxir::handler {

/// Dense-float-vector field handler. Typed Vector / ArrVector values and
/// schema-directed numeric Val arrays are copied to an owned f32 buffer, then stored as
/// fixed-size byte blobs through the binary-column path inherited from
/// StrColHandler. The column's existing fixed-size fast path captures
/// per-segment dims automatically (mono2MetaOff = dims*4).
///
/// dims_ tracks the vector size observed so far in this segment.  If the
/// owning VectorFieldType declares a non-zero dims, it seeds dims_ and any
/// mismatched input is rejected.  Otherwise dims are learned from input, but
/// only committed to dims_ once a value is actually appended to the column:
/// a doc that fails during validation (and is rolled back / marked deleted)
/// must not constrain later docs, while an appended value makes the
/// per-segment fixed-size constraint real even if its doc is later deleted.
class VectorHandler final : public StrColHandler {
  int32_t dims_ = 0;          // 0 until first value appended (or set from schema)
  int32_t pendingDims_ = 0;   // dims learned from the current value, not yet appended
  VectorFieldType::Metric metric = VectorFieldType::METRIC_NONE;
  bool trustNormalized = false;
  bool normalizeOnWrite = false;
  std::vector<float> floats;
  std::vector<int32_t> lens;
  std::vector<std::string_view> views;

public:
  VectorHandler(Inverter& inverter, const std::string_view& fieldName,
                const std::shared_ptr<FieldType>& fieldType)
    : StrColHandler(inverter, fieldName, fieldType, ValueStorage::STREAMED) {
    // Caller (Inverter::createIndexHandler) only constructs us for VECTOR-typed
    // fields, which fromProto always builds as VectorFieldType.
    auto* vft = (VectorFieldType*)(fieldType.get());
    dims_ = vft->dims_;
    metric = vft->metric_;
    trustNormalized = vft->normalized_;
    normalizeOnWrite = vft->normalizeOnWrite_;
  }

  int32_t dims() const { return dims_; }

protected:
  // Persist the valueRank -> docId map for multi-valued vector fields so the kNN
  // engine can group per-vector hits back to their owning document.
  bool writesValueDocMap() const override { return true; }

public:
  void index(Inverter& inverter, const IndexVal& val) override {
    pendingDims_ = 0;  // dims learned by a previous (possibly failed) value don't carry over
    if (coerce::isNull(val)) return;

    bool multi = (fieldType->flags_ & FieldType::MULTI_VALUED) != 0;
    views.clear();
    bool isList = coerce::toVectors(val, std::string_view(fieldName), floats, lens);
    if (isList && !multi && !lens.empty()) {
      throw std::runtime_error(fmt::format(
          "field '{}': single-valued field received a list of {} vectors",
          std::string_view(fieldName), lens.size()));
    }
    if (lens.empty()) {
      if (!multi) {
        throw std::runtime_error(fmt::format("field '{}': empty vector",
                                             std::string_view(fieldName)));
      }
      return;
    }

    views.reserve(lens.size());
    size_t offset = 0;
    for (size_t i = 0; i < lens.size(); i++) {
      auto vec = std::span<float>(floats).subspan(offset, (size_t)lens[i]);
      offset += (size_t)lens[i];
      if (validate(vec, isList ? (int32_t)i : -1)) views.push_back(bytesOf(vec));
    }
    if (views.empty()) return;

    commitDims();
    if (multi) {
      indexMulti(inverter, std::span<const std::string_view>(views));
    } else {
      assert(views.size() == 1);
      indexSingle(inverter, views[0]);
    }
  }

private:
  static constexpr double MIN_COSINE_NORM_SQ = 1.0e-30;

  // The dims constraint to validate against: committed segment dims if any,
  // else dims learned earlier in the current value.
  int32_t effectiveDims() const {
    return dims_ != 0 ? dims_ : pendingDims_;
  }

  // Commit tentatively learned dims; called right before a value is appended.
  void commitDims() {
    if (dims_ == 0) {
      dims_ = pendingDims_;
    }
  }

  // Returns false for a zero / near-zero cosine vector. Otherwise validates
  // dimensions and applies write-time normalization in place.
  bool validate(std::span<float> vec, int32_t ordinal) {
    auto prefix = [&]() { return ordinal < 0 ? std::string() : fmt::format("vector {}: ", ordinal); };
    if (vec.empty()) {
      throw std::runtime_error(fmt::format(
          "field '{}': {}empty vector", std::string_view(fieldName), prefix()));
    }
    int32_t n = (int32_t)vec.size();
    int32_t expected = effectiveDims();
    if (expected == 0) {
      pendingDims_ = n;
    } else if (n != expected) {
      throw std::runtime_error(fmt::format(
          "field '{}': {}expected dims={}, got {}",
          std::string_view(fieldName), prefix(), expected, n));
    }
    if (metric == VectorFieldType::METRIC_COSINE && !trustNormalized) {
      double normSq = 0.0;
      for (float x : vec) {
        double d = (double)x;
        normSq += d * d;
      }
      if (!std::isfinite(normSq) || normSq <= MIN_COSINE_NORM_SQ) {
        // No cosine direction - skip this value rather than abort the update.
        LOG_WARN("VectorHandler: cosine field '{}' skipping zero / near-zero vector",
                 std::string_view(fieldName));
        return false;
      }
      if (normalizeOnWrite) {
        float invNorm = (float)(1.0 / std::sqrt(normSq));
        for (float& x : vec) x *= invNorm;
      }
    }
    return true;
  }

  static std::string_view bytesOf(std::span<const float> vec) {
    return std::string_view((const char*)vec.data(), vec.size_bytes());
  }
};

} // namespace luxir::handler
