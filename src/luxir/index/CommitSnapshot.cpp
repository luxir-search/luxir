// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CommitSnapshot.h"
#include <charconv>
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
      CommitId{std::string(info.incarnation), info.index_gen}, info.commit_time, filesOf(info));
}

}
