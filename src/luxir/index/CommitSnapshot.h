// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/schema/Schema.h"
#include "luxir/store/OutputStream.h"

namespace luxir {
namespace api { struct IndexInfo; }

// Owning snapshot identity for asynchronous completion and synchronous commits.
struct CommitId {
  std::string incarnation;
  uint64_t index_gen = 0;
  std::string token() const;
  static CommitId parse(std::string_view token);
  // A collection identity: 64 random bits as 16 lowercase hex digits. It also
  // names the incarnation's directory.
  static std::string newIncarnation();
  static bool validIncarnation(std::string_view value);
  auto operator<=>(const CommitId&) const = default;
};

// Immutable publication shared by the reader manager and transfer pins.
// The bytes are the wire manifest, without the local disk footer.
struct CommitSnapshot {
  using Bytes = std::shared_ptr<const std::vector<std::byte>>;
  Bytes bytes;
  std::shared_ptr<Schema> schema;
  CommitId id;
  uint64_t commitTime;
  std::vector<FileDescriptor> files;
  bool populated; // some segment has a live document
  uint64_t digest; // xxh3-64 of `bytes`: announcements bind a token to it

  static std::shared_ptr<const CommitSnapshot> fromBytes(Bytes bytes);
  static uint64_t digestOf(const std::vector<std::byte>& bytes);
  // A digest's text form on the wire: 16 lowercase hex digits.
  static std::string digestText(uint64_t digest);
  static uint64_t parseDigest(std::string_view text);
  static bool populatedOf(const api::IndexInfo& info);
};

}
