// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace luxir {

// Call before starting engine workers. Settings are process-wide and read once;
// no build or query changes the BLAS worker pool while other calls are active.
void initializeNumericalLibraries();

// FAISS OpenMP team size for one vector build: LUXIR_VECTOR_BUILD_THREADS, or
// the CPUs available to the process (affinity and cgroup quota) when unset.
int vectorBuildThreads();

// OpenMP settings belong to the calling task, unlike the BLAS worker pool.
// Each TBB build task establishes its own budget and restores it on exit.
class VectorBuildThreads {
  int previous;
public:
  VectorBuildThreads();
  ~VectorBuildThreads();
  VectorBuildThreads(const VectorBuildThreads&) = delete;
  VectorBuildThreads& operator=(const VectorBuildThreads&) = delete;
};

}
