#pragma once

#include <string>
#include <thread>
#include <CLI/CLI.hpp>

namespace luxir {

struct ServerConfig {
  // Cap on buffered (rendered/serialized) response bytes, per HTTP connection
  // and per gRPC call (an HTTP/2 connection can multiplex several calls, each
  // with its own cap).  Streaming producers pause above this high-water mark
  // and resume once the buffer drains below half of it.  Values < 1 are
  // clamped to 1 by the servers.
  int64_t stream_buffer_bytes = 1 << 20;

  struct Grpc {
    // <0 means "derive from the HTTP port" (http.port + 1), resolved in normalize().
    // An explicit --server.grpc.port / -p overrides.
    int port = -1;
    int threads = 0;  // 0 = auto

    /// Resolve threads: 0 means auto (hw_concurrency/2, minimum 1).
    int resolveThreads() const {
      if (threads > 0) return threads;
      return std::max(1u, std::thread::hardware_concurrency() / 2);
    }
  } grpc;

  struct Http {
    bool enabled = true;
    int port = 9400;
    int threads = 0;  // 0 = auto

    /// Resolve threads: 0 means auto (hw_concurrency/2, minimum 1).
    int resolveThreads() const {
      if (threads > 0) return threads;
      return std::max(1u, std::thread::hardware_concurrency() / 2);
    }
  } http;
};

struct CheckedDirConfig {
  std::string sync = "off";              // off, warn, throw
};

struct StoreConfig {
  std::string backend = "ram";           // ram, fs
  std::string data_dir = "luxir_data";
  CheckedDirConfig checked_dir;
};

struct IndexConfig {
  // An inverter's MemPool can address at most 4 GiB.  The per-inverter cap is
  // clamped below that, leaving ~282 MiB of headroom for the overshoot of the
  // batch that trips the check (it is evaluated once per batch, not per doc).
  static constexpr int64_t MAX_INVERTER_RAM_CAP_MB = 4000000000LL / (1024 * 1024);

  // Per-inverter auto-flush cap, checked once at the end of each update batch:
  // an inverter past it is flushed to a segment and the next batch gets a fresh
  // one.  See IndexWriter::perInverterRamBytes.
  //
  // It is a backstop, not a budget of its own: bounding total indexing RAM is
  // the shared max_ram_mb's job (inverters reserve against it and idle ones are
  // shed under pressure), so -1 = auto just keeps one inverter from outgrowing
  // that shared cap - or from outgrowing pool addressability, which is what it
  // resolves to when the shared cap is unlimited.
  int64_t max_inverter_ram_mb = -1;              // RAM cap (MiB), -1 = auto
  // Shared indexing RAM cap (MiB) for merge admission and inverter flushing.
  // -1 = auto: half of the node-wide max_ram_mb, or 0 on a read-only node (it
  // never indexes, so it carves out no indexing share).  0 = unlimited.
  // Resolved by LuxirConfig::resolveRamBudgets().
  int64_t max_ram_mb = -1;
  int64_t pressure_flush_floor_mb = 4;           // min idle-inverter size to shed when over the shared cap
  int merge_factor = 10;
};

struct IngestConfig {
  // All byte sizes are stored in bytes; the CLI accepts size suffixes (e.g. 32MB).

  // Max size of a single buffered (non-streaming) request body - e.g. a /update
  // carrying an all_or_none request, or a /query.  Oversized -> 413, rejected at the
  // JSON layer (gRPC has its own max message size).  A whole such request lands in one
  // inverter, so this is also what bounds a single non-streaming update's RAM.
  // Keep it GENEROUS: a low limit is a trap - it passes in testing and fails fatally
  // in production on a larger doc or atomic batch.  A future goal is to GUARANTEE a
  // minimum acceptable size (a fixed doc count, or a whole nested document + its
  // children, must always fit).
  int64_t max_request_body = 32 * 1024 * 1024;

