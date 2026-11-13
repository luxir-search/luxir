// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <charconv>
#include <map>
#include "Directory.h"
#include "luxir/schema/SchemaBinding.h"
#include "IncarnationState.h"
#include "luxir/reader/Postings.h"
#include "IndexFormat.h"
#include "luxir/api/index_files.h"
#include "luxir/api/padded_input.h"

namespace luxir {

// The payload is also the immutable, transportable snapshot. The local footer
// detects incomplete writes without changing the wire representation.
struct Manifest {
  using Bytes = std::shared_ptr<const std::vector<std::byte>>;
  std::vector<std::string> names;
  // The directory's files, once something needed them: load() lists only to
  // fall back below a torn tip. listingOf() lists at most once per Manifest.
  std::vector<Directory::FileInfo> listing;
  bool listed = false;
  Bytes bytes;
  uint64_t generation = 0;
  uint64_t highestGeneration = 0;

  uint64_t epoch = 0;
  static constexpr std::string_view kPrefix = "commit_";

  static std::string tip(uint64_t gen, uint64_t epoch) {
    return std::to_string(gen) + "." + Postings::epochString(epoch);
  }
  static std::string name(uint64_t gen, uint64_t epoch) { return std::string(kPrefix) + tip(gen, epoch); }

  static std::pair<uint64_t, uint64_t> candidate(std::string_view name) {
    if (!name.starts_with(kPrefix)) return {};
    name.remove_prefix(kPrefix.size());
    auto dot = name.find('.');
    if (dot == std::string_view::npos) return {};
    uint64_t gen = 0;
    auto [end, error] = std::from_chars(name.data(), name.data() + dot, gen);
    auto epoch = Postings::parseEpoch(name.substr(dot + 1));
    if (error != std::errc() || end != name.data() + dot || !gen || !epoch || tip(gen, epoch) != name) return {};
    return {gen, epoch};
  }
  static uint64_t generationOf(std::string_view name) { return candidate(name).first; }

  static uint64_t fileEpoch(std::string_view name) {
    if (auto id = candidate(name); id.first) return id.second;
    if (name.ends_with(".tmp")) name.remove_suffix(4);
    auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return 0;
    auto end = name.find('_', dot);
    return Postings::parseEpoch(name.substr(dot + 1, end == std::string_view::npos ? end : end - dot - 1));
  }

  static bool indexFile(std::string_view name) {
    if (name.ends_with(".tmp")) return false;
    if (candidate(name).first || SchemaBinding::isFile(name)) return true;
    if (!name.starts_with("s")) return false;
    auto dot = name.find('.'), underscore = name.find('_');
    if (dot == std::string_view::npos || underscore == std::string_view::npos || dot >= underscore) return false;
    auto sortable = [](std::string_view text) {
      return !text.empty() && Postings::getSortableString(Postings::parseSortableString(text)) == text;
    };
    auto seg = name.substr(1, dot - 1);
    if (!sortable(seg) || !Postings::parseEpoch(name.substr(dot + 1, underscore - dot - 1))) return false;
    auto suffix = name.substr(underscore + 1);
    if (!suffix.starts_with('_')) return sortable(suffix);
    suffix.remove_prefix(1);
    if (suffix.starts_with('L') && suffix.find('_') == std::string_view::npos) suffix.remove_prefix(1);
    else {
      auto file = suffix.rfind('_');
      if (file == std::string_view::npos || !sortable(suffix.substr(file + 1))) return false;
      suffix = suffix.substr(0, file);
      auto generation = suffix.rfind('_');
      if (generation == std::string_view::npos || generation == 0) return false;
      suffix.remove_prefix(generation + 1);
    }
    dot = suffix.find('.');
    return dot != std::string_view::npos && sortable(suffix.substr(0, dot))
        && Postings::parseEpoch(suffix.substr(dot + 1));
  }

  static api::IndexInfo decode(const Bytes& bytes, std::pmr::memory_resource& arena) {
    api::IndexInfo info;
    if (!bytes || !api::decode(info, api::copyToPaddedInput(*bytes, arena), arena)) {
      throw std::runtime_error("Invalid snapshot manifest");
    }
    if (info.version != IndexFormat::VERSION) throw IndexFormatError(info.version);
    if (!info.schema || info.schema_file.name.empty() || info.incarnation.empty()) {
      throw std::runtime_error("Invalid snapshot manifest");
    }
    return info;
  }

  static std::vector<FileDescriptor> files(const Bytes& bytes) {
    if (!bytes) return {};
    std::pmr::monotonic_buffer_resource arena;
    return filesOf(decode(bytes, arena));
  }

  static Bytes read(Directory& dir, uint64_t gen, uint64_t epoch) {
    auto file = dir.openFile(name(gen, epoch));
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
      auto info = decode(bytes, arena);
      if (info.index_gen != gen || info.writer_epoch != epoch) return {};
    } catch (const IndexFormatError&) {
      throw; // not torn: falling back to an older generation would not help
    } catch (const std::runtime_error&) { return {}; }
    return bytes;
  }

