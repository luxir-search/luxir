#include "SoluxConfig.h"
#include "spdlog/spdlog.h"

namespace solux {

/*
Rough hierarchy:
[store]          # storage backend
[index]          # indexing behavior (ram buffer, etc.)
[merge]          # merge policy/scheduling
[cache.filter]   # named cache instances
[cache.query]
[query]          # query parsing defaults
[request]        # request handling (threads, timeouts)
[search]         # search defaults (hits, etc.)
*/


void SoluxConfig::addOptions(CLI::App& app) {
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error, critical)")
      ->default_val(log_level);

  app.add_option("--server.grpc.port,-p", server.grpc.port, "gRPC listen port")
      ->default_val(server.grpc.port);
  app.add_option("--server.grpc.threads,-t", server.grpc.threads, "Number of server threads (0 = auto)")
      ->default_val(server.grpc.threads);

  app.add_option("--store.backend", store.backend, "Storage backend (ram, fs)")
      ->default_val(store.backend)
      ->check(CLI::IsMember({"ram", "fs"}));
  app.add_option("--store.data-dir", store.data_dir, "Base path for filesystem storage")
      ->default_val(store.data_dir);
}

void SoluxConfig::normalize() {
  if (store.backend == "ram" && store.data_dir != "solux_data") {
    spdlog::warn("store.data-dir is ignored when store.backend=ram");
  }
}

void SoluxConfig::apply() const {
  auto level = spdlog::level::from_str(log_level);
  spdlog::set_level(level);
}

} // namespace solux
