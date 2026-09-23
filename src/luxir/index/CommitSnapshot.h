// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/schema/Schema.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

// Owning snapshot identity for asynchronous completion and synchronous commits.
struct CommitId {
  std::string incarnation;
  uint64_t index_gen = 0;
  auto operator<=>(const CommitId&) const = default;
};

// Immutable publication shared by the reader manager and transfer reservations.
// The bytes are the wire manifest, without the local disk footer.
struct CommitSnapshot {
  using Bytes = std::shared_ptr<const std::vector<std::byte>>;
  Bytes bytes;
  std::shared_ptr<Schema> schema;
  CommitId id;
  uint64_t commitTime;
  std::vector<FileDescriptor> files;

  static std::shared_ptr<const CommitSnapshot> fromBytes(Bytes bytes);
};

}
