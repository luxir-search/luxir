// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "LuxirConfig.h"
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include "spdlog/spdlog.h"

namespace luxir {

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

int64_t systemRamBytes() {
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


void LuxirConfig::addOptions(CLI::App& app) {
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
  app.add_option("--malloc-mmap-threshold", malloc_mmap_threshold,
                 "glibc malloc mmap threshold in bytes (0 = glibc dynamic default, "
                 "-1 = pin at the MemPool block size; recommended for "
                 "memory-constrained nodes)")
      ->check(CLI::Number)
      ->default_str("0");

  // No default_val: leaving the bound field at its sentinel (<0) lets normalize()
  // derive the gRPC port as http.port + 1 unless the user sets one explicitly.
  app.add_option("--server.grpc.port", server.grpc.port, "gRPC listen port (default: HTTP port + 1)");
  app.add_option("--server.grpc.threads", server.grpc.threads, "Number of server threads (0 = auto)")
      ->default_val(server.grpc.threads);

  app.add_flag("--server.http.enabled,!--no-http", server.http.enabled, "Enable the HTTP/JSON server")
      ->default_val(server.http.enabled);
  // -p is the HTTP port: the HTTP/JSON API is the surface developers hit first.
  app.add_option("--server.http.port,-p", server.http.port, "HTTP/JSON listen port")
      ->default_val(server.http.port);
  app.add_option("--server.http.threads", server.http.threads,
                 "HTTP connection I/O shards (one thread each, plus one accept thread; 0 = auto)")
      ->default_val(server.http.threads);
  app.add_option("--server.stream_buffer_bytes", server.stream_buffer_bytes,
                 "Per-connection buffered response bytes before streaming producers pause")
      ->default_val(server.stream_buffer_bytes);

  app.add_option("--store.backend", store.backend, "Storage backend (ram, fs)")
      ->default_val(store.backend)
      ->check(CLI::IsMember({"ram", "fs"}));
  app.add_option("--store.data-dir", store.data_dir, "Base path for filesystem storage")
      ->default_val(store.data_dir);
  app.add_option("--store.checked-dir.sync", store.checked_dir.sync, "Check fsync correctness: off, warn, throw")
      ->default_val(store.checked_dir.sync)
      ->check(CLI::IsMember({"off", "warn", "throw"}));

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
                 "Streaming NDJSON: batches in flight per connection (0 = task arena concurrency + 2)")
      ->default_val(ingest.max_inflight_batches)
      ->check(CLI::NonNegativeNumber);
  app.add_flag("--indexing.auto-create-collection,!--no-indexing.auto-create-collection",
               ingest.auto_create_collection, "Create missing collections on first use")
      ->default_val(ingest.auto_create_collection);

  app.add_option("--search.max-op-depth", search.max_op_depth,
                 "Max nesting depth of search operations in one request")
      ->default_val(search.max_op_depth)
      ->check(CLI::PositiveNumber);
  app.add_option("--search.request-memory-max-bytes",
                 search.request_memory_max_bytes,
                 "Per-request query-memory breaker ceiling (0 = unlimited)")
      ->default_val(search.request_memory_max_bytes)
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
    index.max_ram_mb = read_only ? 0 : max_ram_mb / 2;
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

  if (read_only && store.backend != "fs") {
    throw std::runtime_error("--read-only requires --store.backend=fs; there is no "
                             "existing data directory to serve with backend=" + store.backend);
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
  auto level = spdlog::level::from_str(log_level);
  spdlog::set_level(level);
}

} // namespace luxir
