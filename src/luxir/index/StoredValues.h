// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/ValCoerce.h"

namespace luxir {

// The values a stored field keeps for one input value, as the stored-fields
// writer receives them: strings and bytes as they are, numbers and bools as
// canonical text, arrays element by element, nothing for null.  Calls
// emit(std::span<const std::string_view>) at most once; the views last for the
// call.  Ingest and a read of the value back out of a document's source both
// go through here, so they agree byte for byte.
template <typename Emit>
void storedValues(const api::Val& val, std::string_view name, Emit&& emit) {
  if (const auto* s = std::get_if<std::string_view>(&val.kind)) {
    emit(std::span<const std::string_view>(s, 1));
  } else if (const auto* b = std::get_if<::hpp_proto::bytes_view>(&val.kind)) {
    std::string_view view((const char*)b->data(), b->size());
    emit(std::span<const std::string_view>(&view, 1));
  } else if (const auto* arr = std::get_if<api::ArrStr>(&val.kind)) {
    emit(std::span<const std::string_view>(arr->v.data(), arr->v.size()));
  } else if (const auto* bins = std::get_if<api::ArrBin>(&val.kind)) {
    std::vector<std::string_view> views;
    views.reserve(bins->v.size());
    for (const auto& bin : bins->v) views.emplace_back((const char*)bin.data(), bin.size());
    emit(std::span<const std::string_view>(views));
  } else if (coerce::isNull(val)) {
    // no value: nothing stored
  } else if (coerce::isArray(val)) {
    // numeric / mixed arrays: each element's canonical rendering (materialized,
    // as buf is per-element transient)
    char buf[coerce::TEXT_BUF_SIZE];
    std::vector<std::string> storage;
    coerce::forEachElement(val, [&](const api::Val& elem) { storage.emplace_back(coerce::toText(elem, name, buf)); });
    std::vector<std::string_view> views(storage.begin(), storage.end());
    emit(std::span<const std::string_view>(views));
  } else {
    // numeric / bool scalars: canonical text
    char buf[coerce::TEXT_BUF_SIZE];
    std::string_view text = coerce::toText(val, name, buf);
    emit(std::span<const std::string_view>(&text, 1));
  }
}

}  // namespace luxir
