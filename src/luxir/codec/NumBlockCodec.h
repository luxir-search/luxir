// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/codec/NumBlockPlanner.h"
#include "luxir/codec/NumColumn.h"

namespace luxir {

class NumBlockCodec {
public:
  // `payload` holds the payload of a non-FOR plan (NumBlockPlanner); a FOR
  // or raw plan needs none.
  static void write(OutputStream& out, size_t origin, std::span<const int64_t> values,
                    NumColumnFormat::BlockPlan& plan, std::span<const uint64_t> quotients,
                    NumBlockPlanner::Payload* payload = nullptr) {
    if (plan.raw()) out.align(MAX_ALIGN);
    plan.info.setPayload(out.size() - origin, plan.bits, plan.kind);
    if (plan.raw()) {
      out.write(values.data(), values.size_bytes());
    } else if (plan.kind != NumBlockKind::FOR) {
      assert(payload != nullptr && payload->slots.size() == values.size());
      std::vector<char>& bytes = payload->bytes;
      bytes.clear();
      encode(plan, *payload, bytes);
      assert(verify(plan.info, bytes, values));
      out.write(bytes.data(), bytes.size());
    } else {
      LinearPack::Writer writer(out, plan.bits);
      pack(writer, values.size(), plan, quotients);
      writer.finish(false);
    }
  }

  template<class Values>
  static NumBlockInfo append(std::vector<char>& out, const Values& values, bool useGcd) {
    std::vector<uint64_t> quotients;
    auto plan = NumColumnFormat::planBlock(values, quotients, useGcd, false);
    plan.info.setPayload(out.size(), plan.bits);
    size_t bytes = plan.raw() ? values.size() * sizeof(int64_t)
        : LinearPack::packedByteSize(values.size(), plan.bits);
    out.reserve(out.size() + bytes + LinearPack::TAIL_PAD + 7);
    if (plan.raw()) {
      size_t start = out.size();
      out.resize(start + values.size() * sizeof(int64_t));
      for (size_t i = 0; i < values.size(); i++) {
        int64_t value = values[i];
        memcpy(out.data() + start + i * sizeof(value), &value, sizeof(value));
      }
    } else {
      LinearPack::Writer writer(out, plan.bits);
      pack(writer, values.size(), plan, quotients);
      writer.finish(false);
    }
    return plan.info;
  }

private:
  static void encode(const NumColumnFormat::BlockPlan& plan,
                     const NumBlockPlanner::Payload& payload,
                     std::vector<char>& bytes) {
    if (plan.kind == NumBlockKind::DELTA) {
      LinearPack::Writer anchors(bytes, plan.info.anchorBits());
      for (uint64_t anchor : payload.anchors) anchors.append(anchor);
      anchors.finish(false);
    } else if (plan.kind == NumBlockKind::DICT) {
      bytes.resize(payload.dict.size() * sizeof(int64_t));
      memcpy(bytes.data(), payload.dict.data(), bytes.size());
    }
    LinearPack::Writer slots(bytes, plan.bits);
    for (uint64_t slot : payload.slots) slots.append(slot);
    slots.finish(false);
    size_t at = bytes.size();
    bytes.resize(at + payload.positions.size() * sizeof(uint16_t)
                 + payload.exceptions.size() * sizeof(int64_t));
    memcpy(bytes.data() + at, payload.positions.data(),
           payload.positions.size() * sizeof(uint16_t));
    memcpy(bytes.data() + at + payload.positions.size() * sizeof(uint16_t),
           payload.exceptions.data(),
           payload.exceptions.size() * sizeof(int64_t));
  }

  // The writer's lossless check: the encoded block reads back as `values`
  // through the frame decode, and at sampled ranks through the point read.
  // Debug builds only: DELTA, DICT and FOR_PATCHED are exact by
  // construction, and every ALP slot was already checked value by value in
  // all builds (NumBlockPlanner::alpEncode).
  static bool verify(const NumBlockInfo& info, std::span<const char> bytes,
                     std::span<const int64_t> values) {
    std::vector<char> padded(bytes.begin(), bytes.end());
    padded.resize(padded.size() + LinearPack::TAIL_PAD);
    int64_t frame[NumColumnFormat::BULK_SIZE];
    uint64_t count = values.size();
    for (uint64_t start = 0; start < count; start += NumColumnFormat::BULK_SIZE) {
      uint32_t n = (uint32_t)std::min<uint64_t>(NumColumnFormat::BULK_SIZE, count - start);
      NumBlockCodecs::decodeFrame(info, padded.data(), count, start, n, frame);
      for (uint32_t i = 0; i < n; i++) {
        if (frame[i] != values[start + i]) return false;
      }
      // Point reads at a few ranks per frame, its last included.
      for (uint32_t i = n - 1; i < n; i = i >= 37 ? i - 37 : n) {
        if (NumBlockCodecs::valueAt(info, padded.data(), count, start + i)
            != values[start + i]) {
          return false;
        }
      }
    }
    return true;
  }

  static void pack(LinearPack::Writer& writer, size_t count,
                   const NumColumnFormat::BlockPlan& plan, std::span<const uint64_t> quotients) {
    for (uint64_t i = 0; i < count; i++) {
      uint64_t residual = NumColumnFormat::residual(plan, i, quotients[(size_t)i]);
      assert(residual <= LinearPack::mask64(plan.bits));
      writer.append(residual);
    }
  }
};

} // namespace luxir
