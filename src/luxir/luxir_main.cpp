#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include "luxir/luxir_main.h"
#include "luxir/util/luxir_util.h"
#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/LuxirConfig.h"

namespace fs = std::filesystem;

using namespace luxir;

int luxir_main(int argc, char** argv) {
  std::cout << luxir_banner() << std::endl;

  spdlog::set_pattern("%L %H:%M:%S.%f T%t %s:%# %v");

  CLI::App app{"Luxir search engine"};
  LuxirConfig config;
  config.addOptions(app);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  try {
    config.normalize();
    config.apply();
  } catch (const std::exception &e) {
    LOG_ERROR("Invalid configuration: {}", e.what());
    return 1;
  }

  if (config.read_only) {
    LOG_INFO("Read-only mode: serving {} without the write lock; updates are rejected",
             config.store.data_dir);
  }

  LOG_INFO("RAM budget: max-ram-mb={} indexing.max-ram-mb={} indexing.max-inverter-ram-mb={} "
           "(system RAM {} MiB; 0 = unlimited)",
           config.max_ram_mb, config.index.max_ram_mb, config.index.max_inverter_ram_mb,
           systemRamBytes() / (1024 * 1024));

  LOG_INFO("Logging: compile-time={}, runtime={}",
           spdlog::level::to_string_view((spdlog::level::level_enum)SPDLOG_ACTIVE_LEVEL),
           spdlog::level::to_string_view(spdlog::get_level()));

  try {
    LuxirNode node{config};

    // The HTTP/JSON server runs on its own io threads; start it (non-blocking)
    // before the blocking gRPC run().
    std::optional<HttpServer> httpServer;
    if (config.server.http.enabled) {
      httpServer.emplace(node, config.server.http.resolveThreads(), config.server.http.port);
      httpServer->start();
    }

    GRPCServer server(node, config.server.grpc.resolveThreads(), config.server.grpc.port);
    server.run();

    if (httpServer) httpServer->shutdown();
  } catch (const std::exception &e) {
    LOG_ERROR("Startup failed: {}", e.what());
    return 1;
  }
  return 0;
}

std::string luxir_banner() {
  std::stringstream ss;
  ss << "luxir (insert cool ascii art here ;-) ";
#ifdef NDEBUG
  ss << " Release (NDEBUG)";
#else
  ss << " Debugging!";
#endif
#ifdef __OPTIMIZE__
  ss << " __OPTIMIZE__=" << __OPTIMIZE__;
#endif
  ss << " __cplusplus=" << __cplusplus;
#ifdef __clang__
  ss << " __clang__=" << __clang__;
#endif
#ifdef __GNUC__
  ss << " __GNUC__=" << __GNUC__;
#endif
#ifdef _MSC_VER
  ss << " _MSC_VER=" << _MSC_VER;
#endif
#ifdef __VERSION__
  ss << " __VERSION__=" << __VERSION__;
#endif
#ifdef _GLIBCXX_RELEASE
  ss << " _GLIBCXX_RELEASE=" << _GLIBCXX_RELEASE;
#endif
#ifdef __GLIBCXX__
  ss << " __GLIBCXX__=" << __GLIBCXX__;
#endif
#ifdef __linux__
  ss << " __linux__=" << __linux__;
#endif

  ss << std::endl;
  ss << "\thw_threads=" << std::thread::hardware_concurrency();
  ss << " cwd=" << fs::current_path();
  ss << " tmp=" << fs::temp_directory_path();


  return ss.str();
}
