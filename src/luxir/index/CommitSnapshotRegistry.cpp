// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CommitSnapshotRegistry.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

void CommitSnapshotRegistry::publish(std::shared_ptr<const CommitSnapshot> snapshot,
                                     std::shared_ptr<IndexReader> opened) {
  // No reservation lock: a large acquire or slow file open cannot stall publication.
  if (opened) readers.installOpened(std::move(snapshot), std::move(opened));
  else readers.install(std::move(snapshot));
}

void CommitSnapshotRegistry::openLocalSnapshot() {
  auto manifest = Manifest::load(dir);
  for (;;) {
    if (!manifest.bytes) throw ReadOnlyError("collection has no snapshot");
    auto snapshot = CommitSnapshot::fromBytes(manifest.bytes);
    try {
      auto reader = readers.prepare(*snapshot);
      publish(std::move(snapshot), std::move(reader));
      return;
    } catch (...) {
      auto latest = Manifest::load(dir);
      if (!latest.bytes || *latest.bytes == *manifest.bytes) throw;
      manifest = std::move(latest);
    }
  }
}

std::shared_ptr<const CommitSnapshot> CommitSnapshotRegistry::acquire() {
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlink(retired); });
  std::lock_guard lock(mutex);
  expireLocked(retired);
  auto commit = current.load();
  if (closed || !commit) throw SnapshotExpiredError();
  if (reservations.contains(commit->id)) return commit;
  size_t added = 0;
  try {
    for (const auto& file : commit->files) {
      auto [it, inserted] = files.try_emplace(file.name, FileRef{file.size});
      it->second.pins++;
      added++;
    }
    auto time = now();
    reservations.emplace(commit->id, Reservation{commit, time, time});
  } catch (...) {
    for (size_t i = 0; i < added; i++) {
      auto it = files.find(commit->files[i].name);
      if (--it->second.pins == 0) files.erase(it);
    }
    throw;
  }
  return commit;
}

std::shared_ptr<InputFile> CommitSnapshotRegistry::openFile(const CommitId& id, std::string_view name) {
  // Validate against revocation before opening; eviction may then revoke the
  // reservation, but cannot unlink until this in-flight open owns its file.
  std::lock_guard retirementLock(retirementMutex);
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlinkFiles(retired); });
  {
    std::lock_guard lock(mutex);
    expireLocked(retired);
    auto it = reservations.find(id);
    if (closed || it == reservations.end()) throw SnapshotExpiredError();
    if (std::ranges::none_of(it->second.snapshot->files,
        [&](const auto& file) { return file.name == name; })) {
      throw std::invalid_argument("file is not in the reserved snapshot");
    }
  }
  auto file = dir.openFile(name, true);
  if (!file) throw std::runtime_error("reserved snapshot file is missing");
  return file;
}

bool CommitSnapshotRegistry::touch(const CommitId& id, uint64_t bytes) {
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlink(retired); });
  std::lock_guard lock(mutex);
  expireLocked(retired);
  auto it = reservations.find(id);
  if (closed || it == reservations.end()) return false;
  if (bytes != 0) it->second.lastRead = now();
  return true;
}

void CommitSnapshotRegistry::releaseLocked(const CommitId& id, std::vector<std::string>& retired) {
  auto pin = reservations.find(id);
  if (pin == reservations.end()) return;
  retired.reserve(retired.size() + pin->second.snapshot->files.size());
  for (const auto& file : pin->second.snapshot->files) {
    auto it = files.find(file.name);
    auto& ref = it->second;
    if (--ref.pins == 0) {
      if (ref.retired) {
        counters.retainedBytes -= ref.size;
        retired.push_back(std::move(files.extract(it).key()));
      } else files.erase(it);
    }
  }
  reservations.erase(pin);
}

void CommitSnapshotRegistry::enforceBudgetLocked(std::vector<std::string>& retired) {
  while (counters.retainedBytes > policy.retainedBytes && !reservations.empty()) {
    auto oldest = std::ranges::min_element(reservations, {}, [](const auto& entry) { return entry.second.created; });
    releaseLocked(oldest->first, retired);
    counters.budgetDrops++;
  }
}

bool CommitSnapshotRegistry::evictOldest() {
  std::vector<std::string> retired;
  {
    std::lock_guard lock(mutex);
    if (closed || reservations.empty()) return false;
    auto oldest = std::ranges::min_element(reservations, {}, [](const auto& entry) { return entry.second.created; });
    releaseLocked(oldest->first, retired);
    counters.budgetDrops++;
  }
  Signal::emit("snapshotReservationDropped", this);
  unlink(retired);
  return true;
}

