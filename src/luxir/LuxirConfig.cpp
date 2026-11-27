// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "LuxirConfig.h"
#include <CLI/CLI.hpp>
#include "luxir/util/ProcessAllocator.h"
#include "luxir/util/TaskWork.h"
#include "luxir/util/LoadSignal.h"
#include "luxir/server/Collections.h"
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <algorithm>
#include <fstream>
#include <limits>
#include "spdlog/spdlog.h"

namespace luxir {

#ifndef __APPLE__
namespace {

// Single integer from a one-line file; 0 if it is missing or not a number
// (cgroup v2 spells "no limit" as the word "max").
int64_t readInt64File(const char* path) {
  std::ifstream in(path);
  int64_t value = 0;
  if (in >> value && value > 0) return value;
  return 0;
}

}  // namespace
#endif

int64_t systemRamBytes() {
#ifdef __APPLE__
  uint64_t bytes = 0;
  size_t size = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0
      || bytes > (uint64_t)std::numeric_limits<int64_t>::max()) return 0;
  return (int64_t)bytes;
#else
  long pages = sysconf(_SC_PHYS_PAGES);
  long pageSize = sysconf(_SC_PAGESIZE);
  if (pages <= 0 || pageSize <= 0) return 0;
  int64_t bytes = (int64_t)pages * (int64_t)pageSize;

  // Under a container the cgroup limit is what the kernel actually enforces, so
  // sizing off the host's physical RAM would overcommit by a wide margin.  Both
  // "no limit" spellings (v2 "max", v1's huge sentinel) fail the < bytes test.
  for (const char* path : {"/sys/fs/cgroup/memory.max",                      // v2
                           "/sys/fs/cgroup/memory/memory.limit_in_bytes"}) {  // v1
    int64_t limit = readInt64File(path);
    if (limit > 0 && limit < bytes) bytes = limit;
  }
  return bytes;
#endif
}

/*
Rough hierarchy:
[store]          # storage backend
[index]          # indexing behavior (ram buffer, etc.)
[merge]          # merge policy/scheduling
[cache.query]    # named cache instances
[cache.results]
[query]          # query parsing defaults
[request]        # request handling (threads, timeouts)
[search]         # search defaults (hits, etc.)
*/

void NodeConfig::validate() const {
  if (id.empty()) throw std::invalid_argument("node.id must not be empty");
  if (id.size() > 255) throw std::invalid_argument("node.id exceeds 255 bytes");
  if (id == "." || id == "..") throw std::invalid_argument("node.id must not be '.' or '..'");
  for (unsigned char c : id) {
    if (c < 0x20 || c > 0x7e || c == '/' || c == '\\') {
      throw std::invalid_argument("node.id must be printable ASCII without '/' or '\\\\'");
    }
  }
}

