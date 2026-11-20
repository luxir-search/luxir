// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/schema/FieldType.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/util/StrRef.h"

namespace luxir::handler {

// Prepare external values before any postings or column appends. Scalar views
// last until the next preparation; array views survive the whole append pass.
template <bool Packed>
class StringValue {
  const FieldType& type;
  const bool rawTextTerms;
  std::unique_ptr<TokenChain> normalizer;
  std::string normalized;
  char buffer[coerce::TEXT_BUF_SIZE];
  PackedTerm::TermBuffer fitted;
  std::vector<std::string_view> views;
  std::vector<std::string> owned;
  size_t ownedBytes = 0;
  size_t retained = 0;

  static size_t heapBytes(const std::string& value) {
    return value.capacity() > std::string{}.capacity() ? value.capacity() + 1 : 0;
  }

  void updateRetainedBytes() {
    retained = heapBytes(normalized) + ownedBytes + owned.capacity() * sizeof(std::string)
        + views.capacity() * sizeof(std::string_view);
  }

  template <typename Visit>
  std::span<const std::string_view> prepareMany(size_t count, std::string_view fieldName, Visit visit) {
    auto account = scope_guard([&] { updateRetainedBytes(); });
    views.clear();
    views.reserve(count);
    size_t used = 0;
    visit([&](const auto& value) {
      auto term = prepare(value, fieldName);
      if (normalizer || term.data() == buffer || term.data() == fitted.data()) {
        // Reserve before retaining SSO views: moving strings would invalidate them.
        owned.reserve(count);
        if (used == owned.size()) owned.emplace_back();
        auto& target = owned[used++];
        size_t before = heapBytes(target);
        target.assign(term);
        ownedBytes = ownedBytes - before + heapBytes(target);
        term = target;
      }
      views.push_back(term);
    });
    return views;
  }

public:
  explicit StringValue(const FieldType& type) : type(type), rawTextTerms(type.rawTextTerms) {
    if (auto* stringType = dynamic_cast<const StrFieldType*>(&type)) {
      if (stringType->normalizer) normalizer = stringType->normalizer->createChain();
    }
  }

  size_t retainedBytes() const { return retained; }

  bool hasNormalizer() const { return normalizer != nullptr; }

  [[gnu::always_inline]] std::string_view prepare(std::string_view value, std::string_view fieldName) {
    return finish(rawTextTerms ? value : coerceText(value, fieldName), fieldName);
  }

  std::string_view prepare(::hpp_proto::bytes_view value, std::string_view fieldName) {
    return finish(rawTextTerms ? std::string_view((const char*)value.data(), value.size())
        : coerceText(value, fieldName), fieldName);
  }

  std::string_view prepare(const api::Val& value, std::string_view fieldName) {
    if (rawTextTerms) {
      if (auto s = std::get_if<std::string_view>(&value.kind)) return finish(*s, fieldName);
      if (auto b = std::get_if<::hpp_proto::bytes_view>(&value.kind)) {
        return finish(std::string_view((const char*)b->data(), b->size()), fieldName);
      }
    }
    return finish(type.coerceTerm(value, fieldName, buffer), fieldName);
  }

private:
  template <typename T>
  [[gnu::noinline]] std::string_view coerceText(T value, std::string_view fieldName) {
    return type.coerceTerm(coerce::scalarVal(value), fieldName, buffer);
  }

  [[noreturn, gnu::cold, gnu::noinline]] static void throwTooLong(std::string_view fieldName, size_t size) {
    throw DocumentError(fmt::format(
        "Field '{}': string value is {} bytes after normalization; maximum is {}",
        fieldName, size, PackedTerm::MAX_LEN));
  }

  [[gnu::noinline]] std::string_view normalize(std::string_view term) {
    auto account = scope_guard([&] { updateRetainedBytes(); });
    normalized.assign(term);
    normalizer->normalizeTerm(normalized);
    return normalized;
  }

  [[gnu::always_inline]] std::string_view finish(std::string_view term, std::string_view fieldName) {
    if (normalizer) term = normalize(term);
    if constexpr (Packed) {
      if (term.size() > PackedTerm::MAX_LEN) {
        auto result = PackedTerm::fitTerm(type.longTerms, term, fitted);
        if (!result) throwTooLong(fieldName, term.size());
        term = *result;
      }
    }
    return term;
  }

public:
  std::span<const std::string_view> prepareMany(const api::Val& values, std::string_view fieldName) {
    if (auto a = std::get_if<api::ArrStr>(&values.kind)) return prepareMany(a->v, fieldName);
    if (auto a = std::get_if<api::ArrBin>(&values.kind)) {
      return prepareMany(a->v.size(), fieldName, [&](auto accept) {
        for (auto value : a->v) accept(value);
      });
    }
    size_t count = 0;
    coerce::forEachElement(values, [&](const api::Val&) { count++; });
    return prepareMany(count, fieldName, [&](auto accept) { coerce::forEachElement(values, accept); });
  }

  std::span<const std::string_view> prepareMany(std::span<const std::string_view> values,
      std::string_view fieldName) {
    if constexpr (!Packed) {
      if (rawTextTerms && !normalizer) return values;
    }
    return prepareMany(values.size(), fieldName, [&](auto accept) {
      for (auto value : values) accept(value);
    });
  }
};

} // namespace luxir::handler
