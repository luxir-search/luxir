// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "TenantCatalog.h"
#include "luxir/store/IncarnationState.h"
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
std::vector<std::string> catalogTenants(DirectoryFactory& factory) {
  std::vector<std::string> names;
  for (const auto& child : factory.children(authoritative, "t")) if (child.directory) {
    try { Collections::validateName(child.name, "tenant"); }
    catch (const InvalidCollectionNameError&) { continue; }
    names.push_back(child.name);
  }
  return names;
}
}

TenantCatalog::TenantCatalog(DirectoryFactory& factory, std::string_view tenant, std::string_view node)
    : factory(factory), tenant(tenant), node(node), path("t/" + std::string(tenant)) {
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
    if (entry.state != "reserved" && entry.state != "active" && entry.state != "retiring") throw std::runtime_error("invalid catalog entry state");
    if (entry.name) Collections::validateName(*entry.name);
    if (!entry.name) throw std::runtime_error("catalog entry has no name");
    if (!entry.node.empty()) NodeConfig{entry.node}.validate();
    if (entry.state == "active" && !active.insert(*entry.name).second) throw std::runtime_error("duplicate active catalog name");
  }
  return result;
}

TenantCatalog::State TenantCatalog::read() { return snapshot().state; }

void TenantCatalog::requireOwner(const Entry& entry) const {
  if (entry.node != node) throw CollectionServedByError(entry.node);
}

void TenantCatalog::adopt() {
  if (!factory.exclusive()) throw std::logic_error("catalog adoption requires exclusive storage");
  mutate([&](State& state) {
    bool changed = false;
    for (auto& entry : state.entries) if (entry.node != node) { entry.node = node; changed = true; }
    return changed;
  });
}

bool TenantCatalog::assign(std::string_view id, std::string_view expectedNode, std::string_view target) {
  NodeConfig{std::string(target)}.validate();
  bool assigned = false;
  mutate([&](State& state) {
    auto entry = std::ranges::find(state.entries, id, &Entry::id);
    assigned = entry != state.entries.end() && entry->node == expectedNode;
    if (!assigned || entry->node == target) return false;
    entry->node = target;
    return true;
  });
  return assigned;
}

std::optional<TenantCatalog::Entry> TenantCatalog::find(std::string_view name) {
  auto state = read();
  for (const auto& entry : state.entries) if (entry.name == name && entry.state == "active") return entry;
  for (const auto& entry : state.entries) if (entry.name == name && entry.state == "reserved") return entry;
  return {};
}

TenantCatalog::Entry TenantCatalog::owned(std::string_view id) {
  for (const auto& entry : read().entries) if (entry.id == id) { requireOwner(entry); return entry; }
  throw CollectionUnavailableError("incarnation is no longer in the catalog");
}

IncarnationState TenantCatalog::claim(std::string_view name, std::string_view id, std::string_view state) {
  auto dir = directory(id);
  for (;;) {
    auto before = IncarnationState::read(*dir);
    auto entry = owned(id);
    if (entry.name != name || entry.state != state) throw CollectionUnavailableError("catalog selection changed before writer claim");
    if (!before) throw WriterSupersededError();
    Signal::emit("collectionClaimChecked", this, (void*)&entry);
    if (auto claimed = IncarnationState::claim(*dir, *before)) return *claimed;
    // Re-read STATE and re-confirm assignment before every attempt, including
    // conflicts with another lifetime of the same node id.
  }
}

std::function<void()> TenantCatalog::supersededLogger(std::string id) {
  return [dir = factory.directory(authoritative, path), node = node, id = std::move(id)] {
    auto publication = dir->readPublished("CATALOG");
    State state;
    if (!publication || glz::read_json(state, publication->bytes)) return;
    auto entry = std::ranges::find(state.entries, id, &Entry::id);
    auto assigned = entry == state.entries.end() ? std::string{} : entry->node;
    if (assigned == node) LOG_WARN("Incarnation {}: another instance with node id {} took over", id, node);
    else LOG_WARN("Incarnation {}: assigned to {}", id, assigned);
  };
}

uint64_t TenantCatalog::mutate(const std::function<bool(State&)>& mutation) {
  if (node.empty()) throw std::logic_error("catalog mutation requires a node id");
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
      return before.state.rev;
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
        return next.rev;
      }
    } catch (...) {
      auto failure = std::current_exception();
      auto observed = dir->readPublished("CATALOG");
      if (observed && observed->bytes == bytes) {
        // Visibility alone does not prove FS durability after a failed directory sync.
        std::array<std::string, 1> names{"CATALOG"};
        dir->sync(names);
        return next.rev;
      }
      std::rethrow_exception(failure);
    }
    before = snapshot(); // Re-evaluate the mutation; never resubmit stale entries.
  }
}

std::shared_ptr<StorageMemory> TenantCatalog::account(std::string_view id) {
  return tenantAccount ? tenantAccount->child(std::string(id)) : nullptr;
}
uint64_t TenantCatalog::bytes(std::string_view id) { auto a = account(id); return a ? a->bytes() : 0; }

