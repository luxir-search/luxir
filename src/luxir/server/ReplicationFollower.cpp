// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationFollower.h"
#include "LuxirNode.h"
#include "CollectionEvents.h"
#include "ReplicationState.h"
#include "luxir/api/build.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/util/Signal.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <glaze/glaze.hpp>
#include <condition_variable>
#include <format>
#include <thread>
#include <set>
#include <ostream>

namespace luxir {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

uint64_t wallTime() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string escape(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
void validateIncarnation(std::string_view value) {
  if (!CommitId::validIncarnation(value)) throw std::invalid_argument("invalid source incarnation");
}

struct Source {
  std::string host, port, authority;
  explicit Source(const std::string& url) {
    if (!url.starts_with("http://")) throw std::invalid_argument("replication.source must be an http:// URL");
    authority = url.substr(7);
    while (authority.ends_with('/')) authority.pop_back();
    if (authority.empty() || authority.find_first_of("/?#@") != std::string::npos) {
      throw std::invalid_argument("replication.source must name a host and optional port, without a path or credentials");
    }
    auto colon = authority.rfind(':');
    if (authority.front() == '[') {
      auto bracket = authority.find(']');
      if (bracket == std::string::npos) throw std::invalid_argument("invalid IPv6 source URL");
      host = authority.substr(1, bracket - 1);
      port = bracket + 1 == authority.size() ? "80" : authority.substr(bracket + 2);
    } else {
      host = authority.substr(0, colon);
      port = colon == std::string::npos ? "80" : authority.substr(colon + 1);
    }
    uint64_t portNumber = 0;
    auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), portNumber);
    if (host.empty() || port.empty() || error != std::errc() || end != port.data() + port.size() || portNumber == 0 || portNumber > 65535) {
      throw std::invalid_argument("invalid replication source address");
    }
  }
};

// Blocking only on dedicated follower threads. Asynchronous socket operations
// retain Beast deadlines and stop cancellation, without blocking a server shard.
class Client {
  net::io_context io;
  net::ip::tcp::resolver resolver{io};
  beast::tcp_stream stream{io};
  beast::flat_buffer buffer;
  std::optional<std::stop_callback<std::function<void()>>> cancellation;
  const Source& source;
  std::stop_token stop;
  net::ip::tcp::resolver::results_type endpoints;
  http::request<http::string_body> outgoing;
  bool retryFresh = false;
  size_t responseBytes = 0;
  static bool disconnected(beast::error_code error) {
    return error == net::error::eof || error == http::error::end_of_stream ||
           error == net::error::connection_reset || error == net::error::broken_pipe;
  }
  template<class Start> void readResponse(Start start) {
    for (;;) {
      beast::error_code error;
      io.restart();
      start([&](beast::error_code ec, size_t bytes) { error = ec; responseBytes += bytes; });
      io.run();
      if (!error) return;
      if (!retryFresh || responseBytes != 0 || stop.stop_requested() || !disconnected(error)) throw beast::system_error(error);
      retryFresh = false;
      reset();
      writeRequest();
    }
  }
  template<class Start> auto wait(Start start) {
    io.restart();
    auto future = start(net::use_future);
    io.run();
    return future.get();
  }
  void writeRequest() {
    connect();
    stream.expires_after(10s);
    wait([&](auto token) { return http::async_write(stream, outgoing, token); });
  }

public:
  Client(const Source& source, std::stop_token stop) : source(source), stop(stop) {
    buffer.reserve(64 * 1024);
    cancellation.emplace(stop, [this]() noexcept {
      try { net::post(io, [this] { resolver.cancel(); stream.cancel(); }); }
      catch (...) { LOG_WARN("Follower cancellation post failed"); }
    });
  }
  void reset() {
    beast::error_code error;
    stream.socket().close(error);
    buffer.consume(buffer.size());
    endpoints = {};
  }
  void connect() {
    if (stop.stop_requested()) throw std::runtime_error("follower stopped");
    if (stream.socket().is_open()) return;
    if (endpoints.empty()) endpoints = wait([&](auto token) { return resolver.async_resolve(source.host, source.port, token); });
    stream.expires_after(5s);
    wait([&](auto token) { return stream.async_connect(endpoints, token); });
  }
  void send(http::verb method, const std::string& target, const std::string& body = {}, uint64_t offset = 0) {
    retryFresh = stream.socket().is_open();
    responseBytes = buffer.size();
    outgoing = http::request<http::string_body>(method, target, 11);
    auto& request = outgoing;
    request.set(http::field::host, source.authority);
    request.set(http::field::content_type, "application/json");
    request.keep_alive(true);
    if (offset) {
      request.set(http::field::range, "bytes=" + std::to_string(offset) + "-");
      Signal::emit("replicationRangeResume", &offset);
    }
    request.body() = body;
    request.prepare_payload();
    try { writeRequest(); }
    catch (const beast::system_error& error) {
      if (!retryFresh || stop.stop_requested() || !disconnected(error.code())) throw;
      retryFresh = false; reset(); writeRequest();
    }
  }
  http::response<http::string_body> response(std::chrono::milliseconds timeout) {
    http::response_parser<http::string_body> parser;
    parser.body_limit(UINT64_MAX);
    stream.expires_after(timeout);
    readResponse([&](auto completion) { http::async_read(stream, buffer, parser, completion); });
    auto result = parser.release();
    if (!result.keep_alive()) reset();
    return result;
  }
  void header(http::response_parser<http::buffer_body>& parser) {
    stream.expires_after(10s);
    readResponse([&](auto completion) { http::async_read_header(stream, buffer, parser, completion); });
  }
  // The header has validated an unchunked Content-Length. Consume any body
  // read with it, then read directly into the download buffer in one io.run().
  template<class Consume> void body(uint64_t length, Consume consume) {
    uint64_t remaining = length;
    size_t buffered = (size_t)std::min<uint64_t>(remaining, buffer.size());
    if (buffered) {
      consume((const char*)buffer.data().data(), buffered);
      buffer.consume(buffered);
      remaining -= buffered;
    }
    if (!remaining) return;
    std::array<char, 256 * 1024> bytes;
    std::exception_ptr failure;
    std::function<void()> read;
    read = [&] {
      if (stop.stop_requested()) throw std::runtime_error("follower stopped");
      stream.expires_after(10s);
      stream.async_read_some(net::buffer(bytes.data(), (size_t)std::min<uint64_t>(remaining, bytes.size())),
          [&](beast::error_code error, size_t received) {
        try {
          Signal::emit("replicationBodyRead", &received, &length);
          if (received) {
            consume(bytes.data(), received);
            remaining -= received;
          }
          if (error) throw beast::system_error(error);
          if (remaining) read();
        } catch (...) { failure = std::current_exception(); }
      });
    };
    io.restart();
    read();
    io.run();
    if (failure) std::rethrow_exception(failure);
  }
};
std::string errorMessage(const std::exception& error) {
  if (auto* system = dynamic_cast<const boost::system::system_error*>(&error)) return system->code().message();
  return error.what();
}
struct PinGone : std::runtime_error { PinGone() : std::runtime_error("source snapshot pin expired") {} };
struct PeerMismatch : std::runtime_error {
  explicit PeerMismatch(const std::string& name) : std::runtime_error("peer file checksum mismatch: " + name) {}
};
}

