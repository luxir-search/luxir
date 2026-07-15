#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include "solux/solux_main.h"
#include "solux/util/solux_util.h"
#include "solux/server/GRPCServer.h"
#include "solux/server/HttpServer.h"
#include "solux/SoluxConfig.h"

namespace fs = std::filesystem;

using namespace solux;

int solux_main(int argc, char** argv) {
  std::cout << solux_banner() << std::endl;

  spdlog::set_pattern("%L %H:%M:%S.%f T%t %s:%# %v");

  CLI::App app{"Solux search engine"};
  SoluxConfig config;
  config.addOptions(app);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  config.normalize();
  config.apply();

  LOG_INFO("Logging: compile-time={}, runtime={}",
           spdlog::level::to_string_view((spdlog::level::level_enum)SPDLOG_ACTIVE_LEVEL),
           spdlog::level::to_string_view(spdlog::get_level()));

  try {
    SoluxNode node{config};

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

std::string solux_banner() {
  std::stringstream ss;
  ss << "solux (insert cool ascii art here ;-) ";
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