std::shared_ptr<Directory> TenantCatalog::directory(std::string_view id) {
  incarnationNumber(id);
  return factory.directory(authoritative, path + "/i/" + std::string(id), {account(id)});
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
  auto lock = transition();
  Collections::validateName(name);
  if (!sourceId.empty()) incarnationNumber(sourceId);
  std::string reserved;
  mutate([&](State& state) {
    if (exclusive && std::ranges::any_of(state.entries, [&](const auto& e) {
      return e.name == name && e.state != "retiring";
    })) throw CollectionExistsError("collection '" + std::string(name) + "' already exists");
    for (const auto& entry : state.entries) if (entry.name == name && entry.state != "retiring") requireOwner(entry);
    if (!sourceId.empty()) for (const auto& entry : state.entries) if (entry.id == sourceId) {
      requireOwner(entry);
      if (entry.state != "reserved" && entry.state != "active") throw std::runtime_error("incarnation is not available for installation");
      reserved = sourceId;
      return false;
    }
    auto now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t number = sourceId.empty() ? std::max(now, state.high + 1) : incarnationNumber(sourceId);
    if (number > 0xfffffffffffULL) throw std::runtime_error("incarnation ids exhausted");
    reserved = sourceId.empty() ? std::format("{:011x}", number) : std::string(sourceId);
    state.high = std::max(state.high, number);
    state.entries.push_back({reserved, std::string(name), "reserved", node});
    return true;
  });
  IncarnationState::create(*directory(reserved));
  return reserved;
}

void TenantCatalog::activate(std::string_view name, std::string_view id, std::string_view replacing) {
  mutate([&](State& state) {
    auto selected = std::ranges::find(state.entries, id, &Entry::id);
    if (selected == state.entries.end() || selected->name != name || (selected->state != "reserved" && selected->state != "active")) {
      throw std::runtime_error("activation requires a reserved incarnation");
    }
    requireOwner(*selected);
    if (!replacing.empty()) {
      auto old = std::ranges::find(state.entries, replacing, &Entry::id);
      if (old == state.entries.end() || old->name != name || old->state != "active") throw CollectionUnavailableError("catalog selection changed during recovery");
      requireOwner(*old);
    }
    if (selected->state == "active") return false;
    for (auto& entry : state.entries) if (entry.name == name && entry.state == "active") { requireOwner(entry); entry.state = "retiring"; }
    selected->state = "active";
    return true;
  });
}

uint64_t TenantCatalog::rename(std::string_view from, std::string_view to) {
  Collections::validateName(from); Collections::validateName(to);
  if (from == to) throw InvalidCollectionNameError("rename requires different names");
  return mutate([&](State& state) {
    auto entry = std::ranges::find_if(state.entries, [&](const auto& e) { return e.name == from && e.state == "active"; });
    if (entry == state.entries.end()) throw CollectionNotFoundError("collection '" + std::string(from) + "' does not exist");
    requireOwner(*entry);
    for (const auto& e : state.entries) {
      if (e.name == to && (e.state == "active" || e.state == "reserved")) throw CollectionExistsError("collection '" + std::string(to) + "' already exists");
      if ((e.name == from || e.name == to) && e.state != "active") throw CollectionUnavailableError("collection is changing");
    }
    entry->name = to;
    return true;
  });
}

uint64_t TenantCatalog::swap(std::string_view a, std::string_view b) {
  Collections::validateName(a); Collections::validateName(b);
  if (a == b) throw InvalidCollectionNameError("swap requires different names");
  return mutate([&](State& state) {
    Entry *first = nullptr, *second = nullptr;
    for (auto& entry : state.entries) if (entry.name == a || entry.name == b) {
      requireOwner(entry);
      if (entry.state != "active") throw CollectionUnavailableError("collection is changing");
      (entry.name == a ? first : second) = &entry;
    }
    if (!first || !second) throw CollectionNotFoundError("swap requires two active collections");
    std::swap(first->name, second->name);
    return true;
  });
}

uint64_t TenantCatalog::apply(const std::map<std::string, std::string>& names,
                              const std::map<std::string, std::string>& desired) {
  auto lock = transition();
  std::map<std::string, std::string> byId;
  for (const auto& [name, id] : names) {
    Collections::validateName(name); incarnationNumber(id);
    if (!byId.emplace(id, name).second) throw std::runtime_error("duplicate incarnation in mapping");
  }
  std::map<std::string, std::string> pending;
  for (const auto& [name, id] : desired) if (!id.empty()) {
    Collections::validateName(name); incarnationNumber(id);
    if (!byId.contains(id)) pending.emplace(id, name);
  }
  auto rev = mutate([&](State& state) {
    bool changed = false;
    for (const auto& [id, name] : pending) {
      auto entry = std::ranges::find(state.entries, id, &Entry::id);
      if (entry == state.entries.end()) {
        state.high = std::max(state.high, incarnationNumber(id));
        state.entries.push_back({id, name, "reserved", node});
        changed = true;
      } else if (entry->state != "active" && entry->state != "reserved") throw std::runtime_error("desired incarnation is not available");
    }
    for (const auto& [id, name] : byId) {
      auto entry = std::ranges::find(state.entries, id, &Entry::id);
      if (entry == state.entries.end() || (entry->state != "active" && entry->state != "reserved")) throw std::runtime_error("mapping requires a ready incarnation");
    }
    for (auto& entry : state.entries) {
      if (byId.contains(entry.id) || pending.contains(entry.id) || entry.state == "active") requireOwner(entry);
      if (auto it = byId.find(entry.id); it != byId.end()) {
        changed |= entry.name != it->second || entry.state != "active";
        entry.name = it->second; entry.state = "active";
      } else if (auto it = pending.find(entry.id); it != pending.end()) {
        changed |= entry.name != it->second || entry.state != "reserved";
        entry.name = it->second; entry.state = "reserved";
      } else if (entry.state == "active") { entry.state = "retiring"; changed = true; }
    }
    return changed;
  });
  for (const auto& [id, name] : pending) IncarnationState::create(*directory(id));
  return rev;
}

