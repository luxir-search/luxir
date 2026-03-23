#pragma once

#include <string>
#include <thread>
#include <CLI/CLI.hpp>

namespace solux {

struct SoluxConfig {
  int port = 50051;
  int threads = 0;  // 0 = auto
  std::string log_level = "info";

  /// Register common CLI options on an app, bound to this config's fields.
  void addOptions(CLI::App& app);

  /// Apply non-node settings (e.g. spdlog level). Call after parse.
  void apply() const;

  /// Resolve threads: 0 means auto (hw_concurrency/2, minimum 1).
  int resolveThreads() const {
    if (threads > 0) return threads;
    return std::max(1u, std::thread::hardware_concurrency() / 2);
  }
};

} // namespace solux