struct ReplicationFollower::Impl {
  struct Key {
    std::string tenant, incarnation;
    auto operator<=>(const Key&) const = default;
  };
  struct Mapping {
    std::string boot;
    uint64_t revision = 0;
    std::map<std::string, std::string> names;
    bool available = true, applying = false;
  };
  struct Repair {
    std::weak_ptr<Collection> collection;
    std::vector<FileDescriptor> suspects;
    std::string reason;
  };
  struct State {
    CollectionId name; // current source route; never the transfer identity
    std::optional<std::string> selectedName;
    std::shared_ptr<Collection> local;
    std::unique_ptr<Collections::Prepared> prepared;
    std::optional<CommitId> ready;
    // advertised: the source's current commit; serving: installed locally;
    // acknowledged: last serving commit the source accepted. Independent.
    std::optional<CommitId> advertised, serving, acknowledged;
    std::string error;
    bool busy = false, waiting = false, remove = false, unavailable = false, retiring = false;
    std::string seenBoot; // last boot advertising this incarnation or its selected name
    bool replaceEmpty = true;
    // The local copy could not be opened; the catalog, if it could be read, selects
    // this incarnation ("" otherwise). It is retained until an install replaces it.
    std::optional<std::string> unreadable;
    std::shared_ptr<const Repair> corrupt; // a failed copy or a replacement sharing its suspects
    uint64_t advertisedDigest = 0; // manifest digest announced with `advertised`
    unsigned failures = 0;
    uint64_t progress = 0;
    // Node-clock times: when the work became ready (unset until a commit is
    // advertised), and when a failed sync may retry.
    Clock::time_point readySince{};
    Clock::time_point retry{};
    uint64_t downloaded = 0, reused = 0, total = 0;
    // The incarnation `verified` describes, and its staged candidate when
    // that is not the active collection's incarnation.
    std::string targetIncarnation;
    std::optional<Collections::Candidate> candidate;
    std::map<std::string, FileDescriptor> verified;
    bool needsAck() const {
      return serving && selectedName == name.name && acknowledged != serving && advertised && advertised->incarnation == serving->incarnation;
    }
  };
  LuxirNode& node;
  const std::chrono::milliseconds retryDelay; // reconnect, and first sync retry
  Source source;
  std::vector<Source> peers; // never resized after construction: clients reference them
  const std::vector<std::string> tenants;
  ReplicationState binding;
  std::string follower; // set by start(); pull runs anonymously
  std::mutex metadataMutex;
  std::string persisted;
  std::shared_ptr<Directory> metadata;
  std::mutex mutex; // status/scheduling only, never held across storage or network I/O
  std::condition_variable_any changed;
  std::map<Key, State> states;
  std::map<std::string, Mapping> mappings;
  std::mutex mappingMutex;
  bool connected = false, discovered = false;
  uint64_t lastContact = 0;
  std::string boot, discoveryError;
  std::string cursor;
  std::stop_source stopping;
  std::vector<std::jthread> threads;

  explicit Impl(LuxirNode& node) : node(node), retryDelay(node.getConfig().replication.retry_ms),
      source(node.getConfig().replication.source),
      tenants(node.getConfig().replication.tenantFilter()) {
    for (const auto& peer : node.getConfig().replication.peerList()) peers.emplace_back(peer);
    if (node.getConfig().replication.downloads < 1 || node.getConfig().replication.downloads > 64) {
      throw std::invalid_argument("replication.downloads must be between 1 and 64");
    }
    binding.source = "http://" + source.authority;
    metadata = node.collections().storage().directory(StorageRoot::NODE_PRIVATE, "");
    if (auto stored = metadata->openFile("replication.json")) {
      auto existing = ReplicationState::read(*stored);
      existing.source = binding.source;
      binding = std::move(existing);
    } else {
      auto& storage = node.collections().storage();
      if (!TenantCatalog::empty(storage)) throw std::invalid_argument("replication requires an empty data directory or a follower data directory");
      for (const auto& entry : storage.children(StorageRoot::NODE_PRIVATE, "")) {
        if (entry.name != "write.lock") throw std::invalid_argument("replication requires an empty data directory or a follower data directory");
      }
      binding.write(*metadata);
    }
  }

  // A worker's connections: the source, then each peer in configured order.
  struct Clients {
    Client source;
    std::vector<std::unique_ptr<Client>> peers;
    explicit Clients(Impl& impl) : source(impl.source, impl.stopping.get_token()) {
      for (const auto& peer : impl.peers) peers.push_back(std::make_unique<Client>(peer, impl.stopping.get_token()));
    }
  };

  bool includes(const CollectionId& id) const {
    return tenants.empty() || std::ranges::find(tenants, id.tenant) != tenants.end();
  }

  void seed(const std::vector<Collections::Opened>& opened) {
    for (const auto& [tenant, saved] : binding.tenants) {
      if (!saved.boot.empty() && includes({tenant, ""}))
        mappings[tenant] = {saved.boot, saved.revision, saved.names, false, false};
      for (const auto& [incarnation, history] : saved.incarnations) {
        auto& state = states[{tenant, incarnation}];
        state.seenBoot = history.boot; state.replaceEmpty = history.replace_empty;
        if (!history.advertised.empty()) state.advertised = CommitId::parse(history.advertised);
        for (const auto& [name, id] : saved.names) if (id == incarnation) state.name = {tenant, name};
      }
    }
    for (const auto& row : opened) {
      if (!row.collection || !row.incarnation) continue;
      auto& state = states[{row.id.tenant, *row.incarnation}];
      state.name = row.id; state.selectedName = row.id.name; state.local = row.collection;
      if (row.error.empty()) state.serving = row.collection->getShard()->getSnapshots().snapshot()->id;
      else { state.unreadable = row.incarnation; state.error = row.error; }
    }
    persistState();
  }

