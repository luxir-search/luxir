// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/util/CrashHandler.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <cxxabi.h>
#include <pthread.h>
#include <unistd.h>
#include <unwind.h>
#include "luxir/BuildInfo.h"
#include "luxir/util/InflightRegistry.h"
#include "luxir/util/SignalWriter.h"

#if defined(__linux__)
#include <link.h>
#include <sys/prctl.h>
#if __has_include(<stacktrace>)
#include <stacktrace>
#endif
#endif

// std::stacktrace (libstdc++exp, libbacktrace) reads the symbol table and any
// DWARF of the running executable; Linux links it in (see CMakeLists.txt).
#if defined(__linux__) && defined(__cpp_lib_stacktrace)
#define LUXIR_CRASH_STACKTRACE 1
#else
#define LUXIR_CRASH_STACKTRACE 0
#endif

namespace luxir {

namespace {

// ASan installs its own reporting handlers; keep them.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#else
constexpr bool kAddressSanitizer = false;
#endif

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr int kMaxFrames = 64;
constexpr size_t kAltStackSize = 128 * 1024;
// Symbolizing is not async-signal-safe (it allocates and reads files); a crash
// inside the allocator could make it hang, so it gets this long before the
// process dies by the original signal anyway.
constexpr unsigned kSymbolizeSeconds = 5;

std::atomic<bool> installed{false};
std::atomic<bool> crashing{false};
volatile sig_atomic_t fatalSignal = 0;

// Filled once at install so the handler only copies bytes.
char versionLine[256];
char buildId[2 * 64 + 1];
uintptr_t loadBase = 0;
uintptr_t textBegin = 0;
uintptr_t textEnd = 0;

// Buffered stderr writer that only calls write(2): usable in a signal handler.
class Out : public SignalWriter {
public:
  explicit Out(int fd = STDERR_FILENO) : SignalWriter(fd) {}
};

const char* signalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
  }
}

struct Frames {
  uintptr_t pc[kMaxFrames];
  int n = 0;
};

_Unwind_Reason_Code collectFrame(_Unwind_Context* context, void* arg) {
  auto& frames = *static_cast<Frames*>(arg);
  int beforeInsn = 0;
  uintptr_t pc = _Unwind_GetIPInfo(context, &beforeInsn);
  if (pc == 0) return _URC_END_OF_STACK;
  // A return address points after its call; step back into the call so
  // addr2line names the calling line. A signal frame's pc is exact.
  frames.pc[frames.n++] = beforeInsn ? pc : pc - 1;
  return frames.n == kMaxFrames ? _URC_END_OF_STACK : _URC_NO_REASON;
}

#if defined(__linux__)
// The first object dl_iterate_phdr reports is the executable.
int readExecutable(dl_phdr_info* info, size_t, void*) {
  loadBase = info->dlpi_addr;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
      uintptr_t begin = loadBase + ph.p_vaddr;
      if (!textBegin || begin < textBegin) textBegin = begin;
      if (begin + ph.p_memsz > textEnd) textEnd = begin + ph.p_memsz;
    }
    if (ph.p_type != PT_NOTE) continue;
    auto* p = reinterpret_cast<const char*>(loadBase + ph.p_vaddr);
    const char* end = p + ph.p_memsz;
    while (p + sizeof(ElfW(Nhdr)) <= end) {
      auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
      const char* name = p + sizeof(ElfW(Nhdr));
      const auto* desc = reinterpret_cast<const unsigned char*>(name + ((note->n_namesz + 3) & ~3u));
      if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && memcmp(name, "GNU", 4) == 0) {
        size_t len = std::min<size_t>(note->n_descsz, (sizeof(buildId) - 1) / 2);
        for (size_t j = 0; j < len; j++) {
          buildId[2 * j] = "0123456789abcdef"[desc[j] >> 4];
          buildId[2 * j + 1] = "0123456789abcdef"[desc[j] & 15];
        }
        buildId[2 * len] = '\0';
      }
      p = reinterpret_cast<const char*>(desc) + ((note->n_descsz + 3) & ~3u);
    }
  }
  return 1;  // stop after the executable
}
#endif

void writeThreadName(Out& out) {
  char name[32] = {};
#if defined(__linux__)
  prctl(PR_GET_NAME, name, 0, 0, 0);
#else
  pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
  out.str(name[0] ? name : "?");
}

// Die by `sig` with its default action (a core dump for these signals).
[[noreturn]] void dieBy(int sig) {
  struct sigaction dfl {};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);
  raise(sig);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);  // deliver it now, even inside its handler
  _exit(128 + sig);
}

#if LUXIR_CRASH_STACKTRACE
void onSymbolizeTimeout(int) {
  Out().str("(symbolized stack trace timed out)\n");
  dieBy(fatalSignal ? (int)fatalSignal : SIGABRT);
}

void writeSymbolizedTrace() {
  struct sigaction alarmAction {};
  alarmAction.sa_handler = onSymbolizeTimeout;
  sigemptyset(&alarmAction.sa_mask);
  sigaction(SIGALRM, &alarmAction, nullptr);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGALRM);
  pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
  alarm(kSymbolizeSeconds);
  try {
    std::string text = "Stack trace:\n" + std::to_string(std::stacktrace::current(1, kMaxFrames)) + "\n";
    size_t done = 0;
    while (done < text.size()) {
      ssize_t w = ::write(STDERR_FILENO, text.data() + done, text.size() - done);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) break;
      done += (size_t)w;
    }
  } catch (...) {
  }
  alarm(0);
}
#endif

