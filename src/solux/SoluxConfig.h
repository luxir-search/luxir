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
};

struct StoreConfig {
  std::string backend = "ram";           // ram, fs, afs, uring
  std::string data_dir = "solux_data";
};

struct SoluxConfig {
  std::string log_level = "info";
  ServerConfig server;
  StoreConfig store;

  /// Register common CLI options on an app, bound to this config's fields.
  void addOptions(CLI::App& app);

  /// Normalize and validate config after parsing.
  void normalize();

  /// Apply non-node settings (e.g. spdlog level). Call after parse.
  void apply() const;
};

} // namespace solux
