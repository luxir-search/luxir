// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CommitSnapshot.h"
#include "luxir/api/index_files.h"
#include "luxir/store/Manifest.h"

namespace luxir {

std::shared_ptr<const CommitSnapshot> CommitSnapshot::fromBytes(Bytes bytes) {
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(bytes, arena);
  return std::make_shared<const CommitSnapshot>(std::move(bytes),
      Schema::fromStored(*info.schema, info.schema_gen),
      CommitId{std::string(info.incarnation), info.index_gen}, info.commit_time, filesOf(info));
}

}
