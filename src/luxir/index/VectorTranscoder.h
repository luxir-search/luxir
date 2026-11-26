// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "luxir/codec/VectorCodec.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/log.h"

namespace luxir {

/// The encoding policy for one vector field's column in a merge: the merge-side
/// counterpart of VectorHandler::describeColumn.  SegmentMerger asks it for the
/// output encoding and passes every value through it, without knowing what the
/// bytes mean.
///
/// The output is fixed at construction. It is the schema's current encoding
/// when all inputs can convert to it, otherwise the first input's encoding.
/// Merge admission keeps bit and float inputs apart (vcodec::convertible), so
/// inputs on both sides of that boundary are a logic error. Values of
/// an input already in the output encoding are copied verbatim.  Others are
/// decoded and re-encoded: f16 clamps elements to its range, counting them for
/// one warning per merged field, and cosine rows promoted to f32 in a field
/// that stores unit vectors are renormalized, since a decoded unit vector is
/// only approximately unit length.  bf16 and sq8 represent every accepted
/// element (at most VectorFieldType::MAX_ELEMENT_MAGNITUDE), so they never clamp.
class VectorTranscoder {
  std::string_view field;
  VectorEncoding output;
  bool renormalize = false;
  VectorEncoding input = VectorEncoding::F32;
  int32_t dims = 0;
  int64_t clamped = 0;
  std::vector<float> floats;
  std::vector<char> encoded;

public:
  /// inputs: the recorded encodings of the sources with a vector column.
  VectorTranscoder(std::string_view field, const FieldType* current, std::span<const VectorEncoding> inputs)
    : field(field), output(inputs.front()) {
    for (auto encoding : inputs) {
      if (!vcodec::convertible(encoding, output)) {
        throw std::logic_error(std::format(
            "vector field '{}': merge inputs mix bit and float encodings", field));
      }
    }
    if (auto* vtype = dynamic_cast<const VectorFieldType*>(current)) {
      // Convertibility partitions the encodings, so checking the first input suffices.
      if (vcodec::convertible(output, vtype->encoding())) {
        output = vtype->encoding();
      } else {
        LOG_WARN("VectorTranscoder: field '{}': keeping the inputs' {} encoding; a merge does not convert to {}",
                 field, vcodec::name(output), vcodec::name(vtype->encoding()));
      }
      renormalize = output == VectorEncoding::F32 && vtype->metric() == VectorFieldType::METRIC_COSINE
          && (vtype->normalizeOnWrite() || vtype->normalized());
    }
  }

  /// Starts the values of one input segment's column.
  void startInput(PostingsReader& reader, const DecodedFieldRecord& record) {
    VectorReader column(reader, record);
    input = column.encoding();
    dims = column.dims();
    if (input != output) {
      floats.resize((size_t)dims);
      encoded.resize((size_t)vcodec::bytesPerVector(output, dims));
    }
  }

  /// value in the output encoding; valid until the next call.
  std::string_view operator()(std::string_view value) {
    if (input == output) return value;
    vcodec::decode(input, value.data(), dims, floats.data());
    if (output == VectorEncoding::F16) {
      for (float& x : floats) {
        if (std::fabs(x) > vcodec::F16_MAX) {
          x = std::copysign(vcodec::F16_MAX, x);
          clamped++;
        }
      }
    } else if (renormalize) {
      double normSq = 0.0;
      for (float x : floats) normSq += (double)x * (double)x;
      if (normSq > VectorFieldType::MIN_COSINE_NORM_SQ) {
        float inv = (float)(1.0 / std::sqrt(normSq));
        for (float& x : floats) x *= inv;
      }
    }
    vcodec::encode(output, floats.data(), dims, encoded.data());
    return std::string_view(encoded.data(), encoded.size());
  }

  VectorEncoding outputEncoding() const { return output; }

  /// Elements clamped so far, and a way to fold in those of a copy that
  /// converted part of the values.
  int64_t clampedCount() const { return clamped; }
  void addClamped(int64_t count) { clamped += count; }

  /// Called once the field's values are written.
  void finish() const {
    if (clamped > 0) {
      LOG_WARN("VectorTranscoder: field '{}': clamped {} element(s) to the f16 range (+-{}) while merging",
               field, clamped, vcodec::F16_MAX);
    }
  }
};

} // namespace luxir
