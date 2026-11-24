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
/// The output is the schema's current encoding, so a field converges to its
/// declared encoding as its segments merge; when the schema no longer has a
/// vector type under the name, it is the first non-empty input's.  Values of
/// an input already in the output encoding are copied verbatim.  Others are
/// decoded and re-encoded: f16 clamps elements to its range, counting them for
/// one warning per merged field, and cosine rows promoted to f32 in a field
/// that stores unit vectors are renormalized, since a decoded unit vector is
/// only approximately unit length.  bf16 and sq8 represent every accepted
/// element (at most VectorFieldType::MAX_ELEMENT_MAGNITUDE), so they never clamp.
///
/// bit is not a precision of the float encodings: it stores signs, its dims
/// are a multiple of 8, and only the hamming metric scores it.  So a merge
/// never converts between bit and the others (vcodec::convertible), in either
/// direction.  A schema change of a declared field across that boundary is
/// rejected (IndexWriter::updateSchema), but a field declared again after its
/// removal, or one a template defines, can still meet older segments on the
/// other side: inputs that cannot convert to the schema's encoding keep their
/// own, and a merge whose inputs straddle the boundary fails before writing
/// anything (queries over such a field fail the same way; reindex it).
class VectorTranscoder {
  std::string_view field;
  bool hasOutput = false;
  VectorEncoding output = VectorEncoding::F32;
  // Cosine rows promoted to f32 in a field that stores unit vectors.
  bool renormalizeF32 = false;
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
      renormalizeF32 = vtype->metric() == VectorFieldType::METRIC_COSINE
          && (vtype->normalizeOnWrite() || vtype->normalized());
    }
  }

  /// Fixes the output encoding from the encodings of the inputs with values,
  /// before the first startInput: the schema's when every input converts to
  /// it, else the first input's when every input converts to that.  Throws
  /// when the inputs straddle the bit boundary.
  void chooseOutput(std::span<const VectorEncoding> inputs) {
    auto convertsTo = [&](VectorEncoding target) {
      for (auto e : inputs) {
        if (!vcodec::convertible(e, target)) return false;
      }
      return true;
    };
    if (!inputs.empty() && !(hasOutput && convertsTo(output))) {
      if (!convertsTo(inputs[0])) {
        throw std::runtime_error(std::format(
            "vector field '{}': cannot merge segments encoded as bit with segments in another encoding; "
            "reindex the field", field));
      }
      if (hasOutput) {
        LOG_WARN("VectorTranscoder: field '{}': keeping the inputs' {} encoding; a merge does not convert to {}",
                 field, vcodec::name(inputs[0]), vcodec::name(output));
      }
      output = inputs[0];
    }
    hasOutput = hasOutput || !inputs.empty();
    renormalize = renormalizeF32 && output == VectorEncoding::F32;
  }

  /// Starts the values of one input segment's column.
  void startInput(PostingsReader& reader, const DecodedFieldRecord& record) {
    VectorReader column(reader, record);
    input = column.encoding();
    dims = column.dims();
    // chooseOutput saw this input, so it converts.
    if (!hasOutput || !vcodec::convertible(input, output)) {
      throw std::logic_error(std::format("vector field '{}': input not seen by chooseOutput", field));
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
