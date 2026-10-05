// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationFollower.h"
#include "LuxirNode.h"
#include "CollectionEvents.h"
#include "ReplicationState.h"
#include "luxir/api/build.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <glaze/glaze.hpp>
#include <condition_variable>
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
  if (value.size() != 36) throw std::invalid_argument("invalid source incarnation");
  for (size_t i = 0; i < value.size(); i++) {
    bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? value[i] != '-' : !std::isxdigit((unsigned char)value[i])) {
      throw std::invalid_argument("invalid source incarnation");
    }
  }
}
void syncDir(Directory& dir) {
  std::array<std::string, 1> names{"."};
  dir.sync(names);
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
}

struct ReplicationFollower::Impl {
  struct State {
    // advertised: the source's current commit; serving: installed locally;
    // acknowledged: last serving commit the source accepted. Independent.
    std::optional<CommitId> advertised, serving, acknowledged;
    std::string error;
    bool busy = false, waiting = false, remove = false, unavailable = false;
    std::string seenBoot;
    bool replaceEmpty = true;
    // The local copy could not be opened; CURRENT, if it could be read, selects
    // this incarnation ("" otherwise). It is retained until an install replaces it.
    std::optional<std::string> unreadable;
    unsigned failures = 0;
    uint64_t progress = 0;
    Clock::time_point readySince = Clock::now();
    Clock::time_point retry{};
    uint64_t downloaded = 0, reused = 0, total = 0;
    // The incarnation `verified` describes, and its staged candidate when
    // that is not the active collection's incarnation.
    std::string targetIncarnation;
    std::optional<Collections::Candidate> candidate;
    std::map<std::string, FileDescriptor> verified;
    bool needsAck() const {
      return serving && acknowledged != serving && advertised && advertised->incarnation == serving->incarnation;
    }
  };
  LuxirNode& node;
  Source source;
  ReplicationState binding;
  std::mutex metadataMutex;
  std::string persisted;
  std::shared_ptr<Directory> metadata;
  std::mutex mutex; // status/scheduling only, never held across storage or network I/O
  std::condition_variable_any changed;
  std::map<std::string, State> states;
  bool connected = false, discovered = false;
  uint64_t lastContact = 0;
  std::string boot, discoveryError;
  std::string cursor;
  std::stop_source stopping;
  std::vector<std::jthread> threads;

  explicit Impl(LuxirNode& node) : node(node), source(node.getConfig().replication.source) {
    if (node.getConfig().replication.downloads < 1 || node.getConfig().replication.downloads > 64) {
      throw std::invalid_argument("replication.downloads must be between 1 and 64");
    }
    binding.source = "http://" + source.authority;
    binding.follower = node.getConfig().replication.follower_id;
    bool fs = node.getConfig().store.backend == "fs";
    metadata = fs ? std::shared_ptr<Directory>(std::make_shared<FSDirectory>(node.getConfig().store.data_dir))
                  : std::make_shared<RAMDir>();
    if (auto stored = metadata->openFile("replication.json")) {
      auto existing = ReplicationState::read(*stored);
      if (!binding.follower.empty() && binding.follower != existing.follower) {
        throw std::invalid_argument("configured follower id differs from persisted follower id");
      }
      existing.source = binding.source;
      binding = std::move(existing);
    } else {
      if (fs) for (const auto& entry : std::filesystem::directory_iterator(node.getConfig().store.data_dir)) {
        auto name = entry.path().filename().string();
        if (name == "write.lock" || name == "replication.json.pending") continue;
        if (name == "c" && std::filesystem::is_empty(entry.path())) continue;
        throw std::invalid_argument("replication requires an empty data directory or a follower data directory");
      }
      if (binding.follower.empty()) binding.follower = newUuid();
      if (binding.follower.size() > 255) throw std::invalid_argument("invalid follower id");
      binding.write(*metadata);
    }
    if (binding.follower.empty() || binding.follower.size() > 255) throw std::invalid_argument("invalid follower id");
  }

