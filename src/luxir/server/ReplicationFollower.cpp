// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationFollower.h"
#include "LuxirNode.h"
#include "ReplicationCatalog.h"
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

struct CatalogEntry { std::string commit; std::string state; };
struct Catalog {
  std::string boot;
  std::string cursor;
  std::map<std::string, CatalogEntry> collections;
};
struct Ack { std::string follower; std::string collection; std::string commit; };

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
struct ReservationGone : std::runtime_error { ReservationGone() : std::runtime_error("source reservation expired") {} };
}

struct ReplicationFollower::Impl {
  struct State {
    std::string source, serving, acknowledged, error;
    bool busy = false, waiting = false, remove = false, unavailable = false;
    std::string seenBoot;
    bool replaceEmpty = true;
    unsigned failures = 0;
    uint64_t progress = 0;
    Clock::time_point readySince = Clock::now();
    Clock::time_point retry{};
    uint64_t downloaded = 0, reused = 0, total = 0;
    std::shared_ptr<Collection> candidate;
    std::string candidateIncarnation;
    std::map<std::string, FileDescriptor> verified;
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

  explicit Impl(LuxirNode& node) : node(node), source(node.config.replication.source) {
    if (node.config.replication.downloads < 1 || node.config.replication.downloads > 64) {
      throw std::invalid_argument("replication.downloads must be between 1 and 64");
    }
    binding.source = "http://" + source.authority;
    binding.follower = node.config.replication.follower_id;
    bool fs = node.config.store.backend == "fs";
    metadata = fs ? std::shared_ptr<Directory>(std::make_shared<FSDirectory>(node.config.store.data_dir))
                  : std::make_shared<RAMDir>();
    if (auto stored = metadata->openFile("replication.json")) {
      auto existing = ReplicationState::read(*stored);
      if (!binding.follower.empty() && binding.follower != existing.follower) {
        throw std::invalid_argument("configured follower id differs from persisted follower id");
      }
      existing.source = binding.source;
      binding = std::move(existing);
    } else {
      if (fs) for (const auto& entry : std::filesystem::directory_iterator(node.config.store.data_dir)) {
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
    for (const auto& name : node.dirFactory->listDirectories()) {
      try {
        LuxirNode::validateCollectionName(name);
        auto selection = DirectoryFactory::current(*node.dirFactory->create(name));
        if (selection.incarnation.empty()) {
          std::vector<Directory::FileInfo> files;
          node.dirFactory->create(name)->listFiles(files);
          if (!files.empty()) throw std::runtime_error("Collection has files without CURRENT");
          for (const auto& incarnation : node.dirFactory->listDirectories(name)) validateIncarnation(incarnation);
          continue; // retain completed candidate files for a restarted download
        }
        validateIncarnation(selection.incarnation);
        auto col = node.makeCollection(name, node.dirFactory->create(name + "/" + selection.incarnation));
        auto& snapshots = col->getShard()->getSnapshots();
        snapshots.openLocalSnapshot();
        if (snapshots.snapshot()->id.incarnation != selection.incarnation) throw std::runtime_error("local incarnation does not match CURRENT");
        // Candidates can contain verified files from an interrupted transfer.
        // The next successful install sweeps them against its installed root.
        node.root->collections.getOrCreate(name, [&] { return col; });
        node.observeCollection(name, *col);
        auto& state = states[name];
        state.serving = snapshots.snapshot()->id.token();
        auto& saved = binding.collections[name];
        state.seenBoot = saved.boot;
        state.source = saved.source;
        state.replaceEmpty = saved.replace_empty;
      } catch (const std::exception& e) {
        LOG_WARN("Discarding local replica '{}': {}", name, errorMessage(e));
        try { node.dirFactory->remove(name); }
        catch (const std::exception& cleanup) { LOG_WARN("Replica cleanup '{}' failed: {}", name, errorMessage(cleanup)); }
      }
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
        next.collections.emplace(name, ReplicationState::Collection{state.source, state.seenBoot, state.replaceEmpty, {}});
    }
    auto bytes = glz::write_json(next).value();
    if (bytes != persisted) { next.write(*metadata); persisted = std::move(bytes); }
  }
  std::string followerQuery() const { return "follower=" + escape(binding.follower); }
  http::response<http::string_body> request(Client& client, http::verb method, const std::string& path, const std::string& body = {}, std::chrono::milliseconds timeout = 40s) {
    http::response<http::string_body> response;
    try { client.send(method, path, body); response = client.response(timeout); }
    catch (...) { client.reset(); throw; }
    if (response.result_int() == 410) throw ReservationGone();
    if (response.result_int() != 200) throw std::runtime_error("source HTTP " + std::to_string(response.result_int()) + ": " + response.body());
    { std::lock_guard lock(mutex); lastContact = wallTime(); }
    return response;
  }
  // The worker (or orphan admin) owns this collection's busy flag. Discovery
  // only updates desired state, and never waits for storage operations.
  void eraseLocal(const std::string& name) {
    auto old = node.root->collections.get(name);
    if (old) {
      node.root->collections.erase(name, old);
      old->getShard()->getSnapshots().detach();
      node.replication->remove(name);
    }
    std::shared_ptr<Collection> candidate;
    { std::lock_guard lock(mutex); candidate = std::move(states[name].candidate); }
    if (candidate && candidate != old) candidate->getShard()->getSnapshots().close();
    node.dirFactory->remove(name);
    std::lock_guard lock(mutex);
    auto& state = states[name];
    state.serving.clear(); state.acknowledged.clear();
    state.verified.clear(); state.candidateIncarnation.clear(); state.remove = false;
  }

  Catalog fetchCatalog(Client& client, const std::string& cursor, std::chrono::milliseconds timeout) {
    auto response = request(client, http::verb::get, "/_replication/watch?" + followerQuery()
        + "&since=" + escape(cursor) + "&timeout_ms=" + std::to_string(timeout.count()), {}, timeout + 10s);
    Catalog catalog;
    if (glz::read_json(catalog, response.body()) || catalog.boot.empty() || catalog.cursor.empty()) throw std::runtime_error("invalid source catalog");
    for (const auto& [name, entry] : catalog.collections) {
      LuxirNode::validateCollectionName(name);
      if (!entry.commit.empty()) validateIncarnation(CommitId::parse(entry.commit).incarnation);
    }
    return catalog;
  }

  void watch() {
    Client client(source, stopping.get_token());
    bool refresh = true;
    auto timeout = std::chrono::milliseconds(node.config.replication.follower_timeout_ms / 3);
    while (!stopping.stop_requested()) {
      try {
        auto catalog = fetchCatalog(client, refresh ? "" : cursor, timeout);
        refresh = false;
        {
          std::lock_guard lock(mutex);
          if (boot != catalog.boot) for (auto& [name, state] : states) state.acknowledged.clear();
          boot = catalog.boot; cursor = catalog.cursor;
          connected = discovered = true; discoveryError.clear();
          for (const auto& [name, entry] : catalog.collections) states.try_emplace(name);
          for (auto& [name, state] : states) {
            auto found = catalog.collections.find(name);
            state.unavailable = found != catalog.collections.end() && found->second.state == "unavailable";
            std::string desired = found == catalog.collections.end() ? "" : found->second.commit;
            if (!desired.empty() && (state.source.empty() || CommitId::parse(desired).incarnation != CommitId::parse(state.source).incarnation)) {
              state.replaceEmpty = state.seenBoot == boot;
            }
            if (desired != state.source) {
              if (state.source.empty() || state.source == state.serving || state.waiting) state.readySince = Clock::now();
              state.source = desired; state.waiting = false;
            }
            if (found != catalog.collections.end()) {
              state.seenBoot = boot;
              state.remove = false;
            } else state.remove = state.seenBoot == boot;
            if (!state.busy && !state.serving.empty() && state.source == state.serving && state.acknowledged == state.serving) {
              state.error.clear(); state.failures = 0; state.retry = {};
            }
          }
          std::erase_if(states, [](const auto& entry) {
            const auto& state = entry.second;
            return !state.unavailable && state.source.empty() && state.serving.empty() && !state.candidate && !state.busy;
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
        auto path = "/_replication/" + collection + "/file/" + escape(descriptor.name) + "?commit=" + escape(snapshot.id.token()) + "&" + followerQuery();
        client.send(http::verb::get, path, {}, offset);
        http::response_parser<http::buffer_body> parser;
        // Error bodies (especially 410 for a tiny file) can exceed file size.
        // Check status first, then require the exact remaining Content-Length.
        parser.body_limit(UINT64_MAX);
        client.header(parser);
        int status = parser.get().result_int();
        if (status == 410) throw ReservationGone();
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
      } catch (const ReservationGone&) { client.reset(); throw; }
      catch (...) { client.reset(); if (++failures >= 3 || stopping.stop_requested()) throw; }
    }
    out.close();
    if (file->size() != descriptor.size || file->digest() != descriptor.xxh3) throw std::runtime_error("source file checksum mismatch: " + descriptor.name);
    dir.finishFile(*file);
    Signal::emit("replicationFileVerified", &file);
  }

  void sync(Client& client, const std::string& name) {
    { std::lock_guard lock(mutex);
      auto& state = states[name]; state.total = state.downloaded = state.reused = 0;
    }
    auto response = request(client, http::verb::get, "/_replication/" + name + "/snapshot?" + followerQuery());
    auto& body = response.body();
    auto bytes = std::make_shared<const std::vector<std::byte>>((const std::byte*)body.data(), (const std::byte*)body.data() + body.size());
    auto snapshot = CommitSnapshot::fromBytes(bytes);
    validateIncarnation(snapshot->id.incarnation);
    if (response["X-Luxir-Commit"] != snapshot->id.token()) throw std::runtime_error("snapshot commit mismatch");
    std::pmr::monotonic_buffer_resource arena;
    auto info = Manifest::decode(bytes, arena);
    if (!info.core_gen) Signal::emit("replicationEmptySnapshotReceived");
    std::shared_ptr<Collection> candidate, obsoleteCandidate;
    std::string obsoleteIncarnation;
    bool eligible;
    {
      std::lock_guard lock(mutex);
      auto& state = states[name];
      if (state.source.empty()) throw std::runtime_error("source collection disappeared during transfer");
      if (CommitId::parse(state.source).incarnation != snapshot->id.incarnation)
        throw std::runtime_error("source incarnation changed; retry snapshot");
      for (const auto& file : snapshot->files) state.total += file.size;
      auto active = node.root->collections.get(name);
      bool sameIncarnation = active && active->getShard()->getSnapshots().snapshot()->id.incarnation == snapshot->id.incarnation;
      if (sameIncarnation && snapshot->id.index_gen <= active->getShard()->getSnapshots().snapshot()->id.index_gen) {
        if (snapshot->id.token() == state.serving) {
          state.reused = state.total;
          return;
        }
        throw std::runtime_error("source went backwards");
      }
      bool populated = false;
      if (active) {
        auto serving = active->getShard()->getSnapshots().snapshot();
        auto servingInfo = Manifest::decode(serving->bytes, arena);
        populated = std::ranges::any_of(servingInfo.segments, [](const auto& segment) { return segment.live_docs != 0; });
      }
      eligible = !populated || sameIncarnation || state.replaceEmpty
          || std::ranges::any_of(info.segments, [](const auto& segment) { return segment.live_docs != 0; });
      // Discovery may have advanced while this snapshot request was in flight.
      // An older empty snapshot must not park a newer eligible publication.
      state.waiting = !eligible && state.source == snapshot->id.token();
      if (state.candidateIncarnation != snapshot->id.incarnation) {
        if (state.candidate != active) { obsoleteCandidate = state.candidate; obsoleteIncarnation = state.candidateIncarnation; }
        state.verified.clear();
        state.candidateIncarnation = snapshot->id.incarnation;
        state.candidate = sameIncarnation ? active : nullptr;
      }
      candidate = state.candidate;
    }
    if (obsoleteCandidate) {
      obsoleteCandidate->getShard()->getSnapshots().close();
      node.dirFactory->remove(name + "/" + obsoleteIncarnation);
    }
    if (!eligible) return;
    if (!candidate) {
      node.dirFactory->create(name); // collection parent, selected only after a durable root
      candidate = node.makeCollection(name, node.dirFactory->create(name + "/" + snapshot->id.incarnation));
    }
    auto& registry = candidate->getShard()->getSnapshots();
    auto& dir = registry.dir;
    std::map<std::string, FileDescriptor> verified;
    {
      std::lock_guard lock(mutex);
      auto& state = states[name]; state.candidate = candidate;
      verified = state.verified;
    }
    auto previous = registry.snapshot();
    if (!previous) {
      if (auto active = node.root->collections.get(name); active && active->getShard()) {
        auto& source = active->getShard()->getSnapshots();
        if (auto serving = source.snapshot()) {
          for (auto& file : dir.reuseFiles(source.dir, serving->files, snapshot->files))
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
    for (const auto& file : snapshot->files) {
      if (file.name.empty() || file.name == "." || file.name == ".." || file.name.find_first_of("/\\") != std::string::npos
          || file.name.starts_with("s.olux") || file.name.ends_with(".tmp") || !unique.insert(file.name).second) throw std::runtime_error("invalid snapshot file name");
      bool reuse = verified.contains(file.name) && verified.at(file.name) == file;
      if (!reuse) {
        // Unpublished verified files can survive a reconnect or failed install.
        // After process restart only these candidates need to be hashed.
        if (auto local = dir.openFile(file.name)) {
          auto data = local->read();
          reuse = data.size() == file.size && XXH3_64bits(data.data(), data.size()) == file.xxh3;
          if (!reuse && registry.snapshot()) {
            for (const auto& live : registry.snapshot()->files) if (live.name == file.name) throw std::runtime_error("source changed immutable file: " + file.name);
          }
        }
      }
      if (!reuse) download(client, name, dir, *snapshot, file);
      { std::lock_guard lock(mutex);
        auto& state = states[name];
        if (reuse) state.reused += file.size;
        if (!state.verified.contains(file.name)) state.progress++;
        state.verified.insert_or_assign(file.name, file);
      }
      // Files in the serving snapshot are already durable. Retried candidates
      // may have been verified but not synced before their reservation vanished.
      if (!durableNames.contains(file.name)) names.push_back(file.name);
    }
    dir.sync(names); syncDir(dir);
    auto opened = registry.readers.prepare(*snapshot);
    {
      std::lock_guard lock(mutex);
      if (states[name].source.empty() || CommitId::parse(states[name].source).incarnation != snapshot->id.incarnation)
        throw std::runtime_error("source collection changed during transfer");
    }
    if (stopping.stop_requested()) return;
    bool wroteRoot = false;
    try {
      Manifest::write(dir, snapshot->id.index_gen, *bytes);
      std::array<std::string, 1> root{Manifest::name(snapshot->id.index_gen)};
      dir.sync(root); syncDir(dir); wroteRoot = true;
      Signal::emit("replicationRootWritten");
      if (!previous) node.dirFactory->select(name, {snapshot->id.incarnation});
      registry.publish(snapshot, std::move(opened));
      auto old = node.root->collections.get(name);
      if (old != candidate) {
        if (old) {
          if (!node.root->collections.replace(name, old, candidate)) throw std::runtime_error("collection changed during installation");
          auto oldIncarnation = old->getShard()->getSnapshots().snapshot()->id.incarnation;
          old->getShard()->getSnapshots().detach();
          try { node.dirFactory->remove(name + "/" + oldIncarnation); }
          catch (const std::exception& e) { LOG_WARN("Retired incarnation cleanup failed: {}", e.what()); }
        } else node.root->collections.getOrCreate(name, [&] { return candidate; });
        node.observeCollection(name, *candidate);
        node.replication->changed(name, snapshot->id.incarnation);
      }
    } catch (...) {
      if (!wroteRoot) {
        try { dir.deleteFile(Manifest::name(snapshot->id.index_gen)); syncDir(dir); } catch (...) {}
      }
      throw;
    }
    try {
      if (previous) registry.retire(CommitSnapshotRegistry::obsoleteFiles(*previous, *snapshot));
      registry.sweepOrphans();
      for (const auto& incarnation : node.dirFactory->listDirectories(name)) {
        if (incarnation != snapshot->id.incarnation) node.dirFactory->remove(name + "/" + incarnation);
      }
    } catch (const std::exception& e) { LOG_WARN("Follower retirement failed: {}", e.what()); }
    {
      std::lock_guard lock(mutex);
      auto& state = states[name]; state.serving = snapshot->id.token(); state.error.clear(); state.verified.clear();
      if (!state.source.empty()) {
        auto advertised = CommitId::parse(state.source);
        if (advertised.incarnation == snapshot->id.incarnation && advertised.index_gen < snapshot->id.index_gen) state.source = state.serving;
      }
    }
  }

  bool pull(std::ostream& output) {
    if (!threads.empty() || stopping.stop_requested()) throw std::logic_error("pull requires an unstarted follower");
    Client client(source, stopping.get_token());
    Catalog catalog;
    try { catalog = fetchCatalog(client, {}, 0ms); }
    catch (const std::exception& e) {
      output << "Pull failed: " << binding.source << ": " << errorMessage(e) << '\n';
      return false;
    }
    size_t installed = 0, failed = 0;
    uint64_t transferred = 0, reused = 0;
    for (const auto& [name, entry] : catalog.collections) {
      auto& state = states[name];
      if (!entry.commit.empty() && (state.source.empty() || CommitId::parse(entry.commit).incarnation != CommitId::parse(state.source).incarnation))
        state.replaceEmpty = state.seenBoot == catalog.boot;
      state.source = entry.commit;
      state.seenBoot = catalog.boot; state.downloaded = state.reused = 0;
      uint64_t downloaded = 0;
      try {
        if (entry.state == "unavailable") throw std::runtime_error("source collection unavailable");
        for (unsigned attempt = 0;; attempt++) {
          try { sync(client, name); break; }
          catch (const ReservationGone&) { if (attempt == 2) throw; downloaded += state.downloaded; }
        }
        persistState();
        if (state.serving != state.source) throw std::runtime_error("waiting for source data before replacing the local snapshot");
        installed++;
        output << name << ' ' << state.serving << " transferred=" << downloaded + state.downloaded << " reused=" << state.reused << '\n';
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
            bool work = state.remove || (!state.source.empty() && ((state.source != state.serving && !state.waiting)
                    || (state.source == state.serving && state.acknowledged != state.serving)));
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
        std::string serving;
        {
          std::lock_guard lock(mutex);
          auto& state = states[name]; remove = state.remove;
          needsSync = !state.source.empty() && state.source != state.serving && !state.waiting;
        }
        if (remove) eraseLocal(name);
        else {
          if (needsSync) sync(client, name);
          {
            std::lock_guard lock(mutex);
            auto& state = states[name];
            serving = state.source == state.serving ? state.serving : "";
          }
          if (!serving.empty()) {
            request(client, http::verb::post, "/_replication/installed", glz::write_json(Ack{binding.follower, name, serving}).value());
            std::lock_guard lock(mutex); states[name].acknowledged = serving;
          }
        }
        persistState();
      } catch (const ReservationGone& e) { error = e.what(); Signal::emit("replicationReservationGone"); }
      catch (const std::exception& e) { error = errorMessage(e); }
      if (!error.empty()) client.reset();
      {
        std::lock_guard lock(mutex);
        auto& state = states[name];
        // Discovery may have superseded a failed target while this job ran.
        if (!state.serving.empty() && state.source == state.serving && state.acknowledged == state.serving) error.clear();
        state.busy = false; state.error = error; state.readySince = Clock::now();
        if (error.empty()) { state.failures = 0; state.retry = {}; }
        else {
          if (state.progress != progress) state.failures = 0;
          auto delay = std::chrono::seconds(std::min(60u, 1u << std::min(6u, state.failures)));
          state.failures++; state.retry = Clock::now() + delay;
        }
        if (state.source.empty() && state.serving.empty() && !state.candidate && error.empty()) states.erase(name);
      }
      changed.notify_all();
    }
  }

};

ReplicationFollower::ReplicationFollower(LuxirNode& node) : impl(std::make_unique<Impl>(node)) {}
ReplicationFollower::~ReplicationFollower() { stop(); }
bool ReplicationFollower::pull(std::ostream& output) { return impl->pull(output); }
void ReplicationFollower::start() {
  impl->threads.emplace_back([this] { impl->watch(); });
  for (int i = 0; i < impl->node.config.replication.downloads; i++) impl->threads.emplace_back([this] { impl->worker(); });
}
void ReplicationFollower::stop() {
  impl->stopping.request_stop(); impl->changed.notify_all(); impl->threads.clear();
}
void ReplicationFollower::deleteOrphan(std::string_view name) {
  LuxirNode::validateCollectionName(name);
  std::string key(name);
  {
    std::unique_lock lock(impl->mutex);
    if (!impl->connected || !impl->discovered || (impl->states.contains(key) && (!impl->states.at(key).source.empty() || impl->states.at(key).unavailable))) {
      throw ReadOnlyError("only a local orphan absent from the connected source may be deleted");
    }
    if (impl->states.contains(key) && impl->states.at(key).busy) throw CollectionUnavailableError("local orphan is busy");
    if (!impl->node.root->collections.get(key)) throw CollectionNotFoundError("local orphan does not exist");
    impl->states[key].busy = true;
  }
  try { impl->eraseLocal(key); }
  catch (...) {
    std::lock_guard lock(impl->mutex); impl->states[key].busy = false; impl->changed.notify_all(); throw;
  }
  std::lock_guard lock(impl->mutex);
  impl->states[key].busy = false;
  if (impl->states[key].source.empty()) impl->states.erase(key);
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
    auto& row = rows[i++]; row.name = str(name); row.source_commit = str(state.source); row.serving_commit = str(state.serving);
    row.state = (!impl->connected || state.unavailable) ? "stale" : state.source.empty() ? "orphan" : !state.error.empty() ? "error"
        : state.waiting ? "waiting" : state.source == state.serving ? "serving" : "syncing";
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(state.retry - Clock::now()).count();
    if (!state.error.empty() && remaining > 0) row.next_retry = wallTime() + (uint64_t)remaining;
    row.bytes_downloaded = state.downloaded; row.bytes_total = state.total; row.last_error = str(state.error.empty() && !impl->connected ? impl->discoveryError : state.error);
  }
}
}
