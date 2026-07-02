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

struct SoluxConfig {
  std::string log_level = "info";
  ServerConfig server;
  StoreConfig store;
  IndexConfig index;

  /// Register common CLI options on an app, bound to this config's fields.
  void addOptions(CLI::App& app);

  /// Normalize and validate config after parsing.
  void normalize();

  /// Apply non-node settings (e.g. spdlog level). Call after parse.
  void apply() const;
};

} // namespace solux
