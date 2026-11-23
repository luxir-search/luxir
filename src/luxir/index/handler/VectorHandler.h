// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/ApiError.h"

#include "StrColHandler.h"
#include "luxir/schema/FieldType.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/log.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace luxir::handler {

/// Dense-float-vector field handler. Typed Vector / ArrVector values and
/// schema-directed numeric Val arrays are copied to an owned f32 buffer,
/// validated (and normalized for cosine), encoded in the field's
/// VectorEncoding, then stored as fixed-size byte blobs through the
/// binary-column path inherited from StrColHandler. The column's fixed-size
/// path captures per-segment dims in the value stride (the encoded value
/// size); the encoding is recorded on the column descriptor.
///
/// dims_ tracks the stored vector size observed so far in this segment.  If
/// the owning VectorFieldType declares a non-zero dims, its stored dims seed
/// dims_ and any mismatched input is rejected.  With truncate_dims, an input
/// carries dims or truncate_dims components and is cut to its truncate_dims
/// prefix before normalization and storage.  Otherwise dims are learned from input, but
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
  VectorEncoding encoding = VectorEncoding::F32;
  // Raw cosine vectors in a non-f32 encoding: check the stored direction.
  bool checkStoredDirection = false;
  std::vector<float> floats;
  std::vector<int32_t> lens;
  std::vector<char> encoded;
  std::vector<float> decoded;
  std::vector<std::string_view> views;

public:
  VectorHandler(Inverter& inverter, const std::string_view& fieldName,
                const std::shared_ptr<FieldType>& fieldType, SpaceOrdinal space)
    : StrColHandler(inverter, fieldName, fieldType, space, ValueStorage::STREAMED) {
    // Caller (Inverter::createPhysicalHandler) only constructs us for VECTOR-typed
    // fields, which fromProto always builds as VectorFieldType.
    auto* vft = (VectorFieldType*)(fieldType.get());
    dims_ = vft->storedDims();
    metric = vft->metric_;
    trustNormalized = vft->normalized_;
    normalizeOnWrite = vft->normalizeOnWrite_;
    encoding = vft->encoding_;
    checkStoredDirection = metric == VectorFieldType::METRIC_COSINE && !trustNormalized
        && !normalizeOnWrite && encoding != VectorEncoding::F32;
  }

  int32_t dims() const { return dims_; }

protected:
  // Persist the valueRank -> docId map for multi-valued vector fields so the kNN
  // engine can group per-vector hits back to their owning document.
  bool writesOwners() const override { return true; }

  void describeColumn(field_parts::BytesColumn& column) const override {
    column.vectorEncoding = (uint8_t)encoding;
  }

public:
  void index(Inverter& inverter, const IndexVal& val) override {
    pendingDims_ = 0;  // dims learned by a previous (possibly failed) value don't carry over
    if (coerce::isNull(val)) return;

    bool multi = (fieldType->flags_ & FieldType::MULTI_VALUED) != 0;
    views.clear();
    bool isList = coerce::toVectors(val, std::string_view(fieldName), floats, lens);
    if (isList && !multi && !lens.empty()) {
      throw DocumentError(fmt::format(
          "field '{}': single-valued field received a list of {} vectors",
          std::string_view(fieldName), lens.size()));
    }
    if (lens.empty()) {
      if (!multi) {
        throw DocumentError(fmt::format("field '{}': empty vector",
                                             std::string_view(fieldName)));
      }
      return;
    }

    if (encoding != VectorEncoding::F32) {
      // Room for every vector of the value, so views into it stay valid.
      int32_t maxLen = *std::max_element(lens.begin(), lens.end());
      encoded.resize(lens.size() * (size_t)vcodec::bytesPerVector(encoding, maxLen));
    }
    size_t offset = 0;
    for (size_t i = 0; i < lens.size(); i++) {
      auto vec = std::span<float>(floats).subspan(offset, (size_t)lens[i]);
      offset += (size_t)lens[i];
      if (validate(vec, isList ? (int32_t)i : -1)) store(vec);
    }
    if (views.empty()) return;

    commitDims();
    if (multi) {
      appendMulti(inverter, std::span<const std::string_view>(views));
    } else {
      assert(views.size() == 1);
      appendSingle(inverter, views[0]);
    }
  }

