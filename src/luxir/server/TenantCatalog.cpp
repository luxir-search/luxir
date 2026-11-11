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
void logSuperseded(std::atomic<bool>& logged, std::string_view tenant, std::string_view node) {
  if (!logged.exchange(true, std::memory_order_relaxed)) LOG_WARN("Tenant {}: another process took over node id {}", tenant, node);
}
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

TenantCatalog::TenantCatalog(DirectoryFactory& factory, std::string_view tenant, std::string_view node, std::string_view lifetime, Now now)
    : factory(factory), tenant(tenant), node(node), lifetime(lifetime), path("t/" + std::string(tenant)), now(std::move(now)) {
  Collections::validateName(tenant, "tenant");
  if (!node.empty()) NodeConfig{std::string(node)}.validate();
  if (!lifetime.empty() && !isUuid(lifetime)) throw std::invalid_argument("node lifetime must be a UUID");
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
  for (const auto& [id, lifetime] : result.state.nodes) {
    NodeConfig{id}.validate();
    if (!isUuid(lifetime)) throw std::runtime_error("invalid catalog node lifetime");
  }
  std::set<std::string> ids, active;
  for (const auto& entry : result.state.entries) {
    if (incarnationNumber(entry.id) > result.state.high || !ids.insert(entry.id).second) throw std::runtime_error("invalid catalog incarnation");
    if (entry.state != "reserved" && entry.state != "active" && entry.state != "retiring") throw std::runtime_error("invalid catalog entry state");
    if (entry.retired_ms && *entry.retired_ms < 0) throw std::runtime_error("invalid catalog retirement time");
    if (entry.name) Collections::validateName(*entry.name);
    if (!entry.name) throw std::runtime_error("catalog entry has no name");
    if (!entry.node.empty()) NodeConfig{entry.node}.validate();
    if (entry.state == "active" && !active.insert(*entry.name).second) throw std::runtime_error("duplicate active catalog name");
  }
  observe(result.state);
  return result;
}

TenantCatalog::State TenantCatalog::read() { return snapshot().state; }

bool TenantCatalog::hasRetirements(const State& state) const {
  auto current = state.nodes.find(node);
  if (current != state.nodes.end() && current->second != lifetime) return false;
  return std::ranges::any_of(state.entries, [&](const auto& entry) { return entry.node == node && entry.state == "retiring"; });
}

void TenantCatalog::observe(const State& state) {
  if (hasRetirements(state)) pendingRetirements.fetch_add(1, std::memory_order_relaxed);
}

void TenantCatalog::requireOwner(const Entry& entry) const {
  if (entry.node != node) throw CollectionServedByError(entry.node);
}

void TenantCatalog::requireLifetime(const State& state) {
  if (node.empty() || lifetime.empty()) throw std::logic_error("catalog mutation requires a node id and lifetime");
  auto current = state.nodes.find(node);
  if (current != state.nodes.end() && current->second != lifetime) {
    pendingRetirements.store(0, std::memory_order_relaxed);
    logSuperseded(*supersededLogged, tenant, node);
    throw NodeSupersededError(node);
  }
}