  // The files of `dir`, listed on first use and kept: one open lists once
  // however many of its steps need the listing.
  const std::vector<Directory::FileInfo>& listingOf(Directory& dir) {
    if (!listed) {
      dir.listFiles(listing);
      listed = true;
    }
    return listing;
  }

  // Only STATE selects a root. Scans are recovery fallback, never publication:
  // a healthy tip is read without listing the directory.
  static Manifest load(Directory& dir, const IncarnationState* claimed = nullptr) {
    auto readCandidate = [&](uint64_t gen, uint64_t epoch) -> Bytes {
      try { return read(dir, gen, epoch); }
      catch (const FileIOException&) { return {}; }
    };
    auto state = claimed ? std::optional(*claimed) : IncarnationState::read(dir);
    if (!state) throw WriterSupersededError();
    for (;;) {
      Manifest result;
      if (state->value.tip.empty()) return result;
      auto [gen, epoch] = candidate(std::string(kPrefix) + state->value.tip);
      if (!gen) throw std::runtime_error("invalid STATE tip");
      result.highestGeneration = gen;
      if (auto bytes = readCandidate(gen, epoch)) {
        result.bytes = std::move(bytes);
        result.generation = gen;
        result.epoch = epoch;
        result.names.push_back(name(gen, epoch));
        return result;
      }
      auto latest = IncarnationState::read(dir);
      if (!latest) throw WriterSupersededError();
      if (!claimed && latest->version != state->version) { state = std::move(latest); continue; }
      std::vector<std::pair<uint64_t, uint64_t>> older;
      std::map<std::string, uint64_t> sizes;
      for (const auto& file : result.listingOf(dir)) {
        sizes.emplace(file.name, file.size);
        auto id = candidate(file.name);
        if (id.first && id.first < gen) older.push_back(id);
      }
      // Higher (gen, epoch) favors lineage candidates; writer-open sweeps remove older-epoch strays.
      // An unswept, never-published candidate can still win if every lineage candidate above it is unusable.
      std::ranges::sort(older, std::greater<>());
      for (auto [oldGen, oldEpoch] : older) {
        auto bytes = readCandidate(oldGen, oldEpoch);
        if (!bytes) continue;
        bool present = true;
        for (const auto& file : files(bytes)) {
          auto entry = sizes.find(file.name);
          if (entry == sizes.end() || entry->second != file.size) { present = false; break; }
        }
        if (!present) continue;
        result.bytes = std::move(bytes);
        result.generation = oldGen;
        result.epoch = oldEpoch;
        result.names.push_back(name(oldGen, oldEpoch));
        return result;
      }
      throw std::runtime_error("No valid snapshot manifest");
    }
  }

  // A candidate is immutable. An identical retry re-establishes durability;
  // it becomes authoritative only when the caller advances STATE.
  static void commit(Directory& dir, uint64_t gen, const std::vector<std::byte>& bytes, uint64_t epoch) {
    {
      std::pmr::monotonic_buffer_resource arena;
      api::IndexInfo info;
      if (api::decode(info, api::copyToPaddedInput(bytes, arena), arena)) dir.expectSynced(filesOf(info));
    }
    std::array<std::string, 1> root{name(gen, epoch)};
    auto contents = withFooter(bytes);
    if (dir.publishFile(root[0], contents, Directory::PublishMode::CREATE_ONLY)) return;
    auto existing = dir.openFile(root[0]);
    auto body = existing ? existing->read() : std::string_view{};
    if (body.size() != contents.size() || memcmp(body.data(), contents.data(), contents.size())) {
      throw std::runtime_error("manifest candidate name already contains different bytes");
    }
    dir.sync(root);
  }

  // A follower can repair a corrupt local copy from the authoritative bytes.
  static void install(Directory& dir, const Bytes& bytes) {
    std::pmr::monotonic_buffer_resource arena;
    auto info = decode(bytes, arena);
    dir.expectSynced(filesOf(info));
    auto state = IncarnationState::read(dir);
    if (!state) throw WriterSupersededError();
    dir.publishFile(name(info.index_gen, info.writer_epoch), withFooter(*bytes), Directory::PublishMode::REPLACE);
    IncarnationState::install(dir, tip(info.index_gen, info.writer_epoch));
  }

  static std::vector<std::byte> withFooter(const std::vector<std::byte>& bytes) {
    std::vector<std::byte> result(bytes);
    std::array<uint64_t, 2> footer{bytes.size(), XXH3_64bits(bytes.data(), bytes.size())};
    auto encoded = std::as_bytes(std::span(footer));
    result.insert(result.end(), encoded.begin(), encoded.end());
    return result;
  }

  static void write(Directory& dir, uint64_t gen, const std::vector<std::byte>& bytes, uint64_t epoch) {
    write(dir, name(gen, epoch), bytes);
  }
  static void write(Directory& dir, const std::string& fileName, const std::vector<std::byte>& bytes) {
    auto contents = withFooter(bytes);
    Directory::FileCreateOptions options; options.expectedSize = contents.size();
    auto file = dir.createFile(fileName, options);
    OutputStream out(file.get());
    out.write(contents.data(), contents.size());
    out.close();
    dir.finishFile(*file);
  }
};
} // namespace luxir
