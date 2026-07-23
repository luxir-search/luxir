#pragma once

#include "StrColHandler.h"
#include "solux/schema/FieldType.h"
#include "solux/util/log.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace solux::handler {

/// Dense-float-vector field handler.  Values arrive as solux::api::Vector
/// (single) or solux::api::ArrVector (multi-valued); we reinterpret each f32
/// vector as a fixed-size byte blob and delegate to the binary-column path
/// inherited from StrColHandler.  The column's existing fixed-size fast path
/// captures per-segment dims automatically (mono2MetaOff = dims*4).
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
  std::vector<float> normalizedScratch;

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
    bool multi = (fieldType->flags_ & FieldType::MULTI_VALUED) != 0;
    if (std::holds_alternative<solux::api::Vector>(val.kind)) {
      if (multi) {
        // A single Vector on a multi-valued field is treated as a one-element list.
        indexOne(inverter, std::get<solux::api::Vector>(val.kind));
      } else {
        indexSingleVec(inverter, std::get<solux::api::Vector>(val.kind));
      }
    } else if (std::holds_alternative<solux::api::ArrVector>(val.kind)) {
      if (!multi) {
        throw std::runtime_error(fmt::format(
            "VectorHandler: field '{}' is single-valued but received arr_vec",
            std::string_view(fieldName)));
      }
      indexMultiVec(inverter, std::get<solux::api::ArrVector>(val.kind));
    } else {
      throw std::runtime_error("VectorHandler: expected vec or arr_vec");
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

  // Validate a Vector against the effective dims, learning pendingDims_ on the
  // first vector of a value.  Returns the f32 floats, or nullptr if the value
  // should be skipped: a cosine field's zero / near-zero vector has no
  // direction, so we drop it (and log) rather than fail the whole update.
  // Throws on hard errors (bad encoding, empty vector, dims mismatch).
  const solux::api::ArrFloat* validate(const solux::api::Vector& vec, double* normSq = nullptr) {
    if (!vec.f32.has_value()) {
      throw std::runtime_error(fmt::format(
          "VectorHandler: field '{}' got Vector with unsupported / unset encoding",
          std::string_view(fieldName)));
    }
    auto& f = (*vec.f32);
    int32_t n = f.v.size();
    if (n == 0) {
      throw std::runtime_error(fmt::format(
          "VectorHandler: empty vector in field '{}'", std::string_view(fieldName)));
    }
    int32_t expected = effectiveDims();
    if (expected == 0) {
      pendingDims_ = n;
    } else if (n != expected) {
      throw std::runtime_error(fmt::format(
          "VectorHandler: field '{}' expects dims={}, got {}",
          std::string_view(fieldName), expected, n));
    }
    if (metric == VectorFieldType::METRIC_COSINE && !trustNormalized) {
      double sum = 0.0;
      for (float x : f.v) {
        double d = (double)x;
        sum += d * d;
      }
      if (!std::isfinite(sum) || sum <= MIN_COSINE_NORM_SQ) {
        // No cosine direction - skip this value rather than abort the update.
        LOG_WARN("VectorHandler: cosine field '{}' skipping zero / near-zero vector",
                 std::string_view(fieldName));
        return nullptr;
      }
      if (normSq != nullptr) *normSq = sum;
    }
    return &f;
  }

  static std::string_view bytesOf(const solux::api::ArrFloat& vec) {
    return std::string_view((const char*)vec.v.data(),
                            (size_t)vec.v.size() * sizeof(float));
  }

  static std::string_view bytesOf(const float* data, int32_t dims) {
    return std::string_view((const char*)data, (size_t)dims * sizeof(float));
  }

  std::string_view storedBytes(const solux::api::ArrFloat& vec,
                               double normSq,
                               std::vector<float>& scratch) const {
    if (!normalizeOnWrite) return bytesOf(vec);

    size_t start = scratch.size();
    scratch.resize(start + (size_t)vec.v.size());
    float invNorm = (float)(1.0 / std::sqrt(normSq));
    for (int32_t i = 0; i < (int32_t)vec.v.size(); i++) {
      scratch[start + (size_t)i] = vec.v[i] * invNorm;
    }
    return bytesOf(scratch.data() + start, vec.v.size());
  }

  void indexSingleVec(Inverter& inverter, const solux::api::Vector& vec) {
    double normSq = 0.0;
    auto* f = validate(vec, &normSq);
    if (f == nullptr) return;  // skipped: doc gets no value for this field
    commitDims();
    normalizedScratch.clear();
    if (normalizeOnWrite) normalizedScratch.reserve((size_t)f->v.size());
    indexSingle(inverter, storedBytes(*f, normSq, normalizedScratch));
  }

  void indexOne(Inverter& inverter, const solux::api::Vector& vec) {
    double normSq = 0.0;
    auto* f = validate(vec, &normSq);
    if (f == nullptr) return;  // skipped: doc gets no value for this field
    commitDims();
    normalizedScratch.clear();
    if (normalizeOnWrite) normalizedScratch.reserve((size_t)f->v.size());
    std::string_view views[] = { storedBytes(*f, normSq, normalizedScratch) };
    indexMulti(inverter, std::span<const std::string_view>(views));
  }

  void indexMultiVec(Inverter& inverter, const solux::api::ArrVector& arr) {
    auto& vecs = arr.v;
    // Empty list = "no value for this doc" (indistinguishable from field
    // unset); skip without recording the doc in docsWithValue.
    if (vecs.empty()) return;
    // Validate first so dims_ is locked before we capture byte views.
    std::vector<const solux::api::ArrFloat*> floats;
    std::vector<double> normSq;
    floats.reserve(vecs.size());
    normSq.reserve(vecs.size());
    for (auto& v : vecs) {
      double sq = 0.0;
      auto* f = validate(v, &sq);
      if (f == nullptr) continue;  // skip zero / near-zero cosine vector
      floats.push_back(f);
      normSq.push_back(sq);
    }
    // All values skipped => doc has no vector value (like an empty list).
    if (floats.empty()) return;
    commitDims();

    std::vector<std::string_view> views;
    views.reserve(floats.size());
    normalizedScratch.clear();
    if (normalizeOnWrite) {
      // storedBytes returns views into normalizedScratch; reserve the full
      // batch before appending so earlier views cannot be invalidated.
      normalizedScratch.reserve((size_t)dims_ * floats.size());
    }
    for (size_t i = 0; i < floats.size(); i++) {
      views.push_back(storedBytes(*floats[i], normSq[i], normalizedScratch));
    }
    assert(!normalizeOnWrite || normalizedScratch.size() == (size_t)dims_ * floats.size());
    indexMulti(inverter, std::span<const std::string_view>(views));
  }
};

} // namespace solux::handler