void TenantCatalog::adopt() {
  if (!factory.exclusive()) throw std::logic_error("catalog adoption requires exclusive storage");
  if (node.empty() || lifetime.empty()) throw std::logic_error("catalog adoption requires a node id and lifetime");
  std::lock_guard lock(mutex);
  update([&](State& state) {
    bool changed = state.nodes != std::map<std::string, std::string>{{node, lifetime}};
    state.nodes = {{node, lifetime}};
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
    if (!assigned) return false;
    bool changed = false;
    for (auto& other : state.entries) if (other.id == id || (other.name == entry->name && other.state == "retiring")) {
      changed |= other.node != target;
      other.node = target;
    }
    return changed;
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
  auto state = read();
  requireLifetime(state);
  if (!state.nodes.contains(node)) state = mutate([](State&) { return false; });
  for (const auto& entry : state.entries) if (entry.id == id) { requireOwner(entry); return entry; }
  throw CollectionUnavailableError("incarnation is no longer in the catalog");
}

IncarnationState TenantCatalog::claim(std::string_view name, std::string_view id, std::string_view state) {
  auto dir = directory(id);
  // Read STATE before checking the catalog lifetime. Startup stamps before
  // opening: an old process can claim first, but the new claim fences it;
  // if the new claim wins, the old retry sees the new lifetime.
  for (;;) {
    auto before = IncarnationState::read(*dir);
    auto entry = owned(id);
    if (entry.name != name || entry.state != state) throw CollectionUnavailableError("catalog selection changed before writer claim");
    if (!before) throw WriterSupersededError();
    Signal::emit("collectionClaimChecked", this, (void*)&entry);
    if (auto claimed = IncarnationState::claim(*dir, *before)) return *claimed;
  }
}

std::function<void()> TenantCatalog::supersededLogger(std::string id) {
  return [dir = factory.directory(authoritative, path), node = node, lifetime = lifetime, tenant = tenant,
          logged = supersededLogged, id = std::move(id)] {
    auto publication = dir->readPublished("CATALOG");
    State state;
    if (!publication || glz::read_json(state, publication->bytes)) return;
    if (auto current = state.nodes.find(node); current != state.nodes.end() && current->second != lifetime) {
      logSuperseded(*logged, tenant, node);
      return;
    }
    auto entry = std::ranges::find(state.entries, id, &Entry::id);
    auto assigned = entry == state.entries.end() ? std::string{} : entry->node;
    if (assigned == node) LOG_WARN("Incarnation {}: another instance with node id {} took over", id, node);
    else LOG_WARN("Incarnation {}: assigned to {}", id, assigned);
  };
}

TenantCatalog::State TenantCatalog::mutate(const std::function<bool(State&)>& mutation) {
  std::lock_guard lock(mutex);
  auto apply = [&](const std::function<bool(State&)>& change) {
    return update([&](State& state) {
      requireLifetime(state);
      bool stamped = state.nodes.try_emplace(node, lifetime).second;
      return change(state) || stamped;
    });
  };
  auto state = apply(mutation);
  if (factory.retention.count() && std::ranges::any_of(state.entries, [&](const auto& e) { return e.node == node && e.retired_ms == 0; })) {
    // Stamp after the retiring catalog is visible, so a stalled publication
    // cannot consume retention. A crash leaves zero for the next owner to stamp.
    state = apply([&](State& next) {
      bool changed = false;
      auto time = std::max((int64_t)1, (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now().time_since_epoch()).count());
      for (auto& entry : next.entries) if (entry.node == node && entry.retired_ms == 0) {
        entry.retired_ms = time;
        changed = true;
      }
      return changed;
    });
  }
  return state;
}

TenantCatalog::State TenantCatalog::update(const std::function<bool(State&)>& mutation) {
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
      return std::move(before.state);
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
        observe(next);
        Signal::emit("catalogPublished", this);
        return next;
      }
    } catch (...) {
      auto failure = std::current_exception();
      auto observed = dir->readPublished("CATALOG");
      if (observed && observed->bytes == bytes) {
        observe(next);
        // Visibility alone does not prove FS durability after a failed directory sync.
        std::array<std::string, 1> names{"CATALOG"};
        dir->sync(names);
        return next;
      }
      std::rethrow_exception(failure);
    }
    before = snapshot(); // Re-evaluate the mutation; never resubmit stale entries.
  }
}

void TenantCatalog::retiring(Entry& entry) {
  if (entry.state == "active") entry.retired_ms = 0;
  entry.state = "retiring";
}

bool TenantCatalog::removalDue(const Entry& entry) {
  if (!entry.retired_ms || !factory.retention.count()) return true;
  if (!*entry.retired_ms) return false;
  auto time = std::chrono::duration_cast<std::chrono::milliseconds>(now().time_since_epoch()).count();
  return time >= *entry.retired_ms && time - *entry.retired_ms >= factory.retention.count();
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
    for (auto& entry : state.entries) if (entry.name == name && entry.state == "active") { requireOwner(entry); retiring(entry); }
    selected->state = "active";
    selected->retired_ms.reset();
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
      if ((e.name == from || e.name == to) && e.state == "reserved") throw CollectionUnavailableError("collection is changing");
    }
    for (auto& e : state.entries) if (e.name == from && e.node == node) e.name = to;
    return true;
  }).rev;
}