  // Hard cap on ONE NDJSON record (one document) on the STREAMING path, where the
  // buffered-body cap does NOT apply (the body is read unbounded).  It exists to stop
  // a single record with no newline from accumulating unbounded in the framer; the
  // buffered-body cap cannot serve that role because streaming lifts it.  Defaults to
  // max_request_body (a single doc need be no larger than a whole buffered request);
  // set explicitly only to cap individual streamed docs differently.
  int64_t max_record = 0;   // 0 = inherit max_request_body

  // Streaming NDJSON: the stream is cut into internal update batches (each a separate
  // NON-ATOMIC UpdateRequest) at whichever of these it reaches first.  NOT a limit on
  // how many docs may be streamed (that is unbounded) - just the granularity at which
  // docs are handed to the engine.  Bigger batches amortize the update-graph overhead
  // at the cost of transient staging memory.
  int64_t stream_batch_size = 1 * 1024 * 1024;
  int64_t stream_batch_docs = 10000;

  // Maximum number of streaming NDJSON batches submitted by one connection at
  // once.  0 derives the window from the node task arena; 1 is strict serial
  // batch submission.  Staging memory grows roughly with this window, and live
  // inverter memory grows with min(window, arena concurrency).
  int64_t max_inflight_batches = 0;

  // Effective per-record cap: max_record if set, else the buffered-body cap.
  int64_t maxRecordBytes() const { return max_record != 0 ? max_record : max_request_body; }

  // Missing collections are created on first use by default.
  bool auto_create_collection = true;
};

struct SearchConfig {
  // Max nesting depth of search operations in one request (ops within ops).
  // A request-shape limit like the query parsers' nesting budget, but much
  // lower: every level is a full search operation, not just a query node.
  int max_op_depth = 10;
};

struct LuxirConfig {
  // Serve an existing data directory without owning it: the write lock is not taken,
  // nothing is written, and every mutating request is rejected.  Lets a second process
  // query a directory another instance is writing.  The view is the commit that was
  // current when this node started; there is no reopen yet, so later commits by the
  // writer are not picked up until restart.
  bool read_only = false;

  // Node-wide RAM budget (MiB): what this process may use for the memory it
  // manages explicitly - indexing structures today, caches as they are folded
  // in.  -1 = auto (25% of system RAM, or of the cgroup limit under a
  // container); 0 = unlimited.  Subsystem budgets left at their own auto
  // sentinel take a share of this.  Resolved by resolveRamBudgets().
  int64_t max_ram_mb = -1;

  // glibc malloc mmap threshold (bytes): allocations at or above it are mmap'd
  // and returned to the OS on free.  0 = leave glibc's dynamic default (no
  // mallopt call); -1 = pin at MemPool::BYTE_BLOCK_SIZE so pool blocks and
  // large buffers bypass malloc arenas - arenas retain freed memory and
  // serialize cross-thread frees, so pinning cuts indexing RSS and wall-clock
  // sharply on memory-constrained nodes, at some cost to queries that malloc
  // above the threshold per request (large facet counters); > 0 = explicit
  // bytes.  Applied once at startup by luxir_main.
  int64_t malloc_mmap_threshold = 0;

  std::string log_level = "info";
  size_t filterCacheBytes = 64ULL * 1024 * 1024;
  ServerConfig server;
  StoreConfig store;
  IndexConfig index;
  IngestConfig ingest;
  SearchConfig search;

  /// Register common CLI options on an app, bound to this config's fields.
  void addOptions(CLI::App& app);

  /// Resolve the auto (-1) RAM sentinels: max_ram_mb to a share of system RAM,
  /// index.max_ram_mb to a share of that.  Idempotent, and called both by
  /// normalize() and by LuxirNode, so a config built in code (tests, embedding)
  /// gets the same budgets as one parsed from the command line.
  void resolveRamBudgets();

  /// Normalize and validate config after parsing.
  void normalize();

  /// Apply non-node settings (e.g. spdlog level). Call after parse.
  void apply() const;
};

/// RAM this process should consider available, in bytes: total system RAM,
/// capped by the cgroup memory limit when running under one.  0 when it cannot
/// be determined.
int64_t systemRamBytes();

} // namespace luxir
