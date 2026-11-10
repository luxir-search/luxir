// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "CommitSnapshotRegistry.h"
#include "luxir/store/VerifiedDirectory.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

std::shared_ptr<const CommitSnapshot> CommitSnapshotRegistry::resolve(CommitSnapshot::Bytes bytes, Directory& source) {
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(bytes, arena);
  auto schema = schemas->load(source, info);
  return CommitSnapshot::fromBytes(std::move(bytes), std::move(schema));
}

void CommitSnapshotRegistry::publish(std::shared_ptr<const CommitSnapshot> snapshot,
                                     std::shared_ptr<IndexReader> opened) {
  if (retention.count()) {
    auto previous = this->snapshot();
    if (previous && previous->manifestName() != snapshot->manifestName()) {
      if (auto until = retentionUntil(snapshot->commitTime)) retain(*previous, *until);
    }
  }
  // With retention disabled, publication takes no pin lock.
  readers.publish(std::move(snapshot), std::move(opened));
  notifyChange();
  // Publication also ends expired holds: the periodic expiry runs only with HTTP.
  if (retention.count()) expire();
}

std::optional<CommitSnapshotRegistry::Clock::time_point> CommitSnapshotRegistry::retentionUntil(uint64_t successorTime) {
  if (!retention.count()) return {};
  uint64_t duration = (uint64_t)retention.count() * 1000;
  uint64_t expiry = successorTime > UINT64_MAX - duration ? UINT64_MAX : successorTime + duration;
  auto wall = std::chrono::duration_cast<std::chrono::microseconds>(wallNow().time_since_epoch()).count();
  uint64_t current = (uint64_t)std::max((int64_t)0, (int64_t)wall);
  if (expiry <= current) return {};
  auto time = now();
  auto maximum = std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - time).count();
  if (expiry - current >= (uint64_t)maximum) return Clock::time_point::max();
  return time + std::chrono::microseconds(expiry - current);
}

void CommitSnapshotRegistry::retain(const CommitSnapshot& snapshot, Clock::time_point until) {
  std::lock_guard lock(mutex);
  if (closed) return;
  auto hold = [&](const std::string& name, uint64_t size) {
    auto& ref = files.try_emplace(name, FileRef{size}).first->second;
    if (!ref.until && ref.retired && ref.pins) counters.retainedBytes -= ref.size;
    ref.until = std::max(ref.until.value_or(until), until);
  };
  for (const auto& file : snapshot.files) hold(file.name, file.size);
  hold(snapshot.manifestName(), snapshot.bytes->size() + 16);
}

void CommitSnapshotRegistry::retainHistory(std::span<const Directory::FileInfo> listing, const CommitSnapshot& current) {
  if (!retention.count()) return;
  std::vector<std::shared_ptr<const CommitSnapshot>> history;
  for (const auto& file : listing) {
    auto [generation, epoch] = Manifest::candidate(file.name);
    if (!generation || generation >= current.id.index_gen) continue;
    if (auto bytes = Manifest::read(dir, generation, epoch)) history.push_back(CommitSnapshot::fromBytes(std::move(bytes)));
  }
  std::ranges::sort(history, std::greater<>(), [](const auto& snapshot) { return snapshot->id.index_gen; });
  uint64_t successorTime = current.commitTime;
  for (size_t i = 0; i < history.size();) {
    size_t end = i + 1;
    while (end < history.size() && history[end]->id.index_gen == history[i]->id.index_gen) end++;
    auto until = retentionUntil(successorTime);
    successorTime = 0;
    for (; i < end; i++) {
      if (until) retain(*history[i], *until);
      // Competing candidates of one generation are indistinguishable from
      // the listing. The latest timestamp conservatively bounds their hold.
      successorTime = std::max(successorTime, history[i]->commitTime);
    }
  }
}

void CommitSnapshotRegistry::commit(std::shared_ptr<const CommitSnapshot> snapshot, std::shared_ptr<IndexReader> opened) {
  auto previous = this->snapshot();
  Manifest::install(dir, snapshot->bytes);
  Signal::emit("snapshotRootDurable", this);
  publish(snapshot, std::move(opened));
  // Best-effort: obsolete names that reappear after a crash are unreferenced.
  try {
    if (previous) {
      auto names = obsoleteFiles(*previous, *snapshot);
      if (previous->id.index_gen != snapshot->id.index_gen) names.push_back(previous->manifestName());
      retire(names);
    }
  } catch (const std::exception& e) { LOG_WARN("Snapshot retirement failed: {}", e.what()); }
}