uint64_t TenantCatalog::swap(std::string_view a, std::string_view b) {
  Collections::validateName(a); Collections::validateName(b);
  if (a == b) throw InvalidCollectionNameError("swap requires different names");
  return mutate([&](State& state) {
    Entry *first = nullptr, *second = nullptr;
    for (auto& entry : state.entries) if ((entry.name == a || entry.name == b) && entry.state != "retiring") {
      requireOwner(entry);
      if (entry.state != "active") throw CollectionUnavailableError("collection is changing");
      (entry.name == a ? first : second) = &entry;
    }
    if (!first || !second) throw CollectionNotFoundError("swap requires two active collections");
    for (auto& entry : state.entries) if (entry.node == node) {
      if (entry.name == a) entry.name = b;
      else if (entry.name == b) entry.name = a;
    }
    return true;
  }).rev;
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
        entry.retired_ms.reset();
      } else if (auto it = pending.find(entry.id); it != pending.end()) {
        changed |= entry.name != it->second || entry.state != "reserved";
        if (entry.state == "active") entry.retired_ms = 0;
        entry.name = it->second; entry.state = "reserved";
      } else if (entry.state == "active") { retiring(entry); changed = true; }
    }
    return changed;
  }).rev;
  for (const auto& [id, name] : pending) IncarnationState::create(*directory(id));
  return rev;
}

void TenantCatalog::retire(std::string_view name, std::string_view id) {
  mutate([&](State& state) {
    bool changed = false;
    for (auto& entry : state.entries) if (entry.name == name && (id.empty() || entry.id == id)) {
      if (entry.state == "retiring") continue;
      requireOwner(entry);
      retiring(entry);
      changed = true;
    }
    return changed;
  });
}
void TenantCatalog::removeIncarnation(std::string_view id) {
  auto lock = transition();
  incarnationNumber(id);
  auto state = mutate([&](State& state) {
    auto entry = std::ranges::find(state.entries, id, &Entry::id);
    if (entry == state.entries.end()) return false;
    requireOwner(*entry);
    if (entry->state == "active") throw std::runtime_error("cannot remove a retained incarnation");
    if (entry->state == "retiring") return false;
    retiring(*entry);
    return true;
  });
  auto entry = std::ranges::find(state.entries, id, &Entry::id);
  if (entry == state.entries.end()) return;
  requireOwner(*entry);
  if (!removalDue(*entry)) return;
  auto dir = directory(id);
  std::array<std::string, 1> names{"STATE"};
  dir->deleteDurably(names);
  Signal::emit("incarnationStateRemoved", this);
  factory.removeTree(authoritative, path + "/i/" + std::string(id));
  mutate([&](State& state) {
    return std::erase_if(state.entries, [&](const auto& current) {
      if (current.id != id || current.state != "retiring") return false;
      requireOwner(current);
      return true;
    }) != 0;
  });
}

void TenantCatalog::expire(const std::function<bool(const Entry&)>& canRemove) {
  auto lock = transition();
  auto state = read();
  if (factory.retention.count() && std::ranges::any_of(state.entries, [&](const auto& e) { return e.node == node && e.retired_ms == 0; })) {
    mutate([](State&) { return false; });
    state = read();
  }
  for (const auto& entry : state.entries) {
    if (entry.node != node || entry.state != "retiring" || !removalDue(entry)) continue;
    if (canRemove && !canRemove(entry)) continue;
    try { removeIncarnation(entry.id); }
    catch (const CollectionServedByError&) {} // Assignment moved since the listing.
  }
  // Keep pending if a positive observation races this final read.
  auto observed = pendingRetirements.load(std::memory_order_relaxed);
  if (!hasRetirements(read())) pendingRetirements.compare_exchange_strong(observed, 0, std::memory_order_relaxed);
}
void TenantCatalog::remove(std::string_view name) {
  retire(name);
  for (const auto& id : incarnations(name)) removeIncarnation(id);
}

// Startup runs before admitting creators. Followers preserve their download reservations.
void TenantCatalog::cleanup(bool follower) {
  mutate([](State&) { return false; });
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
void TenantCatalog::recover(DirectoryFactory& factory, bool follower, std::string_view node, std::string_view lifetime) {
  for (const auto& tenant : catalogTenants(factory)) {
    TenantCatalog catalog(factory, tenant, node, lifetime);
    catalog.recover(follower);
  }
}
void TenantCatalog::recover(bool follower) {
  if (factory.exclusive()) adopt();
  else {
    if (node.empty() || lifetime.empty()) throw std::logic_error("catalog recovery requires a node id and lifetime");
    std::lock_guard lock(mutex);
    auto state = update([&](State& state) {
      bool mentioned = state.nodes.contains(node) || std::ranges::any_of(state.entries, [&](const auto& entry) { return entry.node == node; });
      if (!mentioned || (state.nodes.contains(node) && state.nodes.at(node) == lifetime)) return false;
      state.nodes[node] = lifetime;
      return true;
    });
    if (!state.nodes.contains(node)) return;
  }
  cleanup(follower);
}
std::vector<std::string> TenantCatalog::tenants(DirectoryFactory& factory) { return catalogTenants(factory); }
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
