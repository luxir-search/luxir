// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "luxir/util/DateTime.h"

namespace luxir::test {

// A temporary zoneinfo tree of hand-written TZif files, so zone tests do not
// depend on the host's tzdata or on its rules staying the same.
class ZoneinfoFixture {
  std::filesystem::path root;
  std::string records = "# version 2099z\n";

public:
  struct Type {
    int32_t offset;
    bool dst;
    std::string abbr;
  };
  struct Transition {
    int64_t at;  // unix seconds
    uint8_t type;
  };

  ZoneinfoFixture() {
    static std::atomic<int> sequence = 0;
    root = std::filesystem::temp_directory_path()
        / ("luxir-zoneinfo-" + std::to_string(getpid()) + "-" + std::to_string(sequence++));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }
  ~ZoneinfoFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  const std::filesystem::path& path() const { return root; }

  // Write a TZif version 2 file; footer is the POSIX TZ rule for instants
  // after the last transition. Types[0] applies before the first one.
  void zone(const std::string& name, const std::vector<Type>& types,
            const std::vector<Transition>& transitions, const std::string& footer) {
    std::string out;
    auto put = [&](uint64_t value, int bytes) {
      for (int i = bytes - 1; i >= 0; i--) out += (char)(value >> (8 * i));
    };
    auto header = [&](size_t timecnt, size_t typecnt, size_t charcnt) {
      out += "TZif2";
      out.append(15, '\0');
      for (size_t count : {(size_t)0, (size_t)0, (size_t)0, timecnt, typecnt, charcnt}) put(count, 4);
    };
    header(0, 1, 1);  // a minimal version 1 block, which readers skip
    out.append(7, '\0');
    std::string designations;
    std::vector<size_t> designation;
    for (const auto& type : types) {
      designation.push_back(designations.size());
      designations += type.abbr + '\0';
    }
    header(transitions.size(), types.size(), designations.size());
    for (const auto& t : transitions) put((uint64_t)t.at, 8);
    for (const auto& t : transitions) out += (char)t.type;
    for (size_t i = 0; i < types.size(); i++) {
      put((uint32_t)types[i].offset, 4);
      out += (char)types[i].dst;
      out += (char)designation[i];
    }
    out += designations + "\n" + footer + "\n";
    std::filesystem::create_directories((root / name).parent_path());
    std::ofstream(root / name, std::ios::binary) << out;
  }

  // A raw tzdata.zi line, e.g. "Z Test/Zone 0 - X" or "L Test/Zone Test/Alias".
  void record(const std::string& line) { records += line + "\n"; }

  datetime_detail::ZoneRegistry load() {
    std::ofstream(root / "tzdata.zi") << records;
    return datetime_detail::ZoneRegistry::load(root.string());
  }
};

} // namespace luxir::test
