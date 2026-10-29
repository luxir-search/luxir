// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/api/RequestFlags.h"
#include "luxir/api/build.h"
#include "luxir/value/ValueTypes.h"

namespace luxir {

// Select once per output column. Identity is inlined away, so native loops
// retain their existing writes, with no per-value dispatch or conversion.
struct NativeDateOutput {
  template <typename T> T operator()(T value) const { return value; }
};

struct NumericDateOutput {
  DateUnit from;
  DateUnit to;
  std::string source;
  int64_t operator()(int64_t value) const;
  double operator()(double value) const;
};

struct IsoDateOutput {
  DateUnit unit;
  std::string source;
  std::pmr::memory_resource& mr;
  std::string_view operator()(int64_t value) const;
  std::string_view operator()(double value) const;
};

template <typename Source, typename Emit>
void withDateOutput(ValueNature nature, uint64_t flags,
                    std::pmr::memory_resource& mr, Source source, Emit emit) {
  auto mode = RequestFlags::dateMode(flags);
  if (!valueDate(nature) || mode == RequestFlags::DateMode::NATIVE) {
    emit(NativeDateOutput{});
  } else if (mode == RequestFlags::DateMode::ISO) {
    emit(IsoDateOutput{valueDateUnit(nature), std::string(source()), mr});
  } else {
    DateUnit from = valueDateUnit(nature);
    DateUnit to = mode == RequestFlags::DateMode::MILLIS ? DateUnit::MILLIS : DateUnit::NANOS;
    if (from == to) emit(NativeDateOutput{});
    else emit(NumericDateOutput{from, to, std::string(source())});
  }
}

template <typename Output>
void renderDateValue(api::Val& value, const Output& output) {
  if constexpr (!std::is_same_v<Output, NativeDateOutput>) {
    if (auto* integer = std::get_if<int64_t>(&value.kind)) value.kind = output(*integer);
    else if (auto* number = std::get_if<double>(&value.kind)) value.kind = output(*number);
  }
}

} // namespace luxir
