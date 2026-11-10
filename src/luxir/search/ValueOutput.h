// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/api/RequestFlags.h"
#include "luxir/api/build.h"
#include "luxir/value/ValueTypes.h"

namespace luxir {

// Select once per output column. Identity is inlined away, so native loops
// retain their existing writes, with no per-value dispatch or conversion.
struct NativeValueOutput {
  template <typename T> T operator()(T value) const { return value; }
};

struct ValueOutputContext {
  std::string source;
  std::pmr::memory_resource& mr;
};

struct NumericValueOutput {
  DateUnit from;
  DateUnit to;
  ValueOutputContext context;
  int64_t operator()(int64_t value) const;
  double operator()(double value) const;
};

struct IsoValueOutput {
  DateUnit unit;
  ValueOutputContext context;
  std::string_view operator()(int64_t value) const;
  std::string_view operator()(double value) const;
};

// Select the policy type and its parameters without binding output resources.
// Native checks and rendering share this dispatch.
template <typename Emit>
void selectValueOutput(ValueNature nature, uint64_t flags, Emit emit) {
  switch (nature) {
    case ValueNature::DATE_MS:
    case ValueNature::DATE_NS: {
      auto mode = RequestFlags::dateMode(flags);
      if (mode == RequestFlags::DateMode::ISO) {
        emit.template operator()<IsoValueOutput>(valueDateUnit(nature));
        return;
      }
      if (mode != RequestFlags::DateMode::NATIVE) {
        DateUnit from = valueDateUnit(nature);
        DateUnit to = mode == RequestFlags::DateMode::MILLIS ? DateUnit::MILLIS : DateUnit::NANOS;
        if (from != to) {
          emit.template operator()<NumericValueOutput>(from, to);
          return;
        }
      }
      break;
    }
    default: break;
  }
  emit.template operator()<NativeValueOutput>();
}

inline bool nativeValueOutput(ValueNature nature, uint64_t flags) {
  bool native = false;
  selectValueOutput(nature, flags, [&]<typename Output>(auto...) {
    native = std::is_same_v<Output, NativeValueOutput>;
  });
  return native;
}

template <typename Source, typename Emit>
void withNatureOutput(ValueNature nature, uint64_t flags,
                      std::pmr::memory_resource& mr, Source source, Emit emit) {
  selectValueOutput(nature, flags, [&]<typename Output>(auto... parameters) {
    if constexpr (std::is_same_v<Output, NativeValueOutput>) emit(Output{});
    else emit(Output{parameters..., ValueOutputContext{std::string(source()), mr}});
  });
}

template <typename Output>
void renderValue(api::Val& value, const Output& output) {
  if constexpr (!std::is_same_v<Output, NativeValueOutput>) {
    if (auto* integer = std::get_if<int64_t>(&value.kind)) value.kind = output(*integer);
    else if (auto* number = std::get_if<double>(&value.kind)) value.kind = output(*number);
  }
}

} // namespace luxir