struct Report {
  int sig;
  siginfo_t* info;
  uint64_t thread;
  Frames frames;
};

// The async-signal-safe part of the report: signal, build and raw frames.
void writeReport(int fd, void* context) {
  const auto& report = *static_cast<const Report*>(context);
  Out out(fd);
  out.str("Luxir crashed: ").str(signalName(report.sig)).str(" (code ").dec(report.info->si_code);
  // si_addr is meaningful only for a fault the kernel raised (si_code > 0);
  // kill(2), raise(3) and abort(3) fill in the sender instead.
  if (report.info->si_code > 0 && report.sig != SIGABRT) out.str(", addr ").hex((uintptr_t)report.info->si_addr);
  if (report.info->si_code <= 0) out.str(", sent by pid ").dec(report.info->si_pid);
  out.str(") in thread ");
  writeThreadName(out);
  out.str(" (tid ").dec((long long)report.thread).str(")\n").str(versionLine).str("\nbuild-id ")
     .str(buildId[0] ? buildId : "unknown").str(" load base ").hex(loadBase).str("\n");
  for (int i = 0; i < report.frames.n; i++) {
    uintptr_t pc = report.frames.pc[i];
    out.str("#").dec(i).str(" ").hex(pc);
    if (pc >= textBegin && pc < textEnd) out.str(" (luxir+").hex(pc - loadBase).str(")");
    out.str("\n");
  }
}

void onFatalSignal(int sig, siginfo_t* info, void*) {
  if (crashing.exchange(true)) {
    // Another thread is reporting; it ends the process.
    for (;;) pause();
  }
  fatalSignal = sig;
  Report report{sig, info, InflightRegistry::currentThread(), {}};
  _Unwind_Backtrace(collectFrame, &report.frames);
  writeReport(STDERR_FILENO, &report);
  {
    Out out;
    InflightRegistry::dumpAll(out, report.thread);
  }
  // The next start reads the cause from the request file; the crash file in
  // the data directory keeps this report when stderr is lost.
  InflightRegistry::recordFatalSignal(sig);
  InflightRegistry::writeCrashFiles(writeReport, &report, report.thread);
#if LUXIR_CRASH_STACKTRACE
  writeSymbolizedTrace();
#endif
  dieBy(sig);
}

[[noreturn]] void onTerminate() {
  // Not a signal handler: this may allocate.
  std::string text;
  try {
    if (auto error = std::current_exception()) {
      text = "uncaught exception";
      if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        text += " ";
        text += status == 0 && name ? name : type->name();
        free(name);
      }
      try {
        std::rethrow_exception(error);
      } catch (const std::exception& e) {
        text += ": ";
        text += e.what();
      } catch (...) {
      }
    } else {
      text = "std::terminate called without an active exception";
    }
  } catch (...) {
    if (text.empty()) text = "uncaught exception";
  }
  {
    Out out;
    out.str("Luxir terminating: ").str(text.c_str()).str("\n");
  }
  InflightRegistry::recordTerminate(text.c_str());
  std::abort();
}

struct AltStack {
  void* memory = nullptr;

  ~AltStack() {
    if (!memory) return;
    stack_t off {};
    off.ss_flags = SS_DISABLE;
    sigaltstack(&off, nullptr);
    free(memory);
  }
};

thread_local AltStack altStack;

} // namespace

bool crashHandlerInstalled() {
  return installed.load(std::memory_order_acquire);
}

void installCrashAltStack() {
  if (!crashHandlerInstalled() || altStack.memory) return;
  stack_t current {};
  if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) return;  // keep a runtime's own
  void* memory = malloc(kAltStackSize);
  if (!memory) return;
  stack_t stack {};
  stack.ss_sp = memory;
  stack.ss_size = kAltStackSize;
  if (sigaltstack(&stack, nullptr) != 0) {
    free(memory);
    return;
  }
  altStack.memory = memory;
}

void installCrashHandler() {
  if (kAddressSanitizer || installed.load(std::memory_order_acquire)) return;
  const auto& build = buildInfo();
  snprintf(versionLine, sizeof(versionLine), "Luxir %.*s (%.*s) commit %.*s",
           (int)build.version.size(), build.version.data(),
           (int)build.cpuTarget.size(), build.cpuTarget.data(),
           (int)build.sourceCommit.size(), build.sourceCommit.data());
#if defined(__linux__)
  dl_iterate_phdr(readExecutable, nullptr);
#endif
  // The unwinder registers and caches on first use; do that now, not in a crash.
  Frames warmup;
  _Unwind_Backtrace(collectFrame, &warmup);

  struct sigaction action {};
  action.sa_sigaction = onFatalSignal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  for (int sig : kFatalSignals) sigaction(sig, &action, nullptr);
  std::set_terminate(onTerminate);
  installed.store(true, std::memory_order_release);
  installCrashAltStack();
}

} // namespace luxir
