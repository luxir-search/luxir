// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationState.h"
#include <glaze/glaze.hpp>

namespace luxir {

ReplicationState ReplicationState::read(InputFile& file) {
  ReplicationState state;
  std::string text(file.read()); // the parser reads up to a terminator
  if (glz::read_json(state, text)) throw std::runtime_error("Invalid follower state");
  return state;
}

void ReplicationState::write(Directory& dir) const {
  auto bytes = glz::write_json(*this).value();
  Directory::FileCreateOptions options; options.expectedSize = bytes.size();
  auto file = dir.createFile("replication.json.pending", options);
  OutputStream out(file.get());
  out.write(bytes.data(), bytes.size()); out.close();
  dir.finishFile(*file);
  std::array<std::string, 1> pending{"replication.json.pending"};
  dir.sync(pending);
  dir.renameFile("replication.json.pending", "replication.json");
  std::array<std::string, 1> directory{"."};
  dir.sync(directory);
}

} // namespace luxir
