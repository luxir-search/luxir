// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Promotion.h"
#include "ReplicationState.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

namespace luxir {

std::string copySnapshot(CollectionStorage& storage, std::string_view from, const Manifest& manifest) {
  auto old = storage.open(from);
  auto incarnation = newUuid();
  auto next = storage.create(incarnation);
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(manifest.bytes, arena);
  auto files = filesOf(info);
  next->reuseFiles(*old, files, files);
  std::array<std::string, 1> directory{"."};
  next->sync(directory);
  info.incarnation = incarnation;
  info.index_gen = manifest.highestGeneration + 1;
  info.commit_time++;
  std::vector<std::byte> bytes;
  if (!api::encode(info, bytes)) throw std::runtime_error("Failed to encode copied snapshot");
  Manifest::commit(*next, info.index_gen, bytes);
  return incarnation;
}

namespace {
bool validTarget(CollectionStorage& storage, const std::string& incarnation) {
  try {
    auto incarnations = storage.incarnations();
    if (!isUuid(incarnation) || std::ranges::find(incarnations, incarnation) == incarnations.end()) return false;
    auto target = Manifest::load(*storage.open(incarnation));
    if (!target.bytes) return false;
    std::pmr::monotonic_buffer_resource arena;
    auto info = Manifest::decode(target.bytes, arena);
    if (info.incarnation != incarnation) return false;
    for (const auto& file : filesOf(info)) {
      auto listed = std::ranges::find_if(target.listing, [&](const auto& entry) { return entry.name == file.name; });
      if (listed == target.listing.end() || listed->size != file.size) return false;
    }
    return true;
  } catch (const std::exception& e) {
    LOG_WARN("Cannot validate promotion target for '{}': {}", storage.name(), e.what());
    return false;
  }
}
}

std::map<std::string, std::string> promote(DirectoryFactory& factory, Directory& metadata, ReplicationState& state) {
  std::map<std::string, std::string> errors;
  for (const auto& name : factory.collections()) {
    try {
      auto storage = factory.collection(name);
      auto selected = storage.current();
      if (!selected) continue;
      auto& promoted = state.collections[name].promoted;
      if (!promoted.empty() && !validTarget(storage, promoted)) promoted.clear();
      if (promoted.empty()) {
        Signal::emit("replicationPromotingCollection", (void*)&name);
        promoted = copySnapshot(storage, *selected, Manifest::load(*storage.open(*selected)));
        state.write(metadata);
      }
      if (*selected != promoted) storage.select(promoted);
    } catch (const std::exception& e) {
      errors.emplace(name, "promotion failed: " + std::string(e.what()));
    }
  }
  if (errors.empty()) {
    metadata.deleteFile("replication.json");
    std::array<std::string, 1> directory{"."};
    metadata.sync(directory);
  }
  return errors;
}

}
