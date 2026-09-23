// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <glaze/glaze.hpp>
#include "luxir/store/Directory.h"

namespace luxir {

// Node-local follower identity and discovery history. CURRENT only selects data.
struct ReplicationState {
  struct Collection {
    std::string source;
    std::string boot;
    bool replace_empty = true;
    // Durable promotion target, recorded before switching CURRENT. A restart
    // finishes that switch instead of minting another incarnation.
    std::string promoted;
  };
  std::string source;
  std::string follower;
  std::map<std::string, Collection> collections;

  static ReplicationState read(InputFile& file) {
    ReplicationState state;
    if (glz::read_json(state, file.read()) || state.follower.empty() || state.follower.size() > 255)
      throw std::runtime_error("Invalid follower state");
    return state;
  }
  void write(Directory& dir) const {
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
};
}
