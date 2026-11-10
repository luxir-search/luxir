// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/api/RequestFlags.h"
#include "luxir/api/build.h"
#include "luxir/value/ValueTypes.h"
#include "luxir/util/Decimal.h"

namespace luxir {

// Select once per output column. Identity is inlined away, so native loops
// retain their existing writes, with no per-value dispatch or conversion.
struct NativeValueOutput {
  template <typename T> T operator()(T value) const { return value; }
};

struct BooleanValueOutput {
  template <typename T> requires (std::is_arithmetic_v<T> || std::is_same_v<T, __int128>)
  bool operator()(T value) const { return value != 0; }
};

struct DecimalValueOutput {
  DecimalScale scale;
  explicit DecimalValueOutput(int scale) : scale(scale) {}
  template <typename T> requires (std::is_arithmetic_v<T> || std::is_same_v<T, __int128>)
  double operator()(T raw) const { return scale(raw); }
};

struct ValueOutputContext {
  std::string source;
  std::pmr::memory_resource& mr;
};

struct IpValueOutput {
  ValueOutputContext context;
  std::string_view operator()(std::string_view value) const;
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
  if (valueDecimal(nature) && !(flags & RequestFlags::DECIMAL_SCALED)) {
    emit.template operator()<DecimalValueOutput>(valueDecimalScale(nature));
    return;
  }
  switch (nature) {
    case ValueNature::IP:
      emit.template operator()<IpValueOutput>();
      return;
    case ValueNature::BOOLEAN:
      emit.template operator()<BooleanValueOutput>();
      return;
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

template <typename Value = void, typename Source, typename Emit>
void withNatureOutput(ValueNature nature, uint64_t flags,
                      std::pmr::memory_resource& mr, Source source, Emit emit) {
  selectValueOutput(nature, flags, [&]<typename Output>(auto... parameters) {
    if constexpr (!std::is_void_v<Value> && !std::is_invocable_v<Output, Value>) {
      throw std::logic_error("Output policy does not support the column value type");
    } else if constexpr (std::is_constructible_v<Output, decltype(parameters)...>) emit(Output{parameters...});
    else emit(Output{parameters..., ValueOutputContext{std::string(source()), mr}});
  });
}

template <typename Output>
void renderValueVariant(auto& value, const Output& output) {
  if constexpr (!std::is_same_v<Output, NativeValueOutput>) {
    if constexpr (std::is_invocable_v<Output, int64_t>) {
      if (auto* integer = std::get_if<int64_t>(&value)) { value = output(*integer); return; }
    }
    if constexpr (std::is_invocable_v<Output, double>) {
      if (auto* number = std::get_if<double>(&value)) { value = output(*number); return; }
    }
    if constexpr (std::is_invocable_v<Output, std::string_view>) {
      if (auto* text = std::get_if<std::string_view>(&value)) value = output(*text);
    }
  }
}

template <typename Output>
void renderValue(api::Val& value, const Output& output) {
  renderValueVariant(value.kind, output);
}

template <typename Output>
std::string_view outputTerm(std::string_view value, std::pmr::memory_resource& mr, const Output& output) {
  if constexpr (std::is_same_v<Output, NativeValueOutput>) return api::build::arenaStr(mr, value);
  else return output(value);
}

} // namespace luxir