  // History is keyed by incarnation. Names describe only the observed mapping.
  void persistState() {
    std::lock_guard metadataLock(metadataMutex);
    ReplicationState next;
    {
      std::lock_guard lock(mutex);
      next.source = binding.source;
      for (const auto& [key, state] : states) {
        next.incarnation(key.tenant, key.incarnation) = ReplicationState::Collection{
            state.advertised ? state.advertised->token() : "", state.seenBoot, state.replaceEmpty, {}};
        if (state.selectedName) next.tenants[key.tenant].names[*state.selectedName] = key.incarnation;
      }
      for (const auto& [tenant, mapping] : mappings) {
        auto& saved = next.tenants[tenant];
        saved.boot = mapping.boot; saved.revision = mapping.revision;
        saved.names = mapping.names;
      }
    }
    auto bytes = glz::write_json(next).value();
    if (bytes != persisted) { next.write(*metadata); persisted = std::move(bytes); }
  }
  std::string followerQuery() const { return "follower=" + escape(follower); }
  http::response<http::string_body> request(Client& client, http::verb method, const std::string& path, const std::string& body = {}, std::chrono::milliseconds timeout = 40s) {
    http::response<http::string_body> response;
    try { client.send(method, path, body); response = client.response(timeout); }
    catch (...) { client.reset(); throw; }
    if (response.result_int() == 410) throw PinGone();
    if (response.result_int() != 200) throw std::runtime_error("source HTTP " + std::to_string(response.result_int()) + ": " + response.body());
    { std::lock_guard lock(mutex); lastContact = wallTime(); }
    return response;
  }
  // The worker (or orphan admin) owns this collection's busy flag. Discovery
  // only updates desired state, and never waits for storage operations.
  void eraseLocal(const Key& key) {
    std::optional<Collections::Candidate> candidate;
    std::unique_ptr<Collections::Prepared> prepared;
    std::optional<std::string> selected;
    {
      std::lock_guard lock(mutex);
      auto& state = states[key];
      candidate = std::exchange(state.candidate, std::nullopt);
      prepared = std::move(state.prepared); selected = state.selectedName;
    }
    if (candidate) node.collections().discard(*candidate);
    prepared.reset();
    if (selected) node.collections().remove({key.tenant, *selected}, false);
    else node.collections().catalog(key.tenant).removeIncarnation(key.incarnation);
    std::lock_guard lock(mutex);
    auto& state = states[key];
    state.serving.reset(); state.acknowledged.reset(); state.unreadable.reset();
    state.local.reset(); state.selectedName.reset(); state.ready.reset(); state.prepared.reset();
    state.verified.clear(); state.targetIncarnation.clear(); state.remove = state.retiring = false;
  }

  void unloadCorrupt(const Key& key, const std::shared_ptr<const Repair>& repair) {
    std::shared_ptr<Collection> active;
    { std::lock_guard lock(mutex); auto& state = states[key]; if (state.selectedName) active = state.local; }
    auto shard = active ? active->getShard() : nullptr;
    auto snapshot = shard ? shard->getSnapshots().snapshot() : nullptr;
    auto failed = repair->collection.lock();
    std::vector<std::string> suspects;
    if (snapshot) for (const auto& file : repair->suspects) {
      if (std::ranges::find(snapshot->files, file) != snapshot->files.end()) suspects.push_back(file.name);
    }
    if (shard && (active == failed || !suspects.empty())) {
      auto name = active->getId();
      if (active != failed) node.collections().fail(name, active, repair->reason, suspects);
      node.collections().unload(name, active, repair->reason);
      std::lock_guard lock(mutex);
      auto& state = states[key];
      state.local = node.collections().get(name);
      state.unreadable = snapshot ? snapshot->id.incarnation : "";
      state.serving.reset(); state.acknowledged.reset(); state.ready.reset(); state.prepared.reset();
      state.verified.clear(); state.targetIncarnation.clear(); state.error = repair->reason;
    }
    {
      std::lock_guard lock(mutex);
      if (states[key].corrupt == repair) states[key].corrupt.reset();
    }
    auto name = active ? active->getId() : CollectionId{};
    Signal::emit("replicationRepairProcessed", &name);
  }

  api::ReplicationCatalog fetchCatalog(Client& client, const std::string& cursor, std::chrono::milliseconds timeout, std::pmr::memory_resource& arena) {
    std::string subscription;
    for (const auto& tenant : tenants) subscription += "&tenant=" + escape(tenant);
    auto response = request(client, http::verb::get, "/_replication/watch?" + followerQuery() + subscription
        + "&since=" + escape(cursor) + "&timeout_ms=" + std::to_string(timeout.count()), {}, timeout + 10s);
    api::ReplicationCatalog catalog;
    if (!api::read_json(catalog, api::build::arenaStr(arena, response.body()), arena) || catalog.boot.empty() || catalog.cursor.empty()) throw std::runtime_error("invalid source catalog");
    Signal::emit("replicationCatalogReceived", &catalog);
    std::set<std::string> catalogTenants;
    for (const auto& tenant : catalog.tenants) {
      Collections::validateName(tenant.tenant, "tenant");
      if (!catalogTenants.emplace(tenant.tenant).second) throw std::runtime_error("duplicate tenant in source catalog");
    }
    std::set<CollectionId> seen;
    std::set<Key> incarnations;
    for (const auto& entry : catalog.collections) {
      CollectionId id{std::string(entry.tenant), std::string(entry.collection)};
      Collections::validate(id);
      if (!catalogTenants.contains(id.tenant)) throw std::runtime_error("source catalog has no tenant revision");
      if (!seen.insert(std::move(id)).second) throw std::runtime_error("duplicate collection in source catalog");
      if (entry.available && entry.commit.empty()) throw std::runtime_error("available source collection has no commit");
      if (auto announced = announcement(entry)) {
        validateIncarnation(announced->commit.incarnation);
        if (!incarnations.insert({std::string(entry.tenant), announced->commit.incarnation}).second) throw std::runtime_error("duplicate incarnation in source catalog");
      }
    }
    return catalog;
  }

  // A commit as the source announced it: its token and its manifest digest.
  struct Announcement {
    CommitId commit;
    uint64_t digest;
  };
  static std::optional<Announcement> announcement(const api::ReplicationCatalogEntry& entry) {
    if (entry.commit.empty()) return std::nullopt;
    return Announcement{CommitId::parse(entry.commit), CommitSnapshot::parseDigest(entry.manifest_xxh3)};
  }

  // Applies one collection's entry from a source catalog (absent: not in it).
  // A new incarnation may replace a populated copy with an empty snapshot only
  // if this follower saw the previous incarnation in the same source boot.
  void observe(State& state, std::optional<Announcement> announced, bool present, const std::string& sourceBoot) {
    auto desired = announced ? std::optional(announced->commit) : std::nullopt;
    if (desired && (!state.advertised || desired->incarnation != state.advertised->incarnation))
      state.replaceEmpty = state.seenBoot == sourceBoot;
    if (desired != state.advertised) {
      if (!state.advertised || state.advertised == state.serving || state.waiting) state.readySince = node.now();
      state.advertised = std::move(desired); state.waiting = false;
    }
    state.advertisedDigest = announced ? announced->digest : 0;
    if (present) { state.seenBoot = sourceBoot; state.remove = false; }
    else state.remove = state.seenBoot == sourceBoot;
  }

