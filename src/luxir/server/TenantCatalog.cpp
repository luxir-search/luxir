// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "TenantCatalog.h"
#include "Collections.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include <charconv>
#include <chrono>
#include <glaze/glaze.hpp>

namespace luxir {
namespace {
constexpr auto authoritative = StorageRoot::AUTHORITATIVE;
constexpr std::string_view layout = "tenant/catalog/incarnation 2\n";
}

TenantCatalog::TenantCatalog(DirectoryFactory& factory, std::string_view tenant)
    : factory(factory), tenant(tenant), path("t/" + std::string(tenant)) {
  Collections::validateName(tenant, "tenant");
  if (auto memory = factory.memory()) tenantAccount = memory->child(std::string(tenant));
}

void TenantCatalog::initialize(DirectoryFactory& factory, bool writable) {
  auto root = factory.directory(authoritative, "");
  auto marker = root->openFile("LAYOUT");
  if (!marker) {
    if (!factory.children(authoritative, "").empty()) throw std::runtime_error("expected an empty data directory or a supported layout; reindex it");
    if (!writable) throw ReadOnlyError("data directory has no layout marker; reindex it");
    root->publishFile("LAYOUT", std::as_bytes(std::span(layout)), Directory::PublishMode::CREATE_ONLY);
    marker = root->openFile("LAYOUT");
  }
  if (!marker || marker->read() != layout) throw std::runtime_error("unsupported data directory layout; reindex it");
}

uint64_t TenantCatalog::incarnationNumber(std::string_view id) {
  uint64_t number = 0;
  auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), number, 16);
  if (id.size() != 11 || error != std::errc() || end != id.data() + id.size()
      || std::ranges::any_of(id, [](char c) { return c >= 'A' && c <= 'F'; })) {
    throw std::invalid_argument("invalid incarnation id: " + std::string(id));
  }
  return number;
}

TenantCatalog::Snapshot TenantCatalog::snapshot() {
  auto dir = factory.directory(authoritative, path, {tenantAccount});
  Snapshot result;
  result.publication = dir->readPublished("CATALOG");
  if (!result.publication) return result;
  if (glz::read_json(result.state, result.publication->bytes)) throw std::runtime_error("invalid tenant CATALOG: " + tenant);
  if (result.state.high > 0xfffffffffffULL) throw std::runtime_error("invalid catalog high-water");
  std::set<std::string> ids, active;
  for (const auto& entry : result.state.entries) {
    if (incarnationNumber(entry.id) > result.state.high || !ids.insert(entry.id).second) throw std::runtime_error("invalid catalog incarnation");
    if (entry.state != "reserved" && entry.state != "active" && entry.state != "retiring" && entry.state != "detached") throw std::runtime_error("invalid catalog entry state");
    if (entry.name) Collections::validateName(*entry.name);
    if (entry.state != "detached" && !entry.name) throw std::runtime_error("catalog entry has no name");
    if (entry.state == "active" && !active.insert(*entry.name).second) throw std::runtime_error("duplicate active catalog name");
  }
  return result;
}

TenantCatalog::State TenantCatalog::read() { return snapshot().state; }

void TenantCatalog::mutate(const std::function<bool(State&)>& mutation) {
  std::lock_guard lock(mutex);
  auto before = snapshot();
  Signal::emit("catalogRead", this);
  auto dir = factory.directory(authoritative, path, {tenantAccount});
  for (;;) {
    auto next = before.state;
    if (!mutation(next)) {
      auto latest = snapshot();
      if (before.publication.has_value() != latest.publication.has_value()
          || (before.publication && before.publication->version != latest.publication->version)) {
        before = std::move(latest);
        continue;
      }
      if (before.publication) { std::array<std::string, 1> names{"CATALOG"}; dir->sync(names); }
      return;
    }
    if (next.rev == UINT64_MAX) throw std::runtime_error("catalog revision exhausted");
    next.rev++;
    next.op = newUuid();
    std::ranges::sort(next.entries, {}, &Entry::id);
    auto bytes = glz::write_json(next).value();
    auto condition = before.publication ? Directory::PublishCondition::match(before.publication->version)
                                        : Directory::PublishCondition(Directory::PublishMode::CREATE_ONLY);
    try {
      if (dir->publishFile("CATALOG", std::as_bytes(std::span(bytes)), condition)) {
        Signal::emit("catalogPublished", this);
        return;
      }
    } catch (...) {
      auto failure = std::current_exception();
      auto observed = dir->readPublished("CATALOG");
      if (observed && observed->bytes == bytes) {
        // Visibility alone does not prove FS durability after a failed directory sync.
        std::array<std::string, 1> names{"CATALOG"};
        dir->sync(names);
        return;
      }
      std::rethrow_exception(failure);
    }
    before = snapshot(); // Re-evaluate the mutation; never resubmit stale entries.
  }
}

std::shared_ptr<StorageMemory> TenantCatalog::account(std::string_view name) {
  return tenantAccount ? tenantAccount->child(std::string(name)) : nullptr;
}
uint64_t TenantCatalog::bytes(std::string_view name) { auto a = account(name); return a ? a->bytes() : 0; }

std::shared_ptr<Directory> TenantCatalog::directory(std::string_view name, std::string_view id) {
  incarnationNumber(id);
  return factory.directory(authoritative, path + "/i/" + std::string(id), {account(name)});
}