void CommitSnapshotRegistry::notifyChange() noexcept {
  if (onChange) onChange();
}

void CommitSnapshotRegistry::fail() noexcept {
  close();
  notifyChange();
}

void CommitSnapshotRegistry::openLocalSnapshot(bool verify) {
  auto manifest = Manifest::load(dir);
  auto changed = [&] {
    auto latest = Manifest::load(dir);
    if (!latest.bytes || *latest.bytes == *manifest.bytes) return false;
    manifest = std::move(latest);
    return true;
  };
  for (;;) {
    if (!manifest.bytes) throw ReadOnlyError("collection has no snapshot");
    auto snapshot = CommitSnapshot::fromBytes(manifest.bytes);
    std::shared_ptr<IndexReader> reader;
    {
      FileVerification verification;
      try { verification.open(dir, snapshot->files, verify); }
      catch (...) {
        if (!changed()) throw;
        continue;
      }
      // Verify before mapping or parsing any checked file. A hash failure is
      // final; successful handles survive retirement and supply the mappings.
      verification.run();
      try {
        VerifiedDirectory source(dir, verification);
        snapshot = resolve(snapshot->bytes, source);
        reader = readers.prepare(*snapshot, source);
      } catch (...) {
        // Full holds every file: a prepare failure is final. Under none an
        // unchecked file may have been retired while hashing, so retry it.
        if (verify || !changed()) throw;
        continue;
      }
    }
    publish(std::move(snapshot), std::move(reader));
    return;
  }
}

void CommitSnapshotRegistry::sweepOrphans(const Manifest& manifest, uint64_t writerEpoch) {
  if (!manifest.bytes) return;
  if (manifest.generation != manifest.highestGeneration) {
    LOG_ERROR("Recovered snapshot {} below newest generation {}; skipping orphan cleanup",
              manifest.generation, manifest.highestGeneration);
    return;
  }
  auto listing = manifest.listing;
  std::erase_if(listing, [&](const auto& file) {
    auto epoch = Manifest::fileEpoch(file.name);
    return !epoch || epoch >= writerEpoch;
  });
  retireUnreferenced(listing);
}

void CommitSnapshotRegistry::sweepOrphans() {
  std::vector<Directory::FileInfo> listing;
  dir.listFiles(listing);
  retireUnreferenced(listing);
}

void CommitSnapshotRegistry::retireUnreferenced(std::span<const Directory::FileInfo> listing) {
  boost::unordered_flat_set<std::string> retained;
  auto installed = snapshot();
  retainHistory(listing, *installed);
  retained.insert(installed->manifestName());
  for (const auto& file : installed->files) retained.insert(file.name);
  std::vector<std::string> obsolete;
  for (const auto& file : listing) {
    if (Manifest::indexFile(file.name) && !retained.contains(file.name)) {
      obsolete.push_back(file.name);
    }
  }
  retire(obsolete);
}

std::vector<std::string> CommitSnapshotRegistry::obsoleteFiles(const CommitSnapshot& previous,
    const CommitSnapshot& next, boost::unordered_flat_set<std::string> retained) {
  for (const auto& file : next.files) retained.insert(file.name);
  std::vector<std::string> result;
  for (const auto& file : previous.files) {
    if (!retained.contains(file.name)) result.push_back(file.name);
  }
  return result;
}

std::shared_ptr<const CommitSnapshot> CommitSnapshotRegistry::acquire(std::stop_token* cancellation) {
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlink(retired); });
  std::lock_guard lock(mutex);
  expireLocked(retired);
  auto commit = snapshot();
  if (closed || !commit) throw SnapshotExpiredError();
  if (auto it = pins.find(commit->id); it != pins.end()) {
    it->second.lastRead = now();
    if (cancellation) *cancellation = it->second.cancellation.get_token();
    return commit;
  }
  auto time = now();
  Pin pin{commit, {}, time, time, {}};
  if (cancellation) *cancellation = pin.cancellation.get_token();
  size_t added = 0;
  try {
    // Transfer lookup belongs to the pin, so local publications pay
    // nothing for it. Views borrow immutable names from its owning snapshot.
    pin.files.reserve(commit->files.size());
    for (const auto& file : commit->files) {
      pin.files.emplace(file.name, &file);
      auto [it, inserted] = files.try_emplace(file.name, FileRef{file.size});
      it->second.pins++;
      added++;
    }
    pins.emplace(commit->id, std::move(pin));
  } catch (...) {
    for (size_t i = 0; i < added; i++) {
      auto it = files.find(commit->files[i].name);
      if (--it->second.pins == 0 && !it->second.until) files.erase(it);
    }
    throw;
  }
  return commit;
}

