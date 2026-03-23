#include "SoluxConfig.h"
#include "spdlog/spdlog.h"

namespace solux {

void SoluxConfig::addOptions(CLI::App& app) {
  app.add_option("-p,--port", port, "gRPC listen port")->default_val(port);
  app.add_option("-t,--threads", threads, "Number of server threads (0 = auto)")->default_val(threads);
  app.add_option("--log-level", log_level, "Log level (trace, debug, info, warn, error, critical)")
      ->default_val(log_level);
  app.add_option("--store", store, "Storage backend (ram, fs)")
      ->default_val(store)
      ->check(CLI::IsMember({"ram", "fs"}));
  app.add_option("--data-dir", data_dir, "Base path for filesystem storage")
      ->default_val(data_dir);
}

void SoluxConfig::apply() const {
  auto level = spdlog::level::from_str(log_level);
  spdlog::set_level(level);
}

} // namespace solux