  void observeCatalog(const api::ReplicationCatalog& catalog) {
    std::lock_guard lock(mutex);
    if (boot != catalog.boot) for (auto& [key, state] : states) state.acknowledged.reset();
    boot = catalog.boot; cursor = catalog.cursor;
    connected = discovered = true; discoveryError.clear();
    std::map<std::string, Mapping> next;
    for (const auto& tenant : catalog.tenants) {
      if (!includes({std::string(tenant.tenant), ""})) continue;
      next[std::string(tenant.tenant)] = {boot, tenant.revision, {}, true, false};
    }
    std::set<Key> seen;
    for (const auto& entry : catalog.collections) {
      CollectionId name{std::string(entry.tenant), std::string(entry.collection)};
      if (!includes(name)) continue;
      auto& mapping = next[name.tenant]; mapping.boot = boot;
      auto announced = announcement(entry);
      if (!announced) { mapping.names[name.name] = {}; continue; }
      Key key{name.tenant, announced->commit.incarnation};
      seen.insert(key); mapping.names[name.name] = key.incarnation;
      auto& state = states[key];
      if (!state.advertised) {
        state.replaceEmpty = false;
        for (const auto& [other, previous] : states) if (other != key && previous.name == name && previous.seenBoot == boot)
          state.replaceEmpty = true;
      }
      auto replaceEmpty = state.replaceEmpty;
      if (state.name != name) state.acknowledged.reset();
      state.name = name; state.unavailable = !entry.available;
      observe(state, announced, true, boot);
      state.replaceEmpty = replaceEmpty;
      if (!state.busy && state.serving && state.advertised == state.serving && state.acknowledged == state.serving) {
        state.error.clear(); state.failures = 0; state.retry = {};
      }
    }
    for (auto& [key, state] : states) if (includes({key.tenant, ""}) && !seen.contains(key)) {
      // An unavailable source row without an identity cannot authorize deletion.
      auto tenant = next.find(key.tenant);
      if (tenant != next.end()) {
        auto unknown = tenant->second.names.find(state.selectedName.value_or(state.name.name));
        if (unknown != tenant->second.names.end()) {
          if (unknown->second.empty()) { state.unavailable = true; continue; }
          // Keep the name's boot history with the serving copy even while an
          // empty replacement waits. Retiring that candidate must not forget
          // that a later delete or recreate in this boot is authoritative.
          if (state.selectedName) state.seenBoot = boot;
        }
      }
      state.unavailable = false;
      observe(state, {}, false, boot);
    }
    for (const auto& [key, state] : states) if (includes({key.tenant, ""}) && !next.contains(key.tenant))
      next[key.tenant] = {boot, 0, {}, true, false};
    for (auto& [tenant, mapping] : next) {
      mapping.applying = mappings[tenant].applying;
      mappings[tenant] = std::move(mapping);
    }
  }

  // Ready bindings advance independently. A lagging name may keep its old
  // incarnation only if the source has not assigned that data to another name.
  // Storage I/O holds neither the scheduler mutex nor any lookup lock.
  void applyMappings() {
    std::unique_lock applyLock(mappingMutex, std::try_to_lock);
    if (!applyLock) return;
    std::vector<std::string> tenants;
    { std::lock_guard lock(mutex); for (const auto& [tenant, mapping] : mappings) tenants.push_back(tenant); }
    for (const auto& tenant : tenants) {
      std::map<std::string, std::string> names, desired;
      std::vector<Collections::Prepared> prepared;
      std::vector<std::string> retained;
      {
        std::lock_guard lock(mutex);
        auto& mapping = mappings[tenant];
        if (!mapping.available || mapping.applying) continue;
        desired = mapping.names;
        std::map<std::string, std::string> current, owners;
        for (const auto& entry : node.collections().entries(tenant)) current[entry.id.name] = {};
        for (const auto& [key, state] : states) if (key.tenant == tenant && state.selectedName) current[*state.selectedName] = key.incarnation;
        for (const auto& [name, id] : mapping.names) if (!id.empty()) owners[id] = name;
        for (const auto& [name, id] : mapping.names) {
          auto it = states.find({tenant, id});
          bool ready = it != states.end() && (it->second.prepared ||
              (it->second.local && !it->second.local->isFailed() && it->second.serving));
          if (ready) names[name] = id;
          else {
            auto old = current.find(name);
            if (old != current.end() && (!owners.contains(old->second) || owners.at(old->second) == name)) names[name] = old->second;
            else names[name] = {}; // Never serve data now assigned to another name.
          }
        }
        for (const auto& [key, state] : states) if (key.tenant == tenant && state.selectedName && !state.advertised && !state.remove
            && !names.contains(*state.selectedName) && !owners.contains(key.incarnation))
          names[*state.selectedName] = key.incarnation; // orphan after a source reboot
        bool changed = current != names;
        for (const auto& [name, id] : names) if (!id.empty() && states.at({tenant, id}).prepared) changed = true;
        if (!changed) continue;
        mapping.applying = true;
        for (const auto& [key, state] : states) if (key.tenant == tenant && state.busy) retained.push_back(key.incarnation);
        for (const auto& [name, id] : names) if (!id.empty()) {
          auto& state = states.at({tenant, id});
          if (state.prepared) { prepared.push_back(std::move(*state.prepared)); state.prepared.reset(); }
        }
      }
      try {
        Signal::emit("replicationMappingReady", (void*)&tenant);
        node.collections().applyMapping(tenant, names, desired, std::move(prepared), retained);
        std::map<std::string, std::string> selected;
        for (const auto& [name, id] : names) if (!id.empty()) selected[id] = name;
        std::lock_guard lock(mutex);
        for (auto& [key, state] : states) if (key.tenant == tenant) {
          auto it = selected.find(key.incarnation);
          if (it == selected.end()) {
            bool wanted = std::ranges::any_of(desired, [&](const auto& e) { return e.second == key.incarnation; });
            if (state.selectedName) {
              state.retiring = !wanted;
              if (wanted) { state.unreadable = key.incarnation; state.local.reset(); state.serving.reset(); }
            }
            state.selectedName.reset(); state.acknowledged.reset();
            if (state.retiring && !state.busy) { state.serving.reset(); state.local.reset(); }
          } else {
            auto local = node.collections().get({tenant, it->second});
            bool installed = state.local != local;
            if (installed || state.selectedName != it->second) state.acknowledged.reset();
            state.local = std::move(local); state.selectedName = it->second; state.retiring = false;
            auto shard = state.local->getShard();
            if (shard) {
              state.serving = shard->getSnapshots().snapshot()->id;
              if (installed) { state.ready.reset(); state.unreadable.reset(); state.error.clear(); state.verified.clear(); }
            }
          }
        }
        mappings[tenant].applying = false;
      } catch (const std::exception& e) {
        std::lock_guard lock(mutex);
        mappings[tenant].applying = false;
        for (const auto& [name, id] : names) if (!id.empty()) {
          auto& state = states.at({tenant, id});
          state.ready.reset(); state.error = e.what();
        }
      }
    }
    changed.notify_all();
  }