std::shared_ptr<InputFile> CommitSnapshotRegistry::openFile(const CommitId& id, std::string_view name, std::stop_token* cancellation) {
  struct PinnedFile {
    std::shared_ptr<InputFile> file;
    std::shared_ptr<Pin::OpenFiles> lease;
  };
  auto owner = std::make_shared<PinnedFile>();
  // Validate against revocation before opening; eviction may then revoke the
  // pin, but cannot unlink until this in-flight open owns its file.
  std::lock_guard retirementLock(retirementMutex);
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlinkFiles(retired); });
  {
    std::lock_guard lock(mutex);
    expireLocked(retired);
    auto it = pins.find(id);
    if (closed || it == pins.end()) throw SnapshotExpiredError();
    if (!it->second.files.contains(name)) {
      throw ApiError(ErrorKind::NOT_FOUND, "file_not_in_snapshot", "file is not in the pinned snapshot");
    }
    if (cancellation) *cancellation = it->second.cancellation.get_token();
    owner->lease = it->second.openFiles;
  }
  auto file = dir.openFile(name, true);
  if (!file) throw std::runtime_error("pinned snapshot file is missing");
  owner->file = file;
  return std::shared_ptr<InputFile>(std::move(owner), file.get());
}

void CommitSnapshotRegistry::requireHeld(const FileDescriptor& descriptor) {
  std::lock_guard lock(mutex);
  auto published = snapshot();
  if (published != indexed) {
    indexedFiles.clear();
    if (published) for (const auto& file : published->files) indexedFiles.emplace(file.name, &file);
    indexed = published;
  }
  auto holds = [&](const auto& files) {
    auto it = files.find(descriptor.name);
    return it != files.end() && *it->second == descriptor;
  };
  bool held = !closed && (holds(indexedFiles) || std::ranges::any_of(pins, [&](const auto& pin) { return holds(pin.second.files); }));
  if (!held) throw ApiError(ErrorKind::NOT_FOUND, "file_not_held", "no current or pinned snapshot holds this file");
}

std::shared_ptr<InputFile> CommitSnapshotRegistry::openFile(const FileDescriptor& descriptor) {
  // Joined with retirement, so a referenced file cannot be unlinked between
  // the check and the open; once open, its bytes outlive an unlink.
  std::lock_guard retirementLock(retirementMutex);
  requireHeld(descriptor);
  auto file = dir.openFile(descriptor.name, true);
  if (!file) throw std::runtime_error("held snapshot file is missing");
  return file;
}

std::shared_ptr<ChecksumReader> CommitSnapshotRegistry::openChecksum(const FileDescriptor& descriptor, bool direct) {
  std::lock_guard retirementLock(retirementMutex);
  requireHeld(descriptor);
  return dir.openChecksum(descriptor.name, direct);
}

bool CommitSnapshotRegistry::touch(const CommitId& id, uint64_t bytes) {
  std::lock_guard lock(mutex);
  auto it = pins.find(id);
  if (closed || it == pins.end()) return false;
  auto time = now();
  if (time - it->second.lastRead >= policy.idleTimeout) return false;
  if (bytes != 0) it->second.lastRead = time;
  return true;
}

void CommitSnapshotRegistry::releaseLocked(const CommitId& id, std::vector<std::string>& retired) {
  auto pin = pins.find(id);
  if (pin == pins.end()) return;
  retired.reserve(retired.size() + pin->second.snapshot->files.size());
  for (const auto& file : pin->second.snapshot->files) {
    auto it = files.find(file.name);
    auto& ref = it->second;
    if (--ref.pins == 0) {
      if (ref.until) continue;
      if (ref.retired) {
        counters.retainedBytes -= ref.size;
        retired.push_back(std::move(files.extract(it).key()));
      } else files.erase(it);
    }
  }
  pin->second.cancellation.request_stop();
  pins.erase(pin);
}

void CommitSnapshotRegistry::enforceBudgetLocked(std::vector<std::string>& retired) {
  while (counters.retainedBytes > policy.retainedBytes && !pins.empty()) {
    auto oldest = std::ranges::min_element(pins, {}, [](const auto& entry) { return entry.second.created; });
    releaseLocked(oldest->first, retired);
    counters.budgetDrops++;
  }
}

