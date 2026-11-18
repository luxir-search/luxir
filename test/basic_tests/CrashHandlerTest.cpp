// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>
#include <gtest/gtest.h>
#include "luxir/util/CrashHandler.h"
#include "luxir/util/thread.h"

using namespace luxir;

namespace {

// The child of a death test must not leave a core file behind.
void installWithoutCore() {
  rlimit none{0, 0};
  setrlimit(RLIMIT_CORE, &none);
  installCrashHandler();
}

// Read through a volatile so the compiler cannot see (and warn about) the bad address.
volatile uintptr_t faultAddress = 16;

[[gnu::noinline]] void crashHandlerTestFault() {
  *(volatile int*)faultAddress = 1;
}

[[gnu::noinline]] void crashHandlerTestAbort() {
  std::abort();
}

[[gnu::noinline]] int crashHandlerTestRecurse(int depth) {
  if (depth == INT32_MAX) return 0;
  volatile char frame[4096];
  frame[0] = (char)depth;
  return crashHandlerTestRecurse(depth + 1) + frame[0];
}

class CrashHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
#if defined(__SANITIZE_ADDRESS__)
    GTEST_SKIP() << "ASan builds keep ASan's own crash reports";
#endif
    GTEST_FLAG_SET(death_test_style, "threadsafe");
  }
};

} // namespace

// The report names the signal, fault address and build, then the faulting
// function, and the process still dies by the signal (so it dumps core).
TEST_F(CrashHandlerTest, segfaultReportsAndDiesBySignal) {
  EXPECT_EXIT({
    installWithoutCore();
    crashHandlerTestFault();
  }, ::testing::KilledBySignal(SIGSEGV),
     "Luxir crashed: SIGSEGV \\(code [0-9]+, addr 0x10\\) in thread .*\n"
     "Luxir .* commit .*\nbuild-id [0-9a-f]+ load base 0x[0-9a-f]+\n#0 0x[0-9a-f]+ \\(luxir\\+0x"
     ".*Stack trace:.*crashHandlerTestFault");
}

TEST_F(CrashHandlerTest, abortReports) {
  EXPECT_EXIT({
    installWithoutCore();
    crashHandlerTestAbort();
  }, ::testing::KilledBySignal(SIGABRT), "Luxir crashed: SIGABRT.*crashHandlerTestAbort");
}

TEST_F(CrashHandlerTest, uncaughtExceptionNamesItThenAborts) {
  EXPECT_EXIT({
    installWithoutCore();
    std::thread([] { throw std::runtime_error("crash handler test"); }).join();
  }, ::testing::KilledBySignal(SIGABRT),
     "Luxir terminating: uncaught exception std::runtime_error: crash handler test\n"
     "Luxir crashed: SIGABRT");
}

// A named thread has an alternate signal stack, so its stack overflow is reported.
TEST_F(CrashHandlerTest, stackOverflowOnNamedThreadReports) {
  EXPECT_EXIT({
    installWithoutCore();
    std::thread([] {
      nameThisThread("crash_test");
      crashHandlerTestRecurse(0);
    }).join();
  }, ::testing::KilledBySignal(SIGSEGV), "Luxir crashed: SIGSEGV .* in thread crash_test");
}
