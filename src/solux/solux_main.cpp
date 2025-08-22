#include <filesystem>
#include <sstream>
#include <thread>
#include "solux/util/solux_util.h"
#include "solux/server/GRPCServer.h"

namespace fs = std::filesystem;

using namespace solux;

int solux_main(int argc, char** argv) {
  unused(argc, argv);
  SoluxNode node;
  // Use default port 50051 for production
  GRPCServer server(node, std::max(1u, std::thread::hardware_concurrency() / 2), 50051);
  server.run();
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
