// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace luxir {

// Report fatal signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) and
// std::terminate on stderr, then die by the same signal so the process still
// dumps core. The report starts with lines written by async-signal-safe code
// (signal, fault address, thread, build ID, load base, raw frames), followed
// by a symbolized stack trace on Linux when one can be produced within a few
// seconds. Does nothing in AddressSanitizer builds, which report crashes
// themselves. Idempotent.
void installCrashHandler();

// Give the calling thread an alternate signal stack so a stack overflow on it
// is still reported. No-op until installCrashHandler() has run; the stack is
// released when the thread exits. nameThisThread() calls this.
void installCrashAltStack();

bool crashHandlerInstalled();

} // namespace luxir
