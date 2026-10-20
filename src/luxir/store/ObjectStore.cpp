// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/store/ObjectStore.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace luxir {

std::vector<std::string> ObjectStore::listDirectories(std::string_view prefix) {
  std::set<std::string> names;
  for (const auto& object : list(prefix)) {
    std::string_view rest = std::string_view(object.name).substr(prefix.size());
    auto slash = rest.find('/');
    if (slash != std::string_view::npos) names.emplace(rest.substr(0, slash));
  }
  return {names.begin(), names.end()};
}

namespace {
std::mutex namedMutex;
std::map<std::string, std::shared_ptr<RamObjectStore>, std::less<>>& namedStores() {
  static std::map<std::string, std::shared_ptr<RamObjectStore>, std::less<>> stores;
  return stores;
}
}  // namespace

std::shared_ptr<RamObjectStore> RamObjectStore::named(std::string_view name) {
  std::lock_guard lock(namedMutex);
  auto& stores = namedStores();
  auto it = stores.find(name);
  if (it == stores.end()) it = stores.emplace(std::string(name), std::make_shared<RamObjectStore>(std::string(name))).first;
  return it->second;
}

void RamObjectStore::drop(std::string_view name) {
  std::lock_guard lock(namedMutex);
  auto& stores = namedStores();
  if (auto it = stores.find(name); it != stores.end()) stores.erase(it);
}

void RamObjectStore::record(std::string_view op, std::string_view name, uint64_t offset, uint64_t length) {
  // Caller holds the mutex.
  requests_.fetch_add(1, std::memory_order_relaxed);
  log.push_back({std::string(op), std::string(name), offset, length});
}

std::vector<ObjectStore::ObjectInfo> RamObjectStore::list(std::string_view prefix) {
  std::lock_guard lock(mutex);
  record("list", prefix, 0, 0);
  std::vector<ObjectInfo> result;
  for (auto it = objects.lower_bound(prefix); it != objects.end() && it->first.starts_with(prefix); ++it) {
    result.push_back({it->first, (uint64_t)it->second.size()});
  }
  return result;
}

std::optional<uint64_t> RamObjectStore::size(std::string_view name) {
  std::lock_guard lock(mutex);
  record("size", name, 0, 0);
  auto it = objects.find(name);
  if (it == objects.end()) return std::nullopt;
  return (uint64_t)it->second.size();
}

void RamObjectStore::readRange(std::string_view name, uint64_t offset, std::span<char> out) {
  std::lock_guard lock(mutex);
  record("read", name, offset, out.size());
  auto it = objects.find(name);
  if (it == objects.end()) throw std::runtime_error("object does not exist: " + std::string(name));
  const std::string& bytes = it->second;
  if (offset > bytes.size() || out.size() > bytes.size() - offset) {
    throw std::runtime_error("short read of object " + std::string(name));
  }
  if (!out.empty()) std::memcpy(out.data(), bytes.data() + offset, out.size());
  bytesRead_.fetch_add(out.size(), std::memory_order_relaxed);
}

void RamObjectStore::put(std::string_view name, std::string_view bytes, bool ifAbsent) {
  std::lock_guard lock(mutex);
  record("put", name, 0, bytes.size());
  auto it = objects.find(name);
  if (it != objects.end()) {
    if (ifAbsent) throw std::runtime_error("object already exists: " + std::string(name));
    it->second.assign(bytes);
  } else {
    objects.emplace(std::string(name), std::string(bytes));
  }
  bytesWritten_.fetch_add(bytes.size(), std::memory_order_relaxed);
}

bool RamObjectStore::remove(std::string_view name) {
  std::lock_guard lock(mutex);
  record("remove", name, 0, 0);
  auto it = objects.find(name);
  if (it == objects.end()) return false;
  objects.erase(it);
  return true;
}

void RamObjectStore::copy(std::string_view from, std::string_view to) {
  std::lock_guard lock(mutex);
  record("copy", from, 0, 0);
  auto it = objects.find(from);
  if (it == objects.end()) throw std::runtime_error("object does not exist: " + std::string(from));
  std::string bytes = it->second;
  objects[std::string(to)] = std::move(bytes);
}

uint64_t RamObjectStore::requests(std::string_view op, std::string_view prefix) const {
  std::lock_guard lock(mutex);
  return (uint64_t)std::ranges::count_if(log, [&](const Request& r) { return r.op == op && r.name.starts_with(prefix); });
}

std::vector<RamObjectStore::Request> RamObjectStore::requestLog() const {
  std::lock_guard lock(mutex);
  return log;
}

uint64_t RamObjectStore::bytesRead(std::string_view prefix) const {
  std::lock_guard lock(mutex);
  uint64_t total = 0;
  for (const auto& r : log) if (r.op == "read" && r.name.starts_with(prefix)) total += r.length;
  return total;
}

void RamObjectStore::resetStats() {
  std::lock_guard lock(mutex);
  log.clear();
  requests_ = 0;
  bytesRead_ = 0;
  bytesWritten_ = 0;
}

uint64_t RamObjectStore::storedBytes(std::string_view prefix) {
  std::lock_guard lock(mutex);
  uint64_t total = 0;
  for (auto it = objects.lower_bound(prefix); it != objects.end() && it->first.starts_with(prefix); ++it) total += it->second.size();
  return total;
}

}  // namespace luxir