  void seed(const std::vector<Collections::Opened>& opened) {
    for (const auto& row : opened) {
      // A collection without CURRENT keeps verified candidate files for a
      // restarted download; the next successful install sweeps them.
      if (!row.collection) continue;
      auto& state = states[row.name];
      if (row.error.empty()) state.serving = row.collection->getShard()->getSnapshots().snapshot()->id;
      else {
        state.unreadable = row.incarnation.value_or("");
        state.error = row.error;
      }
      auto& saved = binding.collections[row.name];
      state.seenBoot = saved.boot;
      if (!saved.advertised.empty()) state.advertised = CommitId::parse(saved.advertised);
      state.replaceEmpty = saved.replace_empty;
    }
    persistState();
  }

  // Serialize metadata I/O separately from discovery/scheduling. No fsync holds
  // the state mutex or stalls a watch response being applied in memory.
  void persistState() {
    std::lock_guard metadataLock(metadataMutex);
    ReplicationState next;
    {
      std::lock_guard lock(mutex);
      next.source = binding.source; next.follower = binding.follower;
      for (const auto& [name, state] : states)
        next.collections.emplace(name, ReplicationState::Collection{
            state.advertised ? state.advertised->token() : "", state.seenBoot, state.replaceEmpty, {}});
    }
    auto bytes = glz::write_json(next).value();
    if (bytes != persisted) { next.write(*metadata); persisted = std::move(bytes); }
  }
  std::string followerQuery() const { return "follower=" + escape(binding.follower); }
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
  void eraseLocal(const std::string& name) {
    std::optional<Collections::Candidate> candidate;
    { std::lock_guard lock(mutex); candidate = std::exchange(states[name].candidate, std::nullopt); }
    if (candidate) node.collections().discard(*candidate);
    node.collections().remove(name, false);
    std::lock_guard lock(mutex);
    auto& state = states[name];
    state.serving.reset(); state.acknowledged.reset(); state.unreadable.reset();
    state.verified.clear(); state.targetIncarnation.clear(); state.remove = false;
  }

  api::ReplicationCatalog fetchCatalog(Client& client, const std::string& cursor, std::chrono::milliseconds timeout, std::pmr::memory_resource& arena) {
    auto response = request(client, http::verb::get, "/_replication/watch?" + followerQuery()
        + "&since=" + escape(cursor) + "&timeout_ms=" + std::to_string(timeout.count()), {}, timeout + 10s);
    api::ReplicationCatalog catalog;
    if (!api::read_json(catalog, api::build::arenaStr(arena, response.body()), arena) || catalog.boot.empty() || catalog.cursor.empty()) throw std::runtime_error("invalid source catalog");
    for (const auto& [name, entry] : catalog.collections) {
      Collections::validateName(name);
      if (!entry.commit.empty()) validateIncarnation(CommitId::parse(entry.commit).incarnation);
    }
    return catalog;
  }

  static std::optional<CommitId> advertisedCommit(const api::ReplicationCatalogEntry& entry) {
    if (entry.commit.empty()) return std::nullopt;
    return CommitId::parse(entry.commit);
  }

