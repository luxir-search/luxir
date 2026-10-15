// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <optional>
#include <pthread.h>
#include <semaphore>
#include <sstream>
#include <thread>
#include "luxir/luxir_main.h"
#include "luxir/server/Promotion.h"
#include "luxir/util/ProcessAllocator.h"
#include "luxir/util/LogFormatter.h"
#include "luxir/util/luxir_util.h"
#include "luxir/server/GRPCServer.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/ReplicationFollower.h"
#include "luxir/LuxirConfig.h"

namespace fs = std::filesystem;

using namespace luxir;

namespace {

sigset_t shutdownSignals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  return set;
}

// Turns SIGTERM/SIGINT into a graceful stop.  The signals must already be
// blocked in every thread (see luxir_main), so only sigwait here sees them.
// The first signal releases `stop`; a second one while draining force-exits.
class SignalWatcher {
  std::binary_semaphore& stop;
  std::atomic<bool> done{false};
  std::thread thread;

  void run() {
    pthread_setname_np(pthread_self(), "luxir_signals");
    sigset_t set = shutdownSignals();
    bool stopping = false;
    for (;;) {
      int sig = 0;
      if (sigwait(&set, &sig) != 0) continue;
      if (done.load(std::memory_order_acquire)) return;
      if (stopping) {
        LOG_WARN("Received {} while shutting down; exiting immediately", strsignal(sig));
        std::_Exit(128 + sig);
      }
      LOG_INFO("Received {}; shutting down", strsignal(sig));
      stopping = true;
      stop.release();
    }
  }

public:
  explicit SignalWatcher(std::binary_semaphore& stop) : stop(stop), thread([this] { run(); }) {}

  ~SignalWatcher() {
    done.store(true, std::memory_order_release);
    pthread_kill(thread.native_handle(), SIGTERM);  // wake sigwait
    thread.join();
  }
};

} // namespace

int luxir_main(int argc, char** argv) {
  spdlog::set_formatter(std::make_unique<LogFormatter>());

  CLI::App app{"Luxir search engine"};
  app.set_version_flag("--version", "Luxir " LUXIR_VERSION " (" LUXIR_CPU_NAME ")");
  LuxirConfig config;
  config.addOptions(app);
  auto* pull = app.add_subcommand("pull", "Copy current source snapshots to a follower data directory");
  pull->add_option("source", config.replication.source, "HTTP source URL")->required();
  pull->add_option("dir", config.store.data_dir, "Destination data directory")->required();
  auto* promote = app.add_subcommand("promote", "Make a stopped follower data directory an independent writer");
  promote->add_option("dir", config.store.data_dir, "Follower data directory")->required();

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }
  if (*pull) {
    if (config.replication.source.empty() || config.store.data_dir.empty()) {
      std::cerr << "pull requires nonempty source and directory arguments\n";
      return 1;
    }
    if (config.read_only) {
      std::cerr << "pull cannot be combined with --read-only\n";
      return 1;
    }
    config.store.backend = "fs";
    if (!app.get_option("--log-level")->count()) config.log_level = "warn";
  } else if (*promote) {
    if (config.store.data_dir.empty() || config.read_only || !config.replication.source.empty()) {
      std::cerr << "promote takes a nonempty directory, without --read-only or --replicate-from\n";
      return 1;
    }
    if (!app.get_option("--log-level")->count()) config.log_level = "warn";
  } else {
    // Block the shutdown signals before any thread exists so every thread
    // inherits the mask and only SignalWatcher's sigwait receives them.
    sigset_t set = shutdownSignals();
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::cout << luxir_banner() << std::endl;
  }

  try {
    config.normalize();
    config.apply();
  } catch (const std::exception &e) {
    LOG_ERROR("Invalid configuration: {}", e.what());
    return 1;
  }

  if (*pull) {
    try {
      LuxirNode node{config, LuxirNode::Mode::PULL};
      return node.getFollower()->pull(std::cout) ? 0 : 1;
    } catch (const std::exception& e) {
      std::cerr << "Pull failed: " << e.what() << '\n';
      return 1;
    }
  }

  if (*promote) {
    try {
      return luxir::promote(config.store.data_dir, std::cout) ? 0 : 1;
    } catch (const std::exception& e) {
      std::cerr << "Promote failed: " << e.what() << '\n';
      return 1;
    }
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

  bool started = false;
  try {
    // Released by the first SIGTERM/SIGINT, or when gRPC run() returns early.
    std::binary_semaphore stop{0};
    LuxirNode node{config};

    // The HTTP/JSON server runs on its own connection-I/O shards and accept
    // thread; start it (non-blocking) before gRPC.
    std::optional<HttpServer> httpServer;
    if (config.server.http.enabled) {
      httpServer.emplace(node, config.server.http.resolveThreads(), config.server.http.port);
      httpServer->start();
    }

    GRPCServer server(node, config.server.grpc.resolveThreads(), config.server.grpc.port);
    // Declared after the servers so it is joined before they are destroyed.
    SignalWatcher signals{stop};
    std::thread grpcThread([&] { server.run(); stop.release(); });
    started = server.waitForStart();
    if (started) stop.acquire();

    // Drain: stop accepting, let in-flight requests finish, then join threads.
    server.shutdown();
    grpcThread.join();
    if (httpServer) httpServer->shutdown();
  } catch (const std::exception &e) {
    LOG_ERROR("Startup failed: {}", e.what());
    return 1;
  }
  if (!started) return 1;
  LOG_INFO("Shutdown complete");
  return 0;
}

std::string luxir_banner() {
  std::stringstream ss;
  ss << "Luxir " LUXIR_VERSION " (" LUXIR_CPU_NAME ")";
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
  ss << "\tallocator=" << allocatorName();
  ss << " hw_threads=" << std::thread::hardware_concurrency();
  ss << " cwd=" << fs::current_path();
  ss << " tmp=" << fs::temp_directory_path();


  return ss.str();
}
