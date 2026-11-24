// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ArenaNice.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <thread>
#include "luxir/util/log.h"
#include "luxir/util/luxir_util.h"
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace luxir {

#if defined(__linux__)
static id_t threadId() { return (id_t)syscall(SYS_gettid); }

// A thread's nice value, or INT_MIN when it cannot be read (getpriority can
// legitimately return -1).
static int threadNice() {
  errno = 0;
  int nice = getpriority(PRIO_PROCESS, threadId());
  return errno != 0 ? INT_MIN : nice;
}

static void restoreNice(id_t thread, int nice) {
  if (setpriority(PRIO_PROCESS, thread, nice) == 0 || errno == ESRCH) return;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true)) {
    LOG_WARN("Restoring a worker's nice after background work failed ({}); it keeps the background priority",
             strerror(errno));
  }
}
#endif

void ArenaNice::on_scheduler_entry(bool worker) {
#if defined(__linux__)
  if (!worker) return;
  int base = threadNice();
  if (base == INT_MIN) return;
  std::lock_guard lock(mutex);
  if (setpriority(PRIO_PROCESS, threadId(), std::min(19, base + increment)) == 0) niced[(long)threadId()] = base;
#else
  unused(worker);
#endif
}

void ArenaNice::on_scheduler_exit(bool worker) {
#if defined(__linux__)
  if (!worker) return;
  std::lock_guard lock(mutex);
  auto it = niced.find((long)threadId());
  if (it == niced.end()) return;
  restoreNice(threadId(), it->second);
  niced.erase(it);
#else
  unused(worker);
#endif
}

ArenaNice::~ArenaNice() {
  observe(false);  // returns once no callback is running
#if defined(__linux__)
  std::lock_guard lock(mutex);
  for (auto [thread, nice] : niced) restoreNice((id_t)thread, nice);
#endif
}

int ArenaNice::resolve(int configured, int autoIncrement, std::string_view option) {
  if (configured == 0) return 0;
  int increment = configured < 0 ? autoIncrement : configured;
#if defined(__linux__)
  bool restored = false;
  std::thread([&] {
    int base = threadNice();
    if (base == INT_MIN || setpriority(PRIO_PROCESS, threadId(), std::min(19, base + increment)) != 0) return;
    restored = setpriority(PRIO_PROCESS, threadId(), base) == 0;
  }).join();
  if (restored) return increment;
  if (configured > 0) {
    LOG_WARN("{}={} ignored: restoring a worker's priority needs CAP_SYS_NICE or RLIMIT_NICE >= {}",
             option, configured, 20 - threadNice());
  } else {
    LOG_DEBUG("{} auto: off, restoring a worker's priority is not permitted", option);
  }
#else
  if (configured > 0) LOG_WARN("{}={} ignored: per-thread nice is Linux only", option, configured);
#endif
  return 0;
}

}
