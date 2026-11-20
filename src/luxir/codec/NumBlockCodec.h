// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/codec/NumColumn.h"

namespace luxir {

class NumBlockCodec {
public:
  static void write(OutputStream& out, size_t origin, std::span<const int64_t> values,
                    NumColumnFormat::BlockPlan& plan, std::span<const uint64_t> quotients) {
    if (plan.raw()) out.align(MAX_ALIGN);
    plan.info.setPayload(out.size() - origin, plan.bits);
    if (plan.raw()) {
      out.write(values.data(), values.size_bytes());
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