void TenantCatalog::retire(std::string_view name, std::string_view id) {
  mutate([&](State& state) {
    bool changed = false;
    for (auto& entry : state.entries) if (entry.name == name && (id.empty() || entry.id == id)) {
      requireOwner(entry);
      if (entry.state != "retiring") { entry.state = "retiring"; changed = true; }
    }
    return changed;
  });
}
void TenantCatalog::removeIncarnation(std::string_view id) {
  auto lock = transition();
  incarnationNumber(id);
  mutate([&](State& state) {
    auto entry = std::ranges::find(state.entries, id, &Entry::id);
    if (entry == state.entries.end()) return false;
    requireOwner(*entry);
    if (entry->state == "active") throw std::runtime_error("cannot remove a retained incarnation");
    if (entry->state == "retiring") return false;
    entry->state = "retiring";
    return true;
  });
  auto dir = directory(id);
  std::array<std::string, 1> names{"STATE"};
  dir->deleteDurably(names);
  Signal::emit("incarnationStateRemoved", this);
  factory.removeTree(authoritative, path + "/i/" + std::string(id));
  mutate([&](State& state) {
    return std::erase_if(state.entries, [&](const auto& entry) {
      if (entry.id != id || entry.state != "retiring") return false;
      requireOwner(entry);
      return true;
    }) != 0;
  });
}
void TenantCatalog::remove(std::string_view name) {
  retire(name);
  for (const auto& id : incarnations(name)) removeIncarnation(id);
}

// Startup runs before admitting creators. Followers preserve their download reservations.
void TenantCatalog::cleanup(bool follower) {
  for (const auto& entry : read().entries) {
    if (entry.node != node) continue;
    if (entry.state == "retiring" || (!follower && entry.state == "reserved")) {
      try { removeIncarnation(entry.id); }
      catch (const CollectionServedByError&) {} // Assignment changed since the listing.
    }
  }
  if (follower) return;
  auto state = read();
  std::set<std::string> retained;
  for (const auto& entry : state.entries) retained.insert(entry.id);
  std::vector<std::string> orphans;
  auto high = state.high;
  for (const auto& child : factory.children(authoritative, path + "/i")) {
    if (retained.contains(child.name)) continue;
    if (!factory.exclusive()) {
      LOG_WARN("Uncataloged incarnation tree: {}/i/{}", path, child.name);
      continue;
    }
    orphans.push_back(child.name);
    if (CommitId::validIncarnation(child.name)) high = std::max(high, incarnationNumber(child.name));
  }
  if (high > state.high) mutate([&](State& current) { if (current.high >= high) return false; current.high = high; return true; });
  for (const auto& id : orphans) factory.removeTree(authoritative, path + "/i/" + id);
}

std::vector<CollectionId> TenantCatalog::collections(DirectoryFactory& factory) {
  std::set<CollectionId> names;
  for (const auto& tenant : catalogTenants(factory)) {
    for (const auto& entry : TenantCatalog(factory, tenant).read().entries) {
      if (entry.name && (entry.state == "active" || entry.state == "reserved")) names.emplace(tenant, *entry.name);
    }
  }
  return {names.begin(), names.end()};
}
void TenantCatalog::recover(DirectoryFactory& factory, bool follower, std::string_view node) {
  for (const auto& tenant : catalogTenants(factory)) {
    TenantCatalog catalog(factory, tenant, node);
    if (factory.exclusive()) catalog.adopt();
    catalog.cleanup(follower);
  }
}
bool TenantCatalog::empty(DirectoryFactory& factory) {
  for (const auto& child : factory.children(authoritative, "")) {
    if (child.name != "LAYOUT" && child.name != "t") return false;
  }
  for (const auto& child : factory.children(authoritative, "t")) {
    if (!child.directory || !TenantCatalog(factory, child.name).read().entries.empty()) return false;
    for (const auto& entry : factory.children(authoritative, "t/" + child.name)) {
      if (entry.name == "CATALOG") continue;
      if (entry.name != "i") return false;
      if (!factory.children(authoritative, "t/" + child.name + "/i").empty()) return false;
    }
  }
  return true;
}

} // namespace luxir