void CommitSnapshotRegistry::expireLocked(std::vector<std::string>& retired) {
  if (closed || reservations.empty()) return;
  auto time = now();
  for (auto it = reservations.begin(); it != reservations.end();) {
    auto pin = it++;
    if (time - pin->second.lastRead >= policy.idleTimeout) {
      releaseLocked(pin->first, retired);
      counters.idleDrops++;
    }
  }
}

void CommitSnapshotRegistry::expire() {
  std::vector<std::string> retired;
  {
    std::lock_guard lock(mutex);
    expireLocked(retired);
  }
  unlink(retired);
}

void CommitSnapshotRegistry::setPolicy(Policy value) {
  if (value.idleTimeout.count() <= 0) throw std::invalid_argument("pin idle timeout must be positive");
  std::vector<std::string> retired;
  {
    std::lock_guard lock(mutex);
    policy = value;
    expireLocked(retired);
    enforceBudgetLocked(retired);
  }
  unlink(retired);
}

CommitSnapshotRegistry::Stats CommitSnapshotRegistry::stats() {
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlink(retired); });
  std::lock_guard lock(mutex);
  expireLocked(retired);
  auto result = counters;
  result.pins = reservations.size();
  return result;
}

void CommitSnapshotRegistry::retire(std::span<const std::string> names) {
  std::vector<std::string> retired;
  retired.reserve(names.size());
  {
    std::lock_guard lock(mutex);
    if (closed) return;
    expireLocked(retired);
#ifndef NDEBUG
    auto published = current.load();
#endif
    for (const auto& name : names) {
      assert(!published || std::ranges::none_of(published->files,
          [&](const auto& file) { return file.name == name; }));
      auto it = files.find(name);
      if (it == files.end()) retired.push_back(name);
      else if (!it->second.retired) {
        it->second.retired = true;
        counters.retainedBytes += it->second.size;
      }
    }
    enforceBudgetLocked(retired);
  }
  unlink(retired);
}

void CommitSnapshotRegistry::testReset() {
  close();
  std::lock_guard lock(mutex);
  closed = false;
  counters = {};
  current.store(nullptr);
  readers.testReset();
}

void CommitSnapshotRegistry::retirePrefix(std::string_view prefix) {
  expire();
  std::vector<std::string> names;
  {
    std::lock_guard retirementLock(retirementMutex);
    bool unpinned;
    {
      std::lock_guard lock(mutex);
      if (closed) return;
      unpinned = reservations.empty();
    }
    // New reservations can only acquire current, which cannot contain this dead
    // segment. Preserve the backend's cheap prefix erase with no pins.
    if (unpinned) {
      dir.deletePrefix(prefix);
      return;
    }
    std::vector<Directory::FileInfo> listing;
    dir.listFiles(listing);
    for (auto& file : listing) if (file.name.starts_with(prefix)) names.push_back(std::move(file.name));
  }
  retire(names);
}

void CommitSnapshotRegistry::unlink(std::span<const std::string> names) noexcept {
  if (names.empty()) return;
  std::lock_guard retirementLock(retirementMutex);
  {
    std::lock_guard lock(mutex);
    if (closed) return;
  }
  unlinkFiles(names);
}

void CommitSnapshotRegistry::unlinkFiles(std::span<const std::string> names) noexcept {
  for (const auto& name : names) {
    try {
      dir.deleteFile(name);
    } catch (const std::exception& e) {
      LOG_WARN("Snapshot file cleanup failed for {}: {}", name, e.what());
    }
  }
}

void CommitSnapshotRegistry::close() noexcept {
  {
    std::lock_guard lock(mutex);
    closed = true;
  }
  readers.close();
  // Wait out directory access. A release already holding collected names can
  // arrive later, but unlink() will see closed and never touch a reused path.
  std::lock_guard retirementLock(retirementMutex);
  // Shutdown needs no allocations even when many reservations remain.
  decltype(files) retired;
  {
    std::lock_guard lock(mutex);
    reservations.clear();
    retired.swap(files);
    counters.retainedBytes = 0;
    // Admitted metadata requests can still hold the collection after close.
    // Keep its immutable current snapshot until the owner is destroyed.
  }
  for (const auto& [name, ref] : retired) {
    if (ref.retired) unlinkFiles({&name, 1});
  }
}

}
