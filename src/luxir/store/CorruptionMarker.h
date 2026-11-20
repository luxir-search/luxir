// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string>
#include <vector>
#include "luxir/store/Directory.h"

namespace luxir {

// Persisted suspicion: `corrupted_<id>` files in an incarnation directory name
// the index files a check found damaged while the collection served (today
// the scrub; later a SIGBUS handler, which is why the format is plain text a
// signal handler can write: one file name per line, and lines starting with
// '#' that say why). The collection is unavailable while a marker names a
// file that still fails; opening re-verifies exactly those files and clears
// the markers once they pass or an install replaced them.
struct CorruptionMarker {
  static constexpr std::string_view kPrefix = "corrupted_";
  struct Observation {
    std::vector<std::string> markers; // Only markers successfully read.
    std::vector<std::string> suspects; // Their file names, sorted and unique.
  };

  static bool isMarker(std::string_view name) { return name.starts_with(kPrefix) && !name.ends_with(".tmp"); }

  // Writes a durable marker naming `files`.
  static void write(Directory& dir, std::span<const std::string> files, std::string_view reason);
  static Observation read(Directory& dir);
  static Observation read(Directory& dir, std::span<const Directory::FileInfo> listing);
  static bool present(Directory& dir);
};

}
