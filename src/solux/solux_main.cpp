#include <sstream>
#include <solux/util/solux_util.h>

using namespace solux;

int solux_main(int argc, char** argv) {
  unused(argc, argv);
  return 0;
}

std::string compile_env() {
  std::stringstream ss;
  ss << "INFO:";
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
#ifdef __linux__
  ss << " __linux__=" << __linux__;
#endif
  return ss.str();
}