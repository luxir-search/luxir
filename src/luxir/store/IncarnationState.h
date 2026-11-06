// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Directory.h"

namespace luxir {

class WriterSupersededError : public std::runtime_error {
public:
  WriterSupersededError() : std::runtime_error("index writer was superseded") {}
};

// The version fences every publication by one writer lifetime. Only a catalog
// reservation creates STATE; removing it fences every existing writer.
struct IncarnationState {
  struct Value {
    uint64_t rev = 0;
    uint64_t epoch = 0;
    std::string owner;
    std::string tip;
  };
  Value value;
  Directory::PublicationVersion version;

  static std::optional<IncarnationState> read(Directory& dir);
  static IncarnationState claim(Directory& dir);
  // After catalog reservation; an existing STATE is left intact.
  static void create(Directory& dir);
  // Follower ownership is local; source epochs occur only in the tip's name.
  static void install(Directory& dir, std::string_view tip);
  void advance(Directory& dir, std::string_view tip);
};
}