  void watch() {
    Client client(source, stopping.get_token());
    bool refresh = true;
    auto timeout = std::chrono::milliseconds(node.getConfig().replication.follower_timeout_ms / 3);
    while (!stopping.stop_requested()) {
      try {
        std::pmr::monotonic_buffer_resource arena;
        auto catalog = fetchCatalog(client, refresh ? "" : cursor, timeout, arena);
        refresh = false;
        observeCatalog(catalog);
        persistState();
        applyMappings();
        changed.notify_all();
      } catch (const std::exception& e) {
        if (stopping.stop_requested()) break;
        client.reset(); refresh = true;
        auto message = errorMessage(e);
        { std::lock_guard lock(mutex); connected = false; discoveryError = message; }
        LOG_WARN("Replication source {} unavailable: {}", binding.source, message);
        std::unique_lock lock(mutex);
        changed.wait_for(lock, stopping.get_token(), retryDelay, [] { return false; });
      }
    }
  }

  // Downloads and verifies one file. Peers serve it by identity, in order and
  // once each; the source serves it from the pinned commit and is retried. A
  // transfer resumes at the same offset on the next source, and the digest
  // check covers the whole file whatever mix of sources supplied it.
  void download(Clients& clients, const Key& key, const CollectionId& collection, Directory& dir, const CommitSnapshot& snapshot,
                const FileDescriptor& descriptor, bool usePeers = true) {
    Signal::emit("replicationDownloadStart", (void*)&descriptor, (void*)&collection);
    Directory::FileCreateOptions options; options.expectedSize = descriptor.size;
    auto file = dir.createFile(descriptor.name, options);
    OutputStream out(file.get());
    uint64_t offset = 0;
    int failures = 0;
    size_t next = usePeers ? 0 : clients.peers.size();
    bool fromPeer = false;
    for (;;) {
      if (stopping.stop_requested()) throw std::runtime_error("follower stopped");
      bool peer = next < clients.peers.size();
      Client& client = peer ? *clients.peers[next] : clients.source;
      try {
        auto path = snapshotPath(collection) + "/files/" + escape(descriptor.name) + "?" + (peer
            ? "size=" + std::to_string(descriptor.size) + "&xxh3=" + CommitSnapshot::digestText(descriptor.xxh3)
            : "commit=" + escape(snapshot.id.token()) + "&" + followerQuery());
        client.send(http::verb::get, path, {}, offset);
        http::response_parser<http::buffer_body> parser;
        // Error bodies (especially 410 for a tiny file) can exceed file size.
        // Check status first, then require the exact remaining Content-Length.
        parser.body_limit(UINT64_MAX);
        client.header(parser);
        int status = parser.get().result_int();
        if (!peer && status == 410) throw PinGone();
        if (status != (offset ? 206 : 200)) throw std::runtime_error("source file HTTP " + std::to_string(status));
        if (parser.chunked() || parser.content_length() != descriptor.size - offset
            || (!peer && parser.get()["X-Luxir-Commit"] != snapshot.id.token())) {
          throw std::runtime_error("source file length or commit mismatch");
        }
        if (offset && parser.get()[http::field::content_range] != "bytes " + std::to_string(offset) + "-" + std::to_string(descriptor.size - 1) + "/" + std::to_string(descriptor.size)) {
          throw std::runtime_error("source file range mismatch");
        }
        client.body(descriptor.size - offset, [&](const char* bytes, size_t received) {
          Signal::emit("replicationDownloadWrite", &dir);
          out.write(bytes, received); offset += received;
          fromPeer = fromPeer || peer;
          {
            std::lock_guard lock(mutex); states[key].downloaded += received;
            if (!peer) lastContact = wallTime();
          }
          Signal::emit("replicationDownloadProgress", &offset);
          if (stopping.stop_requested()) throw std::runtime_error("follower stopped");
        });
        if (!parser.get().keep_alive()) client.reset();
        break;
      } catch (const PinGone&) { client.reset(); throw; }
      catch (...) {
        client.reset();
        if (peer) { Signal::emit("replicationPeerMiss", (void*)&descriptor); next++; continue; }
        if (++failures >= 3 || stopping.stop_requested()) throw;
      }
    }
    out.close();
    if (file->size() != descriptor.size || file->digest() != descriptor.xxh3) {
      if (fromPeer) throw PeerMismatch(descriptor.name);
      throw std::runtime_error("source file checksum mismatch: " + descriptor.name);
    }
    dir.finishFile(*file);
    Signal::emit("replicationFileVerified", &file);
  }

  static std::string snapshotPath(const CollectionId& id) {
    return "/tenants/" + id.tenant + "/collections/" + id.name + "/_snapshot";
  }

  std::shared_ptr<const CommitSnapshot> fetchSnapshot(Client& client, const CollectionId& name) {
    auto response = request(client, http::verb::get, snapshotPath(name) + "?" + followerQuery());
    auto& body = response.body();
    auto bytes = std::make_shared<const std::vector<std::byte>>((const std::byte*)body.data(), (const std::byte*)body.data() + body.size());
    std::shared_ptr<const CommitSnapshot> snapshot;
    try { snapshot = CommitSnapshot::fromBytes(bytes); }
    catch (const IndexFormatError& e) {
      LOG_ERROR("Cannot replicate collection '{}' from {}: {}", name.label(), binding.source, e.what());
      throw;
    }
    validateIncarnation(snapshot->id.incarnation);
    if (response["X-Luxir-Commit"] != snapshot->id.token()) throw std::runtime_error("snapshot commit mismatch");
    if (!snapshot->populated) Signal::emit("replicationEmptySnapshotReceived");
    return snapshot;
  }

  // What to do with a fetched snapshot. `advance` targets the active
  // collection's own incarnation; otherwise a staged candidate replaces it.
  struct Decision {
    bool current = false; // already serving it
    bool eligible = false;
    bool advance = false;
    std::shared_ptr<Collection> active;
    std::optional<Collections::Candidate> obsolete;
  };

