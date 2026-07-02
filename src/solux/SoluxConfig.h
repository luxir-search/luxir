#pragma once

#include <string>
#include <thread>
#include <CLI/CLI.hpp>

namespace solux {

struct ServerConfig {
  struct Grpc {
    int port = 50051;
    int threads = 0;  // 0 = auto

    /// Resolve threads: 0 means auto (hw_concurrency/2, minimum 1).
    int resolveThreads() const {
      if (threads > 0) return threads;
      return std::max(1u, std::thread::hardware_concurrency() / 2);
    }
  } grpc;

  struct Http {
    bool enabled = true;
    int port = 8080;
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
  std::string data_dir = "solux_data";
  CheckedDirConfig checked_dir;
};

struct IndexConfig {
  // Per-inverter auto-flush caps. An indexing request that grows an inverter past
  // either cap flushes it to a segment mid-request (at a safe doc boundary in a
  // non-atomic request), bounding the RAM an unbounded stream holds. See
  // IndexWriter::perInverterRamBytes / perInverterMaxDocs.
  int64_t max_inverter_ram_mb = 64;              // RAM cap (MiB)
  int64_t max_inverter_docs = 8 * 1024 * 1024;   // doc-count cap
};

struct IngestConfig {
  // Max size of a single buffered (non-streaming) request body - e.g. a /update
  // carrying an all_or_none request, or a /query.  Oversized -> 413, rejected at the
  // JSON layer (gRPC has its own max message size).  A whole such request lands in one
  // inverter, so this is also what bounds a single non-streaming update's RAM.
  // Keep it GENEROUS: a low limit is a trap - it passes in testing and fails fatally
  // in production on a larger doc or atomic batch.  A future goal is to GUARANTEE a
  // minimum acceptable size (a fixed doc count, or a whole nested document + its
  // children, must always fit).
  int64_t max_request_body_mb = 32;

  // Streaming NDJSON ingest: soft byte target for auto-cutting the stream into one
  // (non-atomic) UpdateRequest, plus a doc-count cut.  Bigger batches amortize the
  // update-graph overhead at the cost of transient staging memory.
  int64_t stream_batch_target_kb = 1024;   // 1 MiB
  int64_t stream_batch_max_docs = 10000;

  // Hard cap on one NDJSON record (one document).  Generous for the same trap reason;
  // a large (e.g. nested) document must fit in a single record.
  int64_t max_record_mb = 8;
};

struct SoluxConfig {
  std::string log_level = "info";
  ServerConfig server;
  StoreConfig store;
  IndexConfig index;
  IngestConfig ingest;

  /// Register common CLI options on an app, bound to this config's fields.
  void addOptions(CLI::App& app);

  /// Normalize and validate config after parsing.
  void normalize();

  /// Apply non-node settings (e.g. spdlog level). Call after parse.
  void apply() const;
};

} // namespace solux