auto CommitSnapshotRegistry::oldestReclaimableLocked() -> decltype(pins)::iterator {
  auto oldest = pins.end();
  if (closed || counters.retainedBytes == 0) return oldest;
  auto published = snapshot();
  for (auto it = pins.begin(); it != pins.end(); ++it) {
    const auto& pin = it->second;
    if ((published && it->first == published->id) || pin.openFiles.use_count() != 1) continue;
    if (oldest != pins.end() && pin.created >= oldest->second.created) continue;
    for (const auto& file : pin.snapshot->files) {
      auto ref = files.find(file.name);
      if (ref != files.end() && ref->second.retired && !ref->second.until && ref->second.size) {
        oldest = it;
        break;
      }
    }
  }
  return oldest;
}

std::optional<CommitSnapshotRegistry::Clock::time_point> CommitSnapshotRegistry::oldestReclaimablePin() {
  std::lock_guard lock(mutex);
  auto oldest = oldestReclaimableLocked();
  if (oldest == pins.end()) return {};
  return oldest->second.created;
}

bool CommitSnapshotRegistry::reclaimOldestPin() {
  std::vector<std::string> retired;
  {
    std::lock_guard lock(mutex);
    auto oldest = oldestReclaimableLocked();
    if (oldest == pins.end()) return false;
    releaseLocked(oldest->first, retired);
    counters.budgetDrops++;
  }
  unlink(retired);
  return true;
}

bool CommitSnapshotRegistry::testEvictOldest() {
  std::vector<std::string> retired;
  {
    std::lock_guard lock(mutex);
    if (closed || pins.empty()) return false;
    auto oldest = std::ranges::min_element(pins, {}, [](const auto& entry) { return entry.second.created; });
    releaseLocked(oldest->first, retired);
    counters.budgetDrops++;
  }
  Signal::emit("snapshotPinDropped", this);
  unlink(retired);
  return true;
}

void CommitSnapshotRegistry::expireLocked(std::vector<std::string>& retired) {
  if (closed) return;
  auto time = now();
  for (auto it = files.begin(); retention.count() && it != files.end();) {
    auto& ref = it->second;
    if (!ref.until || *ref.until > time) { ++it; continue; }
    ref.until.reset();
    if (ref.pins) {
      if (ref.retired) counters.retainedBytes += ref.size;
      ++it;
    } else {
      if (ref.retired) retired.push_back(it->first);
      it = files.erase(it);
    }
  }
  for (auto it = pins.begin(); it != pins.end();) {
    auto pin = it++;
    if (time - pin->second.lastRead >= policy.idleTimeout) {
      releaseLocked(pin->first, retired);
      counters.idleDrops++;
    }
  }
  if (retention.count()) enforceBudgetLocked(retired);
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

CommitSnapshotRegistry::Stats CommitSnapshotRegistry::stats(bool expire) {
  std::vector<std::string> retired;
  auto cleanup = scope_guard([&] { unlink(retired); });
  std::lock_guard lock(mutex);
  if (expire) expireLocked(retired);
  auto result = counters;
  result.pins = pins.size();
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
    auto published = snapshot();
#endif
    for (const auto& name : names) {
      assert(!published || std::ranges::none_of(published->files,
          [&](const auto& file) { return file.name == name; }));
      auto it = files.find(name);
      if (it == files.end()) retired.push_back(name);
      else if (!it->second.retired) {
        it->second.retired = true;
        if (!it->second.until && it->second.pins) counters.retainedBytes += it->second.size;
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
      unpinned = pins.empty() && files.empty();
    }
    // New pins can only acquire current, which cannot contain this dead
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
  readers.close();
  detach();
}

void CommitSnapshotRegistry::detach() noexcept {
  {
    std::lock_guard lock(mutex);
    closed = true;
  }
  // Wait out directory access. A release already holding collected names can
  // arrive later, but unlink() will see closed and never touch a reused path.
  std::lock_guard retirementLock(retirementMutex);
  // Move retained-file bookkeeping out without allocating during cleanup.
  decltype(files) retired;
  {
    std::lock_guard lock(mutex);
    for (auto& [id, pin] : pins) pin.cancellation.request_stop();
    pins.clear();
    retired.swap(files);
    counters.retainedBytes = 0;
    // Admitted metadata requests can still hold the collection after close.
    // Keep its immutable current snapshot until the owner is destroyed.
  }
  for (const auto& [name, ref] : retired) {
    if (ref.retired && !ref.until) unlinkFiles({&name, 1});
  }
}

}