  Decision decide(const Key& key, const CollectionId& name, const CommitSnapshot& snapshot) {
    Decision decision;
    std::lock_guard lock(mutex);
    auto& state = states[key];
    if (!state.advertised) throw std::runtime_error("source collection disappeared during transfer");
    if (state.advertised->incarnation != snapshot.id.incarnation)
      throw std::runtime_error("source incarnation changed; retry snapshot");
    // A manifest of the announced commit must be the announced bytes.
    if (snapshot.id == state.advertised && snapshot.digest != state.advertisedDigest)
      throw std::runtime_error("manifest does not match its announcement");
    for (const auto& file : snapshot.files) state.total += file.size;
    decision.active = state.selectedName ? state.local : nullptr;
    if (!decision.active) for (const auto& [other, installed] : states) {
      if (other.tenant == name.tenant && installed.selectedName == name.name) { decision.active = installed.local; break; }
    }
    auto shard = decision.active ? decision.active->getShard() : nullptr;
    auto serving = shard ? shard->getSnapshots().snapshot() : nullptr;
    decision.advance = serving && serving->id.incarnation == snapshot.id.incarnation;
    if (decision.advance && snapshot.id.index_gen <= serving->id.index_gen) {
      if (snapshot.id != state.serving) throw std::runtime_error("source went backwards");
      state.reused = state.total;
      decision.current = true;
      return decision;
    }
    // An unreadable copy may contain data. Only a known same incarnation,
    // same-boot replacement, or populated source can replace it with certainty.
    bool sameIncarnation = decision.advance || (!shard && state.unreadable == snapshot.id.incarnation);
    bool populated = serving ? serving->populated : decision.active != nullptr || state.unreadable.has_value();
    decision.eligible = !populated || sameIncarnation || state.replaceEmpty || snapshot.populated;
    // Discovery may have advanced while this snapshot request was in flight.
    // An older empty snapshot must not park a newer eligible publication.
    state.waiting = !decision.eligible && state.advertised == snapshot.id;
    if (state.targetIncarnation != snapshot.id.incarnation) {
      decision.obsolete = std::exchange(state.candidate, std::nullopt);
      state.verified.clear();
      state.targetIncarnation = snapshot.id.incarnation;
    }
    return decision;
  }

  // Verifies every file of `snapshot` in the target directory, downloading what
  // is missing or suspect, and makes them durable.
  void stageFiles(Clients& clients, const Key& key, const CollectionId& name, Directory& dir, const std::shared_ptr<const CommitSnapshot>& previous,
                  const CommitSnapshot& snapshot, const std::shared_ptr<Collection>& active) {
    auto suspects = CorruptionMarker::read(dir);
    std::map<std::string, FileDescriptor> verified;
    { std::lock_guard lock(mutex); verified = states[key].verified; }
    if (!previous && active && !active->isFailed()) {
      if (auto shard = active->getShard()) {
        auto& source = shard->getSnapshots();
        if (auto serving = source.snapshot()) {
          auto sourceSuspects = CorruptionMarker::read(source.dir);
          std::vector<FileDescriptor> available;
          for (const auto& file : serving->files)
            if (!std::ranges::binary_search(sourceSuspects, file.name)
                && !std::ranges::binary_search(suspects, file.name)) available.push_back(file);
          for (auto& file : dir.reuseFiles(source.dir, available, snapshot.files))
            verified.insert_or_assign(file.name, std::move(file));
        }
      }
    }
    boost::unordered_flat_set<std::string_view> durableNames;
    if (previous) for (const auto& file : previous->files) {
      verified.insert_or_assign(file.name, file);
      durableNames.insert(file.name);
    }
    std::vector<std::string> names;
    std::set<std::string> unique;
    for (const auto& file : snapshot.files) {
      if (file.name.empty() || file.name == "." || file.name == ".." || file.name.find_first_of("/\\") != std::string::npos
          || file.name.starts_with(Manifest::kPrefix) || file.name.ends_with(".tmp") || !unique.insert(file.name).second) throw std::runtime_error("invalid snapshot file name");
      bool suspect = std::ranges::binary_search(suspects, file.name);
      if (suspect && durableNames.contains(file.name))
        throw std::runtime_error("cannot replace suspect file in serving snapshot: " + file.name + "; unload and repair the collection");
      bool reuse = !suspect && verified.contains(file.name) && verified.at(file.name) == file;
      if (!reuse && !suspect) {
        // Unpublished verified files can survive a reconnect or failed install.
        // After process restart only these candidates need to be hashed. An
        // unreadable unpublished file is downloaded again and replaced; a
        // serving file must stay readable.
        std::optional<FileCheck> local;
        try { local = dir.checksum(file.name); }
        catch (const FileIOException&) {
          if (durableNames.contains(file.name)) throw;
        }
        if (local) {
          reuse = local->matches(file);
          if (!reuse && previous) {
            for (const auto& live : previous->files) if (live.name == file.name) throw std::runtime_error("source changed immutable file: " + file.name);
          }
        }
      }
      if (!reuse) {
        dir.deleteFile(file.name); // only non-serving, unverified copies reach this path
        // A peer copy that fails verification is fetched again from the source.
        try { download(clients, key, name, dir, snapshot, file); }
        catch (const PeerMismatch&) { download(clients, key, name, dir, snapshot, file, false); }
      }
      { std::lock_guard lock(mutex);
        auto& state = states[key];
        if (reuse) state.reused += file.size;
        if (!state.verified.contains(file.name)) state.progress++;
        state.verified.insert_or_assign(file.name, file);
      }
      // Files in the serving snapshot are already durable. Retried candidates
      // may have been verified but not synced before their pin vanished.
      if (!durableNames.contains(file.name)) names.push_back(file.name);
    }
    dir.sync(names);
  }