  // Applies one collection's entry from a source catalog (absent: not in it).
  // A new incarnation may replace a populated copy with an empty snapshot only
  // if this follower saw the previous incarnation in the same source boot.
  void observe(State& state, std::optional<CommitId> desired, bool present, const std::string& sourceBoot) {
    if (desired && (!state.advertised || desired->incarnation != state.advertised->incarnation))
      state.replaceEmpty = state.seenBoot == sourceBoot;
    if (desired != state.advertised) {
      if (!state.advertised || state.advertised == state.serving || state.waiting) state.readySince = Clock::now();
      state.advertised = std::move(desired); state.waiting = false;
    }
    if (present) { state.seenBoot = sourceBoot; state.remove = false; }
    else state.remove = state.seenBoot == sourceBoot;
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
        {
          std::lock_guard lock(mutex);
          if (boot != catalog.boot) for (auto& [name, state] : states) state.acknowledged.reset();
          boot = catalog.boot; cursor = catalog.cursor;
          connected = discovered = true; discoveryError.clear();
          for (const auto& [name, entry] : catalog.collections) states.try_emplace(std::string(name));
          for (auto& [name, state] : states) {
            auto found = catalog.collections.find(name);
            bool present = found != nullptr;
            state.unavailable = present && !found->available;
            observe(state, present ? advertisedCommit(*found) : std::nullopt, present, boot);
            if (!state.busy && state.serving && state.advertised == state.serving && state.acknowledged == state.serving) {
              state.error.clear(); state.failures = 0; state.retry = {};
            }
          }
          std::erase_if(states, [](const auto& entry) {
            const auto& state = entry.second;
            return !state.unreadable && !state.unavailable && !state.advertised && !state.serving && !state.candidate && !state.busy;
          });
        }
        persistState();
        changed.notify_all();
      } catch (const std::exception& e) {
        if (stopping.stop_requested()) break;
        client.reset(); refresh = true;
        auto message = errorMessage(e);
        { std::lock_guard lock(mutex); connected = false; discoveryError = message; }
        LOG_WARN("Replication source {} unavailable: {}", binding.source, message);
        std::unique_lock lock(mutex);
        changed.wait_for(lock, stopping.get_token(), 1s, [] { return false; });
      }
    }
  }

  void download(Client& client, const std::string& collection, Directory& dir, const CommitSnapshot& snapshot,
                const FileDescriptor& descriptor) {
    Signal::emit("replicationDownloadStart", (void*)&descriptor);
    Directory::FileCreateOptions options; options.expectedSize = descriptor.size;
    auto file = dir.createFile(descriptor.name, options);
    OutputStream out(file.get());
    uint64_t offset = 0;
    int failures = 0;
    while (offset < descriptor.size || (descriptor.size == 0 && failures == 0)) {
      if (stopping.stop_requested()) throw std::runtime_error("follower stopped");
      try {
        auto path = "/collections/" + collection + "/_snapshot/files/" + escape(descriptor.name) + "?commit=" + escape(snapshot.id.token()) + "&" + followerQuery();
        client.send(http::verb::get, path, {}, offset);
        http::response_parser<http::buffer_body> parser;
        // Error bodies (especially 410 for a tiny file) can exceed file size.
        // Check status first, then require the exact remaining Content-Length.
        parser.body_limit(UINT64_MAX);
        client.header(parser);
        int status = parser.get().result_int();
        if (status == 410) throw PinGone();
        if (status != (offset ? 206 : 200)) throw std::runtime_error("source file HTTP " + std::to_string(status));
        if (parser.chunked() || parser.content_length() != descriptor.size - offset || parser.get()["X-Luxir-Commit"] != snapshot.id.token()) {
          throw std::runtime_error("source file length or commit mismatch");
        }
        if (offset && parser.get()[http::field::content_range] != "bytes " + std::to_string(offset) + "-" + std::to_string(descriptor.size - 1) + "/" + std::to_string(descriptor.size)) {
          throw std::runtime_error("source file range mismatch");
        }
        client.body(descriptor.size - offset, [&](const char* bytes, size_t received) {
          Signal::emit("replicationDownloadWrite", &dir);
          out.write(bytes, received); offset += received;
          {
            std::lock_guard lock(mutex); states[collection].downloaded += received; lastContact = wallTime();
          }
          Signal::emit("replicationDownloadProgress", &offset);
          if (stopping.stop_requested()) throw std::runtime_error("follower stopped");
        });
        if (!parser.get().keep_alive()) client.reset();
        break;
      } catch (const PinGone&) { client.reset(); throw; }
      catch (...) { client.reset(); if (++failures >= 3 || stopping.stop_requested()) throw; }
    }
    out.close();
    if (file->size() != descriptor.size || file->digest() != descriptor.xxh3) throw std::runtime_error("source file checksum mismatch: " + descriptor.name);
    dir.finishFile(*file);
    Signal::emit("replicationFileVerified", &file);
  }

  std::shared_ptr<const CommitSnapshot> fetchSnapshot(Client& client, const std::string& name) {
    auto response = request(client, http::verb::get, "/collections/" + name + "/_snapshot?" + followerQuery());
    auto& body = response.body();
    auto bytes = std::make_shared<const std::vector<std::byte>>((const std::byte*)body.data(), (const std::byte*)body.data() + body.size());
    auto snapshot = CommitSnapshot::fromBytes(bytes);
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

  Decision decide(const std::string& name, const CommitSnapshot& snapshot) {
    Decision decision;
    std::lock_guard lock(mutex);
    auto& state = states[name];
    if (!state.advertised) throw std::runtime_error("source collection disappeared during transfer");
    if (state.advertised->incarnation != snapshot.id.incarnation)
      throw std::runtime_error("source incarnation changed; retry snapshot");
    for (const auto& file : snapshot.files) state.total += file.size;
    decision.active = node.collections().get(name);
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
    bool populated = serving ? serving->populated : state.unreadable.has_value();
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
  // is missing, and makes them durable. Returns the target's prior snapshot.
  void stageFiles(Client& client, const std::string& name, Directory& dir, const std::shared_ptr<const CommitSnapshot>& previous,
                  const CommitSnapshot& snapshot, const std::shared_ptr<Collection>& active) {
    std::map<std::string, FileDescriptor> verified;
    { std::lock_guard lock(mutex); verified = states[name].verified; }
    if (!previous) {
      if (auto shard = active ? active->getShard() : nullptr) {
        auto& source = shard->getSnapshots();
        if (auto serving = source.snapshot()) {
          for (auto& file : dir.reuseFiles(source.dir, serving->files, snapshot.files))
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
          || file.name.starts_with("s.olux") || file.name.ends_with(".tmp") || !unique.insert(file.name).second) throw std::runtime_error("invalid snapshot file name");
      bool reuse = verified.contains(file.name) && verified.at(file.name) == file;
      if (!reuse) {
        // Unpublished verified files can survive a reconnect or failed install.
        // After process restart only these candidates need to be hashed.
        if (auto local = dir.openFile(file.name)) {
          auto data = local->read();
          reuse = data.size() == file.size && XXH3_64bits(data.data(), data.size()) == file.xxh3;
          if (!reuse && previous) {
            for (const auto& live : previous->files) if (live.name == file.name) throw std::runtime_error("source changed immutable file: " + file.name);
          }
        }
      }
      if (!reuse) download(client, name, dir, snapshot, file);
      { std::lock_guard lock(mutex);
        auto& state = states[name];
        if (reuse) state.reused += file.size;
        if (!state.verified.contains(file.name)) state.progress++;
        state.verified.insert_or_assign(file.name, file);
      }
      // Files in the serving snapshot are already durable. Retried candidates
      // may have been verified but not synced before their pin vanished.
      if (!durableNames.contains(file.name)) names.push_back(file.name);
    }
    dir.sync(names); syncDir(dir);
  }

  void sync(Client& client, const std::string& name) {
    { std::lock_guard lock(mutex);
      auto& state = states[name]; state.total = state.downloaded = state.reused = 0;
    }
    auto snapshot = fetchSnapshot(client, name);
    auto decision = decide(name, *snapshot);
    if (decision.obsolete) node.collections().discard(*decision.obsolete);
    if (decision.current || !decision.eligible) return;
    auto stillAdvertised = [&] {
      std::lock_guard lock(mutex);
      auto& advertised = states[name].advertised;
      if (!advertised || advertised->incarnation != snapshot->id.incarnation)
        throw std::runtime_error("source collection changed during transfer");
    };
    // This worker owns the name's state while busy, including its candidate.
    std::shared_ptr<Collection> installed;
    if (decision.advance) {
      auto& registry = decision.active->getShard()->getSnapshots();
      auto previous = registry.snapshot();
      stageFiles(client, name, registry.dir, previous, *snapshot, decision.active);
      auto opened = registry.readers.prepare(*snapshot);
      stillAdvertised();
      if (stopping.stop_requested()) return;
      registry.commit(snapshot, std::move(opened));
      node.collections().retainSelected(name);
      installed = decision.active;
    } else {
      auto& state = states[name];
      if (!state.candidate) {
        auto candidate = node.collections().stage(name, snapshot->id.incarnation);
        std::lock_guard lock(mutex); state.candidate = std::move(candidate);
      }
      stageFiles(client, name, state.candidate->dir(), state.candidate->committed(), *snapshot, decision.active);
      stillAdvertised();
      if (stopping.stop_requested()) return;
      // An unregistered candidate's publication is invisible until activation.
      auto prepared = node.collections().prepare(*state.candidate, snapshot);
      { std::lock_guard lock(mutex); state.candidate.reset(); }
      installed = node.collections().install(std::move(prepared), decision.active);
    }
    try { installed->getShard()->getSnapshots().sweepOrphans(); }
    catch (const std::exception& e) { LOG_WARN("Follower retirement failed: {}", e.what()); }
    {
      std::lock_guard lock(mutex);
      auto& state = states[name]; state.unreadable.reset();
      state.serving = snapshot->id; state.error.clear(); state.verified.clear();
      auto& advertised = state.advertised;
      if (advertised && advertised->incarnation == snapshot->id.incarnation && advertised->index_gen < snapshot->id.index_gen) advertised = state.serving;
    }
  }

  bool pull(std::ostream& output) {
    if (!threads.empty() || stopping.stop_requested()) throw std::logic_error("pull requires an unstarted follower");
    Client client(source, stopping.get_token());
    api::ReplicationCatalog catalog;
    std::pmr::monotonic_buffer_resource arena;
    try { catalog = fetchCatalog(client, {}, 0ms, arena); }
    catch (const std::exception& e) {
      output << "Pull failed: " << binding.source << ": " << errorMessage(e) << '\n';
      return false;
    }
    size_t installed = 0, failed = 0;
    uint64_t transferred = 0, reused = 0;
    for (const auto& [name, entry] : catalog.collections) {
      auto& state = states[std::string(name)];
      observe(state, advertisedCommit(entry), true, std::string(catalog.boot));
      state.downloaded = state.reused = 0;
      uint64_t downloaded = 0;
      try {
        if (!entry.available) throw std::runtime_error("source collection unavailable");
        for (unsigned attempt = 0;; attempt++) {
          try { sync(client, std::string(name)); break; }
          catch (const PinGone&) { if (attempt == 2) throw; downloaded += state.downloaded; }
        }
        persistState();
        if (state.serving != state.advertised) throw std::runtime_error("waiting for source data before replacing the local snapshot");
        installed++;
        output << name << ' ' << state.serving->token() << " transferred=" << downloaded + state.downloaded << " reused=" << state.reused << '\n';
      } catch (const std::exception& e) {
        client.reset(); failed++;
        output << name << ' ' << entry.commit << " transferred=" << downloaded + state.downloaded
               << " reused=" << state.reused << " ERROR: " << errorMessage(e) << '\n';
      }
      transferred += downloaded + state.downloaded; reused += state.reused;
      output.flush();
    }
    output << "Pull: " << installed << " installed, " << failed << " failed, transferred=" << transferred << " reused=" << reused << '\n';
    return failed == 0;
  }

  void worker() {
    Client client(source, stopping.get_token());
    while (!stopping.stop_requested()) {
      std::string name;
      uint64_t progress = 0;
      {
        std::unique_lock lock(mutex);
        auto ready = [&] {
          if (!connected) return false;
          State* selected = nullptr;
          for (auto& [key, state] : states) {
            bool work = state.remove || (state.advertised && ((state.advertised != state.serving && !state.waiting)
                    || state.needsAck()));
            if (!state.busy && !state.unavailable && work && Clock::now() >= state.retry && (!selected || std::max(state.readySince, state.retry) < std::max(selected->readySince, selected->retry))) {
              name = key; selected = &state;
            }
          }
          if (!selected) return false;
          selected->busy = true; progress = selected->progress;
          return true;
        };
        changed.wait_for(lock, stopping.get_token(), 200ms, ready);
        if (stopping.stop_requested()) break;
        if (name.empty()) continue;
      }
      std::string error;
      try {
        bool remove, needsSync;
        std::optional<CommitId> serving;
        {
          std::lock_guard lock(mutex);
          auto& state = states[name]; remove = state.remove;
          needsSync = state.advertised && state.advertised != state.serving && !state.waiting;
        }
        if (remove) eraseLocal(name);
        else {
          auto acknowledge = [&] {
            std::string ackBoot;
            {
              std::lock_guard lock(mutex);
              auto& state = states[name];
              serving = state.needsAck() ? state.serving : std::nullopt;
              ackBoot = boot;
            }
            if (!serving) return;
            auto token = serving->token();
            api::ReplicationInstalled ack{binding.follower, name, token};
            std::string body;
            if (!api::write_json(ack, body)) throw std::runtime_error("failed to serialize acknowledgment");
            request(client, http::verb::post, "/_replication/installed", body);
            std::lock_guard lock(mutex);
            if (boot == ackBoot) states[name].acknowledged = serving;
          };
          acknowledge();
          if (needsSync) sync(client, name);
          acknowledge();
        }
        persistState();
      } catch (const PinGone& e) { error = e.what(); Signal::emit("replicationPinGone"); }
      catch (const std::exception& e) { error = errorMessage(e); }
      if (!error.empty()) client.reset();
      {
        std::lock_guard lock(mutex);
        auto& state = states[name];
        // Discovery may have superseded a failed target while this job ran.
        if (state.serving && state.advertised == state.serving && state.acknowledged == state.serving) error.clear();
        state.busy = false; state.error = error; state.readySince = Clock::now();
        if (error.empty()) { state.failures = 0; state.retry = {}; }
        else {
          if (state.progress != progress) state.failures = 0;
          auto delay = std::chrono::seconds(std::min(60u, 1u << std::min(6u, state.failures)));
          state.failures++; state.retry = Clock::now() + delay;
        }
        if (!state.unreadable && !state.advertised && !state.serving && !state.candidate && error.empty()) states.erase(name);
      }
      changed.notify_all();
    }
  }

};

ReplicationFollower::ReplicationFollower(LuxirNode& node) : impl(std::make_unique<Impl>(node)) {}
ReplicationFollower::~ReplicationFollower() { stop(); }
void ReplicationFollower::seed(const std::vector<Collections::Opened>& opened) { impl->seed(opened); }
bool ReplicationFollower::pull(std::ostream& output) { return impl->pull(output); }
void ReplicationFollower::start() {
  impl->threads.emplace_back([this] { impl->watch(); });
  for (int i = 0; i < impl->node.getConfig().replication.downloads; i++) impl->threads.emplace_back([this] { impl->worker(); });
}
void ReplicationFollower::stop() {
  impl->stopping.request_stop(); impl->changed.notify_all(); impl->threads.clear();
}
void ReplicationFollower::deleteOrphan(std::string_view name) {
  Collections::validateName(name);
  std::string key(name);
  {
    std::unique_lock lock(impl->mutex);
    if (!impl->connected || !impl->discovered || (impl->states.contains(key) && (impl->states.at(key).advertised || impl->states.at(key).unavailable))) {
      throw ReadOnlyError("only a local orphan absent from the connected source may be deleted");
    }
    if (impl->states.contains(key) && impl->states.at(key).busy) throw CollectionUnavailableError("local orphan is busy");
    if (!impl->node.collections().get(key)) throw CollectionNotFoundError("local orphan does not exist");
    impl->states[key].busy = true;
  }
  try { impl->eraseLocal(key); }
  catch (...) {
    std::lock_guard lock(impl->mutex); impl->states[key].busy = false; impl->changed.notify_all(); throw;
  }
  std::lock_guard lock(impl->mutex);
  impl->states[key].busy = false;
  if (!impl->states[key].advertised) impl->states.erase(key);
  impl->changed.notify_all();
}
void ReplicationFollower::stats(api::ReplicationStatus& out, std::pmr::memory_resource& arena) {
  std::lock_guard lock(impl->mutex);
  auto str = [&](std::string_view value) { return api::build::arenaStr(arena, value); };
  out.source = str(impl->binding.source); out.follower = str(impl->binding.follower);
  out.connected = impl->connected; out.last_contact = impl->lastContact;
  auto* rows = api::build::allocArray(out.collections, impl->states.size(), arena);
  size_t i = 0;
  for (const auto& [name, state] : impl->states) {
    auto token = [&](const std::optional<CommitId>& id) { return id ? str(id->token()) : std::string_view(); };
    auto& row = rows[i++]; row.name = str(name); row.source_commit = token(state.advertised); row.serving_commit = token(state.serving);
    using State = api::ReplicationCollectionStatus::State;
    row.state = (!impl->connected || state.unavailable) ? State::STALE : !state.advertised ? State::ORPHAN : !state.error.empty() ? State::ERROR
        : state.waiting ? State::WAITING : state.advertised == state.serving ? State::SERVING : State::SYNCING;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(state.retry - Clock::now()).count();
    if (!state.error.empty() && remaining > 0) row.next_retry = wallTime() + (uint64_t)remaining;
    row.bytes_downloaded = state.downloaded; row.bytes_total = state.total; row.last_error = str(state.error.empty() && !impl->connected ? impl->discoveryError : state.error);
  }
}
}