private:
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
  // finiteness and dimensions, narrows vec to the truncate_dims prefix, and
  // applies write-time normalization in place.
  bool validate(std::span<float>& vec, int32_t ordinal) {
    auto prefix = [&]() { return ordinal < 0 ? std::string() : fmt::format("vector {}: ", ordinal); };
    if (vec.empty()) {
      throw DocumentError(fmt::format(
          "field '{}': {}empty vector", std::string_view(fieldName), prefix()));
    }
    // The field's input contract (length, and every element finite and within
    // the magnitude limit whatever arm it arrived in: a double past float32
    // range has narrowed to inf).
    const auto& vft = (const VectorFieldType&)*fieldType;
    if (std::string err = vft.inputError(vec); !err.empty()) {
      throw DocumentError(fmt::format("field '{}': {}{}", std::string_view(fieldName), prefix(), err));
    }
    vec = vec.first(vft.storedSize(vec.size()));
    // Without schema dims, the segment's first value sets them.
    int32_t n = (int32_t)vec.size();
    int32_t expected = effectiveDims();
    if (expected == 0) {
      pendingDims_ = n;
    } else if (n != expected) {
      throw DocumentError(fmt::format(
          "field '{}': {}expected dims={}, got {}",
          std::string_view(fieldName), prefix(), expected, n));
    }
    if (metric == VectorFieldType::METRIC_COSINE && !trustNormalized) {
      double sq = normSq(vec);
      if (!hasDirection(sq)) return false;
      if (normalizeOnWrite) {
        float invNorm = (float)(1.0 / std::sqrt(sq));
        for (float& x : vec) x *= invNorm;
      }
    }
    if (encoding == VectorEncoding::F16) {
      for (float x : vec) {
        if (std::fabs(x) > vcodec::F16_MAX) {
          throw DocumentError(fmt::format(
              "field '{}': {}element {} exceeds the f16 range (+-{})",
              std::string_view(fieldName), prefix(), x, vcodec::F16_MAX));
        }
      }
    }
    return true;
  }

  // Appends the view of an accepted vector as stored.  F32 views point at the
  // float buffer itself; other encodings encode into the next slot of
  // `encoded` (every vector of a value has the stored dims).  A raw cosine
  // vector checks the direction of its stored value, since tiny elements can
  // underflow to zero in f16, and is skipped like a zero vector without one.
  // A normalized vector cannot lose its direction to any encoding.
  void store(std::span<const float> vec) {
    if (encoding == VectorEncoding::F32) {
      views.push_back(bytesOf(vec));
      return;
    }
    int32_t n = (int32_t)vec.size();
    size_t valueBytes = (size_t)vcodec::bytesPerVector(encoding, n);
    char* out = encoded.data() + views.size() * valueBytes;
    vcodec::encode(encoding, vec.data(), n, out);
    if (checkStoredDirection) {
      decoded.resize((size_t)n);
      vcodec::decode(encoding, out, n, decoded.data());
      if (!hasDirection(normSq(decoded))) return;
    }
    views.push_back(std::string_view(out, valueBytes));
  }

  // Finite elements: the double sum of squares cannot overflow.
  static double normSq(std::span<const float> vec) {
    double sum = 0.0;
    for (float x : vec) sum += (double)x * (double)x;
    return sum;
  }

  // A cosine vector without a direction is skipped rather than aborting the
  // update.
  bool hasDirection(double sq) const {
    if (sq > VectorFieldType::MIN_COSINE_NORM_SQ) return true;
    LOG_WARN("VectorHandler: cosine field '{}' skipping zero / near-zero vector",
             std::string_view(fieldName));
    return false;
  }

  static std::string_view bytesOf(std::span<const float> vec) {
    return std::string_view((const char*)vec.data(), vec.size_bytes());
  }
};

} // namespace luxir::handler