  void sync(Clients& clients, const Key& key) {
    CollectionId name;
    State* transfer;
    { std::lock_guard lock(mutex);
      transfer = &states.at(key); name = transfer->name;
      transfer->total = transfer->downloaded = transfer->reused = 0;
    }
    auto snapshot = fetchSnapshot(clients.source, name);
    auto decision = decide(key, name, *snapshot);
    if (decision.obsolete) node.collections().discard(*decision.obsolete);
    if (decision.current || !decision.eligible) return;
    auto stillAdvertised = [&] {
      std::lock_guard lock(mutex);
      auto& advertised = states[key].advertised;
      if (!advertised || advertised->incarnation != snapshot->id.incarnation)
        throw std::runtime_error("source collection changed during transfer");
    };
    // This worker owns the incarnation's transfer state while busy.
    std::shared_ptr<Collection> installed;
    if (decision.advance) {
      auto& registry = decision.active->getShard()->getSnapshots();
      auto previous = registry.snapshot();
      stageFiles(clients, key, name, registry.dir, previous, *snapshot, decision.active);
      snapshot = registry.resolve(snapshot->bytes);
      auto opened = registry.readers.prepare(*snapshot);
      stillAdvertised();
      if (stopping.stop_requested()) return;
      registry.commit(snapshot, std::move(opened));

      installed = decision.active;
    } else {
      auto& state = *transfer;
      if (!state.candidate) {
        auto candidate = node.collections().stage(name, snapshot->id.incarnation);
        std::lock_guard lock(mutex); state.candidate = std::move(candidate);
      }
      stageFiles(clients, key, name, state.candidate->dir(), state.candidate->committed(), *snapshot, decision.active);
      stillAdvertised();
      if (stopping.stop_requested()) return;
      // An unregistered candidate's publication is invisible until activation.
      auto prepared = node.collections().prepare(*state.candidate, snapshot);
      { std::lock_guard lock(mutex); state.candidate.reset(); }
      std::lock_guard lock(mutex);
      state.prepared = std::make_unique<Collections::Prepared>(std::move(prepared));
      state.ready = snapshot->id;
      state.error.clear();
      return;
    }
    try { installed->getShard()->getSnapshots().sweepOrphans(); }
    catch (const std::exception& e) { LOG_WARN("Follower retirement failed: {}", e.what()); }
    {
      std::lock_guard lock(mutex);
      auto& state = states[key];
      if (state.selectedName && state.local == installed && !installed->isFailed()) {
        state.serving = snapshot->id; state.unreadable.reset();
      } else state.serving.reset(); // Remapping may have unbound this worker's copy.
      state.error.clear(); state.verified.clear();
      auto& advertised = state.advertised;
      if (advertised && advertised->incarnation == snapshot->id.incarnation && advertised->index_gen < snapshot->id.index_gen) advertised = snapshot->id;
    }
  }

  bool pull(std::ostream& output) {
    if (!threads.empty() || stopping.stop_requested()) throw std::logic_error("pull requires an unstarted follower");
    Clients clients(*this);
    api::ReplicationCatalog catalog;
    std::pmr::monotonic_buffer_resource arena;
    try { catalog = fetchCatalog(clients.source, {}, 0ms, arena); observeCatalog(catalog); }
    catch (const std::exception& e) {
      output << "Pull failed: " << binding.source << ": " << errorMessage(e) << '\n';
      return false;
    }
    size_t installed = 0, failed = 0;
    struct Result { CollectionId name; Key key; std::string commit, error; uint64_t downloaded = 0, reused = 0; };
    std::vector<Result> results;
    applyMappings();
    for (const auto& entry : catalog.collections) {
      CollectionId name{std::string(entry.tenant), std::string(entry.collection)};
      if (!includes(name)) continue;
      auto announced = announcement(entry);
      Result result{name, {name.tenant, announced ? announced->commit.incarnation : ""}, std::string(entry.commit), {}};
      try {
        if (!entry.available || !announced) throw std::runtime_error("source collection unavailable");
        auto& state = states[result.key];
        state.downloaded = state.reused = 0;
        for (unsigned attempt = 0;; attempt++) {
          try { sync(clients, result.key); break; }
          catch (const PinGone&) { if (attempt == 2) throw; result.downloaded += state.downloaded; }
        }
        persistState();
        if (state.serving != state.advertised && state.ready != state.advertised) throw std::runtime_error("waiting for source data before replacing the local snapshot");
      } catch (const std::exception& e) { clients.source.reset(); result.error = errorMessage(e); }
      if (announced) {
        const auto& state = states[result.key]; result.downloaded += state.downloaded; result.reused = state.reused;
      }
      results.push_back(std::move(result));
    }
    applyMappings();
    persistState();
    uint64_t transferred = 0, reused = 0;
    for (auto& result : results) {
      if (result.error.empty()) {
        const auto& state = states.at(result.key);
        if (!state.serving || state.serving != state.advertised || state.selectedName != result.name.name)
          result.error = state.error.empty() ? "collection is not ready" : state.error;
      }
      if (result.error.empty()) installed++;
      else failed++;
      output << result.name.label() << ' ' << result.commit << " transferred=" << result.downloaded << " reused=" << result.reused;
      if (!result.error.empty()) output << " ERROR: " << result.error;
      output << '\n';
      transferred += result.downloaded; reused += result.reused;
    }
    output << "Pull: " << installed << " installed, " << failed << " failed, transferred=" << transferred << " reused=" << reused << '\n';
    return failed == 0;
  }

  void worker() {
    Clients clients(*this);
    while (!stopping.stop_requested()) {
      Key key;
      uint64_t progress = 0;
      {
        std::unique_lock lock(mutex);
        auto ready = [&] {
          State* selected = nullptr;
          for (auto& [candidate, state] : states) {
            if (!includes({candidate.tenant, ""}) || mappings[candidate.tenant].applying) continue;
            bool cleanup = state.retiring || (state.remove && !state.selectedName && !state.advertised);
            bool work = cleanup || (connected && ((bool)state.corrupt || (state.advertised && ((state.advertised != state.serving && state.advertised != state.ready && !state.waiting)
                    || state.needsAck()))));
            if (!state.busy && (!state.unavailable || cleanup) && work && node.now() >= state.retry && (!selected || std::max(state.readySince, state.retry) < std::max(selected->readySince, selected->retry))) {
              key = candidate; selected = &state;
            }
          }
          if (!selected) return false;
          selected->busy = true; progress = selected->progress;
          return true;
        };
        // Recheck at least every 200 ms, and when the next backed-off retry is due.
        auto recheck = Clock::duration(200ms);
        auto now = node.now();
        for (const auto& [key, state] : states) if (state.retry > now) recheck = std::min(recheck, state.retry - now);
        changed.wait_for(lock, stopping.get_token(), recheck, ready);
        if (stopping.stop_requested()) break;
        if (key.incarnation.empty()) continue;
      }
      std::string error;
      try {
        bool cleanup, needsSync;
        std::shared_ptr<const Repair> corrupt;
        std::optional<CommitId> serving;
        {
          std::lock_guard lock(mutex);
          auto& state = states[key]; corrupt = state.corrupt;
          cleanup = state.retiring || (state.remove && !state.selectedName && !state.advertised);
        }
        if (corrupt && !cleanup) unloadCorrupt(key, corrupt);
        {
          std::lock_guard lock(mutex);
          auto& state = states[key];
          needsSync = state.advertised && state.advertised != state.serving && state.advertised != state.ready && !state.waiting;
        }
        if (cleanup) eraseLocal(key);
        else {
          auto acknowledge = [&] {
            std::string ackBoot;
            CollectionId name;
            {
              std::lock_guard lock(mutex);
              auto& state = states[key];
              serving = state.needsAck() ? state.serving : std::nullopt;
              ackBoot = boot; name = state.name;
            }
            if (!serving) return;
            auto token = serving->token();
            api::ReplicationInstalled ack{follower, name.tenant, name.name, token};
            std::string body;
            if (!api::write_json(ack, body)) throw std::runtime_error("failed to serialize acknowledgment");
            request(clients.source, http::verb::post, "/_replication/installed", body);
            std::lock_guard lock(mutex);
            if (boot == ackBoot && states[key].name == name && states[key].selectedName == name.name) states[key].acknowledged = serving;
          };
          acknowledge();
          if (needsSync) sync(clients, key);
          acknowledge();
        }
        persistState();
      } catch (const PinGone& e) { error = e.what(); Signal::emit("replicationPinGone"); }
      catch (const std::exception& e) { error = errorMessage(e); }
      if (!error.empty()) clients.source.reset();
      {
        std::lock_guard lock(mutex);
        auto& state = states[key];
        // Discovery may have superseded a failed target while this job ran.
        if (state.serving && state.advertised == state.serving && state.acknowledged == state.serving) error.clear();
        state.busy = false; state.error = error; state.readySince = node.now();
        if (error.empty()) { state.failures = 0; state.retry = {}; }
        else {
          if (state.progress != progress) state.failures = 0;
          auto delay = std::min<Clock::duration>(60s, retryDelay * (1 << std::min(6u, state.failures)));
          state.failures++; state.retry = node.now() + delay;
        }
        if (!state.unreadable && !state.advertised && !state.serving && !state.selectedName && !state.candidate && !state.prepared && !state.retiring && error.empty()) states.erase(key);
      }
      applyMappings();
      changed.notify_all();
    }
  }

};

