// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "NumericalLibraries.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <omp.h>
#include <oneapi/tbb/info.h>

#if LUXIR_BLAS_OpenBLAS
extern "C" {
int openblas_get_parallel();
int openblas_get_num_threads();
void openblas_set_num_threads(int);
char* openblas_get_config();
int blas_thread_shutdown_();
}
#endif

namespace luxir {
namespace {
int buildThreads = 1;

int readThreads(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (!value) return fallback;
  int count = 0;
  const char* end = value + std::strlen(value);
  auto result = std::from_chars(value, end, count);
  if (result.ec != std::errc{} || result.ptr != end || count < 1) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  return count;
}
}

void initializeNumericalLibraries() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Commit-time overlays are built one segment at a time on the committing
    // task, blocking the refresh, so a one-worker default leaves the build on
    // a single core.  Default to the CPUs this process may use; TBB counts
    // the affinity mask and the cgroup CPU quota, while libgomp's own default
    // ignores the quota and would size teams by host CPUs in a container.
    int cpus = std::max(1, oneapi::tbb::info::default_concurrency());
    buildThreads = readThreads("LUXIR_VECTOR_BUILD_THREADS", cpus);
#if LUXIR_BLAS_OpenBLAS
    if (openblas_get_parallel() != 1) {
      throw std::runtime_error("Luxir requires pthread OpenBLAS; rebuild dependencies with the threads feature");
    }
    if (std::strstr(openblas_get_config(), "USE64BITINT")) {
      throw std::runtime_error("Luxir/FAISS requires LP64 OpenBLAS, not INTERFACE64");
    }
    // Match OpenBLAS precedence. OMP_NUM_THREADS also controls OpenBLAS when
    // its own vars are unset; with none set, use the same CPU budget. FAISS
    // issues its GEMMs (k-means, list assignment, RaBitQ rotation) from the
    // calling thread outside its OpenMP regions, so the two pools take turns.
    const char* source = nullptr;
    for (const char* name : {"OPENBLAS_NUM_THREADS", "OPENBLAS_DEFAULT_NUM_THREADS",
                             "GOTO_NUM_THREADS", "OMP_NUM_THREADS"}) {
      if (std::getenv(name)) {
        source = name;
        break;
      }
    }
    int threads = source ? readThreads(source, 1) : cpus;
    // OpenBLAS starts its worker pool from a static constructor, before
    // luxir_main blocks the shutdown signals, so those workers would take
    // SIGTERM's default action and kill the process.  Restart the pool here
    // (set_num_threads re-creates it lazily, as after fork) so the workers
    // inherit the caller's signal mask.
    blas_thread_shutdown_();
    openblas_set_num_threads(threads);
    // An explicit request above the library's build limit is an error; the
    // derived default just takes what the library can run.
    if (source && openblas_get_num_threads() != threads) {
      throw std::invalid_argument("Requested OpenBLAS thread count exceeds this library's build limit");
    }
#elif LUXIR_BLAS_AOCL
    // BLIS reads its process-wide environment before concurrent engine work.
    // Require an explicit budget rather than inherit a machine-wide default.
    if (readThreads("BLIS_NUM_THREADS", 0) == 0) {
      throw std::invalid_argument("AOCL requires BLIS_NUM_THREADS to be set before launching Luxir");
    }
#elif LUXIR_BLAS_ArmPL
    if (readThreads("OMP_NUM_THREADS", 0) == 0) {
      throw std::invalid_argument("ArmPL requires OMP_NUM_THREADS to be set before launching Luxir");
    }
#endif
  });
}

int vectorBuildThreads() {
  initializeNumericalLibraries();
  return buildThreads;
}

VectorBuildThreads::VectorBuildThreads() : previous(omp_get_max_threads()) {
  initializeNumericalLibraries();
  omp_set_num_threads(buildThreads);
}

VectorBuildThreads::~VectorBuildThreads() {
  omp_set_num_threads(previous);
}

}
