// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <charconv>
#include <map>
#include "Directory.h"
#include "luxir/api/index_files.h"
#include "luxir/api/padded_input.h"
#include "luxir/util/Signal.h"

namespace luxir {

// The payload is also the immutable, transportable snapshot. The local footer
// detects incomplete writes without changing the wire representation.
struct Manifest {
  using Bytes = std::shared_ptr<const std::vector<std::byte>>;
  std::vector<std::string> names;
  std::vector<Directory::FileInfo> listing; // reused by writer startup retirement
  Bytes bytes;
  uint64_t generation = 0;
  uint64_t highestGeneration = 0;

  static bool indexFile(std::string_view name) {
    return name.starts_with("s.olux") || (name.starts_with("s") && name.find('_') != std::string_view::npos);
  }

  static std::string name(uint64_t gen) { return "s.olux_" + std::to_string(gen); }

  static uint64_t generationOf(std::string_view name) {
    constexpr std::string_view prefix = "s.olux_";
    if (!name.starts_with(prefix)) return 0;
    name.remove_prefix(prefix.size());
    uint64_t gen = 0;
    auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), gen);
    return error == std::errc() && end == name.data() + name.size() ? gen : 0;
  }

  static api::IndexInfo decode(const Bytes& bytes, std::pmr::memory_resource& arena) {
    api::IndexInfo info;
    if (!bytes || !api::decode(info, api::copyToPaddedInput(*bytes, arena), arena)
        || !info.schema || info.incarnation.empty()) {
      throw std::runtime_error("Invalid snapshot manifest");
    }
    return info;
  }

  static std::vector<FileDescriptor> files(const Bytes& bytes) {
    if (!bytes) return {};
    std::pmr::monotonic_buffer_resource arena;
    return filesOf(decode(bytes, arena));
  }

  static Bytes read(Directory& dir, uint64_t gen) {
    auto file = dir.openFile(name(gen));
    if (!file) return {};
    auto input = file->getInputStream();
    if (input.size() < 16) return {};
    size_t length = input.size() - 16;
    auto data = (const std::byte*)input.ptr();
    input.seek(length);
    if ((uint64_t)input.readLong() != length
        || (uint64_t)input.readLong() != XXH3_64bits(data, length)) return {};
    auto bytes = std::make_shared<const std::vector<std::byte>>(data, data + length);
    std::pmr::monotonic_buffer_resource arena;
    try {
      if (decode(bytes, arena).index_gen != gen) return {};
    } catch (const std::runtime_error&) { return {}; }
    return bytes;
  }

  // Data and its directory entries are durable before a manifest is written.
  // Only a fallback needs a presence check: obsolete names can reappear after
  // a crash because retirement does not fsync removals.
  static Manifest load(Directory& dir) {
    std::vector<std::string> failedNames;
    bool retried = false;
    uint64_t highestGeneration = 0;
    for (;;) {
      std::vector<Directory::FileInfo> listing;
      dir.listFiles(listing);
      std::vector<uint64_t> generations;
      std::map<std::string, uint64_t> sizes;
      Manifest result;
      bool indexFiles = false;
      for (const auto& f : listing) {
        sizes.emplace(f.name, f.size);
        if (f.name.starts_with("s.olux_")) result.names.push_back(f.name);
        if (auto gen = generationOf(f.name)) generations.push_back(gen);
        if (indexFile(f.name)) indexFiles = true;
      }
      if (generations.empty() && !indexFiles) return result;
      if (retried && result.names == failedNames) {
        throw std::runtime_error(generations.empty()
            ? "Index files exist without a snapshot manifest" : "No valid snapshot manifest");
      }
      std::ranges::sort(generations, std::greater<>());
      if (!generations.empty()) highestGeneration = std::max(highestGeneration, generations.front());
      result.highestGeneration = highestGeneration;
      for (size_t i = 0; i < generations.size(); i++) {
        auto bytes = read(dir, generations[i]);
        if (!bytes) continue;
        if (i != 0) {
          bool present = true;
          for (const auto& file : files(bytes)) {
            auto entry = sizes.find(file.name);
            if (entry == sizes.end() || entry->second != file.size) { present = false; break; }
          }
          if (!present) continue;
        }
        result.bytes = std::move(bytes);
        result.generation = generations[i];
        result.listing = std::move(listing);
        return result;
      }
      // Publication can retire a root after this scan, before read() opens it.
      // Retry a changed root listing; a stable invalid listing is an error.
      failedNames = std::move(result.names);
      retried = true;
    }
  }

  // A root is durable only after its file and directory entry are synced. It
  // is written and synced under a pending name and renamed into place, so a failure
  // never leaves a partial root and never removes one that already existed; an
  // identical existing root (an interrupted earlier commit) is only re-synced.
  // After a failure the root's durability is unknown. `source` identifies the
  // committer to failure-injection signals.
  static void commit(Directory& dir, uint64_t gen, const std::vector<std::byte>& bytes, void* source = nullptr) {
    Bytes existing;
    try { existing = read(dir, gen); }
    catch (const FileIOException&) {} // an unreadable root is what a repair replaces
    std::array<std::string, 1> root{name(gen)};
    if (!existing || *existing != bytes) {
      auto pending = name(gen) + ".pending";
      try {
        write(dir, pending, bytes);
        Signal::emit("manifestWritten", source);
        std::array<std::string, 1> written{pending};
        dir.sync(written);
        Signal::emit("manifestSynced", source);
        dir.renameFile(pending, root[0]);
      } catch (...) {
        try { dir.deleteFile(pending); } catch (...) {}
        throw;
      }
    } else dir.sync(root);
    std::array<std::string, 1> directory{"."};
    dir.sync(directory);
  }

  static void write(Directory& dir, uint64_t gen, const std::vector<std::byte>& bytes) {
    write(dir, name(gen), bytes);
  }
  static void write(Directory& dir, const std::string& fileName, const std::vector<std::byte>& bytes) {
    Directory::FileCreateOptions options; options.expectedSize = bytes.size() + 16;
    auto file = dir.createFile(fileName, options);
    OutputStream out(file.get());
    out.write(bytes.data(), bytes.size());
    out.writeLong(bytes.size());
    out.writeLong(XXH3_64bits(bytes.data(), bytes.size()));
    out.close();
    dir.finishFile(*file);
  }
};
} // namespace luxir