ReplicationFollower::ReplicationFollower(LuxirNode& node) : impl(std::make_unique<Impl>(node)) {}
ReplicationFollower::~ReplicationFollower() { stop(); }
void ReplicationFollower::seed(const std::vector<Collections::Opened>& opened) { impl->seed(opened); }
bool ReplicationFollower::pull(std::ostream& output) { return impl->pull(output); }
void ReplicationFollower::start() {
  {
    std::lock_guard lock(impl->mutex);
    impl->follower = impl->node.nodeId();
  }
  impl->threads.emplace_back([this] { impl->watch(); });
  for (int i = 0; i < impl->node.getConfig().replication.downloads; i++) impl->threads.emplace_back([this] { impl->worker(); });
}
void ReplicationFollower::stop() {
  impl->stopping.request_stop(); impl->changed.notify_all(); impl->threads.clear();
}
void ReplicationFollower::deleteOrphan(const CollectionId& name) {
  Collections::validate(name);
  Impl::Key key;
  {
    std::lock_guard applyLock(impl->mappingMutex);
    std::lock_guard lock(impl->mutex);
    auto it = std::ranges::find_if(impl->states, [&](const auto& entry) { return entry.first.tenant == name.tenant && entry.second.selectedName == name.name; });
    if (it == impl->states.end()) throw CollectionNotFoundError("local orphan does not exist");
    key = it->first;
    if (impl->includes(name) && (!impl->connected || !impl->discovered || it->second.advertised || it->second.unavailable))
      throw ReadOnlyError("only an excluded local copy or an orphan absent from the connected source may be deleted");
    if (it->second.busy || (impl->mappings.contains(key.tenant) && impl->mappings.at(key.tenant).applying)) throw CollectionUnavailableError("local orphan is busy");
    it->second.busy = true;
  }
  try { impl->eraseLocal(key); }
  catch (...) {
    std::lock_guard lock(impl->mutex); impl->states[key].busy = false; impl->changed.notify_all(); throw;
  }
  {
    std::lock_guard lock(impl->mutex);
    impl->states.erase(key);
  }
  impl->persistState();
  impl->changed.notify_all();
}
void ReplicationFollower::repair(const CollectionId& id, const std::shared_ptr<Collection>& collection,
                                 const std::string& reason, std::span<const FileDescriptor> suspects) {
  auto repair = std::make_shared<const Impl::Repair>(Impl::Repair{collection, {suspects.begin(), suspects.end()}, reason});
  {
    std::lock_guard lock(impl->mutex);
    for (auto& [key, state] : impl->states) if (key.tenant == id.tenant && (state.local == collection || state.selectedName == id.name)) state.corrupt = repair;
  }
  impl->changed.notify_all();
}
void ReplicationFollower::stats(api::ReplicationStatus& out, std::pmr::memory_resource& arena) {
  std::lock_guard lock(impl->mutex);
  auto str = [&](std::string_view value) { return api::build::arenaStr(arena, value); };
  out.source = str(impl->binding.source); out.follower = str(impl->follower);
  out.connected = impl->connected; out.last_contact = impl->lastContact;
  std::map<CollectionId, const Impl::State*> visible;
  std::map<CollectionId, std::optional<CommitId>> serving;
  for (const auto& [key, state] : impl->states) if (state.advertised) visible[state.name] = &state;
  for (const auto& entry : impl->node.collectionEntries()) {
    auto shard = entry.collection->getShard();
    auto snapshot = shard ? shard->getSnapshots().snapshot() : nullptr;
    serving[entry.id] = snapshot ? std::optional(snapshot->id) : std::nullopt;
    if (!visible.contains(entry.id)) {
      for (const auto& [key, state] : impl->states) if (key.tenant == entry.id.tenant && state.selectedName == entry.id.name) {
        visible.emplace(entry.id, &state); break;
      }
    }
  }
  auto* rows = api::build::allocArray(out.collections, visible.size(), arena);
  size_t i = 0;
  for (const auto& [name, value] : visible) {
    const auto& state = *value;
    auto token = [&](const std::optional<CommitId>& id) { return id ? str(id->token()) : std::string_view(); };
    auto installed = serving[name];
    auto advertised = state.name == name ? state.advertised : std::nullopt;
    auto& row = rows[i++];
    row.name = str(name.name); row.tenant = str(name.tenant);
    row.source_commit = token(advertised); row.serving_commit = token(installed);
    using State = api::ReplicationCollectionStatus::State;
    row.state = !impl->includes(name) ? State::ORPHAN : (!impl->connected || state.unavailable) ? State::STALE : !advertised ? State::ORPHAN : !state.error.empty() ? State::ERROR
        : state.waiting ? State::WAITING : advertised == installed ? State::SERVING : State::SYNCING;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(state.retry - impl->node.now()).count();
    if (!state.error.empty() && remaining > 0) row.next_retry = wallTime() + (uint64_t)remaining;
    row.bytes_downloaded = state.downloaded; row.bytes_total = state.total; row.last_error = str(state.error.empty() && !impl->connected ? impl->discoveryError : state.error);
  }
}
}
