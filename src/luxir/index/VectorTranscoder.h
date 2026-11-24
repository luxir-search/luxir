// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>
#include <cstdint>
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
/// The output is the schema's current encoding, so a field converges to its
/// declared encoding as its segments merge; when the schema no longer has a
/// vector type under the name, it is the first non-empty input's.  Values of
/// an input already in the output encoding are copied verbatim.  Others are
/// decoded and re-encoded: f16 clamps elements to its range, counting them for
/// one warning per merged field, and cosine rows promoted to f32 in a field
/// that stores unit vectors are renormalized, since a decoded unit vector is
/// only approximately unit length.  bf16 and sq8 represent every accepted
/// element (at most VectorFieldType::MAX_ELEMENT_MAGNITUDE), so they never clamp.
class VectorTranscoder {
  std::string_view field;
  bool hasOutput = false;
  VectorEncoding output = VectorEncoding::F32;
  bool renormalize = false;
  VectorEncoding input = VectorEncoding::F32;
  int32_t dims = 0;
  int64_t clamped = 0;
  std::vector<float> floats;
  std::vector<char> encoded;

public:
  /// current: the schema's type for the field now, or null.
  VectorTranscoder(std::string_view field, const FieldType* current) : field(field) {
    if (auto* vtype = dynamic_cast<const VectorFieldType*>(current)) {
      hasOutput = true;
      output = vtype->encoding();
      renormalize = output == VectorEncoding::F32 && vtype->metric() == VectorFieldType::METRIC_COSINE
          && (vtype->normalizeOnWrite() || vtype->normalized());
    }
  }

  /// Starts the values of one input segment's column.
  void startInput(PostingsReader& reader, const DecodedFieldRecord& record) {
    VectorReader column(reader, record);
    input = column.encoding();
    dims = column.dims();
    if (!hasOutput) {
      hasOutput = true;
      output = input;
    }
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

  /// Called once the field's values are written.
  void finish() const {
    if (clamped > 0) {
      LOG_WARN("VectorTranscoder: field '{}': clamped {} element(s) to the f16 range (+-{}) while merging",
               field, clamped, vcodec::F16_MAX);
    }
  }
};

} // namespace luxir
