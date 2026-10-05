// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CommitSnapshot.h"
#include <charconv>
#include <format>
#include <random>
#include "luxir/api/index_files.h"
#include "luxir/store/Manifest.h"

namespace luxir {

std::string CommitId::token() const { return incarnation + ":" + std::to_string(index_gen); }

CommitId CommitId::parse(std::string_view token) {
  auto colon = token.find(':');
  uint64_t gen = 0;
  if (colon == std::string_view::npos || colon == 0) throw std::invalid_argument("expected commit=incarnation:gen");
  auto number = token.substr(colon + 1);
  auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), gen);
  if (error != std::errc() || end != number.data() + number.size() || gen == 0) {
    throw std::invalid_argument("invalid commit generation");
  }
  return {std::string(token.substr(0, colon)), gen};
}


std::shared_ptr<const CommitSnapshot> CommitSnapshot::fromBytes(Bytes bytes) {
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(bytes, arena);
  return std::make_shared<const CommitSnapshot>(std::move(bytes),
      Schema::fromStored(*info.schema, info.schema_gen),
      CommitId{std::string(info.incarnation), info.index_gen}, info.commit_time, filesOf(info), populatedOf(info),
      digestOf(*bytes));
}

std::string CommitId::newIncarnation() {
  std::random_device random;
  return std::format("{:016x}", ((uint64_t)random() << 32) | (uint32_t)random());
}

bool CommitId::validIncarnation(std::string_view value) {
  return value.size() == 16 && std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string CommitSnapshot::digestText(uint64_t digest) { return std::format("{:016x}", digest); }

uint64_t CommitSnapshot::parseDigest(std::string_view text) {
  uint64_t result = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, 16);
  if (text.size() != 16 || error != std::errc() || end != text.data() + text.size()
      || std::ranges::any_of(text, [](char c) { return c >= 'A' && c <= 'F'; })) {
    throw std::invalid_argument("expected a 16-digit lowercase hexadecimal xxh3 digest");
  }
  return result;
}

uint64_t CommitSnapshot::digestOf(const std::vector<std::byte>& bytes) {
  return XXH3_64bits(bytes.data(), bytes.size());
}

bool CommitSnapshot::populatedOf(const api::IndexInfo& info) {
  return std::ranges::any_of(info.segments, [](const auto& segment) { return segment.live_docs != 0; });
}

}