std::optional<std::string> TenantCatalog::current(std::string_view name) {
  for (const auto& entry : read().entries) if (entry.state == "active" && entry.name == name) return entry.id;
  return {};
}
std::vector<std::string> TenantCatalog::incarnations(std::string_view name) {
  std::vector<std::string> ids;
  for (const auto& entry : read().entries) if (entry.name == name) ids.push_back(entry.id);
  return ids;
}

std::string TenantCatalog::reserve(std::string_view name, std::string_view sourceId, bool exclusive) {
  Collections::validateName(name);
  if (!sourceId.empty()) incarnationNumber(sourceId);
  std::string reserved;
  mutate([&](State& state) {
    if (exclusive && std::ranges::any_of(state.entries, [&](const auto& e) {
      return e.name == name && e.state != "retiring";
    })) throw CollectionExistsError("collection '" + std::string(name) + "' already exists");
    if (!sourceId.empty()) for (const auto& entry : state.entries) if (entry.id == sourceId) {
      if (entry.name != name || (entry.state != "reserved" && entry.state != "active")) throw std::runtime_error("incarnation is not available for installation");
      reserved = sourceId;
      return false;
    }
    auto now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t number = sourceId.empty() ? std::max(now, state.high + 1) : incarnationNumber(sourceId);
    if (number > 0xfffffffffffULL) throw std::runtime_error("incarnation ids exhausted");
    reserved = sourceId.empty() ? std::format("{:011x}", number) : std::string(sourceId);
    state.high = std::max(state.high, number);
    state.entries.push_back({reserved, std::string(name), "reserved"});
    return true;
  });
  return reserved;
}

void TenantCatalog::activate(std::string_view name, std::string_view id) {
  mutate([&](State& state) {
    auto selected = std::ranges::find(state.entries, id, &Entry::id);
    if (selected == state.entries.end() || selected->name != name || (selected->state != "reserved" && selected->state != "active")) {
      throw std::runtime_error("activation requires a reserved incarnation");
    }
    if (selected->state == "active") return false;
    for (auto& entry : state.entries) if (entry.name == name && entry.state == "active") entry.state = "retiring";
    selected->state = "active";
    return true;
  });
}

void TenantCatalog::retire(std::string_view name) {
  mutate([&](State& state) {
    bool changed = false;
    for (auto& entry : state.entries) if (entry.name == name && entry.state != "retiring") { entry.state = "retiring"; changed = true; }
    return changed;
  });
}
void TenantCatalog::removeIncarnation(std::string_view id) {
  incarnationNumber(id);
  mutate([&](State& state) {
    auto entry = std::ranges::find(state.entries, id, &Entry::id);
    if (entry == state.entries.end()) return false;
    if (entry->state == "active" || entry->state == "detached") throw std::runtime_error("cannot remove a retained incarnation");
    if (entry->state == "retiring") return false;
    entry->state = "retiring";
    return true;
  });
  factory.removeTree(authoritative, path + "/i/" + std::string(id));
  mutate([&](State& state) {
    return std::erase_if(state.entries, [&](const auto& entry) { return entry.id == id && entry.state == "retiring"; }) != 0;
  });
}
void TenantCatalog::remove(std::string_view name) {
  retire(name);
  for (const auto& id : incarnations(name)) removeIncarnation(id);
}

// Startup runs before admitting creators. Followers preserve their download reservations.
void TenantCatalog::cleanup(bool follower) {
  for (const auto& entry : read().entries) {
    if (entry.state == "retiring" || (!follower && entry.state == "reserved")) removeIncarnation(entry.id);
  }
  if (follower) return;
  auto state = read();
  std::set<std::string> retained;
  for (const auto& entry : state.entries) retained.insert(entry.id);
  std::vector<std::string> orphans;
  auto high = state.high;
  for (const auto& child : factory.children(authoritative, path + "/i")) {
    if (retained.contains(child.name)) continue;
    orphans.push_back(child.name);
    if (CommitId::validIncarnation(child.name)) high = std::max(high, incarnationNumber(child.name));
  }
  if (high > state.high) mutate([&](State& current) { if (current.high >= high) return false; current.high = high; return true; });
  for (const auto& id : orphans) factory.removeTree(authoritative, path + "/i/" + id);
}

std::vector<CollectionId> TenantCatalog::collections(DirectoryFactory& factory) {
  std::set<CollectionId> names;
  for (const auto& child : factory.children(authoritative, "t")) if (child.directory) {
    for (const auto& entry : TenantCatalog(factory, child.name).read().entries) {
      if (entry.name && (entry.state == "active" || entry.state == "reserved")) names.emplace(child.name, *entry.name);
    }
  }
  return {names.begin(), names.end()};
}
void TenantCatalog::recover(DirectoryFactory& factory, bool follower) {
  for (const auto& child : factory.children(authoritative, "t")) if (child.directory) TenantCatalog(factory, child.name).cleanup(follower);
}
bool TenantCatalog::empty(DirectoryFactory& factory) {
  for (const auto& child : factory.children(authoritative, "")) {
    if (child.name != "LAYOUT" && child.name != "t") return false;
  }
  for (const auto& child : factory.children(authoritative, "t")) {
    if (!child.directory || !TenantCatalog(factory, child.name).read().entries.empty()) return false;
    for (const auto& entry : factory.children(authoritative, "t/" + child.name)) {
      if (entry.name != "CATALOG" && (entry.name != "i" || !factory.children(authoritative, "t/" + child.name + "/i").empty())) return false;
    }
  }
  return true;
}

} // namespace luxir
