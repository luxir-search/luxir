// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Promotion.h"
#include "Collection.h"
#include "ReplicationState.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include "luxir/index/CommitSnapshot.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/store/CorruptionMarker.h"

namespace luxir {

std::string copySnapshot(TenantCatalog& storage, std::string_view name, std::string_view from, const Manifest& manifest,
                          bool verify, IncarnationState* claimed) {
  auto source = storage.owned(from);
  if (source.name != name || source.state != "active") throw CollectionUnavailableError("snapshot source is no longer selected");
  auto old = storage.directory(from);
  auto incarnation = storage.reserve(name);
  auto next = storage.directory(incarnation);
  auto state = storage.claim(name, incarnation, "reserved");
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(manifest.bytes, arena);
  auto files = filesOf(info);
  next->reuseFiles(*old, files, files);
  auto suspects = CorruptionMarker::read(*old);
  std::erase_if(suspects, [&](const auto& name) {
    return std::ranges::none_of(files, [&](const auto& file) { return file.name == name; });
  });
  if (!suspects.empty()) CorruptionMarker::write(*next, suspects, "copied snapshot suspect files");
  // Under verify-on-open=full the linked files are read once, here, before
  // a writer builds on them. A mismatch names the file; the follower-bound
  // directory keeps the damaged copy for a repair.
  if (verify) verifyFiles(*next, files);
  // The new root may reference only files that are present in full.
  std::vector<Directory::FileInfo> listing;
  next->listFiles(listing);
  for (const auto& file : files) {
    auto present = std::ranges::find_if(listing, [&](const auto& entry) { return entry.name == file.name; });
    if (present == listing.end() || present->size != file.size) throw std::runtime_error("snapshot copy is missing " + file.name);
  }
  std::vector<std::string> names;
  for (const auto& file : files) names.push_back(file.name);
  next->sync(names);
  info.incarnation = incarnation;
  info.index_gen = manifest.highestGeneration + 1;
  info.commit_time++;
  info.writer_epoch = state.value.epoch;
  std::vector<std::byte> bytes;
  if (!api::encode(info, bytes)) throw std::runtime_error("Failed to encode copied snapshot");
  Manifest::commit(*next, info.index_gen, bytes, info.writer_epoch);
  state.advance(*next, Manifest::tip(info.index_gen, info.writer_epoch));
  if (claimed) *claimed = std::move(state);
  return incarnation;
}

namespace {
bool validTarget(TenantCatalog& storage, std::string_view name, const std::string& incarnation) {
  try {
    auto incarnations = storage.incarnations(name);
    if (!CommitId::validIncarnation(incarnation) || std::ranges::find(incarnations, incarnation) == incarnations.end()) return false;
    auto target = Manifest::load(*storage.directory(incarnation));
    if (!target.bytes) return false;
    std::pmr::monotonic_buffer_resource arena;
    auto info = Manifest::decode(target.bytes, arena);
    if (info.incarnation != incarnation) return false;
    for (const auto& file : filesOf(info)) {
      auto listed = std::ranges::find_if(target.listing, [&](const auto& entry) { return entry.name == file.name; });
      if (listed == target.listing.end() || listed->size != file.size) return false;
    }
    return true;
  } catch (const IndexFormatError&) {
    throw;
  } catch (const std::exception& e) {
    LOG_WARN("Cannot validate promotion target for '{}': {}", name, e.what());
    return false;
  }
}
}

bool promote(const std::filesystem::path& dataDir, std::ostream& output, bool verify) {
  if (!std::filesystem::exists(dataDir / "node" / "replication.json")) {
    output << "Promote failed: " << dataDir.string() << " is not a follower data directory\n";
    return false;
  }
  // Offline tool for a follower filesystem data directory, so it opens the FS backend directly.
  FSDirFactory factory(dataDir);
  TenantCatalog::initialize(factory, true);
  auto node = newUuid();
  TenantCatalog::recover(factory, true, node);
  auto metadata = factory.directory(StorageRoot::NODE_PRIVATE, "");
  auto state = ReplicationState::read(*metadata->openFile("replication.json"));
  size_t promotedCount = 0, failed = 0;
  for (const auto& id : TenantCatalog::collections(factory)) {
    try {
      TenantCatalog storage(factory, id.tenant, node);
      auto selected = storage.current(id.name);
      if (!selected) continue;
      auto& history = state.tenants[id.tenant].incarnations;
      auto saved = std::ranges::find_if(history, [&](const auto& entry) { return entry.second.promoted == *selected; });
      auto& promoted = (saved == history.end() ? history[*selected] : saved->second).promoted;
      if (!promoted.empty() && !validTarget(storage, id.name, promoted)) promoted.clear();
      if (promoted.empty()) {
        Signal::emit("replicationPromotingCollection", (void*)&id.name);
        promoted = copySnapshot(storage, id.name, *selected, Manifest::load(*storage.directory(*selected)), verify);
        state.write(*metadata);
      }
      // Re-select even if the catalog already names the target: an earlier attempt
      // may have installed it without making it durable.
      storage.activate(id.name, promoted);
      for (const auto& other : storage.incarnations(id.name)) if (other != promoted) storage.removeIncarnation(other);
      promotedCount++;
      output << id.label() << ' ' << promoted << '\n';
    } catch (const std::exception& e) {
      failed++;
      output << id.label() << " ERROR: " << e.what() << '\n';
    }
  }
  if (!failed) {
    std::array<std::string, 1> names{"replication.json"};
    metadata->deleteDurably(names);
  }
  output << "Promote: " << promotedCount << " promoted, " << failed << " failed";
  if (!failed) output << "; promoted from follower of " << state.source;
  output << '\n';
  return !failed;
}

}
