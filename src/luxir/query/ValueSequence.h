#pragma once

#include <format>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/ValCoerce.h"

namespace luxir {

// Non-owning view of the field values carried by AnyOfQuery and facet
// selections. Homogeneous Val array arms stay typed, so their consumers select
// the arm once and then walk a contiguous scalar span. ArrVal and legacy
// span-of-Val producers retain element-wise dispatch for their mixed values.
class ValueSequence {
public:
  enum class NullError {
    SCALAR_REQUIRED,
    MUST_NOT_BE_NULL,
  };

private:
  using ValSpan = std::span<const api::Val>;
  using StrSpan = std::span<const std::string_view>;
  using IntSpan = std::span<const int64_t>;
  using DoubleSpan = std::span<const double>;
  using FloatSpan = std::span<const float>;
  using BoolSpan = std::span<const bool>;
  using BinSpan = std::span<const ::hpp_proto::bytes_view>;

  struct Unchecked {};

  std::variant<ValSpan, StrSpan, IntSpan, DoubleSpan,
               FloatSpan, BoolSpan, BinSpan> values = ValSpan{};

  ValueSequence(ValSpan value, Unchecked) : values(value) {}
  explicit ValueSequence(StrSpan value) : values(value) {}
  explicit ValueSequence(IntSpan value) : values(value) {}
  explicit ValueSequence(DoubleSpan value) : values(value) {}
  explicit ValueSequence(FloatSpan value) : values(value) {}
  explicit ValueSequence(BoolSpan value) : values(value) {}
  explicit ValueSequence(BinSpan value) : values(value) {}

  static bool scalar(const api::Val& value) {
    return std::holds_alternative<std::string_view>(value.kind)
        || std::holds_alternative<int64_t>(value.kind)
        || std::holds_alternative<double>(value.kind)
        || std::holds_alternative<float>(value.kind)
        || std::holds_alternative<bool>(value.kind)
        || std::holds_alternative<::hpp_proto::bytes_view>(value.kind);
  }

  static std::string_view invalidKind(const api::Val& value) {
    if (coerce::isNull(value)) return "null";
    if (coerce::isArray(value)) return "a nested array";
    if (std::holds_alternative<api::Map>(value.kind)) return "a map";
    if (std::holds_alternative<api::Vector>(value.kind)) return "a vector";
    if (std::holds_alternative<api::ArrVector>(value.kind)) return "a vector array";
    if (std::holds_alternative<api::DocList>(value.kind)) return "documents";
    if (std::holds_alternative<api::FacetResult>(value.kind)) return "a facet result";
    return "an unsupported value";
  }

  static void rejectInvalid(const api::Val& value, std::string_view name,
                            NullError nullError) {
    if (nullError == NullError::MUST_NOT_BE_NULL && coerce::isNull(value)) {
      throw std::runtime_error(std::format("{} must not be null", name));
    }
    throw std::runtime_error(std::format(
        "{} must contain scalar field values, not {}",
        name, invalidKind(value)));
  }

public:
  ValueSequence() = default;
  ValueSequence(ValSpan input, std::string_view name,
                NullError nullError = NullError::SCALAR_REQUIRED) {
    for (size_t i = 0; i < input.size(); i++) {
      if (scalar(input[i])) continue;
      if (nullError == NullError::MUST_NOT_BE_NULL
          && coerce::isNull(input[i])) {
        throw std::runtime_error(std::format(
            "{}[{}] must not be null", name, i));
      }
      throw std::runtime_error(std::format(
          "{}[{}] must be a scalar field value, not {}",
          name, i, invalidKind(input[i])));
    }
    values = input;
  }

  ValueSequence(const api::Val& value, std::string_view name,
                NullError nullError = NullError::SCALAR_REQUIRED) {
    std::visit([&](const auto& arm) {
      using Arm = std::decay_t<decltype(arm)>;
      if constexpr (std::is_same_v<Arm, std::string_view>
                    || std::is_same_v<Arm, int64_t>
                    || std::is_same_v<Arm, double>
                    || std::is_same_v<Arm, float>
                    || std::is_same_v<Arm, bool>
                    || std::is_same_v<Arm, ::hpp_proto::bytes_view>) {
        values = std::span(&arm, (size_t)1);
      } else if constexpr (std::is_same_v<Arm, api::ArrVal>) {
        *this = ValueSequence(ValSpan(arm.v), name, nullError);
      } else if constexpr (std::is_same_v<Arm, api::ArrStr>
                           || std::is_same_v<Arm, api::ArrInt>
                           || std::is_same_v<Arm, api::ArrDouble>
                           || std::is_same_v<Arm, api::ArrFloat>
                           || std::is_same_v<Arm, api::ArrBin>) {
        values = std::span(arm.v);
      } else {
        rejectInvalid(value, name, nullError);
      }
    }, value.kind);
  }

  size_t size() const {
    return std::visit([](const auto& viewed) -> size_t {
      return viewed.size();
    }, values);
  }

  bool empty() const { return size() == 0; }

  ValueSequence element(size_t index) const {
    return std::visit([&](const auto& viewed) -> ValueSequence {
      using Viewed = std::decay_t<decltype(viewed)>;
      if constexpr (std::is_same_v<Viewed, ValSpan>) {
        return ValueSequence(viewed.subspan(index, 1), Unchecked{});
      } else return ValueSequence(viewed.subspan(index, 1));
    }, values);
  }

  template<typename F>
  decltype(auto) visit(F&& visitor) const {
    return std::visit([&](const auto& viewed) -> decltype(auto) {
      return visitor(viewed);
    }, values);
  }
};

} // namespace luxir