std::vector<std::string> ReplicationConfig::tenantFilter() const {
  std::vector<std::string> result;
  if (tenants.empty()) return result;
  std::string_view remaining = tenants;
  for (;;) {
    auto comma = remaining.find(',');
    auto tenant = remaining.substr(0, comma);
    Collections::validateName(tenant, "tenant");
    result.emplace_back(tenant);
    if (comma == std::string_view::npos) break;
    remaining.remove_prefix(comma + 1);
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<std::string> ReplicationConfig::peerList() const {
  std::vector<std::string> result;
  std::string_view remaining = peers;
  while (!remaining.empty()) {
    auto comma = remaining.find(',');
    auto peer = remaining.substr(0, comma);
    if (peer.empty()) throw std::invalid_argument("replication.peers has an empty entry");
    result.emplace_back(peer);
    if (comma == std::string_view::npos) break;
    remaining.remove_prefix(comma + 1);
  }
  return result;
}

void ReplicationConfig::validate() const {
  tenantFilter();
  if (!max_acknowledgments) throw std::invalid_argument("replication.max-acknowledgments must be positive");
  if (pin_idle_timeout_ms < 2 || follower_timeout_ms < 3) {
    throw std::invalid_argument("replication timeouts must be positive (at least 2/3 ms respectively)");
  }
  if (retry_ms < 1) throw std::invalid_argument("replication.retry-ms must be positive");
}

void ServerConfig::validate() const {
  if (memory_limit_mb < 0) throw std::invalid_argument("server.memory-limit-mb must not be negative");
  if (memory_pressure_sample_ms < 0) throw std::invalid_argument("server.memory-pressure-sample-ms must not be negative");
  if (memory_pressure_soft_percent < 1 || memory_pressure_soft_percent > memory_pressure_hard_percent ||
      memory_pressure_hard_percent > 100) {
    throw std::invalid_argument("memory pressure thresholds need 1 <= soft <= hard <= 100 percent");
  }
  if (memory_pressure_hysteresis_percent < 0 || memory_pressure_hysteresis_percent >= memory_pressure_soft_percent) {
    throw std::invalid_argument("server.memory-pressure-hysteresis-percent must be in [0, soft)");
  }
}

void LuxirConfig::addOptions(CLI::App& app) {
  app.add_option("--node.id", node.id, "Node identity (default host:port, otherwise a random UUID)");
  app.add_option("--replicate-from,--replication.source", replication.source, "Follow this HTTP source namespace");
  app.add_option("--replication.tenants", replication.tenants, "Comma-separated tenant subscription (empty follows all)");
  app.add_option("--replication.peers", replication.peers, "Comma-separated http:// nodes serving file copies, tried before the source");
  app.add_option("--replication.max-acknowledgments", replication.max_acknowledgments,
      "Node-wide follower/collection acknowledgment row budget")->default_val(replication.max_acknowledgments);
  app.add_option("--replication.downloads", replication.downloads, "Concurrent collection downloads per follower")->check(CLI::Range(1, 64));
  app.add_flag("--read-only", read_only,
               "Serve an existing data directory without the write lock; rejects all updates");
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error, critical)")
      ->default_val(log_level);
  app.add_option("--query-cache-bytes", queryCacheBytes,
                 "Per-shard query cache payload budget (0 disables it)")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("64MB");
  // No default_val: the -1 sentinel in the bound field is what tells
  // resolveRamBudgets() the user did not set one.
  app.add_option("--max-ram-mb", max_ram_mb,
                 "Node-wide RAM budget (MiB) that subsystem budgets are derived from "
                 "(0 = unlimited)")
      ->check(CLI::NonNegativeNumber)
      ->default_str("25% of system RAM");

  // No default_val: leaving the bound field at its sentinel (<0) lets normalize()
  // derive the gRPC port as http.port + 1 unless the user sets one explicitly.
  app.add_option("--server.grpc.port", server.grpc.port, "gRPC listen port (default: HTTP port + 1)");
  app.add_option("--server.grpc.threads", server.grpc.threads, "Number of server threads (0 = auto)")
      ->default_val(server.grpc.threads);
  app.add_option("--server.grpc.max-receive-message-size", server.grpc.max_receive_message_size,
                 "Max received gRPC message; larger -> RESOURCE_EXHAUSTED (e.g. 64MB)")
      ->transform(CLI::AsSizeValue(false))
      ->check(CLI::NonNegativeNumber)
      ->default_str("--indexing.max-request-body");

  app.add_flag("--server.http.enabled,!--no-http", server.http.enabled, "Enable the HTTP/JSON server")
      ->default_val(server.http.enabled);
  // -p is the HTTP port: the HTTP/JSON API is the surface developers hit first.
  app.add_option("--server.http.port,-p", server.http.port, "HTTP/JSON listen port")
      ->default_val(server.http.port);
  app.add_option("--server.http.threads", server.http.threads,
                 "HTTP connection I/O shards (one thread each, plus one accept thread; 0 = auto)")
      ->default_val(server.http.threads);
  app.add_option("--server.workers", server.workers,
                 "Task-arena worker threads for indexing, merges and parallel search "
                 "(0 = auto: all CPUs on nodes of up to 4, else all but one)")
      ->default_val(server.workers)
      ->check(CLI::NonNegativeNumber);
  app.add_option("--server.memory-limit-mb", server.memory_limit_mb,
                 "Memory limit (MiB) for memory pressure admission (0 = cgroup limit, else system RAM)")
      ->default_val(server.memory_limit_mb)
      ->check(CLI::NonNegativeNumber);
  app.add_option("--server.memory-pressure-soft-percent", server.memory_pressure_soft_percent,
                 "Memory use (% of the limit) at which new update requests are refused")
      ->default_val(server.memory_pressure_soft_percent)
      ->check(CLI::Range(1, 100));
  app.add_option("--server.memory-pressure-hard-percent", server.memory_pressure_hard_percent,
                 "Memory use (% of the limit) at which new searches and update batches are refused")
      ->default_val(server.memory_pressure_hard_percent)
      ->check(CLI::Range(1, 100));
  app.add_option("--server.memory-pressure-hysteresis-percent", server.memory_pressure_hysteresis_percent,
                 "Points below a threshold that memory use must drop to leave its level")
      ->default_val(server.memory_pressure_hysteresis_percent)
      ->check(CLI::Range(0, 99));
  app.add_option("--server.memory-pressure-sample-ms", server.memory_pressure_sample_ms,
                 "Memory pressure sampling interval in milliseconds (0 = off)")
      ->default_val(server.memory_pressure_sample_ms)
      ->check(CLI::NonNegativeNumber);
  app.add_option("--server.control-threads", server.control_threads,
                 "Control-plane threads for administration, schema changes, replication transfers "
                 "and detailed stats, outside the search and indexing workers")
      ->default_val(server.control_threads)
      ->check(CLI::PositiveNumber);
  app.add_option("--server.stream_buffer_bytes", server.stream_buffer_bytes,
                 "Per-connection buffered response bytes before streaming producers pause")
      ->default_val(server.stream_buffer_bytes);

  app.add_option("--replication.pin-idle-timeout-ms", replication.pin_idle_timeout_ms,
                 "Snapshot pin idle timeout in milliseconds")->default_val(replication.pin_idle_timeout_ms);
  app.add_option("--replication.pin-retained-bytes", replication.pin_retained_bytes,
                 "Per-collection bytes retained only by snapshot pins")->transform(CLI::AsSizeValue(false))
      ->default_val(replication.pin_retained_bytes);
  app.add_option("--replication.follower-timeout-ms", replication.follower_timeout_ms,
                 "Follower liveness in milliseconds (watch timeout at most one third)")->default_val(replication.follower_timeout_ms);
  app.add_option("--replication.retry-ms", replication.retry_ms,
                 "Follower reconnect and first sync retry delay in milliseconds (retries double, to 60 s)")
      ->default_val(replication.retry_ms);

  // Names are validated against the registries when storage opens: options are added before plugins register.
  app.add_option("--store.backend", store.backend, "Storage backend name (built in: ram, fs, shared_fs; plugins may add more)")
      ->default_val(store.backend);
  app.add_option("--store.layer", store.layers,
                 "Storage layer wrapped around the backend (repeatable, innermost first; provided by plugins)")
      ->expected(1)
      ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
  app.add_option_function<std::vector<std::string>>("--store.option",
      [&options = store.options](const std::vector<std::string>& pairs) {
        for (const auto& pair : pairs) {
          auto eq = pair.find('=');
          auto dot = pair.find('.');
          if (eq == std::string::npos || dot == std::string::npos || dot == 0 || dot + 1 >= eq) {
            throw CLI::ValidationError("--store.option", "expected COMPONENT.KEY=VALUE, got '" + pair + "'");
          }
          auto component = pair.substr(0, dot);
          if (component.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            throw CLI::ValidationError("--store.option", "component '" + component + "' must use [a-z0-9_]+");
          }
          options[component][pair.substr(dot + 1, eq - dot - 1)] = pair.substr(eq + 1);
        }
      },
      "Backend or layer setting COMPONENT.KEY=VALUE (repeatable)")
      ->expected(1)
      ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
  app.add_option("--store.ram-limit-mb", store.ram_limit_mb, "RAM storage allocation limit in MiB (0 = unlimited)")->check(CLI::Range((uint64_t)0, UINT64_MAX / (1024 * 1024)));
  app.add_option("--store.data-dir", store.data_dir, "Base path for filesystem storage")
      ->default_val(store.data_dir);
  app.add_option("--store.checked-dir.sync", store.checked_dir.sync, "Check fsync correctness: off, warn, throw")
      ->default_val(store.checked_dir.sync)
      ->check(CLI::IsMember({"off", "warn", "throw"}));
  app.add_option("--store.verify-on-open", store.verify_on_open,
                 "Startup check of committed files: none, or full (size and xxh3 of every file)")
      ->default_val(store.verify_on_open)
      ->check(CLI::IsMember({"none", "full"}));
  app.add_option("--store.retention-ms", store.retention_ms,
                 "Keep superseded commits for this many milliseconds (0 disables retention)")
      ->check(CLI::Range((int64_t)0, INT64_MAX / 1000))
      ->default_str("0 on exclusive stores, 300000 on shared stores");
  app.add_option("--store.scrub-interval-s", store.scrub_interval_s,
                 "Seconds between background checksum scrubs of published snapshots (0 = off)")
      ->default_val(store.scrub_interval_s);
  app.add_option("--store.scrub-max-bytes-per-sec", store.scrub_max_bytes_per_sec,
                 "Background scrub read rate limit (0 = unthrottled)")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("64MB");

  app.add_flag("--indexing.huge-pages,!--no-indexing.huge-pages", index.huge_pages,
               "Advise transparent huge pages for the indexing arena (jemalloc builds; "
               "no effect when host THP is disabled)")
      ->default_val(index.huge_pages);
  app.add_option("--indexing.max-inverter-ram-mb", index.max_inverter_ram_mb,
                 "Per-inverter RAM cap (MiB) before an auto-flush to a segment")
      ->check(CLI::PositiveNumber)
      ->default_str("--indexing.max-ram-mb, capped at " +
                    std::to_string(IndexConfig::MAX_INVERTER_RAM_CAP_MB));
  app.add_option("--indexing.merge-factor", index.merge_factor,
                 "Same-level segments required to trigger a merge")
      ->default_val(index.merge_factor)
      ->check(CLI::PositiveNumber)
      ->check(CLI::Range(2, (std::numeric_limits<int>::max)()));
  app.add_option("--indexing.max-ram-mb", index.max_ram_mb,
                 "Shared indexing RAM cap for merge admission and inverter flushing (MiB, 0 = unlimited)")
      ->check(CLI::NonNegativeNumber)
      ->default_str("50% of --max-ram-mb");
  app.add_option("--indexing.pressure-flush-floor-mb", index.pressure_flush_floor_mb,
                 "Min idle-inverter size (MiB) to flush when over the shared indexing RAM cap")
      ->default_val(index.pressure_flush_floor_mb)
      ->check(CLI::PositiveNumber);

  app.add_option("--indexing.id-filter", index.id_filter,
                 "Membership filter over each segment's ids, used to skip segments when "
                 "applying deletes and updates: none, bloom, fuse8 or fuse16")
      ->default_val(index.id_filter)
      ->check(CLI::IsMember({"bloom", "fuse8", "fuse16", "none"}));
  app.add_option("--indexing.id-filter-bits-per-key", index.id_filter_bits_per_key,
                 "Bits per id of a bloom id filter")
      ->default_val(index.id_filter_bits_per_key)
      ->check(CLI::Range(1, 64));
  app.add_option("--indexing.id-filter-min-kb", index.id_filter_min_kb,
                 "Smallest bloom id filter per segment (KiB, 0 = sized by bits per id only)")
      ->default_val(index.id_filter_min_kb)
      ->check(CLI::Range(0, 1024));

  app.add_option("--indexing.max-request-body", ingest.max_request_body,
                 "Max buffered (non-streaming) request body; oversized -> 413 (e.g. 32MB)")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("32MB");
  app.add_option("--indexing.max-record", ingest.max_record,
                 "Max size of one NDJSON record / document (default: max-request-body)")
      ->transform(CLI::AsSizeValue(false));
  app.add_option("--indexing.stream-batch-size", ingest.stream_batch_size,
                 "Streaming NDJSON: byte size for internal (non-atomic) mini-batches")
      ->transform(CLI::AsSizeValue(false))
      ->default_str("1MB");
  app.add_option("--indexing.stream-batch-docs", ingest.stream_batch_docs,
                 "Streaming NDJSON: doc count for internal (non-atomic) mini-batches")
      ->default_val(ingest.stream_batch_docs)
      ->check(CLI::PositiveNumber);
  app.add_option("--indexing.max-inflight-batches", ingest.max_inflight_batches,
                 "Streaming NDJSON: batches decoding or indexing at once per connection (0 = server.workers + 2)")
      ->default_val(ingest.max_inflight_batches)
      ->check(CLI::NonNegativeNumber);
  app.add_flag("--indexing.auto-create-collection,!--no-indexing.auto-create-collection",
               ingest.auto_create_collection, "Create missing collections on first use")
      ->default_val(ingest.auto_create_collection);

  app.add_option("--search.max-op-depth", search.max_op_depth,
                 "Max nesting depth of search operations in one request")
      ->default_val(search.max_op_depth)
      ->check(CLI::PositiveNumber);
  app.add_option("--search.fanout-grain-us", search.fanout_grain_us,
                 "Adaptive fan-out grain and credit in microseconds (-1 = never publish)")
      ->default_val(search.fanout_grain_us)
      ->check(CLI::Range((int64_t)-1, (int64_t)60'000'000))
      ->check(!CLI::Range((int64_t)0, (int64_t)0));
  app.add_option("--search.fanout-sibling-grain-us", search.fanout_sibling_grain_us,
                 "Fan-out grain with only idle SMT siblings in microseconds (-1 = never publish)")
      ->default_val(search.fanout_sibling_grain_us)
      ->check(CLI::Range((int64_t)-1, (int64_t)60'000'000))
      ->check(!CLI::Range((int64_t)0, (int64_t)0));
  app.add_option("--search.request-memory-max-bytes",
                 search.request_memory_max_bytes,
                 "Per-request query-memory breaker ceiling (0 = unlimited)")
      ->default_val(search.request_memory_max_bytes)
      ->check(CLI::NonNegativeNumber);
  // No default_val: the -1 sentinel asks resolveRamBudgets() for the share.
  app.add_option("--search.node-request-memory-max-bytes",
                 search.node_request_memory_max_bytes,
                 "Node-wide ceiling on query memory tracked across running requests (0 = unlimited)")
      ->transform(CLI::AsSizeValue(false))
      ->check(CLI::NonNegativeNumber)
      ->default_str("half of --max-ram-mb");
  app.add_option("--search.multiterm-max-expansions",
                 search.multiterm_max_expansions,
                 "Operator opt-in term cap per segment for prefix/wildcard/regex/range; "
                 "a merge can change acceptance (0 = off, default)")
      ->default_val(search.multiterm_max_expansions)
      ->check(CLI::NonNegativeNumber);
  app.add_option("--search.multiterm-max-terms-examined",
                 search.multiterm_max_terms_examined,
                 "Dictionary terms all multi-term and fuzzy scans of one request "
                 "may visit; operator opt-in (0 = off, default)")
      ->default_val(search.multiterm_max_terms_examined)
      ->check(CLI::NonNegativeNumber);
  app.add_option("--search.multiterm-max-postings",
                 search.multiterm_max_postings,
                 "Operator opt-in postings estimate: summed selected docFreq, excluding "
                 "fuzzy scored rewrites (0 = off, default)")
      ->default_val(search.multiterm_max_postings)
      ->check(CLI::NonNegativeNumber);
  app.add_option("--search.multiterm-min-prefix-length",
                 search.multiterm_min_prefix_length,
                 "Pattern-only minimum literal prefix in bytes for unbounded prefix/wildcard/regex; "
                 "type-ahead keystrokes shorter than N are rejected (0 = off, default)")
      ->default_val(search.multiterm_min_prefix_length)
      ->check(CLI::NonNegativeNumber);
}

void LuxirConfig::resolveRamBudgets() {
  if (max_ram_mb < 0) {
    int64_t systemMb = systemRamBytes() / (1024 * 1024);
    // A quarter leaves room for the OS page cache (mapped segments are read
    // through it), the allocator's own overhead, and everything not yet on a
    // budget.  Unknown system RAM falls back to unlimited rather than to a
    // guess that could be far too small on a big machine.
    max_ram_mb = systemMb / 4;
  }

  if (index.max_ram_mb < 0) {
    // Half the node budget: indexing is one of two big consumers, the other
    // being the search-side caches and reader structures.  A read-only node
    // never indexes, so it carves out nothing (0 is also "unlimited" for the
    // budget object, which is moot when nothing ever reserves against it).
    index.max_ram_mb = (read_only || !replication.source.empty()) ? 0 : max_ram_mb / 2;
  }

  if (search.node_request_memory_max_bytes < 0) {
    // The half of the node budget indexing does not take.
    search.node_request_memory_max_bytes = max_ram_mb > 0 ? max_ram_mb * 1024 * 1024 / 2 : 0;
  }

  if (index.max_inverter_ram_mb < 0) {
    // One inverter may hold the whole indexing budget - with a single stream
    // there is nothing else to hold it - but never more, and never more than
    // the pool can address.
    index.max_inverter_ram_mb =
        index.max_ram_mb > 0 ? (std::min)(index.max_ram_mb, IndexConfig::MAX_INVERTER_RAM_CAP_MB)
                             : IndexConfig::MAX_INVERTER_RAM_CAP_MB;
  }
}

void LuxirConfig::normalize() {
  if (!node.id.empty()) node.validate();
  replication.validate();
  server.validate();
  resolveRamBudgets();

  // Only an explicitly set share can exceed the node budget; a derived one is
  // half of it.  Warned here rather than in resolveRamBudgets(), which runs
  // again per node and must stay quiet on repeat.
  if (max_ram_mb > 0 && index.max_ram_mb > max_ram_mb) {
    spdlog::warn("indexing.max-ram-mb={} exceeds the node budget max-ram-mb={}",
                 index.max_ram_mb, max_ram_mb);
  }
  if (index.max_ram_mb > 0 && index.max_ram_mb < index.max_inverter_ram_mb) {
    spdlog::warn("indexing.max-ram-mb={} is below indexing.max-inverter-ram-mb={}: one inverter "
                 "can fill the whole shared budget", index.max_ram_mb, index.max_inverter_ram_mb);
  }

  // Default the gRPC port to one past the HTTP port unless it was set explicitly.
  if (server.grpc.port < 0) server.grpc.port = server.http.port + 1;

  if (store.backend == "ram" && store.data_dir != "luxir_data") {
    spdlog::warn("store.data-dir is ignored when store.backend=ram");
  }

  // Only an explicit value can be over the pool's addressability; the derived
  // one is already clamped (MemPool itself throws at the hard ceiling if a
  // pathological batch overshoots even the headroom the clamp leaves).
  if (index.max_inverter_ram_mb > IndexConfig::MAX_INVERTER_RAM_CAP_MB) {
    spdlog::warn("indexing.max-inverter-ram-mb={} exceeds the inverter pool's addressability; clamping to {}",
                 index.max_inverter_ram_mb, IndexConfig::MAX_INVERTER_RAM_CAP_MB);
    index.max_inverter_ram_mb = IndexConfig::MAX_INVERTER_RAM_CAP_MB;
  }
}

void LuxirConfig::apply() const {
  configureIndexingArena(index.huge_pages);
  fanoutGrainNs.store(search.fanout_grain_us < 0 ? FANOUT_GRAIN_NEVER
      : (uint64_t)search.fanout_grain_us * 1000, std::memory_order_relaxed);
  fanoutSiblingGrainNs.store(search.fanout_sibling_grain_us < 0 ? FANOUT_GRAIN_NEVER
      : (uint64_t)search.fanout_sibling_grain_us * 1000, std::memory_order_relaxed);
  LoadRegistry::enabled.store(search.fanout_grain_us >= 1, std::memory_order_relaxed);
  auto level = spdlog::level::from_str(log_level);
  spdlog::set_level(level);
}

} // namespace luxir
